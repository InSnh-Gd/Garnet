# Garnet changes and snapshot provenance

Current standalone Garnet architecture: `Garnet` `0.1.1` (`src/garnet`, `org.garnet.driver`), `GarnetAccel` `0.1.1` (`src/garnet-accel`, `org.garnet.accel`), `GarnetMetal` `0.1.1` bundle `44` (`src/garnet-metal/Info.plist`, `org.garnet.metal`), `Garnet.app` `0.1.1` build `2`, 32 Mesa Darwin patches (`mesa-patches/`, including `VK_EXT_metal_surface`, async compute, GFX12 DCC, and `aco::spill` `IDSet` memory reduction), and 87 `metal2vulkan` patches with `garnetxlate` (`third-party/metal2vulkan-rdna4/`), with `Lilu.kext` and `AMDRadeonRDNA4.kext` dependencies removed and selected non-Lilu display/cursor/WSI/VM/sleep/GL-drawable advancements absorbed from `RX4Darwin/AMDRadeonRDNA4.kext`.
Host suites run without a GPU; physical validation requires running on target hardware.

The baseline entries below preserve upstream Cobalt64's 5 October 2026 release notes as historical pre-Garnet context (when components used `tools/pc/`, `src/rdna4-bringup/`, `tools/native/rdna4metal/`, and `librdna4xlate`). Where upstream validated a stage on its Navi 48 bench, that belongs to upstream; local validation evidence lives under `logs/`.

## Release v0.1.1

