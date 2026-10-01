# mochizuki0323/DLSSNR-AMD

The Vulkan network behind `MochizukiNrRuntime.dll` (`NrBackend=mochizuki`).

- Source: https://github.com/mochizuki0323/DLSSNR-AMD
- Pinned commit: `791b04620c34f2b8c3552ccd3be17eaa53a67e6d` (v0.0.1)
- Later upstream code in use: parts of `linux/` at v0.0.2 (`4f62a8a`) and v0.0.2.1 (`4f663b5`), entries 6
  to 15 below
- Licence: MIT, `LICENSE` beside this file

## What is here

Only what the runtime build reads, at the upstream paths:

| Path | Use |
|---|---|
| `windows/src/core/` | the network runtime (`nr::Runtime`), compiled into the DLL |
| `windows/shaders/` | the network and the passes around it, GLSL |
| `windows/build/build_network.py`, `unroll_glsl.py` | GLSL to SPIR-V, the upstream way |
| `windows/build/arch/rdna4.sh` | the constants host and shaders share; `tools/build-mochizuki-runtime.cmd` repeats them |
| `linux/package/model-tools/` | makes `dlssnr.bin` from a user's `nvngx_dlssnr.dll` 310.8.0 |

Upstream's `windows/src/pe/` (its own NGX and ReShade hosts, which need the game to run on vkd3d-proton) is
not used. `OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrRuntime.cpp` hosts the runtime instead, on a
Vulkan device of its own, behind the lmxxf C ABI.

## Local patches

1. `windows/src/core/nr_runtime.cpp`, the depth blit's `srcSubresource`: the aspect is cast to
   `VkImageAspectFlags`. MSVC rejects the enum-to-flags narrowing in a braced initializer; GCC accepts it.
2. `windows/shaders/rdna4/fswin_t.comp`: word views for the e4m3 weight reads and for the persistent runs'
   stores. It speeds up the Windows (LLPC) build, the output is byte-identical, and it is a candidate upstream PR.
   - Weights: `NR_C32_WEIGHT`, `NR_WEIGHT_PAIR` and the packed expand read the packed records through a `uvec2`
     view of binding 4 (`wexpand_u2`) instead of `fe4m3vec4`.
     - The loads were already dword-wide. But LLPC put an identity `v_perm_b32` (a register copy) on every weight
       fragment: 3281 in the 32 pipelines the runtime creates, 611 of them in `g_fswinpds256`. Read as words,
       218 are left, 158 of them in the two pre blocks that keep the old read.
     - The index is counted in words (`base/8`). That is exact because every weight matrix starts on a 256-byte
       boundary (`put(..., 256)` in `nr_graph.cpp`). One `uvec4` per pair record keeps the copies.
     - `NR_C32_WORDS` defaults to 1, and to 0 in the fused image-input pre block (`NR_FUSED_IMAGE_INPUT`). There
       the view takes 192 -> 201 VGPRs (8 -> 7 waves per SIMD) and the block ran 5% slower.
   - Stores: a persistent run (`NR_PERSIST`, coherent arena) stored one byte at a time. Two stores now go through
     uint views:
     - the window epilogue's fragment store (`NR_STORE_ACC_COL_ACT`, binding 0): 64 `buffer_store_b8` become 8
       `buffer_store_b64` per pipeline;
     - the DS projection's store (`act_x4v`, binding 5): 32 `buffer_store_b8` become 8 `buffer_store_b32`.
   - Unchanged: the other e4m3 loads (the `coopMatLoad` A fragments, the post-blend read, `attn.comp`,
     `ffwd3_t.comp`) already compile to dword loads with no instruction before the WMMA.
   - Changed SPIR-V:
     - every `fswin_t.comp` network pipeline except `fswinimagepreds32`: 22 pipelines, which
       `build_network.py --check` lists;
     - `temporal/temporal_post_fp32`.
   - The output is byte-identical. Checked on 2026-09-25 (RX 9070 XT, driver 26.8.1) by running the old and the new
     SPIR-V on the same input. These checks run 18 of the 22 changed network pipelines and `temporal_post_fp32`:
     - M0's EQUIV set (the runtime at 720p, 1080p and 1440p) and `nr_graph --out-image` at 1080p and 1440p, 3 runs
       each. They run the persistent path: `fswin32`, `fswindsp32/64/128`, `fswinfusedup32/64/128`,
       `fswinimagepost32`, `fswinp64/128`, `fswinpds256`, `fswinpup256` and `temporal_post_fp32`.
     - 3840x2160, where the C=256 persistent runs fold in neither the downsample nor the upsample layer:
       `nr_graph --out-image` (3 runs each) and the runtime (`mz_bench` dumps). They add `fswinp256`, `fswindsp256`
       and `fswinfusedup256`.
     - 1080p without persistent runs: `nr_graph --no-persist` (3 runs each) and the runtime with `NR_NO_PERSIST=1`.
       They add `fswin64`, `fswin128` and `fswin256`.
     - `fswinds32/64/128/256` are compiled but never dispatched. `NR_DS_FUSE=15` turns every downsample layer into
       `fswindsp<C>`. The one switch that keeps `fswinds<C>`, `nr_graph --ds-tok-raster`, also needs a `gemmds`
       pipeline, which this build does not make. The only part of this change that `fswinds<C>` compiles is the
       weight read, and `fswindsp<C>`, checked at every width, compiles the same code.
   - Timing, against the previous build:
     - the runtime's D3D12-queue gap (`mz_timing`, 5 interleaved pairs) went from 10.39 to 9.96 ms at 1080p and
       from 17.40 to 16.86 ms at 1440p;
     - standalone `nr_graph` went from 9.88 to 9.50 ms at 1080p, from 16.00 to 15.45 ms at 1440p and from 35.92
       to 35.24 ms at 3840x2160;
     - `fswinpup256` got 30% faster and `fswinpds256` 12% faster.
