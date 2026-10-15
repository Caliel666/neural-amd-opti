// 32-bit dxgi.dll for the D3D10 games OptiScaler.dll (x64 only) cannot load into. It forwards every export to the
// system dxgi.dll and, at each Present of a swap chain on a D3D10 device, hands the back buffer to the same 64-bit
// host as the D3D9 proxy (host.cpp, through bridge.h), which runs OptiScaler on a D3D11 swap chain on a child of the
// game's window. d3d10.dll and d3d10_1.dll make their swap chains through the dxgi.dll beside the game, so this one
// proxy covers both. Swap chains of other devices (a D3D11 game, an overlay's) are presented natively.
#include <windows.h>
#include <d3d10.h>
#include <dxgi1_2.h>

namespace
{
constexpr wchar_t LogName[] = L"dxgi-x86.log", Section[] = L"DxgiX86", SurfaceName[] = L"OptiScalerDxgiX86Surface",
                  PipePrefix[] = L"\\\\.\\pipe\\opti-dxgi-x86-";
}
#include "bridge.h"

namespace
{
HMODULE systemDxgi = nullptr;
INIT_ONCE systemOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK LoadSystem(PINIT_ONCE, PVOID, PVOID*)
{
    wchar_t folder[MAX_PATH] {};
    GetSystemDirectoryW(folder, MAX_PATH);
    systemDxgi = LoadLibraryW((std::wstring(folder) + L"\\dxgi.dll").c_str());
    return TRUE;
}
FARPROC SystemProc(const char* name)
{
    InitOnceExecuteOnce(&systemOnce, LoadSystem, nullptr, nullptr);
    return systemDxgi ? GetProcAddress(systemDxgi, name) : nullptr;
}

using CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                           const DXGI_SWAP_CHAIN_DESC1*,
                                                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                           IDXGISwapChain1**);
using PresentChain = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Chain = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using SetFullscreen = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL, IDXGIOutput*);
using ResizeChain = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using GetFullscreen = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, BOOL*, IDXGIOutput**);
using ResizeTarget = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);
CreateSwapChain createSwapChain = nullptr;
CreateSwapChainForHwnd createSwapChainForHwnd = nullptr;
PresentChain present = nullptr;
Present1Chain present1 = nullptr;
SetFullscreen setFullscreen = nullptr;
ResizeChain resizeBuffers = nullptr;
GetFullscreen getFullscreen = nullptr;
ResizeTarget resizeTarget = nullptr;

// Patches a vtable slot once. All objects of a DXGI class share one table; a second table (another class) whose slot
// holds something else than the first one's original is left alone.
void Hook(void** slot, void* replacement, void** original)
{
    if (*original && *slot != *original)
        return;
    Replace(slot, replacement, original);
}

// The UNORM twin of a back buffer format the host's flip swap chain takes; 0 for the formats it does not. A frame
// crosses at four bytes a texel (Build's pitch).
uint32_t Wire(uint32_t format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    }
    return 0;
}

// The D3D10 side of the bridge (bridge.h). The swap chain stands for the device: it names the window and the device.
struct D3D10
{
    using Device = IDXGISwapChain;
    using Back = ID3D10Texture2D;
    static constexpr const char* Name = "D3D10";