- Release alignment, logging, and sandbox preflight: aligned all four layers to `0.1.1` (`Garnet.kext` `0.1.1`, `GarnetAccel.kext` `0.1.1`, `GarnetMetal.bundle` `0.1.1` build `44`, `Garnet.app` `0.1.1` build `2`), added `RDNA4_RELEASE` `os_log` severity gating (`RDNA4LOG_WARN`/`ERR`/`KEY`), added `rdna4_sb_*` sandbox preflight with unsandboxed and parent-directory metadata handling so absent kill/flag paths report `ENOENT` instead of synthetic `EPERM`, enforced `libgarnetxlate.dylib.patches.sha256` freshness checks, expanded `Garnet.app` diagnostics export, and retired legacy `0.1.0` `AMDRadeonRDNA4MTLDriver.bundle` artifacts on upgrade.
- 8 GB Navi 44 and Full-BAR fallback: added 8 GB Navi 44 VRAM/pool cap reporting and automatic Small-BAR fallback when Full-BAR is requested without a full ReBAR aperture.
- Modular refactor and clean-room translation: split `Garnet.kext` ring submit, session VM, VRAM, and scanout units and centralized MES v12.1 wire layouts; split `GarnetMetal` into 11 modular `.m` compilation units; migrated runtime paths and environment variables to `Garnet`/`GARNET_*`; clean-room replaced sampler-array and cube-storage `metal2vulkan` patches.
- Interrupts, sleep/wake, and 4K@160Hz desktop: enabled MSI EOP IRQ fence wakeup (`garnet-interrupts=1`), DCN 4.01 OTG `VSTARTUP` primary VBLANK and `VUPDATE_NO_LOCK` self-healing, hardware DPMS sleep/wake with forced DP/DSC retrain, dynamic AGDC resolution across macOS 26.x, `Doze` PM, effective `GFXCLK` telemetry, 4K@160Hz WindowServer commit pipelining, and Graphics Pipeline Libraries (GPL) fast-link with cold pre-warm.
- Kernel memory and MEC async compute: exposed a dedicated MEC async compute ring (`ctx.kcq` / `qfi=1`) with dual-ring sync and pure-compute routing, added 2 MiB `PDB0` huge pages with transactional split rollback, and enabled GFX12 `PTE.DCC` by default (`garnet-dcc=0` kill-switch) backed by GPU `CP_DMA` BO clear and ReBAR GL2 invalidation. Physically verified on an RX 9060 XT live `3840x2160@160Hz` Metal desktop (`VRAM_DCC_STRESS_PASS` 8x256 MiB VRAM + 4x4K DCC render targets, 4-way concurrent RT 400f + Wave4 x5 + Metal x10 + Sascha Vulkan RT stress, `145,148/145,148` retired seqs, `0` GCVM faults, `0` hangs, `29,606/29,606` latched scanout presents with `0` GPU failures).
- Hardware ray tracing (`RX 9060 XT`, Vulkan RT and Metal 3 RT):
  - Verified native Vulkan RT on GFX1200 (`4,788.0 MRays/s` at 100k tris, `1,710.6 MRays/s` at 1M tris; Sascha `raytracingbasic` `398.881 FPS` and `rayquery` `374.329 FPS` on live 4K@160Hz DCC-enabled desktop; `VK_KHR_ray_tracing_position_fetch`).
  - Implemented Metal 3 RT and bindless end-to-end: `intersection_query` and `@air.intersect`, IFT ABI v2 (`attached_bda[31]`), stateless nested VFT, encoder-less 3-bridge Metal 3 bindless, `primitiveDataBuffer`, `MTLHeap` Automatic and Placement AS, async `computeQ` GPU-driven AS build/refit/compact, `reloop_nest` dominance repair (`0086`), `64 MiB` SPIR-V / `512 MiB` cache / `80 MiB` archive caps, and native int64.
  - Verified live on RX 9060 XT: interactive `1280x720` Metal 3 path tracer (`103.93 FPS` avg, `136.8 FPS` peak, `~630.4 MRays/s` under default-on DCC; 400-frame concurrent run `103.35 FPS` avg), `wave4_primdata_heap_test` O0/O2 (`71/71` pass, 5x concurrent), and unmodified Apple Official Metal 3 RT Samples (`AcceleratingRayTracingUsingMetal` 9/9 spheres and `RenderingReflectionsInRealTimeUsingRayTracing` `cb_gpu avg = 1.039 to 1.363 ms`).
  - Verified 63/63 Blender 4.2.23 Cycles (`CYCLES_METALRT=1`) kernels and 14/14 IFT callbacks through `garnetxlate` and `spirv-val`, plus `mesa-patches/0032` cutting ACO spill memory by ~7x to 14x (`shade_shadow` `29.9 GiB` OOM to `2,156 MB`; `shade_dedicated_light` `>12 GiB` OOM to `849 MB`). Live Blender startup built 9 BLAS/TLAS objects and compiled/cached 9/66 kernels before testing was stopped at user request prior to scene render.

## Planned for v0.2.0

