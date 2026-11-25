# Garnet

English | [简体中文](README.zh-CN.md)

Garnet is an experimental open-source macOS GPU and display driver stack for AMD RDNA 4 graphics cards on x86_64 OpenCore systems. It builds on [Cobalt64](https://github.com/Almosst-DEV/Cobalt64) as a standalone four-layer architecture with no `Lilu.kext` or external display plugin dependency, and adapts selected display, cursor, WSI, VM, sleep, and OpenGL surface work from [RX4Darwin/AMDRadeonRDNA4.kext](https://github.com/RX4Darwin/AMDRadeonRDNA4.kext).

> [!WARNING]
> `Garnet.kext` writes GPU registers directly across PCIe BARs, PSP, SMU, RLC, SDMA, CP, MES, and the DCN display engine. An incompatible build, GPU, or macOS version can freeze, reboot, or corrupt display output. Run it only on a dedicated, recoverable test machine at your own risk; see [LICENSE](LICENSE).

## Supported hardware

| Silicon | Models | PCI device IDs | LLVM / IP targets | Status |
| --- | --- | --- | --- | --- |
| Navi 44 | Radeon RX 9060 XT **16GB** | `1002:7590` | `gfx1200`, `GC 12.0.0`, `MP0 14.0.2`, `SDMA 7.0.0` | Primary physically verified target |
| Navi 44 | Radeon RX 9060 XT **8GB** | `1002:7590` | `gfx1200`, `GC 12.0.0`, `MP0 14.0.2`, `SDMA 7.0.0` | Code-ready, pending physical hardware verification |
| Navi 48 | Radeon RX 9070 / RX 9070 XT / AI PRO R9700 | `1002:7550`, `1002:7551` | `gfx1201`, `GC 12.0.1`, `MP0 14.0.3`, `SDMA 7.0.1` | Chip table, firmware path, and Mesa entry retained from Cobalt64; unverified on physical hardware |

Shared interfaces, boot arguments, and files use `RDNA4` and `Garnet` names. `Navi 44` and `Navi 48` appear only where chip hardware, firmware blobs, or PCI IDs differ. `GarnetAccel.kext` kernel-collection fingerprinting targets x86_64 macOS `26.x` (`25G332` verified baseline), and Release 1 binaries target bare-metal Navi 44 (`1002:7590`) only.

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

- `GarnetPCIBarCompat.kext` (`src/garnet-pci-compat/`): Optional early OpenCore kext for Full-BAR (`8 GiB` on 8GB, `16 GiB` on 16GB) mode. It verifies `com.apple.iokit.IOPCIFamily` via `exact-uuid` (`25G332`) or `structural` checks across macOS `26.x` before enabling large GPU BAR allocation.
- `Garnet.kext` (`src/garnet/`): Initializes PCIe BARs and IP discovery, loads GPU firmware, manages VMIDs and sparse page tables, handles S3 sleep/wake and DP DDC/AUX recovery, drives DCN 4.01 modesetting and 128x128 hardware cursors via `AMDGarnetPrimaryFramebuffer`, and exposes the `'GRNT'` user client (`IOAccelGarnetNativeClient`).
- `GarnetAccel.kext` (`src/garnet-accel/`): Integrates with `IOAcceleratorFamily2` in the Auxiliary Kernel Collection by matching versioned layout fingerprints (`tools/ioaccel-layout/`).
- Mesa RADV Darwin (`mesa-patches/`): Patch series on Mesa `f5cb8ee032adabef599ae892ec4e42d796b84da9` building `libvulkan_radeon.dylib` with ACO over `'GRNT'`, `VK_EXT_metal_surface`, and `tools/rdna4/radv-app.sh`.
- `GarnetMetal.bundle` (`src/garnet-metal/`) and `libgarnetxlate.dylib` (`third-party/metal2vulkan-rdna4/`): Subclasses `MTLIOAccelDevice` over RADV, translates Metal AIR shaders to SPIR-V via `metal2vulkan` patches and `garnetxlate`, and provides `GarnetGLDrawable` for OpenGL-on-Metal processes outside `WindowServer`.
- `Garnet.app` and `garnetctl` (`src/garnet-app/`): SwiftUI installer, CLI, preflight checker, OpenCore configurator/validator, denylist manager, and diagnostics exporter.

## Installation (RX 9060 XT / Navi 44)

> [!IMPORTANT]
> Release 1 supports **bare-metal x86_64 macOS 26.x** with an **AMD Radeon RX 9060 XT (`1002:7590`, Navi 44)**. Virtual machines, Apple Silicon hosts, and Navi 48 GPUs are blocked by preflight. Garnet never mounts your EFI partition automatically. RX 9060 XT **16GB** is physically verified; **8GB** support is code-ready and pending physical hardware verification (see [docs/rdna4/full-bar-aperture.md](docs/rdna4/full-bar-aperture.md)).

### Step 1: BIOS and app launch

1. In motherboard BIOS, enable `Above 4G Decoding` and `Re-Size BAR Support`, and disable `CSM`.
2. Download `Garnet-<version>-Navi44-macOS26.dmg` from Releases and copy `Garnet.app` to `/Applications`.
3. Clear quarantine (or right-click **Open**) and launch `Garnet.app`:

   ```sh
   xattr -dr com.apple.quarantine /Applications/Garnet.app
   ```

### Step 2: Configure OpenCore

1. Mount your EFI partition.
2. In **Garnet.app -> OpenCore Assistant**, select the mode recommended on the main screen (`Full-BAR (8/16 GiB)` or `256 MiB Small-BAR`), click **Configure OpenCore…**, and select your mounted `EFI` folder or `EFI/OC/config.plist` (CLI equivalent: `/Applications/Garnet.app/Contents/Resources/garnetctl configure-oc --mode full --config /Volumes/EFI/EFI/OC/config.plist`). Garnet backs up `config.plist`, copies `GarnetPCIBarCompat.kext` for Full-BAR mode, updates the required settings, and validates the file.
3. If preflight shows `SIP SETUP NEEDED`, reboot once so the updated `csr-active-config` takes effect.

### Step 3: Install and reboot

1. In **Garnet.app -> Install / Uninstall**, wait for `READY` and click **Install / Upgrade Driver…** (or run `sudo /Applications/Garnet.app/Contents/Resources/garnetctl install`; enable the Vulkan ICD option or pass `--with-vulkan` if you want RADV Vulkan). Upgrading from `v0.1.0` automatically migrates `app-denylist.txt` to `/Library/Application Support/Garnet/native` and cleans up legacy runtime directories.
2. Reboot.

### App denylist, uninstall, and recovery

- To exclude an app from Metal acceleration, drop it onto **Garnet.app -> Status & Denylist** or run `sudo /Applications/Garnet.app/Contents/Resources/garnetctl denylist add-app /Applications/ProblemApp.app`.
- To export redacted diagnostics, use **Status & Denylist** or run `garnetctl export-diag --out ~/Desktop/garnet-diag.tar.gz`.
- To uninstall, click **Garnet.app -> Install / Uninstall -> Uninstall Garnet…** (or run `sudo garnetctl uninstall`), then reboot and disable `GarnetPCIBarCompat.kext` and `garnet*` boot-args in OpenCore.
- Emergency boot-arg kill-switches for OpenCore: `garnet=0` disables `Garnet.kext`, `garnet-aux=0` disables `GarnetAccel.kext`, `garnet-rebar-compat=0` disables the Full-BAR hook, and `garnet-apps-all=0` restricts desktop app Metal acceleration.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/garnet` | Primary kext (`Garnet.kext`), DCN 4.01 modeset/cursor code, VM/sleep helpers, shaders, and host tests |
| `src/garnet-accel` | Auxiliary Kernel Collection kext (`GarnetAccel.kext`) and layout gate tests |
| `src/garnet-pci-compat` | Early OpenCore Full-BAR kext (`GarnetPCIBarCompat.kext`) and `IOPCIFamily` tests |
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
| Target macOS KC dump | `BootKernelExtensions.kc` / `SystemKernelExtensions.kc` (25G332) | Input for `tools/ioaccel-layout/` to generate `GarnetAccel` layout headers (`C64_WORK` / `C64_LAYOUT_OUT`) |

Fetch Navi 44 firmware and build `Garnet.kext` on macOS (or cross-compile from Linux with `clang` and a cctools `ld64` supporting `-kext`):

```sh
CHIPS=navi44 bash tools/fetch-firmware.sh /path/to/linux-firmware
make -C src/garnet CHIPS=navi44 MKSDK=/path/to/MacKernelSDK all
```

To build a full matched payload on macOS, follow [BUILDING.md](BUILDING.md) and [tools/rdna4/README.md](tools/rdna4/README.md), then run `tools/rdna4/build-all.sh OUT_DIR`.

## Documentation

- [BUILDING.md](BUILDING.md): Kext prerequisites, Linux cross-build, firmware handling, and `build-all.sh`
- [docs/README.md](docs/README.md): Index of active hardware runbooks and subsystem specs
- [GLOSSARY.md](GLOSSARY.md): Component names, chip naming rule, and domain terms
- [CHANGES.md](CHANGES.md): History from the Cobalt64 baseline through standalone Garnet
- [tools/rdna4/README.md](tools/rdna4/README.md): Payload build, verification, installation, and removal scripts

## Credits and licenses

This repository is licensed under the [MIT License](LICENSE), except for [third-party/metal2vulkan-rdna4](third-party/metal2vulkan-rdna4/NOTICE), which is a derivative work of `metal2vulkan` licensed under the GNU Lesser General Public License v3.0 or later ([COPYING](third-party/metal2vulkan-rdna4/COPYING)). Kernel-side `src/dcn41/` headers derive from AMD's MIT-licensed `amdgpu` register headers. The git source tree does not redistribute AMD binary firmware blobs; official Release 1 payloads (`RDNA4_RELEASE=1`) and `.dmg` installers bundle Navi 44 firmware under `LICENSE.amdgpu`.

Garnet builds on these projects:

- [Cobalt64](https://github.com/Almosst-DEV/Cobalt64) (MIT): Core RDNA 4 bring-up kext, `'GRNT'` native user client, Mesa RADV Darwin backend foundation, `GarnetMetal` bundle design, and `GarnetAccel` layout tooling.
- [RX4Darwin/AMDRadeonRDNA4.kext](https://github.com/RX4Darwin/AMDRadeonRDNA4.kext) (BSD 3-Clause, Copyright (c) 2026 Sunneva N. Mariu, `next` @ `dded456d`): Adapted non-Lilu components, including the Navi 44 `ODM_MEM_PWR_CTRL2` hazard exclusion, CoreDisplay `AMD*` framebuffer prefix recognition, DCN 4.01 128x128 hardware cursor programming, RADV `VK_EXT_metal_surface` WSI fallback and `radv-app.sh`, VMID exact TLB ack / quarantine / sparse page-table rollback, S3 sleep/wake and DP DDC-to-AUX pad recovery, and the `GarnetGLDrawable` surface bridge (`supportsUnalignedVertexFetch`, `commitAndHold`, `setLineWidth:`).
- [RDNA4FB](https://github.com/somestupidgirl/RDNA4FB) (BSD 3-Clause, Copyright (c) 2026 Sunneva N. Mariu): On-die IP discovery (`src/garnet/src/ipdiscovery.*`), BAR5 mapping, and indirect VRAM survey logic.
- [mac-amdgpu](https://github.com/lemonade-sdk/mac-amdgpu) (MIT, Copyright (c) 2026 Geramy Loveless): RDNA 4 IP-block initialization sequence references.
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa) (MIT), [metal2vulkan](https://github.com/steelbrain/metal2vulkan) (LGPL-3.0-or-later), and [NullMoth Systems](https://github.com/nullmoth/nvidia-macos-driver) (LGPL-3.0-or-later compiler adaptations noted in [third-party/metal2vulkan-rdna4/NOTICE](third-party/metal2vulkan-rdna4/NOTICE)).

Full third-party license texts and commit-level attributions are in [third-party/THIRD-PARTY-LICENSES.txt](third-party/THIRD-PARTY-LICENSES.txt). This project is an independent open-source effort and is not affiliated with or endorsed by AMD, Apple, Cobalt64, or RX4Darwin.