    static HWND Window(IDXGISwapChain* swap, HWND& focus)
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        if (FAILED(swap->GetDesc(&desc)))
            return nullptr;
        focus = desc.OutputWindow;
        return desc.OutputWindow;
    }
    static bool Luid(IDXGISwapChain* swap, LUID& luid)
    {
        ComPtr<IDXGIDevice> device;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc {};
        if (FAILED(swap->GetDevice(IID_PPV_ARGS(&device))) || FAILED(device->GetAdapter(&adapter)) ||
            FAILED(adapter->GetDesc(&desc)))
            return false;
        luid = desc.AdapterLuid;
        return true;
    }
    // A removed device presents natively, which gives the game the error it recreates its device on.
    static bool Lost(IDXGISwapChain* swap)
    {
        ComPtr<ID3D10Device> device;
        return FAILED(swap->GetDevice(IID_PPV_ARGS(&device))) || device->GetDeviceRemovedReason() != S_OK;
    }
    static bool Describe(ID3D10Texture2D* back, UINT& width, UINT& height, uint32_t& format)
    {
        D3D10_TEXTURE2D_DESC desc {};
        back->GetDesc(&desc);
        width = desc.Width;
        height = desc.Height;
        format = desc.Format;
        return true;
    }

    // The copy of the back buffer the host reads: a texture shared by its handle, or one read back into a section of
    // memory when the device cannot share one. A multisampled back buffer is resolved into it.
    struct Stage
    {
        ComPtr<ID3D10Device> owner;
        ComPtr<ID3D10Texture2D> texture, readback;
        ComPtr<ID3D10Query> done;
        HANDLE shared = nullptr;
        Handle section;
        void* view = nullptr;
        UINT width = 0, height = 0;
        uint32_t format = 0, wire = 0;

        void Release()
        {
            if (view)
                UnmapViewOfFile(view);
            view = nullptr;
            section.reset();
            done.Reset();
            readback.Reset();
            texture.Reset();
            owner.Reset();
            shared = nullptr;
            width = height = 0;
            format = wire = 0;
        }
        bool Made() const { return texture != nullptr; }

        bool Make(IDXGISwapChain* swap, UINT w, UINT h, uint32_t f, bool cpu)
        {
            wire = Wire(f);
            if (!wire)
            {
                Log("back buffer format %u cannot cross to the host", f);
                return false;
            }
            if (FAILED(swap->GetDevice(IID_PPV_ARGS(&owner))))
                return false;
            D3D10_TEXTURE2D_DESC desc {};
            desc.Width = w;
            desc.Height = h;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = static_cast<DXGI_FORMAT>(wire);
            desc.Usage = D3D10_USAGE_DEFAULT;
            desc.BindFlags = D3D10_BIND_RENDER_TARGET | D3D10_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D10_RESOURCE_MISC_SHARED;
            ComPtr<IDXGIResource> resource;
            if (cpu || FAILED(owner->CreateTexture2D(&desc, nullptr, &texture)) || FAILED(texture.As(&resource)) ||
                FAILED(resource->GetSharedHandle(&shared)) || !shared)
            {
                texture.Reset();
                shared = nullptr;
                desc.MiscFlags = 0;
                if (FAILED(owner->CreateTexture2D(&desc, nullptr, &texture)))
                    return false;
                desc.BindFlags = 0;
                desc.Usage = D3D10_USAGE_STAGING;
                desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
                if (FAILED(owner->CreateTexture2D(&desc, nullptr, &readback)))
                    return false;
                const DWORD bytes = w * h * 4;
                section.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes, nullptr));
                view = section ? MapViewOfFile(section.value, FILE_MAP_WRITE, 0, 0, bytes) : nullptr;
                if (!view)
                    return false;
            }
            D3D10_QUERY_DESC query { D3D10_QUERY_EVENT, 0 };
            return SUCCEEDED(owner->CreateQuery(&query, &done));
        }

        // DXGI presents the whole back buffer: there are no rectangles.
        bool Capture(IDXGISwapChain*, ID3D10Texture2D* back, const RECT*, const RECT*, HWND)
        {
            D3D10_TEXTURE2D_DESC desc {};
            back->GetDesc(&desc);
            if (desc.SampleDesc.Count > 1)
                owner->ResolveSubresource(texture.Get(), 0, back, 0, static_cast<DXGI_FORMAT>(wire));
            else
                owner->CopyResource(texture.Get(), back);
            if (shared)
            {
                done->End();
                const ULONGLONG deadline = GetTickCount64() + GpuWaitMs;
                HRESULT hr = S_FALSE;
                while ((hr = done->GetData(nullptr, 0, 0)) == S_FALSE && GetTickCount64() < deadline)
                    YieldProcessor();
                return hr == S_OK;
            }
            owner->CopyResource(readback.Get(), texture.Get());
            D3D10_MAPPED_TEXTURE2D from {};
            if (FAILED(readback->Map(0, D3D10_MAP_READ, 0, &from)))
                return false;
            const UINT row = width * 4;
            for (UINT y = 0; y < height; ++y)
                std::memcpy(static_cast<BYTE*>(view) + size_t(y) * row,
                            static_cast<const BYTE*>(from.pData) + size_t(y) * from.RowPitch, row);
            readback->Unmap(0);
            return true;
        }
    };
};
// Never destroyed, as the D3D9 proxy's: nothing of it is released from DllMain at process exit.
Bridge<D3D10>& bridge = *new Bridge<D3D10>;