- RDNA 4 VCN 5.0.0 hardware video (`enable-rdna4-vcn5-video`): bring up kernel VCN 5.0.0 (`vcn_5_0_0.bin`, Day-1 Dynamic Power Gating with 1000 ms idle gate, NBIF v6.3.1 doorbell, IH v7.0 fence IRQ, MMHUB v4.1.0 per-session VMIDs, per-queue VCN ring reset, and `'GRNT'` ABI `1.15`), enable Mesa RADV Vulkan Video (`-Dvideo-codecs=all`, `mesa-patches/0034+`, 3-ring `GFX`/`COMPUTE`/`VCN` sync) for `H.264`/`HEVC`/`AV1`/`VP9` decode and `H.264`/`HEVC` (I+P)/`AV1` encode with VCN 5.0 EFC RGB conversion, and add a clean-room MIT VideoToolbox bridge (`libgarnetvt.dylib`, shared `VkDevice` VCN queue, zero-CPU-copy `IOSurface` import, `garnetctl vt-run`, and probe-gated `MediaExtension` decode without Seatbelt `.sb` or `VCPHEVC`/`AppleGVA` edits).
- Browser and Electron sandbox compatibility (`fix-desktop-browser-sandbox-and-recovery`): advance `'GRNT'` to ABI `1.14` with WebKit-silent APP selector aliases (`30`/`31`/`39`, `mesa-patches/0033`) and `rdna4n_info.flags` readiness/headless bits, handle late Seatbelt entry in `rdna4_sb_check`, add two-tier `inproc` shader cache fallback (`TemporaryItems` then 64 MiB in-memory cache), and support atomic APFS root-snapshot deployment of `GarnetMetal.bundle` to `/System/Library/Extensions/` with `/Library/GPUBundles/` symlink fallback and `--last-sealed-snapshot` uninstall (zero `.sb`/ESP edits) so Safari (`com.apple.WebKit.GPU`), Chrome, and Discord render without blank windows.
- Desktop stability, app identity, and zero-fault MES init (`fix-desktop-browser-sandbox-and-recovery`): clear `mv_`/`mf_` when entering `FALLBACK` in `garnet_pipelines.m` (preventing cold-cache WindowServer `Code=49` crash loops), publish IORegistry `"model"` as NUL-terminated `OSData` (fixing Geekbench 7 window launch), apply `VK_SHARING_MODE_CONCURRENT` across the 6 non-heap IOSurface/`vram_surf` paths, and fix the remaining 2 boot MES `SET_HW_RESOURCES_1` faults (`Garnet,IRQFaults == 0`).
- Argument-buffer encoding, cold shader latency, and `MTLHeap` sizing (`fix-desktop-browser-sandbox-and-recovery`): replace full-buffer CPU `argscan` walks on large declared-transport buffers (`cb_encode avg = 845.29 ms` on Apple Sample 2 despite `1.039 to 1.363 ms` GPU execution) with incremental/dirty-range tracking or BDA indirection; bundle remaining macOS 26 QuartzCore shaders and remove artificial sleeps in `native-translate.m`, `garnet_hotswap.h`, and `garnet_dispflip.h` behind the ~1.15 s Control Center cold-click delay; fix tiled `MTLHeap` `VkImage` size queries (`pf 115`, `mediaanalysisd` fallbacks); and persist compiled ACO machine pipelines on disk.
- DCN 4.01 DP hotplug (HPD) recovery and idle power (`fix-desktop-browser-sandbox-and-recovery`): implement DisplayPort HPD link-loss retraining, `DIG_FIFO_CTRL0` error clearing, and `dcn-dp` AUX restoration after monitor input switches or cable replug without rebooting; fix the `~51 to 55 W` (`busy gfx 100%`) baseline idle regression back to the Stage 3 envelope (`~16.6 W` at 4K@60Hz / `~27.6 W` at 4K@160Hz) and close SMU DPM / `GFXOFF` governance jointly with VCN 5.0 DPG.
- Blender Cycles MetalRT and additional GPU verification (`complete-rdna4-raytracing` Task 5.4): complete full Blender 4.2 LTS Cycles `CYCLES_METALRT=1` scene rendering (`Cornell`, `BMW`, `Classroom`) and complete physical bring-up and release verification for RX 9060 XT 8 GB and Navi 48 (`RX 9070` / `RX 9070 XT`, GFX1201) alongside automated `.dmg` validation.

## Multi-display (milestone "M6")

- **Stage 1a** (kext 0.0.659, boot-arg `rdna4-m6=1`)
  The second display's pipe is adopted on Apple's presentation path beside the first:
  every pipe is adopted, and the surface-ID table is learned in the submit hook.
- **Stage 1b** (0.0.661, `rdna4-m6=1` and `rdna4-m6flip=1`)
  The second display gets its own scanout (instance 2): user-client ABI 1.11, selectors
  22..26 (acquire / register / present / status / release), poll-only. The matching Mesa
  entry points are in `mesa-patches/`.
- **Stage 2** (0.0.662, additionally `rdna4-m6flip1=1`)
  A third display (instance 1): per-instance descriptors (HUBP1/OTG1), scanout state,
  locks, pins and watchdog, a teardown-all path, and clash checks against the other
  instances. ABI 1.12 (selectors 22..26 also accept instance 1; an extra output word).
