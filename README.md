# Garnet

English | [简体中文](README.zh-CN.md)

Garnet is an open-source macOS GPU and display driver stack for AMD RDNA 4 graphics cards on x86_64 OpenCore systems. It builds on [Cobalt64](https://github.com/Almosst-DEV/Cobalt64) as a standalone four-layer stack without `Lilu.kext` or external display plugins, and adapts selected display, cursor, WSI, VM, sleep, and OpenGL surface work from [RX4Darwin/AMDRadeonRDNA4.kext](https://github.com/RX4Darwin/AMDRadeonRDNA4.kext).

> [!WARNING]
> `Garnet.kext` writes GPU registers directly across PCIe BARs, PSP, SMU, RLC, SDMA, CP, MES, and the DCN display engine. An incompatible build, GPU, or macOS version can freeze, reboot, or corrupt display output. Run it only on a dedicated, recoverable test machine at your own risk; see [LICENSE](LICENSE).

## Supported hardware

| Silicon | Models | PCI device IDs | LLVM / IP targets | Status |
| --- | --- | --- | --- | --- |
| Navi 44 | Radeon RX 9060 XT **16GB** | `1002:7590` | `gfx1200`, `GC 12.0.0`, `MP0 14.0.2`, `SDMA 7.0.0` | Primary physically verified target |
| Navi 44 | Radeon RX 9060 XT **8GB** | `1002:7590` | `gfx1200`, `GC 12.0.0`, `MP0 14.0.2`, `SDMA 7.0.0` | Code-ready, pending physical hardware verification |
| Navi 48 | Radeon RX 9070 / RX 9070 XT / AI PRO R9700 | `1002:7550`, `1002:7551` | `gfx1201`, `GC 12.0.1`, `MP0 14.0.3`, `SDMA 7.0.1` | Chip table, firmware path, and Mesa entry retained from Cobalt64; unverified on physical hardware |

Driver bundles, boot arguments, and CLI tools use `Garnet` (`garnet-*`) names, while shared GPU-family internals use `RDNA4`. `Navi 44` and `Navi 48` appear only where chip hardware, firmware blobs, or PCI IDs differ. `GarnetAccel.kext` matches x86_64 macOS `26.x` kernel collections by layout fingerprint (`25G332` verified baseline), and v0.1.1 release binaries target bare-metal Navi 44 (`1002:7590`) only.

## Release v0.1.1 and v0.2.0 roadmap

### Highlights in v0.1.1

- **Display, sleep, and 4K@160Hz desktop**: MSI EOP interrupts (`garnet-interrupts=1`), DCN 4.01 `VSTARTUP` VBLANK and `VUPDATE_NO_LOCK` self-heal, hardware DPMS sleep/wake with DP/DSC link retrain, dynamic AGDC resolution across macOS 26.x, WindowServer 4K@160Hz commit pipelining, and Graphics Pipeline Libraries (GPL) fast-link with cold pre-warm.
- **8 GB Navi 44, async compute, and memory**: RX 9060 XT 8 GB VRAM geometry with automatic Small-BAR fallback, a dedicated MEC async compute ring (`ctx.kcq`), 2 MiB `PDB0` huge pages, and GFX12 `PTE.DCC` enabled by default (`garnet-dcc=0` kill-switch) with GPU `CP_DMA` buffer clears.
- **Vulkan ray tracing** (verified on RX 9060 XT): `4,788.0 MRays/s` at 100k triangles and `1,710.6 MRays/s` at 1M triangles; Sascha `raytracingbasic` up to `398.881 FPS` and `rayquery` at `374.329 FPS` on a live 4K@160Hz DCC-enabled desktop; `VK_KHR_ray_tracing_position_fetch`.
- **Metal 3 ray tracing and bindless**: `intersection_query`, `@air.intersect`, intersection function tables (IFT ABI v2), nested visible function tables (VFT), encoder-less Metal 3 bindless, `primitiveDataBuffer`, `MTLHeap` acceleration structures, and async compute AS builds. Verified live on RX 9060 XT with an interactive `1280x720` path tracer (`103.93 FPS` average, `136.8 FPS` peak, ~`630.4 MRays/s` with DCC on), unmodified Apple Metal 3 RT Samples 1 and 2, and 4-way concurrent RT/Metal/Vulkan plus 2 GiB VRAM and 4x4K DCC stress (`145,148` retired sequences, `0` faults or hangs).
- **Offline Blender Cycles compilation**: `63/63` Blender 4.2.23 Cycles (`CYCLES_METALRT=1`) kernels pass Metal AIR to SPIR-V validation, and `mesa-patches/0032` cuts ACO spill memory by ~7x to 14x to avoid OOM. Live Blender built BLAS/TLAS and cached initial kernels; full scene rendering is scheduled for v0.2.0.
- **Logging, packaging, and upgrade cleanup**: release `os_log` severity gating, `rdna4_sb_*` sandbox preflight fix, expanded `Garnet.app` diagnostic export, and automatic removal of legacy `0.1.0` bundles. See [CHANGES.md](CHANGES.md) for the full changelog.

### Planned for v0.2.0

- **VCN 5.0.0 hardware video decode and encode**: kernel VCN 5.0 bring-up with dynamic power gating and per-session MMHUB VMIDs, native Mesa RADV Vulkan Video (`H.264`, `HEVC`, `AV1`, `VP9` decode; `H.264`, `HEVC`, `AV1` encode plus EFC RGB conversion), and a clean-room MIT VideoToolbox bridge (`libgarnetvt.dylib` with zero-copy `IOSurface` import and probe-gated `MediaExtension` decode, without Seatbelt `.sb` edits).
- **Sandboxed browser and desktop app compatibility**: unblock Safari (`com.apple.WebKit.GPU`), Chrome, and Electron rendering via WebKit-safe user-client selector aliases, late-sandbox handling, two-tier in-process shader cache fallback, and optional APFS root-snapshot bundle deployment. Fix cold-cache WindowServer fallback crashes, IORegistry `"model"` reporting (unblocking Geekbench 7), concurrent queue sharing on IOSurface paths, and the remaining boot MES initialization faults.
- **MetalFX upscaling and frame interpolation**: implement `MTLFXSpatialScaler` and `MTLFXTemporalScaler` on top of AMD FidelityFX FSR 3.1 compute passes (with native fallback spatial scaling) so apps and games targeting MetalFX run on RDNA 4.
- **Advanced Metal 3 ray tracing and Blender Cycles**: add curve primitives, motion blur, multi-level instancing, and indirect acceleration-structure builds, and complete end-to-end Blender 4.2 LTS Cycles (`CYCLES_METALRT=1`) scene rendering.
- **Metal 4 core API foundation**: bring up core Metal 4 entry points (`MTL4CommandQueue`, `MTL4CommandBuffer`, `MTL4CommandAllocator`, unified argument tables, and compiler/archive plumbing) over the existing RADV backend.
- **Lower CPU overhead, DisplayPort hotplug, power tuning, and hardware coverage**: replace per-frame full-buffer CPU argument-buffer scans with incremental tracking or BDA indirection, add a persistent on-disk ACO pipeline cache and reduce cold UI shader stalls, recover DisplayPort hotplug (HPD) link loss without rebooting, finish SMU DPM / `GFXOFF` idle power management alongside VCN power gating, and complete physical testing on RX 9060 XT 8 GB and Navi 48 (`RX 9070` / `RX 9070 XT`).

## Architecture

```text
  Metal / OpenGL / Vulkan apps
        │                │
        ▼                ▼
  GarnetMetal.bundle   libvulkan_radeon.dylib (Mesa RADV Darwin + VK_EXT_metal_surface)
  (org.garnet.metal,     ▲
   libgarnetxlate.dylib) │
        │                │ IOConnectCallMethod ('GRNT' ABI)
        ▼                │
  GarnetAccel.kext (org.garnet.accel, Auxiliary Kernel Collection in /Library/Extensions)
        │  GarnetAccelerator / GarnetFramebuffer over GarnetMetalNub
        ▼
  Garnet.kext (org.garnet.driver, Auxiliary Kernel Collection in /Library/Extensions)
        ▲
        │  Early PCI 64-bit Full-BAR (8/16 GiB) enumeration (Full-BAR mode only)
  GarnetPCIBarCompat.kext (org.garnet.pcibarcompat, OpenCore EFI/OC/Kexts/)
```

- `GarnetPCIBarCompat.kext` (`src/garnet-pci-compat/`): Optional early OpenCore kext for Full-BAR (`8 GiB` on 8GB, `16 GiB` on 16GB) mode. Verifies `com.apple.iokit.IOPCIFamily` via `exact-uuid` (`25G332`) or structural checks across macOS `26.x` before enabling large GPU BAR allocation.
- `GarnetRecover.efi` (`src/garnet-recover-efi/`): Standalone OpenCore Picker recovery tool (`EFI/OC/Tools/GarnetRecover.efi`) that performs zero ESP disk writes and communicates via UEFI NVRAM (`[1]` Safe Boot Once, `[2]` Safe Boot & Uninstall Garnet via the self-contained `com.garnet.recovery` LaunchDaemon).
- Optional `AMFIAnchor` trust-anchor path (`GarnetAnchorInjector.kext`): Payloads built with `GARNET_SIGN_CA_DIR` sign userspace Mach-Os with a dedicated code-signing CA and ship a codeless Schema-1 `GarnetAnchorInjector.kext` (`org.garnet.code-signing`) for upstream BSD-3-Clause `b00t0x/AMFIAnchor` (`Lilu.kext -> AMFIAnchor.kext -> GarnetAnchorInjector.kext`), letting macOS keep AMFI enabled without `amfi=0x80` while `Garnet.kext` and `GarnetAccel.kext` retain zero `Lilu.kext` dependency.
- `Garnet.kext` (`src/garnet/`): Initializes PCIe BARs and IP discovery, loads GPU firmware, manages VMIDs and sparse page tables, handles S3 sleep/wake and DP DDC/AUX recovery, drives DCN 4.01 modesetting and 128x128 hardware cursors via `AMDGarnetPrimaryFramebuffer`, and exposes the `'GRNT'` user client (`IOAccelGarnetNativeClient`).
- `GarnetAccel.kext` (`src/garnet-accel/`): Integrates with `IOAcceleratorFamily2` in the Auxiliary Kernel Collection using versioned layout fingerprints (`tools/ioaccel-layout/`).
- Mesa RADV Darwin (`mesa-patches/`): Patch series on Mesa `f5cb8ee032adabef599ae892ec4e42d796b84da9` building `libvulkan_radeon.dylib` with ACO over `'GRNT'`, `VK_EXT_metal_surface`, and `tools/rdna4/radv-app.sh`.
- `GarnetMetal.bundle` (`src/garnet-metal/`) and `libgarnetxlate.dylib` (`third-party/metal2vulkan-rdna4/`): Subclasses `MTLIOAccelDevice` over RADV, translates Metal AIR shaders to SPIR-V via `metal2vulkan` patches and `garnetxlate`, and provides `GarnetGLDrawable` for OpenGL-on-Metal processes outside `WindowServer`.
- `Garnet.app` and `garnetctl` (`src/garnet-app/`): SwiftUI installer, CLI, preflight checker, OpenCore configurator/validator, denylist manager, and diagnostics exporter.

## Installation (RX 9060 XT / Navi 44)

> [!IMPORTANT]
> Release v0.1.1 supports **bare-metal x86_64 macOS 26.x** with an **AMD Radeon RX 9060 XT (`1002:7590`, Navi 44)**. Virtual machines, Apple Silicon hosts, and Navi 48 GPUs are blocked by preflight. Garnet never mounts your EFI partition automatically. RX 9060 XT **16GB** is physically verified; **8GB** support is code-ready and pending physical hardware verification (see [docs/rdna4/full-bar-aperture.md](docs/rdna4/full-bar-aperture.md)).

### Step 1: BIOS and app launch

1. In motherboard BIOS, enable `Above 4G Decoding` and `Re-Size BAR Support`, and disable `CSM`.
2. Download `Garnet-<version>-Navi44-macOS26.dmg` from Releases and copy `Garnet.app` to `/Applications`.
3. Clear quarantine (or right-click **Open**) and launch `Garnet.app`:

   ```sh
   xattr -dr com.apple.quarantine /Applications/Garnet.app
   ```

### Step 2: Configure OpenCore

1. Mount your EFI partition.
2. In **Garnet.app -> OpenCore Assistant**, select the BAR mode recommended on the main screen (`Full-BAR (8/16 GiB)` or `256 MiB Small-BAR`) and the AMFI mode (`AMFIAnchor (AMFI Enabled)` if your payload was built with `GARNET_SIGN_CA_DIR` and `EFI/OC/Kexts/` has `Lilu.kext` + `AMFIAnchor.kext`, or `Legacy (amfi=0x80)` for ad-hoc payloads). Click **Configure OpenCore…** and select your mounted `EFI` folder or `EFI/OC/config.plist` (CLI equivalent: `/Applications/Garnet.app/Contents/Resources/garnetctl configure-oc --mode full --amfi legacy --config /Volumes/EFI/EFI/OC/config.plist`, or `--amfi anchor`). Garnet saves a timestamped backup and a one-time pre-Garnet baseline (`config.plist.garnet-pre-garnet`), copies `GarnetPCIBarCompat.kext` (Full-BAR), `GarnetRecover.efi` (`EFI/OC/Tools/`), and in `anchor` mode enforces `Lilu.kext -> AMFIAnchor.kext -> GarnetAnchorInjector.kext` (before `AMFIPass.kext` if present) without `amfi=0x80`, then validates the file.
3. If preflight shows `SIP SETUP NEEDED`, reboot once so the updated `csr-active-config` takes effect.

### Step 3: Install and reboot

1. In **Garnet.app -> Install / Uninstall**, wait for `READY` and click **Install / Upgrade Driver…** (or run `sudo /Applications/Garnet.app/Contents/Resources/garnetctl install`; enable the Vulkan ICD option or pass `--with-vulkan` if you want RADV Vulkan). Upgrading from `v0.1.0` migrates `app-denylist.txt` to `/Library/Application Support/Garnet/native` and removes legacy runtime directories.
2. Reboot.

### App denylist, uninstall, and recovery

- Exclude an app from Metal acceleration by dropping it onto **Garnet.app -> Status & Denylist**, or run `sudo /Applications/Garnet.app/Contents/Resources/garnetctl denylist add-app /Applications/ProblemApp.app`.
- Export redacted diagnostics from **Status & Denylist** or run `garnetctl export-diag --out ~/Desktop/garnet-diag.tar.gz`.
- **OpenCore Picker recovery (`GarnetRecover.efi`)**: Select **Garnet: Safe Boot & Recovery** in the OpenCore boot menu (zero ESP writes):
  - `[1] Safe Boot Once` appends `garnet=0 garnet-aux=0 garnet-rebar-compat=0` in UEFI NVRAM for the next macOS selection while leaving Garnet installed.
  - `[2] Safe Boot & Uninstall Garnet` sets the same safe-boot flags plus an NVRAM recovery flag; on the next macOS boot, `com.garnet.recovery` clears the flag first, runs self-contained `garnetctl uninstall`, writes `/Library/Logs/Garnet/recovery-complete.txt`, and stays on the safe desktop without auto-rebooting.
- **Restore OpenCore**: After uninstalling (normal or recovery), mount your EFI partition and click **Garnet.app -> OpenCore Assistant -> Restore OpenCore…** or run `/Applications/Garnet.app/Contents/Resources/garnetctl restore-oc --config /Volumes/EFI/EFI/OC/config.plist` (pass `--strip-only` if no `config.plist.garnet-pre-garnet` baseline exists). This restores your pre-Garnet `config.plist`, removes verified Garnet EFI files, and never deletes `Lilu.kext` or `AMFIAnchor.kext`.
- Manual uninstall: click **Garnet.app -> Install / Uninstall -> Uninstall Garnet…** (or run `sudo garnetctl uninstall`), then run `restore-oc` on your mounted EFI and reboot.
- Emergency OpenCore boot-arg kill-switches: `garnet=0` disables `Garnet.kext`, `garnet-aux=0` disables `GarnetAccel.kext`, `garnet-rebar-compat=0` disables the Full-BAR hook, and `garnet-apps-all=0` restricts desktop app Metal acceleration.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/garnet` | Primary kext (`Garnet.kext`), DCN 4.01 modeset/cursor code, VM/sleep helpers, shaders, and host tests |
| `src/garnet-accel` | Auxiliary Kernel Collection kext (`GarnetAccel.kext`) and layout gate tests |
| `src/garnet-pci-compat` | Early OpenCore Full-BAR kext (`GarnetPCIBarCompat.kext`) and `IOPCIFamily` tests |
| `src/garnet-recover-efi` | Freestanding x86_64 UEFI OpenCore Picker recovery tool (`GarnetRecover.efi`) and pure-C host tests |
| `src/garnet-metal` | `GarnetMetal.bundle`, pure-C modules, `spvcache` metadata, and tests |
| `src/garnet-app` | Swift `Garnet.app`, `garnetctl` CLI, KC/OpenCore validator and configurator, and `GarnetCoreTests` |
| `src/dcn41` | DCN 4.01 register headers and generated write allowlist |
| `mesa-patches` | Mesa RADV Darwin patch series and host tests |
| `third-party/metal2vulkan-rdna4` | `metal2vulkan` patch series and `garnetxlate` Rust crate (LGPL-3.0-or-later) |
| `tools/rdna4` | Payload builder (`build-all.sh`), `.dmg` packager (`build-release-dmg.sh`), installers, and `radv-app.sh` |
| `tools/native` | Userspace translation helpers and diagnostic probes |
| `tools/dcn41` | DCN 4.01 allowlist/register codegen and mutation tests |
| `tools/ioaccel-layout` | Kernel collection carving, layout verification, and fingerprint export tools |
| `tools/fetch-firmware.sh` | Copies RDNA 4 microcode blobs from a local `linux-firmware` checkout |
| `docs` | [Documentation index](docs/README.md) and subsystem specifications |

## Prerequisites and building

External binary dependencies and upstream source trees must be supplied locally before building:

| Dependency | Pinned revision / source | Role |
| --- | --- | --- |
| `MacKernelSDK` | Acidanthera `MacKernelSDK` `05094e5e88cec7caedbfb35e8449ed0db94bf95b` | Kernel SDK passed as `MKSDK` when building `Garnet.kext` and `GarnetAccel.kext` |
| `linux-firmware` | Upstream kernel firmware repository | Source for Navi 44 / Navi 48 microcode copied into `src/garnet/firmware/` |
| `Mesa` | Commit `f5cb8ee032adabef599ae892ec4e42d796b84da9` | Base for `mesa-patches/` and `tools/rdna4/build-radv.sh` (requires `glslangValidator` >= 12.2, `meson`, `ninja`) |
| `metal2vulkan` | Commit `43c46ac8a24adf1a6e872b8a52c706ec9614fad0` | Base for `third-party/metal2vulkan-rdna4/` and `tools/rdna4/build-xlate.sh` (requires Rust >= 1.87) |
| Target macOS KC dump | `BootKernelExtensions.kc` / `SystemKernelExtensions.kc` (macOS 26.x) | Input for `tools/ioaccel-layout/` to generate `GarnetAccel` layout headers (`GARNET_KC_WORK` / `GARNET_LAYOUT_OUT`) |

Fetch Navi 44 firmware and build `Garnet.kext` on macOS (or cross-compile from Linux with `clang` and a cctools `ld64` supporting `-kext`):

```sh
CHIPS=navi44 bash tools/fetch-firmware.sh /path/to/linux-firmware
make -C src/garnet CHIPS=navi44 MKSDK=/path/to/MacKernelSDK all
```

To build a full matched payload on macOS, follow [BUILDING.md](BUILDING.md) and [tools/rdna4/README.md](tools/rdna4/README.md), then run `tools/rdna4/build-all.sh OUT_DIR`. To build with `AMFIAnchor` trust-anchor signing (eliminating `amfi=0x80`), generate an out-of-repo local CA with `bash tools/rdna4/make-signing-ca.sh ~/garnet-signing-ca` and pass `GARNET_SIGN_CA_DIR=~/garnet-signing-ca` (plus optional `GARNET_AMFIANCHOR_KEXT=/path/to/AMFIAnchor.kext`) to `tools/rdna4/build-all.sh` (requires `rcodesign`); leaving `GARNET_SIGN_CA_DIR` unset preserves default ad-hoc signing (`amfi=0x80`).

## Documentation

- [BUILDING.md](BUILDING.md): Kext prerequisites, Linux cross-build, firmware handling, and `build-all.sh`
- [docs/README.md](docs/README.md): Index of subsystem specifications and Metal capability matrices
- [GLOSSARY.md](GLOSSARY.md): Component names, chip naming rule, and domain terms
- [CHANGES.md](CHANGES.md): History from the Cobalt64 baseline through standalone Garnet
- [tools/rdna4/README.md](tools/rdna4/README.md): Payload build, verification, installation, and removal scripts

## Credits and licenses

This repository is licensed under the [MIT License](LICENSE), except for [third-party/metal2vulkan-rdna4](third-party/metal2vulkan-rdna4/NOTICE), which is a derivative work of `metal2vulkan` licensed under the GNU Lesser General Public License v3.0 or later ([COPYING](third-party/metal2vulkan-rdna4/COPYING)). Kernel-side `src/dcn41/` headers derive from AMD's MIT-licensed `amdgpu` register headers. The git source tree does not redistribute AMD binary firmware blobs; release payloads (`RDNA4_RELEASE=1`) and `.dmg` installers bundle Navi 44 firmware under `LICENSE.amdgpu`.

Garnet builds on these projects:

- [Cobalt64](https://github.com/Almosst-DEV/Cobalt64) (MIT): Core RDNA 4 bring-up kext, `'GRNT'` native user client, Mesa RADV Darwin backend foundation, `GarnetMetal` bundle design, and `GarnetAccel` layout tooling.
- [RX4Darwin/AMDRadeonRDNA4.kext](https://github.com/RX4Darwin/AMDRadeonRDNA4.kext) (BSD 3-Clause, Copyright (c) 2026 Sunneva N. Mariu, `next` @ `dded456d`): Adapted non-Lilu components, including the Navi 44 `ODM_MEM_PWR_CTRL2` hazard exclusion, CoreDisplay `AMD*` framebuffer prefix recognition, DCN 4.01 128x128 hardware cursor programming, RADV `VK_EXT_metal_surface` WSI fallback and `radv-app.sh`, VMID exact TLB ack / quarantine / sparse page-table rollback, S3 sleep/wake and DP DDC-to-AUX pad recovery, and the `GarnetGLDrawable` surface bridge (`supportsUnalignedVertexFetch`, `commitAndHold`, `setLineWidth:`).
- [RDNA4FB](https://github.com/somestupidgirl/RDNA4FB) (BSD 3-Clause, Copyright (c) 2026 Sunneva N. Mariu): On-die IP discovery (`src/garnet/src/ipdiscovery.*`), BAR5 mapping, and indirect VRAM survey logic.
- [mac-amdgpu](https://github.com/lemonade-sdk/mac-amdgpu) (MIT, Copyright (c) 2026 Geramy Loveless): RDNA 4 IP-block initialization sequence references.
- [AMFIAnchor](https://github.com/b00t0x/AMFIAnchor) (BSD 3-Clause, Copyright (c) 2026 b00t0x): Optional OpenCore trust-anchor kernel extension interface (`SchemaVersion = 1` codeless injector contract) used to keep macOS AMFI enabled without `amfi=0x80`.
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa) (MIT) and [metal2vulkan](https://github.com/steelbrain/metal2vulkan) (LGPL-3.0-or-later, derivative patches noted in [third-party/metal2vulkan-rdna4/NOTICE](third-party/metal2vulkan-rdna4/NOTICE)).

Full third-party license texts and commit-level attributions are in [third-party/THIRD-PARTY-LICENSES.txt](third-party/THIRD-PARTY-LICENSES.txt). This project is an independent open-source effort and is not affiliated with or endorsed by AMD, Apple, Cobalt64, or RX4Darwin.