thread_local bool inPresent = false;

// Runs the bridge for one Present of a D3D10 swap chain; false means present natively.
bool Bridged(IDXGISwapChain* swap, UINT interval, UINT flags)
{
    if (inPresent || (flags & DXGI_PRESENT_TEST))
        return false;
    std::lock_guard guard(lock);
    ComPtr<ID3D10Device> device;
    DXGI_SWAP_CHAIN_DESC desc {};
    // A frame latency object is signalled by the swap chain's own Present, which the game would then wait for forever.
    if (FAILED(swap->GetDevice(IID_PPV_ARGS(&device))) || FAILED(swap->GetDesc(&desc)) ||
        (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
        return false;
    // A new swap chain at the address of one the game released: the copy belongs to the old device.
    if (bridge.stage.owner && bridge.stage.owner.Get() != device.Get())
        bridge.Drop();
    ComPtr<ID3D10Texture2D> back;
    if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back))))
        return false;
    bridge.SetInterval(swap, interval);
    inPresent = true;
    const bool done = bridge.Present(swap, back.Get(), nullptr, nullptr);
    inPresent = false;
    if (!done)
        ++status.native;
    return done;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swap, UINT interval, UINT flags)
{
    if (Bridged(swap, interval, flags))
        return S_OK;
    const bool outer = !inPresent;
    inPresent = true;
    const HRESULT result = present(swap, interval, flags);
    inPresent = !outer;
    return result;
}
HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* swap, UINT interval, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* parameters)
{
    if (Bridged(swap, interval, flags))
        return S_OK;
    const bool outer = !inPresent;
    inPresent = true;
    const HRESULT result = present1(swap, interval, flags, parameters);
    inPresent = !outer;
    return result;
}
HWND WindowOf(IDXGISwapChain* swap)
{
    HWND focus = nullptr;
    return D3D10::Window(swap, focus);
}
// A fullscreen swap chain owns the display, and the host's swap chain on its window would never be seen: the swap
// chain stays windowed and its window becomes a borderless one over the monitor (bridge.h, Restyle), which the game
// is told is fullscreen. [DxgiX86] ForceWindowed=0 opts out.
HRESULT STDMETHODCALLTYPE HookSetFullscreen(IDXGISwapChain* swap, BOOL fullscreen, IDXGIOutput* output)
{
    if (!Setting(L"ForceWindowed", true))
        return setFullscreen(swap, fullscreen, output);
    std::lock_guard guard(lock);
    Restyle(WindowOf(swap), nullptr, fullscreen != FALSE);
    return S_OK;
}
HRESULT STDMETHODCALLTYPE HookGetFullscreen(IDXGISwapChain* swap, BOOL* fullscreen, IDXGIOutput** target)
{
    const HRESULT result = getFullscreen(swap, fullscreen, target);
    std::lock_guard guard(lock);
    if (SUCCEEDED(result) && borderlessFor && borderlessFor == WindowOf(swap))
    {
        if (fullscreen)
            *fullscreen = TRUE;
        if (target && !*target)
            swap->GetContainingOutput(target);
    }
    return result;
}
// A mode change of a fullscreen game would size the borderless window down.
HRESULT STDMETHODCALLTYPE HookResizeTarget(IDXGISwapChain* swap, const DXGI_MODE_DESC* mode)
{
    {
        std::lock_guard guard(lock);
        if (borderlessFor && borderlessFor == WindowOf(swap))
            return S_OK;
    }
    return resizeTarget(swap, mode);
}
// The copy follows the back buffer's size on its own; one the host has is dropped before the buffers go.
HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain* swap, UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                     UINT flags)
{
    {
        std::lock_guard guard(lock);
        if (swap == bridge.device)
            bridge.Drop();
    }
    return resizeBuffers(swap, count, width, height, format, flags);
}

