#include "pch.h"
#include "Selector.h"
#include <Config.h>
#include <Util.h>
#include <filesystem>

namespace DlssNr::Backend
{
namespace
{
struct NeuralHubInfo
{
    HMODULE module = nullptr;
    bool hasExports = false;
    uint32_t abi = 0;
    std::string version;
};

NeuralHubInfo QueryNeuralHub()
{
    NeuralHubInfo info {};
    info.module = GetModuleHandleW(L"neuralhub_host.dll");
    if (!info.module)
        return info;

    using HostAbiFn = uint32_t (*)();
    using HostVersionFn = const char* (*)();
    const auto hostAbi = reinterpret_cast<HostAbiFn>(GetProcAddress(info.module, "NeuralHub_HostAbi"));
    const auto hostVersion = reinterpret_cast<HostVersionFn>(GetProcAddress(info.module, "NeuralHub_HostVersion"));
    if (!hostAbi || !hostVersion)
        return info;

    info.hasExports = true;
    info.abi = hostAbi();
    if (const char* version = hostVersion(); version && *version)
        info.version = version;
    return info;
}

// NrBackend is latched when the game starts. Local backends install their hooks during device creation; the
// NeuralHub mode hands off to its separately loaded host. Menu changes apply from the next launch.
const std::optional<std::string>& LaunchValue()
{
    static const std::optional<std::string> value =
        Config::Instance()->NrBackend.has_value() ? std::optional(Config::Instance()->NrBackend.value()) : std::nullopt;
    return value;
}
} // namespace

Kind RequestedKind()
{
    const auto& raw = LaunchValue();
    return raw ? ParseKind(*raw) : Kind::Daniel;
}

bool NeuralHubHostDetected()
{
    return QueryNeuralHub().module != nullptr;
}

bool NeuralHubHostAvailable()
{
    const auto info = QueryNeuralHub();
    return info.module && info.hasExports && info.abi == 1 && !info.version.empty();
}

std::string NeuralHubHostStatus()
{
    const auto info = QueryNeuralHub();
    if (!info.module)
        return "NeuralHub host not loaded; launch with the NeuralHub stub enabled.";
    if (!info.hasExports)
        return "NeuralHub host loaded, but its ABI/version exports were not found.";
    if (info.abi != 1)
        return "NeuralHub host ABI mismatch: expected ABI 1, found " + std::to_string(info.abi) + ".";
    if (info.version.empty())
        return "NeuralHub host ABI 1 loaded, but the host version is unavailable.";
    return "NeuralHub " + info.version + " loaded (ABI 1). "
           "Set Compute.Backend=D3D12 in NeuralHub to require the D3D12 path.";
}

std::string NeuralHubRuntimeLabel()
{
    const auto info = QueryNeuralHub();
    if (!info.module)
        return "NeuralHub (not loaded)";
    if (!info.hasExports || info.abi != 1 || info.version.empty())
        return "NeuralHub (unsupported host ABI)";
    return "NeuralHub " + info.version + " host";
}

Kind ActiveKindFromConfig()
{
    const auto requested = RequestedKind();
    const auto& raw = LaunchValue();
    const bool isAuto = !raw || raw->empty() || (_stricmp(raw->c_str(), "auto") == 0);
    if (isAuto)
    {
        if (NeuralHubHostAvailable())
            return Kind::NeuralHub;
        std::error_code ec;
        const auto dir = Util::DllPath().parent_path();
        const bool hasDaniel = std::filesystem::exists(dir / L"dlssnr_amd_pass1.dll", ec);
        const bool hasLmxxf = std::filesystem::exists(dir / L"LmxxfNrRuntime.dll", ec);
        if (hasLmxxf && !hasDaniel && LmxxfWired())
            return Kind::Lmxxf;
    }
    return ActiveKind(requested);
}

// Both runtimes that run between the two halves of the game's command list need the split.
bool SubmissionHooksWanted()
{
    const auto kind = ActiveKindFromConfig();
    return (LmxxfWired() && kind == Kind::Lmxxf) || kind == Kind::Mochizuki;
}

bool PresentRuntimeSupported()
{
    const auto kind = ActiveKindFromConfig();
    return kind == Kind::Daniel || kind == Kind::Mochizuki;
}
} // namespace DlssNr::Backend