3. `windows/shaders/rdna4/pipelines.json`: two work-split knobs retuned for LLPC. Each changes only how the work
   is split between waves, so the output is byte-identical. Neither knob is in `shader-constants.txt`, so the host
   is unchanged.
   - `fswin32`: `NR_FWAVES` 1 -> 2, so two waves share a window.
     - It is the one pipeline where `NR_FWAVES` is live. `NR_HWAVES=1` makes it inert at C >= 64, and the DS,
       fused-upsample and `NR_K_REGS` bodies need 1 at C=32.
     - Upstream ships 1 (tuned on RADV; see the `NR_FWAVES` notes in `fswin_t.comp`).
     - On LLPC, 2 waves take the pipeline from 181 to 125 VGPRs and from 8 to 10 waves per SIMD. Its 6 dispatches
       went from 1021 to 952 us at 1080p (-6.8%) and from 1767 to 1707 us at 1440p (-3.4%).
     - 4 waves (91 VGPRs) is faster than 1 but slower than 2: -4.9%.
   - `fswinfusedup128`: `NR_EXPAND_GROUP` 4 -> 2, the value the persistent C=128 body `fswinp128` already uses.
     VGPRs stay at 244. Changed alone, the dispatch went from 197.6 to 193.6 us at 1080p (-2.0%, 5 runs; -2.1% and
     -2.9% in two other batches) and from 336.2 to 317.9 us at 1440p (-5.4%). Beside the `fswin32` change its
     1080p gain moved between sessions, from -0.6% to -9.6%; it was -4.5% and -7.6% at 1440p and -17.4% at
     3840x2160.
   - Rule: a knob was adopted only when it was byte-identical and at least 2% faster on its own dispatches at both
     1080p and 1440p. The sweep used `nr_graph --per-layer`, run interleaved with the unchanged build. The tool is
     `tools/sweep_mochizuki_knobs.py`.
   - Swept on 2026-09-25 (RX 9070 XT, driver 26.8.1) and not adopted. Every variant that built was byte-identical;
     the times are at 1080p.
     - `fswinpds256`, `NR_EXPAND_GROUP` 4 -> 2: 256 -> 222 VGPRs, the 16 B of scratch gone, and 5 -> 6 waves per
       SIMD. It is still 12.7% slower: each B fragment out of LDS now feeds 2 MMAs instead of 4 (`ds_load`
       298 -> 362).
     - `NR_EXPAND_GROUP` 4 -> 2 on `fswinfusedup64` (+0.6%) and `fswindsp128` (+0.8%).
     - `NR_EXPAND_GROUP` 2 -> 4 on:
       - `fswinp64`: -1.3%, and -0.3% alone over 5 runs, below the bar;
       - `fswinp128`: +30%, at 219 -> 255 VGPRs;
       - `fswindsp64`: +0.6%;
       - `fswinpup256`: +46%, at 192 -> 213 VGPRs.
     - `fswinp64` with the token split (`NR_HWAVES=0`, `NR_FWAVES=2`) does not compile. The paired weights exist
       only on the head-split path at C >= 64 (`fswin_t.comp`, "paired weights require the head-split path"), and
       the host packs every C >= 64 fswin matrix in pairs (`weight_layout 3`).
   - Checked by running the old and the new SPIR-V on the same input:
     - `nr_graph --out-image` at 1080p, 1440p and 3840x2160, 3 runs each;
     - the runtime against M0's EQUIV set (720p, 1080p and 1440p).
   - Timing, against the previous build. The gain moved between sessions, so each figure is the range over the
     sessions run:
     - the runtime's D3D12-queue gap (`mz_timing`, median of 5 interleaved pairs, 3 sessions): -0.04 to -0.09 ms
       at 1080p (9.69 -> 9.60 ms in the first) and -0.06 to -0.08 ms at 1440p (16.51 -> 16.35 ms in the first);
     - standalone `nr_graph --per-layer`, interleaved: -0.043 to -0.065 ms at 1080p (3 sessions; 8.963 -> 8.898
       ms in the first), -0.058 to -0.061 ms at 1440p (2 sessions; 15.208 -> 15.150 ms) and -0.116 ms at
       3840x2160 (1 session; 33.694 -> 33.578 ms);
     - cold pipeline compilation is unchanged: network ready after 22.2 and 22.0 s, both before and after.
4. `windows/shaders/rdna4/fswin_t.comp`, `include/coopmm.glsl` and `pipelines.json`: two codegen knobs for LLPC, set
   per pipeline in `pipelines.json`. Both leave the output byte-identical; both default to 0, which compiles the
   previous code (the SPIR-V of a build without them is unchanged).
   - `NR_QUAD`: the fswin e4m3 quantisation sites convert a fragment's eight components as two `fe4m3vec4` instead of
     four `fe4m3vec2` (`nr_quant_quad`/`nr_quant_quad32` in `coopmm.glsl`; the Q/K scalings as `nr_qscale_fast4`/
     `nr_kscale_fast4`).
     - Why: LLPC writes `s_setreg_imm32_b32 hwreg(HW_REG_WAVE_MODE, 23, 1), 0` in front of every e4m3 OpFConvert and
       never merges or hoists the writes. That is one MODE write per `v_cvt_pk_fp8_f32` for a pair, and a scalar
       conversion is a MODE write plus a whole convert of its own. A `fe4m3vec4` is one write for two converts, and
       the second convert writes the high half of the dword itself (op_sel) where two pairs also need a shift and a
       merge. Found with RGA's offline `amdllpc` on a probe shader, then confirmed on the driver's own ISA.
     - Each quad is the two pairs of the old code, component for component; only the grouping changes.
     - The bitmask covers ten sites (`NR_QD_*` in `fswin_t.comp`). Which sites pay is a register-allocation matter:
       all ten take 1.6-28% off the C >= 64 bodies, but the expand activation or the post-MLP requantisation alone
       make `fswinimagepost32` 5-8% slower.
     - Masks: 1023 (every site) on 16 pipelines; `fswindsp32` 1021 (not the expand activation); `fswinimagepost32`
       1013 (neither the expand activation nor the post-MLP requantisation), which `temporal_post_fp32` inherits.
       `fswin32` keeps the pairs: its best mask (178) was within 1% of the paired code at every resolution. The
       `fswinds<C>` pipelines keep them too: they are never dispatched in this build (see entry 2), so a change there
       could not be checked.
     - `s_setreg` in the 32 pipelines the runtime creates at 1080p: 7134 -> 4158. Instructions: 118423 -> 114538.
   - `NR_V_ROW=1` on the head-split pipelines (`NR_HWAVES=1`) except `fswindsp256`: V's tiles in `lds_y` are stored
     and read back RowMajor instead of ColumnMajor.
     - The context product reads each V tile as an A operand, eight contiguous columns of one row per lane. From the
       ColumnMajor tile LLPC gathered those bytes 16 apart: 64 `ds_load_u8` per pipeline plus the `v_perm_b32` and
       `v_lshl_or_b32` that assemble them, all in front of the MMA. RowMajor moves the byte scatter to the store
       (`ds_store_b8`, which nothing waits on) and the read becomes `ds_load_b64`. The same element reaches the same
       A component.
     - `ds_load_u8` 512 -> 0 in the 8 pipelines that had them; `fswinfusedup128` goes from 244 to 240 VGPRs, i.e.
       from 5 to 6 waves per SIMD.
     - `fswindsp256` is left out: there `NR_V_ROW` made the dispatch 42-49% slower at 3840x2160 and 16% slower in
       the 1080p `--no-persist` run.
     - Against `NR_QUAD` alone (runtime `mz_timing`, 5 interleaved pairs): -0.046 ms at 1080p and -0.069 ms at 1440p.
   - Changed SPIR-V: 18 of the 23 `fswin_t.comp` network pipelines (not `fswin32` and the four `fswinds<C>`), and
     `temporal/temporal_pre_fp32` and `temporal_post_fp32`.
   - Checked on 2026-09-25 (RX 9070 XT, driver 26.8.1) by running the old and the new SPIR-V on the same input:
     - `nr_graph --out-image` at 1080p, 1440p and 3840x2160, and at 1080p with `--no-persist`, 3 runs each: every
       image byte-identical. Together they dispatch all 18 changed network pipelines.
     - M0's EQUIV set against the goldens (`mz_equiv.py`), with the rebuilt runtime and with the previous runtime
       DLL beside the new shaders: EQUIV PASS both times. That covers `temporal_pre_fp32` and
       `temporal_post_fp32`.
     - the runtime at 1080p with `NR_NO_PERSIST=1` (`mz_bench` dumps, old against new shaders): identical.
   - Timing, against the previous build:
     - the runtime's D3D12-queue gap (`mz_timing`, median of 5 interleaved pairs): 9.76 -> 9.43 ms at 1080p (-0.35)
       and 16.97 -> 16.39 ms at 1440p (-0.58), same runtime DLL, only the shaders changed;
     - standalone `nr_graph --per-layer`, interleaved: 9.218 -> 8.879 ms at 1080p, 15.111 -> 14.602 ms at 1440p,
       34.422 -> 32.881 ms at 3840x2160, and 9.812 -> 9.498 ms at 1080p `--no-persist`;
     - the largest single changes at 1080p: `fswinfusedup128` 183 -> 108 us, `fswinp64` 819 -> 729 us a frame,
       `fswinp128` 923 -> 863 us a frame;
     - cold pipeline compilation: network ready after 22.2 and 21.8 s, against 23.1 and 22.3 s before.
   - Tried and not adopted (the details are in the P9 work notes):
     - a `uvec2` view for the persistent runs' activation loads: identical ISA, so the 8 identity `v_perm_b32` a
       persistent pipeline has left in its prologue stay;
     - `NR_C32_WORDS=1` in `fswinimagepreds32` (the 79 identity copies P4 left there): no gain beside `NR_QUAD`;
     - `vit_attn.comp`'s 128 `buffer_load_u8`: they gather V^T out of the [token][channel] tiles, 16 bytes apart.
       LLPC lowers a ColumnMajor e4m3 `coopMatLoad` to byte loads from any typed view and never to a transposing
       load, and a dword view would need the same number of loads. The kernel is 0.25 ms of the 1080p frame.