void HookSwapChain(IDXGISwapChain* swap)
{
    std::lock_guard guard(lock);
    auto table = *reinterpret_cast<void***>(swap);
    Hook(&table[8], reinterpret_cast<void*>(HookPresent), reinterpret_cast<void**>(&present));
    Hook(&table[10], reinterpret_cast<void*>(HookSetFullscreen), reinterpret_cast<void**>(&setFullscreen));
    Hook(&table[11], reinterpret_cast<void*>(HookGetFullscreen), reinterpret_cast<void**>(&getFullscreen));
    Hook(&table[13], reinterpret_cast<void*>(HookResize), reinterpret_cast<void**>(&resizeBuffers));
    Hook(&table[14], reinterpret_cast<void*>(HookResizeTarget), reinterpret_cast<void**>(&resizeTarget));
    ComPtr<IDXGISwapChain1> swap1;
    if (SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&swap1))))
        Hook(&(*reinterpret_cast<void***>(swap1.Get()))[22], reinterpret_cast<void*>(HookPresent1),
             reinterpret_cast<void**>(&present1));
}
// Only swap chains of D3D10 devices; a D3D11 device's is left as it is.
bool OfD3D10(IUnknown* device)
{
    ComPtr<ID3D10Device> d3d10;
    return device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&d3d10)));
}
HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** out)
{
    if (!OfD3D10(device) || !desc)
        return createSwapChain(factory, device, desc, out);
    DXGI_SWAP_CHAIN_DESC local = *desc;
    const bool borderless = !local.Windowed && Setting(L"ForceWindowed", true);
    if (borderless)
    {
        local.Windowed = TRUE;
        local.BufferDesc.RefreshRate = {};
        Log("fullscreen %ux%u swap chain requested; windowed and borderless so the host's swap chain is visible",
            local.BufferDesc.Width, local.BufferDesc.Height);
    }
    const HRESULT result = createSwapChain(factory, device, &local, out);
    if (SUCCEEDED(result) && out && *out)
    {
        HookSwapChain(*out);
        if (borderless)
        {
            std::lock_guard guard(lock);
            Restyle(local.OutputWindow, nullptr, true);
        }
    }
    return result;
}
HRESULT STDMETHODCALLTYPE HookCreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen,
                                                     IDXGIOutput* output, IDXGISwapChain1** out)
{
    if (!OfD3D10(device))
        return createSwapChainForHwnd(factory, device, window, desc, fullscreen, output, out);
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC local {};
    const bool borderless = fullscreen && !fullscreen->Windowed && Setting(L"ForceWindowed", true);
    if (borderless)
    {
        local = *fullscreen;
        local.Windowed = TRUE;
        fullscreen = &local;
        Log("fullscreen swap chain requested; windowed and borderless so the host's swap chain is visible");
    }
    const HRESULT result = createSwapChainForHwnd(factory, device, window, desc, fullscreen, output, out);
    if (SUCCEEDED(result) && out && *out)
    {
        HookSwapChain(*out);
        if (borderless)
        {
            std::lock_guard guard(lock);
            Restyle(window, nullptr, true);
        }
    }
    return result;
}
void HookFactory(void* object)
{
    std::lock_guard guard(lock);
    if (!Setting(L"Enabled", true))
        return;
    auto factory = static_cast<IUnknown*>(object);
    auto table = *reinterpret_cast<void***>(factory);
    Hook(&table[10], reinterpret_cast<void*>(HookCreateSwapChain), reinterpret_cast<void**>(&createSwapChain));
    ComPtr<IDXGIFactory2> factory2;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2))))
        Hook(&(*reinterpret_cast<void***>(factory2.Get()))[15], reinterpret_cast<void*>(HookCreateSwapChainForHwnd),
             reinterpret_cast<void**>(&createSwapChainForHwnd));
}
} // namespace

