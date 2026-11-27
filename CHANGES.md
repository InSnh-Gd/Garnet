# Garnet changelog and history

Standalone Garnet stack on `main` (building on `v0.1.1`): `Garnet.kext` (`src/garnet`, `org.garnet.driver`, `'GRNT'` ABI `1.14`), `GarnetAccel.kext` (`src/garnet-accel`, `org.garnet.accel`), `GarnetRecover.efi` (`src/garnet-recover-efi`), `GarnetMetal.bundle` (15 `.m` compilation units in `src/garnet-metal`, `org.garnet.metal`), `Garnet.app` / `garnetctl` (`src/garnet-app`), 33 Mesa Darwin patches (`mesa-patches/`), and 90 `metal2vulkan` patches with `garnetxlate` (`third-party/metal2vulkan-rdna4/`). No `Lilu.kext` or external display plugin is required.

## Unreleased (`main`, toward v0.2.0)

- **Sandboxed browsers and desktop Metal fixes**: `'GRNT'` ABI `1.14` WebKit-safe selector aliases (`30`/`31`/`39`, `mesa-patches/0033`), one-way sandbox latching, two-tier `TemporaryItems` + `64 MiB` in-memory shader cache fallback, MSAA `raster_sample_count` translation (`bits 23..16`), cold-cache `WindowServer` fallback fixes, IORegistry `"model"` reporting (unblocking Geekbench 7), `MTLHeap` tiled alignment, concurrent non-heap `IOSurface` sharing, and incremental dirty-range argument-buffer scanning.
- **DisplayPort hotplug, power, and MES boot**: DCN 4.01 HPD link-loss recovery (`regHPD_INTERRUPT_DEST` plus 1 Hz watchdog), SMU DPM / `GFXOFF` idle power policy, and MES boot initialization fixes.
- **OpenCore Picker recovery and `AMFIAnchor` signing**: standalone x86_64 UEFI `GarnetRecover.efi` (`[1]` Safe Boot Once, `[2]` Safe Boot & Uninstall via `com.garnet.recovery`, zero ESP writes), `restore-oc` baseline rollback, optional `GARNET_SIGN_CA_DIR` CA signing + codeless Schema-1 `GarnetAnchorInjector.kext`, and `Garnet.app` / `garnetctl` integration.
- **Metal 3.1 RT (opt-in) and incremental pipeline cache**: application `garnet-pipecache` raised to `256 MiB` (`WindowServer` capped at `32 MiB`) with debounced atomic saves after slow compiles, `garnet_mesh_rt.m` split into 5 compilation units, opt-in curve geometry (`GARNET_RT_CURVES=1`, `0088`), primitive/instance motion blur (`GARNET_RT_MOTION=1`, `0089`), and `0090` curve/motion/host optimizations (verified on RX 9060 XT: linear curves `4.92x` faster at `1,014.8 MRays/s`, cubic `1.43x`, single-slot IFT updates `2.11x`, `0` GCVM faults or hangs; `supportsPrimitiveMotionBlur` stays `NO` by default).
- **Test layout and build variables**: `GarnetMetal` and RADV host tests moved to `src/garnet-metal/tests/` and `mesa-patches/tests/`, micro C suites consolidated and thin shell wrappers replaced with `Makefile` targets, retired autotranslate/plant scripts removed, and build variables cut over to `GARNET_KC_WORK` and `GARNET_LAYOUT_OUT`.

## Release v0.1.1