5. `windows/src/core/nr_runtime.hpp` and `nr_graph.cpp`: a host's stop flag for a network build.
   - `nr::build_cancel` is a thread-local pointer to an `std::atomic<bool>`; a host sets it around the `Runtime`
     constructor with `nr::BuildCancelScope`. The graph build's pipeline loop checks it before each pipeline. Once the
     flag is set, the loop writes the pipeline cache it has so far (`save_pipeline_cache`) and throws
     `NR build cancelled`.
   - Why: `MochizukiNrRuntime.dll`'s `Destroy` during a build (a game that ends the session during its first, cold
     build) stops waiting out a 20 s compile, and the next start keeps what was compiled.
   - It is thread-local rather than a `RuntimeConfig` field so that the constructor in `nr_runtime.cpp` stays as it
     is. It is null by default, so the standalone `nr_graph` and every other caller are unchanged. No shader, SPIR-V
     or output changes.
   - Checked on 2026-09-25 (RX 9070 XT, driver 26.8.1): a cold build (a freshly named exe, no `pipeline.cache`, no
     prewarm manifest) destroyed 3 s or 8 s into the core's serial compile. `Destroy` returned in 0.37-0.63 s (4 runs),
     and the build stopped at its next pipeline and wrote `pipeline.cache` (0.14 MB at 3 s, 0.37 MB at 8 s). Before
     the change `Destroy` waited for the whole build, 22.2 s. M0's EQUIV set is unchanged.

Entries 6 to 14 bring over parts of upstream v0.0.2 (`4f62a8a`) and v0.0.2.1 (`4f663b5`). After the pin upstream
changed only its `linux/` tree, tuned on RADV (ACO); `windows/` here stays at `791b046` apart from the files these
entries name. A piece was taken only when the output stayed byte-identical and it was faster on LLPC (AMD's Windows
driver, 26.8.1, RX 9070 XT), measured alone against the build with entries 1 to 5 and then all together. Where a
knob keeps a value other than upstream's, the entry says which and why. What was tried from those versions and not
taken is listed after entry 15. Entry 15 is the one change of the output, on purpose: the pre block rounds its
colour input as the original does.
- All nine together, against the build with entries 1 to 5 only, checked on 2026-09-27:
  - the runtime's D3D12-queue gap (`mz_timing`, median of 7 interleaved pairs): 9.641 -> 9.158 ms at 1920x1080
    (median pair delta -0.577 ms, -6.0%) and 16.003 -> 14.903 ms at 2560x1440 (-1.099 ms, -6.9%). An independent
    check rebuilt from the patch measured -0.493 and -0.484 ms at 1920x1080 and -1.096 and -1.031 ms at 2560x1440
    in two more sessions of 7 pairs, all 28 pairs faster;
  - standalone `nr_graph --per-layer` (3 runs interleaved): 10.396 -> 9.872 ms at 1080p and 15.127 -> 14.780 ms at
    1440p, 131 -> 129 and 130 -> 128 dispatches. It leaves out entries 12 to 14, which are outside the network;
  - output byte-identical: M0's EQUIV set against the goldens, `nr_graph --out-image` at 1080p, 1440p and
    3840x2160, `mz_bench` at 3840x2160, and runtime dumps against the previous build with 2 and 3 passes, model
    scale 0.5, no motion, strength, an input alpha from 0.2 to 0.94, RGBA32F and R10G10B10A2 frames, odd sizes and
    dynamic resolution (`drs_mode` 1 and 2);
  - `mz_stress` all 21/21, `MZ_STRESS_DRS_MODE=1` and `=2` all,drspad 22/22 each. An earlier `all` under CPU load
    missed `oombuild`'s 7 s limit (7.8 s), and so did the previous build, interleaved with it then (7.5 to 7.9 s);
  - cold network build at 1080p, interleaved with the previous build: 22.7 and 27.7 s against 22.5 and 27.2 s with
    no prewarm manifest, 8.0, 7.4 and 6.6 s against 7.6, 7.0 and 6.6 s with one;
  - VRAM, this process's growth while the network was built: 698 -> 714 MB at 1080p, 1132 -> 1160 MB at 1440p,
    2299 -> 2363 MB at 3840x2160 (entry 12 adds a model-sized RGBA32F image, entry 13 turns another from RGBA32F
    into the frame's format).

6. `windows/src/core/nr_shader_manifest.hpp` and `nr_graph.cpp`: three `shader-constants.txt` keys from upstream
   (`linux/src/core` at `4f62a8a` and `4f663b5`) and upstream's `slot_reading_diagnostic` helper.
   - `ffwd_gmajor`, `ffwd_fm2_min` and `persist_one` are checked against `NR_FFWD_GMAJOR`, `NR_FFWD_FM2_MIN_TOKENS`
     and `NR_PERSIST_ONE_MASK` like every other key. A shader directory that does not record one of them counts
     as 0, the defines' own default, so an older shader directory and an `nr_graph` built without the defines
     still pair. `NR_PERSIST_ONE_MAX` (400 by default) sits beside them. Entries 8 and 11 set them.
   - `slot_reading_diagnostic(argc, argv)` holds the flags that turn arena reuse off (the per-layer and scoring
     tools, which read values by slot after the frame). The list is the same.
   - Upstream's other new keys (`persist_strag`, `noise_field`, `post_alpha`) are not taken.
   - A directory that records `persist_one 4` is refused by a build without `NR_PERSIST_ONE_MASK=4` ("the SPVs were
     built with persist_one=4 and this binary dispatches 0"), and that build refuses a directory without the key.
     In the runtime DLL the reason goes to stderr only; `mochizuki_nr.log` says "arena reuse: the layout probe did
     not finish".
   - Checked on 2026-09-27 with the keys still off: `build_network.py --check` reports every SPIR-V identical,
     M0's EQUIV set passes, and `mz_timing` moves +0.013 ms at 1080p and +0.014 ms at 1440p (5 interleaved pairs,
     inside the noise).
7. `windows/shaders/rdna4/ffwd3_t.comp` and `gemm1x1.comp` are upstream's `linux/shaders/rdna4/` files at
   `4f663b5`, and two of their knobs are set in `pipelines.json`.
   - At `791b046` the two files were the same in `linux/` and `windows/`. The copies here differ from upstream's
     only in a comment's `tinlayout.hpp` path and in the `precise` below. The new code: `NR_FFWD_GMAJOR` and
     `NR_FFWD_FM` in `ffwd3_t.comp` (entry 8), `NR_SWAP_AB`, `NR_OUT_PERM`, `NR_POOL_FRAG` and `NR_QKV_NO_BCAST` in
     `gemm1x1.comp`. Each defaults to 0, which compiles the previous code: with none set, all 39 network SPVs were
     byte-identical to the previous build.
   - For LLPC the `NR_POOL_FRAG` pool's three sums are `precise`. Without it LLPC added the four rows as one chain
     of f16 adds, not in the `lds_e` pool's pairs, and the output changed (1080p frame 1: max |d| 0.026, 73% of the
     values). With it the pairs stay and the output is byte-identical.
   - `gemmvqkvnorm`: `NR_QKV_NO_BCAST=1`. After the xor butterfly every lane of a 16-lane half already holds the
     same bits, so the broadcast from lane 0 or 16 is left out: 32 `ds_bpermute_b32` and 32 `s_wait_dscnt` a wave
     fewer (its only LDS traffic), 168 -> 167 VGPRs. Its 8 dispatches: 0.338 -> 0.209 ms at 1080p (-38.5%) and
     0.349 -> 0.262 ms at 1440p (-24.8%).
   - `gemmpool`: `NR_POOL_FRAG=1`. The 2x2 pool runs in the fragment epilogue: no LDS (8192 B and 112 `ds_*`
     before), 1672 -> 1172 instructions, VGPRs unchanged at 79; the residual is read with 32 `buffer_load_u8` a
     wave. 16.8 -> 12.6 us at 1080p (-23.8%) and 21.6 -> 17.4 us at 1440p (-19.4%).
   - Both are upstream's settings. Rule as in entry 3: byte-identical and at least 2% faster on the pipeline's own
     dispatches at 1080p and 1440p (`nr_graph --per-layer`, 5 runs interleaved with the previous build).
   - `NR_SWAP_AB` (1 and 2) on `gemmproj`, `gemmprojw` and `gemmvact` stays off. It is byte-identical and the byte
     gathers go, but `gemmproj` was -1.6% / -1.4% at 1080p and +3.7% / +5.4% at 1440p, and `gemmvact` +4.8% / +6.1%
     and +14.9% / +16.1%.
   - Checked on 2026-09-27: M0's EQUIV set; `mz_bench` at 3840x2160 (30 frames) byte-identical to the previous
     build. Alone: `mz_timing` (5 interleaved pairs) -0.125 ms at 1080p and -0.074 ms at 1440p, and -0.113 and
     -0.111 in a second session; `nr_graph --per-layer` 8.687 -> 8.561 ms at 1080p and 14.754 -> 14.677 ms at 1440p.