extern "C" HRESULT WINAPI _CreateDXGIFactory(REFIID riid, void** out)
{
    auto fn = reinterpret_cast<decltype(&CreateDXGIFactory)>(SystemProc("CreateDXGIFactory"));
    const HRESULT result = fn ? fn(riid, out) : E_FAIL;
    if (SUCCEEDED(result) && out && *out)
        HookFactory(*out);
    return result;
}
extern "C" HRESULT WINAPI _CreateDXGIFactory1(REFIID riid, void** out)
{
    auto fn = reinterpret_cast<decltype(&CreateDXGIFactory1)>(SystemProc("CreateDXGIFactory1"));
    const HRESULT result = fn ? fn(riid, out) : E_FAIL;
    if (SUCCEEDED(result) && out && *out)
        HookFactory(*out);
    return result;
}
extern "C" HRESULT WINAPI _CreateDXGIFactory2(UINT flags, REFIID riid, void** out)
{
    using Create2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
    auto fn = reinterpret_cast<Create2>(SystemProc("CreateDXGIFactory2"));
    const HRESULT result = fn ? fn(flags, riid, out) : E_FAIL;
    if (SUCCEEDED(result) && out && *out)
        HookFactory(*out);
    return result;
}

// Every other export goes to the system dxgi.dll as it is: a thunk that finds the function on its first call and
// jumps to it with the caller's stack untouched.
namespace
{
const char* const forwarded[] = {
    "ApplyCompatResolutionQuirking",
    "CompatString",
    "CompatValue",
    "DXGID3D10CreateDevice",
    "DXGID3D10CreateLayeredDevice",
    "DXGID3D10GetLayeredDeviceSize",
    "DXGID3D10RegisterLayers",
    "DXGIDeclareAdapterRemovalSupport",
    "DXGIDisableVBlankVirtualization",
    "DXGIDumpJournal",
    "DXGIGetDebugInterface1",
    "DXGIReportAdapterConfiguration",
    "PIXBeginCapture",
    "PIXEndCapture",
    "PIXGetCaptureState",
    "SetAppCompatStringPointer",
    "UpdateHMDEmulationStatus",
};
FARPROC targets[std::size(forwarded)] {};
} // namespace
extern "C" FARPROC __stdcall ForwardTarget(unsigned index)
{
    if (!targets[index])
        targets[index] = SystemProc(forwarded[index]);
    return targets[index];
}
#define FORWARD(index, name)                                                                                           \
    extern "C" __declspec(naked) void Forward_##name() { __asm push index __asm call ForwardTarget __asm jmp eax }
FORWARD(0, ApplyCompatResolutionQuirking)
FORWARD(1, CompatString)
FORWARD(2, CompatValue)
FORWARD(3, DXGID3D10CreateDevice)
FORWARD(4, DXGID3D10CreateLayeredDevice)
FORWARD(5, DXGID3D10GetLayeredDeviceSize)
FORWARD(6, DXGID3D10RegisterLayers)
FORWARD(7, DXGIDeclareAdapterRemovalSupport)
FORWARD(8, DXGIDisableVBlankVirtualization)
FORWARD(9, DXGIDumpJournal)
FORWARD(10, DXGIGetDebugInterface1)
FORWARD(11, DXGIReportAdapterConfiguration)
FORWARD(12, PIXBeginCapture)
FORWARD(13, PIXEndCapture)
FORWARD(14, PIXGetCaptureState)
FORWARD(15, SetAppCompatStringPointer)
FORWARD(16, UpdateHMDEmulationStatus)