- **Release alignment, logging, and sandbox preflight**: aligned all four layers to `0.1.1`, added release `os_log` severity gating (`RDNA4LOG_WARN`/`ERR`/`KEY`), fixed `rdna4_sb_*` sandbox preflight so absent kill/flag files report `ENOENT` rather than synthetic `EPERM`, enforced `libgarnetxlate.dylib.patches.sha256` freshness checks, expanded `Garnet.app` diagnostics export, and added automatic cleanup of legacy `0.1.0` `AMDRadeonRDNA4MTLDriver.bundle` directories on upgrade.
- **8 GB Navi 44 and Full-BAR fallback**: added 8 GB Navi 44 VRAM/pool cap reporting and automatic Small-BAR fallback when Full-BAR is requested without a full ReBAR aperture.
- **Modular refactor and clean-room translation**: split `Garnet.kext` ring submit, session VM, VRAM, and scanout units; centralized MES v12.1 wire layouts; split `GarnetMetal` into 11 `.m` compilation units; migrated runtime paths and environment variables to `Garnet`/`GARNET_*`; clean-room replaced sampler-array and cube-storage `metal2vulkan` patches.
- **Interrupts, sleep/wake, and 4K@160Hz desktop**: enabled MSI EOP IRQ fence wakeup (`garnet-interrupts=1`), DCN 4.01 OTG `VSTARTUP` primary VBLANK and `VUPDATE_NO_LOCK` self-healing, hardware DPMS sleep/wake with forced DP/DSC retrain, dynamic AGDC resolution across macOS 26.x, `Doze` PM, effective `GFXCLK` telemetry, 4K@160Hz WindowServer commit pipelining, and Graphics Pipeline Libraries (GPL) fast-link with cold pre-warm.
- **Kernel memory and MEC async compute**: exposed a dedicated MEC async compute ring (`ctx.kcq` / `qfi=1`) with dual-ring sync and pure-compute routing, added 2 MiB `PDB0` huge pages with transactional split rollback, and enabled GFX12 `PTE.DCC` by default (`garnet-dcc=0` kill-switch) backed by GPU `CP_DMA` buffer clears and ReBAR GL2 invalidation. Physically verified on RX 9060 XT at `3840x2160@160Hz` (`VRAM_DCC_STRESS_PASS` 8x256 MiB VRAM + 4x4K DCC render targets, 4-way concurrent RT + Metal + Vulkan stress, `145,148/145,148` retired sequences, `0` GCVM faults, `0` hangs, `29,606/29,606` scanout presents).
- **Hardware ray tracing (RX 9060 XT, Vulkan RT and Metal 3 RT)**:
  - Native Vulkan RT on GFX1200: `4,788.0 MRays/s` at 100k triangles, `1,710.6 MRays/s` at 1M triangles; Sascha `raytracingbasic` at `398.881 FPS` and `rayquery` at `374.329 FPS` on live 4K@160Hz DCC-enabled desktop; `VK_KHR_ray_tracing_position_fetch`.
  - Metal 3 RT and bindless: `intersection_query` and `@air.intersect`, IFT ABI v2 (`attached_bda[31]`), stateless nested VFT, encoder-less Metal 3 bindless, `primitiveDataBuffer`, `MTLHeap` Automatic and Placement AS, async `computeQ` GPU-driven AS build/refit/compact, `reloop_nest` dominance repair (`0086`), `64 MiB` SPIR-V / `512 MiB` cache / `80 MiB` archive caps, and native int64.
  - Verified live on RX 9060 XT: interactive `1280x720` Metal 3 path tracer (`103.93 FPS` average, `136.8 FPS` peak, ~`630.4 MRays/s` under DCC; 400-frame concurrent run `103.35 FPS` average), O0/O2 primitive-data heap tests (`71/71` pass, 5x concurrent), and unmodified Apple Metal 3 RT Samples 1 and 2 (`cb_gpu` avg `1.039` to `1.363 ms`).
  - Offline Blender 4.2.23 Cycles (`CYCLES_METALRT=1`) validation: `63/63` kernels and `14/14` IFT callbacks pass `garnetxlate` and `spirv-val`, and `mesa-patches/0032` reduces ACO spill memory by ~7x to 14x (`shade_shadow` `29.9 GiB` OOM down to `2,156 MB`; `shade_dedicated_light` `>12 GiB` OOM down to `849 MB`).

For remaining `v0.2.0` work, see [README.md](README.md#planned-for-v020).

---

## Historical pre-Garnet baseline (Cobalt64, October 2026)

The entries below preserve upstream Cobalt64's pre-Garnet milestones for provenance, from when components lived under `tools/pc/`, `src/rdna4-bringup/`, and `tools/native/rdna4metal/` and were validated on upstream's Navi 48 bench.

## Multi-display ("M6")

- **Stage 1a** (`0.0.659`, boot-arg `rdna4-m6=1`): adopted the second display pipe on Apple's presentation path and learned the surface-ID table in the submit hook.
- **Stage 1b** (`0.0.661`, `rdna4-m6=1`, `rdna4-m6flip=1`): gave the second display its own scanout instance (`instance 2`), ABI 1.11 selectors `22..26` (acquire / register / present / status / release).
- **Stage 2** (`0.0.662`, `rdna4-m6flip1=1`): added third display scanout (`instance 1`, HUBP1/OTG1), per-instance locks/watchdogs, teardown-all, and ABI 1.12 geometry output.
- **Follow-up fixes**: self-healing surface table (staleness, re-learn, eviction), serialized table publish, blob sizing fix, and DP-plane geometry checks in the Metal bundle.

### Resizable BAR (`0.0.663`, `0.0.664`)

- Added unit-tested BAR0 sizing (`plan_bar0` in `rebar_pure.h`) capable of mapping >256 MiB BAR0 apertures while refusing misaligned bases, non-power-of-two lengths, and IOPCIFamily/config-space mismatches, plus doorbell self-ring read-back checks and BAR2 validation.

### In-process shader translation (Metal bundle 14)

- Enabled sandboxed processes outside `WindowServer` to translate Metal AIR to SPIR-V in-process via Apple's compiler library printing LLVM IR text into the LGPL-3.0 `metal2vulkan` C-ABI wrapper (`third-party/metal2vulkan-rdna4/`).

### Application fixes (Metal bundles 15..20)

- WindowServer import-memory exhaustion: live-import ledger, never returning `nil` on refused import, import cache enabled by default.
- Occlusion queries (`setVisibilityResultMode`) via per-command-buffer Vulkan query pools resolved before command-buffer completion handlers.
- `RG16Uint` mapped to `VK_FORMAT_R16G16_UINT` for menu shadow passes.
- Visible-function linking for translated shaders (ABI 2).
- HDMI scanout routing fix with per-slot supersede marks.
- Default-deny GPU application policy (`appallow`, later superseded by Garnet's `app-denylist.txt` model).

### Packaging and source-tree policy

- Pure-function C/C++ headers paired with host-run unit tests and planted-break suites (`src/garnet/tests/native_*_plant.sh`).
- Standalone Garnet builds use `MacKernelSDK` and fetch AMD firmware blobs from `linux-firmware` (`tools/fetch-firmware.sh`) so generated firmware files stay out of `src/`.
- Excluded from version control: AMD firmware binaries, Apple-derived fixtures, generated `.spv` payloads (`src/garnet-metal/spvcache/` tracks `.meta.json` sidecars only), build outputs, and run logs.