8. `ffwd3_t.comp`'s two work-split knobs in `pipelines.json`, with upstream's host side in `nr_graph.cpp` (the grid
   and the choice of `ffwd3w`).
   - `ffwd3`: `NR_FFWD_GMAJOR=1`. The four subgroups of a workgroup share one weight group and take consecutive
     16-token tiles, so a group's weights are read once a workgroup. The host launches `8 * ceil(tiles / 4)`
     workgroups (272 at 1080p, 270 before; 480 at 1440p) and subgroups past the last tile return. 102 -> 100 VGPRs,
     no LDS.
   - `ffwd3w`, a new pipeline: `ffwd3`'s defines plus `NR_FFWD_FM=2`. Each subgroup takes two tiles, so every weight
     fragment feeds two MMAs; 166 VGPRs, no LDS or scratch. The host dispatches it instead of `ffwd3` for a layer of
     at least `NR_FFWD_FM2_MIN_TOKENS` tokens, over half as many units. With an odd tile count the last subgroup's
     second tile reads past the tensor, inside its input slot (which the plan sizes for the padded token count),
     and is not stored.
   - Keys: `ffwd_gmajor 1` and `ffwd_fm2_min 2560` (entry 6); `NR_FFWD_GMAJOR=1` and `NR_FFWD_FM2_MIN_TOKENS=2560`
     in `rdna4.sh` and `tools/build-mochizuki-runtime.cmd`. Both are upstream's values. The layer has 2160 tokens
     at 1080p and 3840 at 1440p, so `ffwd3w` runs at 1440p and above.
   - On LLPC `ffwd3w`'s own dispatches are slower than GMAJOR's `ffwd3` at both sizes (`nr_graph --per-layer`, 3
     interleaved runs: 0.348 against 0.299 ms at 1080p with the threshold at 1, 0.480 against 0.452 ms at 1440p).
     The layer after it, `gemmproj`, which reads its output, gets faster at 1440p: 0.570 ms before, 0.363-0.394
     after GMAJOR's `ffwd3`, 0.282-0.283 after `ffwd3w`. So the runtime decided the threshold (`mz_timing`, 5
     interleaved pairs against GMAJOR alone): at 1, +0.044 ms at 1080p and -0.175 ms at 1440p; at 2560, -0.272 ms at
     1440p. Upstream's note on it: "4K -14% a layer, 1080p +3%".
   - Checked on 2026-09-27: M0's EQUIV set with GMAJOR alone, with the threshold at 1 (1080p then runs `ffwd3w` on
     135 tiles) and at 2560; `nr_graph --out-image` at 1080p, 1440p and 3840x2160, `mz_bench` at 3840x2160, and
     `mz_phases` at 2880x1920 and 2400x1800 (odd tile counts that run `ffwd3w`) byte-identical to the previous
     build. Alone against entries 6 and 7: `mz_timing` -0.028 ms at 1080p and -0.267 ms at 1440p (5 interleaved
     pairs; two later sessions -0.029 / -0.299 and -0.007 / -0.332).