- **Review fixes:** a self-healing surface table (staleness, re-learn, eviction), a serialised
  table publish, the blob sizing hot fix in 0.0.661, and DP-plane geometry checks in the
  Metal bundle. Run kits: `tools/pc/m6-stage1a-*`, `m6-stage1b-*`, `m6-stage2-run.sh` and a
  simulation harness (`tools/pc/m6-stage2-sim`) that checks the scripts' parsers and
  scenarios without hardware.

## Resizable BAR (0.0.663, 0.0.664)

The kext can map a BAR0 larger than 256 MiB (up to 1 GiB). The decision is a pure,
unit-tested function (`src/rdna4-bringup/src/amd/rebar_pure.h`, `plan_bar0`) that refuses a
zero or non-power-of-two length, a misaligned base, a disagreement between IOPCIFamily and
config space, a ReBAR size mismatch and over-large maps. 0.0.664 adds review fixes: the
doorbell self-ring is not enabled after a BASE_LOW/HIGH read-back mismatch, and BAR2 is
refused when IOPCIFamily and config space disagree.

## In-process shader translation (Metal bundle 14)

Processes other than WindowServer (sandboxed applications) can translate Metal AIR to
SPIR-V inside the process: Apple's own compiler library prints the AIR as LLVM text, and a
small C-ABI library (librdna4xlate) translates it, replacing the helper daemon. The library
is the LGPL-3.0 rdna4xlate crate, a wrapper around metal2vulkan; it is NOT part of the MIT
code and lives, with our changes to metal2vulkan, in `third-party/metal2vulkan-rdna4/`
(see its NOTICE). `tools/native/rdna4metal/rdna4_xlate.h` is the loader.

## Application fixes (Metal bundles 15..20)

- WindowServer import-memory exhaustion: a live-import ledger, a refused import never
  returns nil, an import cache on by default.
- Occlusion queries (setVisibilityResultMode) over a per-command-buffer Vulkan query pool;
  resolve before the command buffer completion handler.
- RG16Uint pixel format mapped to VK_FORMAT_R16G16_UINT (menus whose shadow pass needs it).
- Visible-function linking for translated shaders (ABI 2 of librdna4xlate).
- HDMI scanout "rubber band" fix: routed non-tentative writes are scanout writes while the
  routing latch is on; per-slot supersede marks replace an 8-entry surface table.
- GPU-application allow-list (kext: RDNA4AppKey.h, appallow), default-deny.

## Auto-arm (`tools/pc/autoarm`)

A LaunchDaemon (once per boot) plus a fail-safe script that brings up the multi-display
GPU desktop unattended at the login window. The first failing step stops it and leaves the
CPU-rendered desktop; it stops if a user logs in; WindowServer is restarted exactly once.
The expected display CRC is measured on the boot itself (or set with `AA_CRC_RG2` /
`AA_CRC_B2`) rather than compiled in. Kill switch: a file named `autoarm-off`. `install.sh` and
`uninstall.sh` are provided; `run-tests.sh` runs the script against fake tools on a host Mac.

## Other

- Pure-function headers with host-run test suites and planted-break scripts (`src/garnet/tests/native_*_plant.sh`).
- Standalone Garnet backend and SDK: active builds use standalone `Garnet.kext` + `GarnetAccel.kext` + `GarnetMetal.bundle` with `MacKernelSDK` (no `Lilu.kext` or `AMDRadeonRDNA4.kext`); AMD firmware blobs come from `linux-firmware` (`tools/fetch-firmware.sh`) so generated firmware C files stay out of `src/`.
- Excluded from version control: AMD firmware binaries, captures and fixtures derived from Apple binaries, generated `.spv` binaries (`src/garnet-metal/spvcache/` tracks `.meta.json` sidecars; `.spv` payloads are generated at build/translation time), build outputs, and run logs.