9. `windows/shaders/rdna4/attn.comp`, `include/attn_qkv_epi.glsl` (new), `include/vit_attn_vt_chunk.glsl` and
   `vit_attn.comp` come from upstream's `linux/` tree at `4f663b5`, and four of their knobs are on in
   `pipelines.json`.
   - `attn.comp` and `attn_qkv_epi.glsl` are upstream's files plus the Windows note on `NR_QPAD` that `attn.comp`
     had (Windows keeps `NR_QPAD=0`). `vit_attn_vt_chunk.glsl` takes upstream's `NR_VLATE_SHUFFLE` block and keeps
     the Windows activation reads (`nr_act4`, `NR_LOAD_A_ACT`, `NR_LOAD_A_COL_ACT`).
   - `vit_attn.comp` takes `NR_VLATE_SHUFFLE` and `NR_VOUT_VEC`, the second adapted. Upstream's `NR_VOUT_VEC` makes
     binding 2 writable and stores `fe4m3vec4`. Here binding 2 stays readonly, and a lane's eight output bytes go
     out as two dwords through the writable binding-0 view `act_v4`, packed as in entry 2. It ignores mode bit 1
     (interleaved output); the runtime passes mode 4.
   - With every knob off, all 39 network SPVs and the temporal and runtime SPVs were byte-identical to the previous
     build.
   - `attn`: `NR_ATTN_VFRAG=1` (V goes to LDS as ColumnMajor fragment stores), `NR_ATTN_QKSWAP=1` (Q and K are
     computed as W times X^T, so a lane holds eight consecutive dims of one token and Q and K are stored as
     fragments too) and `NR_ATTN_EDGE=1` (a wave with a token tile outside the image runs a copy of the projection
     loop without that tile's loads and MMAs; one copy per tile mask, four in all). EDGE compiles only with
     `NR_AM=2`, the shape shipped. The SPIR-V grows from 194 to 826 KB.
     - The driver's ISA, the previous build then VFRAG, +QKSWAP and +EDGE: `ds_store_b8` 96, 64, 0, 0; instructions
       2080, 2044, 1830, 3481; VGPRs 167, 166, 164, 181; `s_setreg` 67, 67, 67, 121.
     - `nr_graph --per-layer` row, 1080p / 1440p (3 interleaved runs): 0.495 / 0.778, 0.494 / 0.775, 0.461 / 0.749,
       0.459 / 0.721 ms.
   - `vitattn`: `NR_VOUT_VEC=1`. `buffer_store_b8` 32 -> 0 (4 `buffer_store_b64`), instructions 3004 -> 2578,
     VGPRs 123 -> 121; 0.256 -> 0.242 ms at 1080p and 0.511 -> 0.489 ms at 1440p.
   - Left off: `NR_ATTN_BIAS_SEED` and `NR_VLATE_SHUFFLE` change the output; `NR_ATTN_SATQ=2` adds MODE writes on
     LLPC.
   - Checked on 2026-09-27: M0's EQUIV set for each step and for the shipped set; `mz_phases` at odd sizes, model
     scale 0.5 to 0.75, 2 and 3 passes and dynamic resolution, and `mz_bench` at 3840x2160, byte-identical to the
     previous build. Alone: `mz_timing` (5 interleaved pairs) -0.086 ms at 1080p and -0.065 ms at 1440p, and -0.041
     and -0.062 in a second session; cold network build +1.15 s with no prewarm manifest (22.4 and 22.5 s against
     21.0 and 21.6 s) and +0.2 s with one.
10. `windows/src/core/nr_graph.cpp`, `pipelines.json`, `windows/build/arch/rdna4.sh` and
    `tools/build-mochizuki-runtime.cmd`: the C=128 persistent runs take in the layer next to them, the encoder's run
    the downsample layer after it and the decoder's run the wide fused upsample before it. From upstream `4f62a8a`,
    which folds C=64 too.
    - The fold rule is upstream's size test, `C < 256 || gx*gy <= 2*min(128, override)`: C=64 and C=128 pass it at
      every extent, and C=256 still folds only where its standalone grid ends in a partial round (1080p and 1440p,
      not 3840x2160). The masks pick the widths: `NR_PERSIST_DS_MASK` and `NR_PERSIST_UPS_MASK` 4 -> 6 (C=128 and
      C=256), markers `persist_ds 6` and `persist_up 6`. Upstream ships 7.
    - Two new pipelines, `fswinpds128` and `fswinpup128`: `fswinp128`'s defines plus the DS or UPS epilogue defines
      of `fswinpds256` and `fswinpup256`. From `fswinp128` they get `NR_QUANT_EXPLICIT=1`, `NR_QPAD=0` and
      `NR_EXPAND_GROUP=2`, as in `windows/` at the pin, and entry 4's `NR_QUAD=1023` and `NR_V_ROW=1`. No shader code
      changed. Driver statistics: 219 VGPRs in `fswinp128`, 225 in `fswinpds128`, 203 in `fswinpup128`; no scratch.
    - At 1080p and 1440p `fswindsp128` and `fswinfusedup128` leave the plan: 131 -> 129 dispatches at 1080p.
    - C=64 stays unfolded: on LLPC it was slower. Against `fswinp64` plus the standalone layer, `fswinpds64` took +7
      to +9 us at 1080p and +21 to +22 us at 1440p, `fswinpup64` -4 to -5 us and +5 to +7 us (224 VGPRs and 101
      SGPRs against 220 and 64). With masks 7 the runtime was 0.033 ms slower than with 6 at 1440p (`mz_timing`, 5
      pairs, all slower) and level at 1080p. Upstream's `NR_EXPAND_GROUP=4` on the folded C=128 pipelines:
      `fswinpds128` 256 VGPRs and 32 B of scratch, 15% slower at 1080p; `fswinpup128` 242 VGPRs, 26% slower.
    - Checked on 2026-09-27: M0's EQUIV set (720p, 1080p and 1440p all fold); `nr_graph --out-image` and `mz_bench`
      at 3840x2160; `nr_graph --out-image` with `NR_PERSIST_WG_128` at 1, 3 and 37 workgroups; all byte-identical.
      At 3840x2160 the folded C=128 runs have 12,524 items, under the ready queue's 65,535. Alone: the dispatches it
      replaces 1.087 -> 1.057 ms at 1080p (-2.8%) and 1.893 -> 1.852 ms at 1440p (-2.2%); `mz_timing` -0.029 to
      -0.076 ms at 1080p and -0.021 to -0.058 ms at 1440p over four sessions; cold network build 20.8 s against 22.0
      s with no manifest, 5.6 and 6.2 s against 5.8 and 5.8 s with one.
11. `windows/shaders/rdna4/fswin_t.comp`, `pipelines.json` and `windows/src/core/nr_graph.cpp`: upstream's one
    workgroup per item (`NR_PERSIST_ONE`, `4f62a8a`, as at `4f663b5`), for the C=256 run that starts with the folded
    wide upsample.
    - `nr_graph` launches that run (`fswinpup256`) with one workgroup per item when its widest layer has 65 to
      `NR_PERSIST_ONE_MAX` windows, and its persist line ends in "(one per item)". In the shader such a workgroup
      leaves after its item. `NR_PERSIST_ONE_MASK=4` and `NR_PERSIST_ONE_MAX=200` are in `NR_DEFINES` and
      `rdna4.sh`, `fswinpup256` is built with `NR_PERSIST_ONE=1`, and the marker is `persist_one 4` (entry 6). That
      covers 1920x1080 (144 windows) and 1280x720 (77); 2560x1440 (252) and 3840x2160 keep the persistent launch.
    - Upstream also launches the run that ends in the folded downsample one per item, at C=128 and C=256 (mask 6),
      up to 400 windows. On LLPC `fswinpds256` compiles to 256 VGPRs with 16 bytes of scratch, and launched one per
      item it was 15% slower at 1080p (0.715 against 0.619 ms), more than the upsample run gained, so the rule takes
      the upsample folds only. At 1440p the per-item launch gained nothing on its layer (0.877 against 0.876 ms),
      hence the 200. The C=128 folds of entry 10 keep the persistent launch; one per item was not measured on them.
      Upstream's straggler queue (`NR_STRAG`) is not taken.
    - Checked on 2026-09-27: `build_network.py --check` changes only `g_fswinpup256.spv` and `shader-constants.txt`;
      M0's EQUIV set; `mz_phases` at 7 odd and portrait sizes, dynamic resolution and `mz_bench` at 3840x2160,
      byte-identical. Alone: `nr_graph --per-layer` `fswinpup256` 0.555 -> 0.516 ms at 1080p (-7.0%; -6.8% in a
      second session), level at 1440p. `mz_timing` at 1080p: -0.098 ms in the first session and -0.003 to -0.128 ms
      in six later ones (the median of 20 of those pairs: -0.029 ms); 1440p within 0.025 ms either way.
12. `windows/src/core/nr_runtime.cpp`: with one pass the temporal history is two images in turn, and the post block
    writes the next frame's history itself. Upstream has it in `linux/src/core/nr_runtime.cpp` (`4f62a8a`), inside a
    larger change.
    - Taken: `Temporal::history_b`, `pre_pp[2]`/`post_pp[2]`, `hist_store[2]`, `pingpong`, `hcur` and `hist()`.
      `pre_pp[c]` and `post_pp[c]` read `hist(c)`, and `post_pp[c]` writes the model's image (`nr_out1`) into
      `hist(c ^ 1)` through a storage alias where `post` writes `surf1`. `record_all` dispatches the pair for `hcur`,
      records no `surf1` to history copy (one copy and 4 barriers a frame) and flips `hcur` after the frame. The
      feature bind copies and `temporal_history()` use `hist(hcur)`. With `max_passes` above 1 the runtime keeps one
      history image, `pre`/`post` and the copy.
    - Not taken: the caller's colour and depth sampled in place (`DirectSrc`), the depth copy pass, `direct_in`/
      `direct_out`, the post-block alpha and the UNORM transfer change.
    - Cost: one more model-sized RGBA32F image, 33 MB at 1080p and 59 MB at 1440p. The host's VRAM check counts it
      for every pass count: `kModelPixelBytes` in `MochizukiNrRuntime.cpp` is 288. The temporal SPIR-V is created
      twice more and the driver's in-process cache serves both (cold adapter phase 2.02 and 1.98 s against 2.01 and
      2.00 s).
    - The validation layer's `VUID-VkWriteDescriptorSet-descriptorType-00337` (upstream: `flow0_sampled` is a sampled
      alias of a storage-only image) appears 4 times instead of 2, once per descriptor set that binds it.
    - Checked on 2026-09-27: M0's EQUIV set; `mz_bench` dumps against the previous build with `--reset-every 10`
      (frames 1, 55 and 99 of a 99-frame run, and 1440p), `--passes 2` (the copy path), `--no-motion`, strength and
      model scale 0.75; `mz_phases` at odd sizes and with dynamic resolution; all byte-identical. Synchronization
      validation reports no hazard on the history images. Alone: `mz_timing` (5 interleaved pairs) -0.117 ms at
      1080p and -0.192 ms at 1440p, and -0.173 and -0.199 in a second session.
13. `windows/src/core/nr_runtime.cpp`, `nr_runtime.hpp` and `nrvk.hpp`, a new runtime pass
    `windows/shaders/passes/runtime_encode_in.comp`, and `windows/build/build_network.py`: on the linear path the
    core keeps the frame in the frame's own format, and the host fills it.
    - When: `linear_input`, model scale 1, `max_passes` 1, no control mask, and a format other than the 8-bit ones
      (`Transfer::Encoded`) that supports sampling. That covers RGBA16F, R11G11B10, R10G10B10A2 (forced linear) and
      RGBA32F frames at model scale 1. Every other configuration records as before.
    - `keep` is then made at the frame's extent in the frame's format: sampled, `TRANSFER_SRC` and `TRANSFER_DST`, no
      storage (11/11/10 need not be storage-capable). `nrvk::Context::image` takes upstream's `storage` parameter
      for it. `Runtime::frame_image()` returns the image, and `MochizukiNrRuntime.cpp` uploads the frame into it,
      pads it for dynamic resolution, hands it over as the frame and reads the answer back out of it.
    - `record_all` then blits nothing into `tex_in`. `runtime_encode_in.comp`, which is `runtime_encode.comp`
      reading `keep` with `texelFetch` and writing only the proxy, fills `tex_in`, and no RGBA32F copy of the frame
      is written. `runtime_transfer.comp` samples `keep` as before, and the write-back blits into it, since it is the
      frame. `runtime_encode_in.comp` repeats `runtime_encode.comp`'s arithmetic, so a change to one belongs in the
      other.
    - Upstream's analogue is `direct_in` (`4f62a8a`): the network's input in the frame's own format, filled by a
      plain copy, used only for frames that are not linear, with native compose.
    - Byte-identical because `texelFetch` of an FP16 or 11/11/10 texel returns the float the NEAREST 1:1 blit into
      RGBA32F wrote, RGBA32F is copied bit for bit, and 11/11/10's alpha reads 1.0 on both paths.
    - VRAM (this process's growth while the network was built): 1080p RGBA16F 698 -> 681 MB, 1440p 1132 -> 1098 and
      1100 MB, 720p 11/11/10 410 -> 398 MB.
    - Checked on 2026-09-27: the shader folder differs only by the new `runtime/runtime_encode_in.spv`; M0's EQUIV
      set; dumps against the previous build with dynamic resolution (`drs_mode` 1 and 2), 2 passes, model scale 0.5,
      odd sizes, RGBA32F with a varying alpha and R10G10B10A2 frames, all byte-identical. Alone: `mz_timing` (5
      interleaved pairs) -0.072 ms at 1080p and -0.186 ms at 1440p, and -0.042 / -0.184 and -0.098 / -0.165 in two
      later sessions.
14. `windows/src/core/nr_runtime.cpp`, `record_all`: the alpha pass runs only for the control mask, native compose
    or 8-bit frames.
    - The transfer pass already stores `vec4(rgb, keep.a)` into the answer, and the alpha pass stores that `rgb` back
      unchanged with its own source's alpha. The two sources hold the same alpha in every configuration:
      - scaled: both are `keep_full`;
      - SDR at model scale 1: both are `tex_in`, or `shown_keep` with several passes;
      - linear, one pass: `keep` is the frame (entry 13) or an RGBA32F copy that `runtime_encode.comp` writes, and
        the encode writes the same `src.a` into `tex_in`, the alpha pass's source;
      - linear, several passes: the encode writes `src.a` into `keep` and `tex_in`, and `shown_keep`, the alpha
        pass's source, is copied from `tex_in` after it.
    - 8-bit frames keep the pass for its k/255 rounding. The mask and native compose paths have no transfer pass.
    - The compute barrier between the two passes goes too: the barrier that moves `out` to the blit already orders
      the transfer's writes.
    - Upstream's analogue is v0.0.2's `post_alpha` (`4f62a8a`), which skips the pass when native compose's post
      block stored the alpha. No shader changes.
    - Checked on 2026-09-27: M0's EQUIV set; `mz_bench` dumps against the previous build with `--passes 2`,
      `--passes 3`, `--model-scale 0.5` and `--no-motion`; copies of `mz_bench` and `mz_phases` whose input alpha
      varies over the frame (0.2 to 0.94 and 0.01 to 0.99; one and two passes, model scale 0.5, odd sizes, dynamic
      resolution, SDR, 3840x2160); all byte-identical, with the output alpha varying (2332 distinct values in the
      `mz_bench` dumps). Synchronization validation reports the
      same messages as the previous build. Alone: `mz_timing` (5 interleaved pairs) -0.078 ms at 1080p and -0.229
      ms at 1440p, and -0.060 and -0.231 in a second session.

15. `windows/shaders/rdna4/include/image_input.glsl`: the fused pre block's colour input is rounded
   to nearest even (source rounding).
   - The block converts its colour sample to f16. Converted straight from `textureLod`, LLPC folds that into a
     16-bit texture return (`image_sample_lz ... d16`: 2 of 2 image ops in `g_fswinimagepreds32`, 2 of 24 in
     `temporal_pre_fp32`), and the texture unit truncates an f32 texel. The input texture is RGBA32F, so about half
     the colour samples came in one f16 ulp low. The original rounds to nearest even (`cvt.rn.f16.f32`).
   - The sample's bits are rounded to 10 mantissa bits with integer ops, and the f16 conversion that follows is
     exact. That is round to nearest even for every f32 from 2^-14 to 65504, both signs (all 503,300,098 checked on
     the CPU against numpy).
   - Upstream's form (4f62a8a, `NR_HALF_RTE`: the f16 RoundingModeRTE execution mode) was tried first and not kept.
     On LLPC both samples stay d16, and both pre kernels go 192 -> 196 VGPRs (8 -> 7 waves a SIMD) and gain
     633 / 616 instructions.
   - Driver ISA (26.8.1): no d16 image op left, both kernels at 192 VGPRs as before, 7435 -> 7454 and
     7977 -> 8011 instructions. Only `g_fswinimagepreds32.spv` and `temporal/temporal_pre_fp32.spv` change.
   - The output is not byte-identical to M0's goldens, by design: 0 non-finite values, mean |d| 1.1e-3 to 4.8e-3
     a dump (the most in the R11G11B10 phase), spread evenly over the frame, printed mean |out - in| within 1% of
     the goldens. The same integer path truncating instead (`rb&=~0x1FFFu`) is byte-identical to the goldens in the
     whole EQUIV set, so the difference is the rounding alone.
   - mz_pan over 200 frames, frame-to-frame |dr|: no motion 0.00545 -> 0.00547, correct vectors 0.00140 -> 0.00132,
     wrong vectors 0.00594 both, still image 0.00062 -> 0.00063.
   - mz_timing, median of 5 interleaved pairs: +0.001 ms at 1080p and at 1440p.

### Not taken from v0.0.2 and v0.0.2.1

Tried on 2026-09-27 (RX 9070 XT, driver 26.8.1) and left out:
- `NR_EDGE_BODIES` on the C=256 persistent pipelines, with the per-item body moved into `include/fswin_body.glsl`
  as upstream has it. Byte-identical, but an edge body takes `fswinpup256` above 192 VGPRs (+10% at 1080p and +27%
  at 1440p on its layer), and the one form with a runtime gain (`fswinpds256` alone, five bodies: `mz_timing`
  -0.075 / -0.016 ms) compiles in 12.2 s instead of 2.2 s, so a cold build takes 9.3 s longer with the prewarm
  manifest and 10.1 s longer without it. The move into `fswin_body.glsl` alone leaves every SPIR-V identical.
- `NR_UPSVIEW_FOLD` and `NR_REPACK_FOLD` (the upsample view written by its producer; the ViT repacks folded into
  `gemmnores` and a new `gemmprojt`). Byte-identical, but the dispatches they touch got only 1.8% faster at both
  sizes, and `mz_timing` moved -0.025 ms at 1080p and -0.006 ms at 1440p.
- `NR_HALF_RTE` as upstream writes it: on LLPC the d16 returns stay (entry 15 rounds in the source instead).
- C=64 folds and masks 7, `NR_EXPAND_GROUP=4` on the folded pipelines (entry 10); `NR_SWAP_AB` (entry 7);
  `NR_ATTN_BIAS_SEED`, `NR_ATTN_SATQ=2` and `NR_VLATE_SHUFFLE` (entry 9); one per item on the downsample folds
  (entry 11).
- Not tried: `DirectSrc` and the depth copy pass, `post_alpha`, the UNORM transfer change, `persist_strag`,
  `noise_field`, and upstream's 384-workgroup rule for the C=64 runs.

16. From upstream after v0.0.2.1 (51b65a6, 2026-10-01), three changes, ported by hand because the files had moved
   apart:
   - `windows/src/core/nr_runtime.cpp`: the post block's history weight is multiplied by the model's own
     `blend_scale`, 0.7397 (`kPostBlendScale`, from d1185d2); it was 1.0. Changes the output wherever history is used.
   - `transfer_mode`: E5B9G9R9, R16G16B16A16 and R8G8B8A8 SNORM and the 16-bit packed formats go through the blit
     (from 743326d). The host maps R9G9B9E5, B8G8R8X8 and the others to them and takes the blit-only ones where the
     GPU can blit them.
   - The preprocess (729a05d): `windows/shaders/passes/runtime_prep.comp` as upstream has it,
     `RuntimeConfig::preprocess`, `Controls::preprocess` and `Runtime::preprocess_meter`, and the forward, back and
     history-generation steps in `record_all`. The knee is undone on the linear path (`impl_->linear`), which is the
     only one this host takes a knee on. Off, nothing of it is recorded; a network is built able to run it only after
     a frame asked for it.
   - Not taken yet: the in-place motion sampling and the per-frame pre-block noise from d1185d2, and the Windows
     network rebuilt for the AMD compiler (228d3a6, b1419b0). See `handoff/mochizuki-upstream-2026-10-01.md`.

## Build

`tools\build-mochizuki-runtime.cmd [out]` from an MSVC developer prompt, with the Vulkan SDK 1.4.357 or newer
(`VULKAN_SDK`) and Python. It downloads glslang 16.5.0, the version the upstream SPIR-V is pinned to, into
`toolchain\` here, and writes `MochizukiNrRuntime.dll` and `dlssnr-amd\shaders\` to `out`
(default `exports\mochizuki-runtime`).

`build_network.py` unrolls every constant-trip loop of the `fswin_t.comp` pipelines, as upstream does
(`unroll_glsl.py` with no options). Lighter settings were tried on 2026-09-25 (RX 9070 XT, driver 26.8.1, the build
with patches 1-4) and not adopted. Every one is slower at run time. All but `--max-trip 16`, which saves only 0.1 s
of compilation, also change the output. Measured:
- cold network build: a fresh exe name, no `pipeline.cache`, pipelines created one at a time; median of 2;
- `nr_graph --per-layer` at 1080p, interleaved with the shipped build;
- output: `nr_graph --out-image` at 1080p against the shipped build, 3 runs each, then the runtime (M0's EQUIV set
  at 720p, 1080p and 1440p, or `mz_bench` against its golden for the builds `nr_graph` had already found different).

| `fswin_t` build | Cold network ready | `nr_graph` 1080p | Output | Scratch |
|---|---|---|---|---|
| full unroll (shipped) | 22.5 s | 8.94-9.03 ms | reference | 16 B in `fswinpds256` |
| `unroll_glsl.py --private-index` | 18.6 s | +3.31 ms | differs | 144-272 B in 5 pipelines |
| `unroll_glsl.py --max-trip 16` | 22.4 s | +0.11 ms | identical at 1080p and 3840x2160 | unchanged |
| `--private-index`, then `spirv-opt -O` | 18.2 s | +3.16 ms | differs | 144-272 B in 5 pipelines |
| `spirv-opt -O` | 24.6 s | +0.30 ms | differs after the first frame | 32 B in `fswinpds256` |

- `--private-index` unrolls only the loops whose variable indexes a local array. That cuts the fswin SPIR-V from
  19.5 to 8.3 MB, but:
  - LLPC then spills in `fswindsp32`, `fswinp128`, `fswinpup256` and `fswinfusedup32` too;
  - 12 of the 13 fswin kernels that run at 1080p get 9-83% slower.
- `--max-trip 16` changes only `fswinpup256` and `fswinfusedup256`, the two pipelines with longer loops:
  - the output is identical: `nr_graph --out-image` at 1080p, which dispatches `fswinpup256`, and at 3840x2160, the
    one size that dispatches `fswinfusedup256` (see entry 2), and M0's EQUIV set;
  - `fswinpup256` gets 32% slower at 1080p. At 3840x2160, which does not dispatch it, the frame is 0.04 ms slower.
- `spirv-opt -O` (SPIRV-Tools v2026.3, Vulkan SDK 1.4.357) keeps the network's output identical at 1080p, the one
  size checked (`nr_graph --out-image`, which dispatches 13 of the 23 network pipelines it changes), but:
  - every fswin pipeline takes LLPC longer to compile;
  - `fswinpup256` gets 34% slower and `fswinfusedup128` 45% slower;
  - the temporal variants built from it (`temporal_pre_fp32`, `temporal_post_fp32`) change the output of every
    frame after the first.

## Barriers and `NR_CHAIN` (measured, not changed)

The inter-dispatch barriers were priced again on 2026-09-25 (RX 9070 XT, driver 26.8.1, the build with patches 1-5),
and the ways to remove some of them listed below were measured. None is adopted, and no code changed. `NR_CHAIN`
stays what upstream ships it as: an opt-in diagnostic of `nr_graph` (SPVs built with `-DNR_CHAIN=1` plus
`NR_CHAIN_KERNELS`), which the runtime never turns on. Measured with `nr_graph` (`--warmup 60 --repeats 300`, 3 runs
interleaved with the shipped build) and the runtime's `mz_timing` (5 interleaved pairs):
- The bound. The shipped graph is 0.86 ms slower than the barrier-free graph without persistent runs
  (`NR_DIAG_BARRIERS=1 --no-persist --no-barrier`) at 1080p, 0.92 ms at 1440p and 1.87 ms at 3840x2160. The part
  `NR_CHAIN` or a fusion could reach is the boundaries after the bottleneck's small dispatches (`attn`, `ffwd3`,
  `vitattn` and the `gemm*` kernels; 106 of them, 98 at 3840x2160): dropping all of them (`--no-barrier-after`)
  gains at most 0.60-0.64 ms (two sessions), 0.56 and 0.38 ms.
- `NR_CHAIN` with upstream's kernel list, which includes `vitattn`: every `gemmvqkvnorm` -> `vitattn` wait runs out
  its poll budget, 8 timeouts a frame, and a frame takes 330-380 ms at 1080p and 1440p and 274 ms at 3840x2160. The
  waits return only because the budget is bounded. The argument in `nr_chain.glsl` that a waiting workgroup never
  holds a slot its producer needs does not hold on this driver.
- `NR_CHAIN` without `vitattn`: byte-identical, and slower in every form tried. As shipped it is +0.73-0.78 ms at
  1080p, +0.56 at 1440p and +0.43 at 3840x2160. A backoff LLVM cannot fold (LLPC turns the 64 LCG steps into one
  multiply-add) brings that to +0.21, +0.24 and +0.33 ms in `nr_graph`, and +0.21 / +0.17 ms in the runtime. Built
  with the coherent arena it is +2.5 to +3.5 ms. LLPC lowers every device-scope acquire to `global_inv
  scope:SCOPE_DEV` (RADV emits none; see `coherent_act.glsl`), so the non-coherent chain does get its invalidate:
  one in every poll and one after the wait. Polling with relaxed loads, which leaves only the one after the wait,
  did not change the time.
- The runtime would also need `nr_runtime.cpp` to skip the boundaries `g_chain_nobar` marks. And
  `g_chain_epoch_word` is process-global: after a resolution change the next session reads the first one's epoch
  word, and `mz_phases` hung at its third size.
- C=32 persistent runs (`--persist-levels 32,64,128,256`) need a `fswinp32` pipeline, which neither upstream's
  `pipelines.json` nor this one has. The one measured is `fswin32` as this tree ships it (`NR_FWAVES=2`,
  `NR_HWAVES=0`) plus `NR_PERSIST=1`, `NR_PERSIST_DF=1`, `NR_COHERENT_ACT=1` and `NR_WAVE_UNIFORM=1`. It is
  byte-identical at 1080p and 1440p, but +0.11 and +0.25 ms (+0.12 and +0.24 ms in the runtime). At 3840x2160 both
  C=32 runs report `persist error`, a frame takes 1.06 s, and the picture is wrong (max |d| 0.12). Upstream's
  `NR_FWAVES=1` does the same.
- The cause is the `NR_PERSIST_DF` ready queue in `fswin_t.comp`, which is upstream's code unchanged. A queue entry
  is `nr_tag | c`, a 16-bit epoch tag over a 16-bit item index. At 3840x2160 a C=32 run has 33,017 windows a layer
  and 98,433-98,673 items. An index above 65,535 spills into the tag, so its entry never matches (or names another
  item), and the workgroup that claims it polls until its budget runs out. With a 17-bit index (a scratch build
  only) the frame is byte-identical and 0.51 ms slower. Upstream's +0.067 ms at 4K (the `--persist-levels` note in
  `nr_graph.cpp`) counts 8,349 C=32 windows, which is this plan's count at 1080p.
- The shipped levels (64, 128, 256) stay under that limit: the largest run at 3840x2160 is C=64, with 24,857 items.
  That is exactly the C=32 grid at 1080p (8,349 windows a layer), so C=64 at 7680x4320 has the C=32 grid of
  3840x2160 and would pass 65,535 items (worked out from the window counts, not run). The runtime accepts inputs up
  to 16384x16384.
- Fusing bottleneck kernels. The table gives what the barriers between one pair of kernels are worth at 1080p when
  only those barriers are dropped (a scratch `nr_graph` with a per-pair list; a range covers two sessions):

| Pair | Boundaries | Barrier price |
|---|---|---|
| `gemmvqkvnorm` -> `vitattn` | 8 | 0.16-0.21 ms |
| `gemmproj` -> `gemmvqkvnorm` | 8 | 0.15 ms |
| `vitattn` -> `gemmproj` | 8 | 0.04-0.10 ms |
| `attn` -> `gemmproj` | 15 | 0.06-0.08 ms |
| `ffwd3` -> `gemmproj` | 16 | 0.04-0.07 ms |
| `gemmproj` -> `attn` | 16 | 0.05 ms |
| `gemmvact` -> `gemmproj` | 8 | 0.03-0.05 ms |
| `gemmproj` -> `ffwd3` | 14 | 0.03 ms |
| `gemmproj` -> `gemmvact` | 7 | 0.01 ms |

These are only the barrier's part. What a fusion saves in launches and in the activation's round trip through
memory, and what it loses in parallelism, was not measured. The three pairs that reach 0.1 ms follow each other in
every ViT block:
- `gemmvqkvnorm` -> `vitattn` cannot be fused: every `vitattn` query reads the K and V of every token. `vitattn`
  with 64 or 128 queries a workgroup (fewer workgroups behind that boundary) is identical and no faster.
- `gemmproj` -> `gemmvqkvnorm` and `vitattn` -> `gemmproj` are open candidates, not built. The consumer is a
  `gemm1x1` GEMM, which sums all 1024 channels of a token in FP32 and rounds only after the whole sum. So a
  byte-identical fusion needs the producer's whole row in one workgroup, one workgroup per 32-token tile: 20 at
  1080p, where `gemmproj` runs 160 and `vitattn` 640 (a tile's 32 heads are 32 workgroups). Adding partial sums
  across workgroups with atomics would not keep the order.

## The model

Never shipped with the code. `dlssnr-amd\dlssnr.bin` beside the DLL is made from `nvngx_dlssnr.dll` 310.8.0
(SHA-256 `e16bcf15…6e1fc8e`) by the scripts in `linux/package/model-tools/`: `inspect_nr.py --extract`, the
five `unpack_*.py`, then `pack_model.py --verify model-files.sha256`, as `extract_model.sh` runs them. The result
is 599 entries, 147,756,560 bytes.

## Updating

Copy the paths above from a newer upstream commit, reapply the patches, rebuild, and run the runtime against a
D3D12 frame before shipping it.
