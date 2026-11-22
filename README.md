# Garnet

English | [简体中文](README.zh-CN.md)

Garnet is an experimental open-source macOS GPU and display driver stack for AMD RDNA 4 graphics cards on x86_64 systems booted with OpenCore. It evolves the [Cobalt64](https://github.com/Almosst-DEV/Cobalt64) RDNA 4 stack into a standalone four-layer architecture with no `Lilu.kext` or external display plugin dependency, while absorbing selected display, cursor, WSI, VM, sleep, and OpenGL-bridge work from [RX4Darwin/AMDRadeonRDNA4.kext](https://github.com/RX4Darwin/AMDRadeonRDNA4.kext).

> [!WARNING]
> `Garnet.kext` writes GPU registers directly across PCIe BARs, PSP, SMU, RLC, SDMA, CP, MES, and the DCN display engine. An incompatible build, GPU card, or macOS version can freeze, reboot, or corrupt display output. Run it only on a dedicated, recoverable test machine at your own risk; see [LICENSE](LICENSE).

## Supported hardware

| Silicon | Marketing models | PCI device IDs | LLVM / IP targets | Status |
| --- | --- | --- | --- | --- |
| Navi 44 | Radeon RX 9060 XT | `1002:7590` | `gfx1200`, `GC 12.0.0`, `MP0 14.0.2`, `SDMA 7.0.0` | Primary development target in this repository |
| Navi 48 | Radeon RX 9070 / RX 9070 XT / AI PRO R9700 | `1002:7550`, `1002:7551` | `gfx1201`, `GC 12.0.1`, `MP0 14.0.3`, `SDMA 7.0.1` | Per-chip table, firmware path, and canned Mesa entry retained from Cobalt64; unverified on physical hardware here |

Shared interfaces, boot arguments, and files use `RDNA4` and standalone `Garnet` identities. `Navi 44` and `Navi 48` names appear only where chip hardware, firmware blobs, or PCI IDs differ. `GarnetAccel.kext` kernel-collection layout targets macOS 26.7.2 (build `25G332`) on x86_64.

## Architecture

Garnet splits kernel initialization, macOS kernel integration, Vulkan translation, and Metal user-space support across four layers:

```text
  Metal / OpenGL / Vulkan apps
        │                │
        ▼                ▼
  GarnetMetal.bundle   libvulkan_radeon.dylib (Mesa RADV Darwin + VK_EXT_metal_surface)
  (org.garnet.metal,     ▲
   libgarnetxlate.dylib) │
        │                │ IOConnectCallMethod ('GRNT' ABI)
        ▼                │
  GarnetAccel.kext (org.garnet.accel, Auxiliary Kernel Collection)
        │  GarnetAccelerator / GarnetFramebuffer over GarnetMetalNub
        ▼
  Garnet.kext (org.garnet.driver, built-in DCN 4.01 AMDGarnetPrimaryFramebuffer)
```

- `Garnet.kext` (`src/garnet/`, `org.garnet.driver`, `0.0.673`): Initializes PCIe BARs, parses on-die IP discovery, loads PSP/SMU/GMC/GFX/SDMA/MES firmware, manages VMID pools with TLB sequence ack and sparse page-table rollback, handles S3 sleep/wake and DP DDC/AUX pad recovery, drives built-in DCN 4.01 mode-setting and 128x128 hardware cursors through `AMDGarnetPrimaryFramebuffer`, and exposes the `'GRNT'` user client (`IOAccelGarnetNativeClient`).
- `GarnetAccel.kext` (`src/garnet-accel/`, `org.garnet.accel`, `0.0.8`): Integrates with Apple's `IOAcceleratorFamily2` in the Auxiliary Kernel Collection by deriving layout offsets from target macOS kernel collections (`tools/ioaccel-layout/`).
- Mesa RADV Darwin (`mesa-patches/`): 26 patches against Mesa commit `f5cb8ee032adabef599ae892ec4e42d796b84da9` building `libvulkan_radeon.dylib` with the ACO shader compiler over the `'GRNT'` backend, including `VK_EXT_metal_surface` window presentation and the `tools/rdna4/radv-app.sh` launcher wrapper.
- `GarnetMetal.bundle` (`src/garnet-metal/`, `org.garnet.metal`, `0.1.1` build `43`) and `libgarnetxlate.dylib` (`third-party/metal2vulkan-rdna4/`): Subclasses `MTLIOAccelDevice` over RADV, translates Metal AIR shaders to SPIR-V using 65 `metal2vulkan` patches plus the `garnetxlate` crate, and provides a standalone `GarnetGLDrawable` surface bridge for OpenGL-on-Metal processes outside `WindowServer` (without `AppleParavirtGPU` or `glProgramAir` codegen paths).

## Repository layout

| Path | Contents |
| --- | --- |
| `src/garnet` | Primary kext (`Garnet.kext`), DCN 4.01 modeset/cursor code, VM/sleep helpers, shaders, and ASan/UBSan host tests |
| `src/garnet-accel` | Auxiliary Kernel Collection kext (`GarnetAccel.kext`) and host layout gate tests |
| `src/garnet-metal` | `GarnetMetal.bundle` Objective-C implementation, pure-C invariant modules, `spvcache` metadata, and tests |
| `src/dcn41` | DCN 4.01 register headers and generated write allowlist (with Navi 44 `ODM_MEM_PWR_CTRL2` read/write hazard excluded) |
| `mesa-patches` | 26-patch Mesa RADV Darwin series and offline runtime/pipeline/buffer host tests |
| `third-party/metal2vulkan-rdna4` | 65-patch `metal2vulkan` series and `garnetxlate` Rust crate (LGPL-3.0-or-later) |
| `tools/rdna4` | Matched payload builder (`build-all.sh`), target installers, RADV/translator builders, and `radv-app.sh` |
| `tools/native` | Userspace translation helpers and diagnostic probes |
| `tools/dcn41` | DCN 4.01 allowlist/register codegen and mutation test runner |
| `tools/ioaccel-layout` | Kernel collection carving and vtable/layout verification scripts |
| `tools/fetch-firmware.sh` | Copies required RDNA 4 microcode blobs from a local `linux-firmware` checkout |
| `docs` | [Documentation index](docs/README.md) and subsystem specifications |

## Prerequisites and building

The repository omits external binary dependencies and upstream source trees, which must be supplied locally before building:

| Dependency | Pinned revision / source | Role |
| --- | --- | --- |
| `MacKernelSDK` | Acidanthera `MacKernelSDK` `05094e5e88cec7caedbfb35e8449ed0db94bf95b` | Kernel SDK passed as `MKSDK` when building `Garnet.kext` and `GarnetAccel.kext` |
| `linux-firmware` | Upstream kernel firmware repository | Source for Navi 44 / Navi 48 GPU firmware blobs copied into gitignored `src/garnet/firmware/` |
| `Mesa` | Commit `f5cb8ee032adabef599ae892ec4e42d796b84da9` | Base for `mesa-patches/` and `tools/rdna4/build-radv.sh` (requires `glslangValidator` >= 12.2, `meson`, `ninja`) |
| `metal2vulkan` | Commit `43c46ac8a24adf1a6e872b8a52c706ec9614fad0` | Base for `third-party/metal2vulkan-rdna4/` and `tools/rdna4/build-xlate.sh` (requires Rust >= 1.87) |
| Target macOS KC dump | `BootKernelExtensions.kc` / `SystemKernelExtensions.kc` (25G332) | Required by `tools/ioaccel-layout/` to generate `GarnetAccel` layout headers (`C64_WORK` / `C64_LAYOUT_OUT`) |

Fetch Navi 44 firmware and build `Garnet.kext` on macOS (or cross-compile from Linux with `clang` and a cctools `ld64` supporting `-kext`):

```sh
CHIPS=navi44 bash tools/fetch-firmware.sh /path/to/linux-firmware
make -C src/garnet CHIPS=navi44 MKSDK=/path/to/MacKernelSDK all
```

To assemble a matched signed payload containing `Garnet.kext`, `GarnetAccel.kext`, `GarnetMetal.bundle`, `libvulkan_radeon.dylib`, `libgarnetxlate.dylib`, and native tools on macOS, follow [BUILDING.md](BUILDING.md) and [tools/rdna4/README.md](tools/rdna4/README.md), then run `tools/rdna4/build-all.sh OUT_DIR`.

## Host tests

Offline host suites run on CPU without a physical RDNA 4 GPU to validate pure-C/C++ invariants, ABI contracts, patch application, and script logic. Passing these checks does not substitute for macOS linking or live hardware execution.

```sh
make -C src/garnet/tests all
python3 tools/dcn41/build.py regs
python3 tools/dcn41/build.py unit
python3 tools/dcn41/build.py cxx
python3 tools/dcn41/build.py allowcontrol
make -C src/garnet-accel/tests BRING_ROOT="$(pwd)" all
python3 tools/rdna4/tests/build-identity-test.py -v
bash tools/rdna4/tests/radv-app-test.sh
bash tools/rdna4/tests/collect-installed-test.sh
```

See [src/garnet-metal/TESTING.md](src/garnet-metal/TESTING.md) and [mesa-patches/README.txt](mesa-patches/README.txt) for additional offline Metal, GL-surface, and RADV mock tests.

## Documentation

- [BUILDING.md](BUILDING.md): Kext prerequisites, Linux kernel-only cross-build, firmware handling, and `build-all.sh`
- [docs/README.md](docs/README.md): Index of active hardware runbooks and subsystem specs
- [GLOSSARY.md](GLOSSARY.md): Component names, chip naming rule, and domain terms
- [CHANGES.md](CHANGES.md): History from the Cobalt64 baseline through standalone Garnet
- [tools/rdna4/README.md](tools/rdna4/README.md): Payload build, verification, installation, and removal scripts

## Credits and licenses

This repository is licensed under the [MIT License](LICENSE), except for [third-party/metal2vulkan-rdna4](third-party/metal2vulkan-rdna4/NOTICE), which is a derivative work of `metal2vulkan` licensed separately under the GNU Lesser General Public License v3.0 or later ([COPYING](third-party/metal2vulkan-rdna4/COPYING)). Registered kernel-side `src/dcn41/` headers derive from AMD's MIT-licensed `amdgpu` register headers. AMD firmware blobs are not redistributed in this repository.

Garnet builds on and credits the following projects:

- [Cobalt64](https://github.com/Almosst-DEV/Cobalt64) (MIT): Core RDNA 4 bring-up kext, `'GRNT'` native user client, Mesa RADV Darwin backend foundation, `GarnetMetal` bundle design, and `GarnetAccel` layout tooling.
- [RX4Darwin/AMDRadeonRDNA4.kext](https://github.com/RX4Darwin/AMDRadeonRDNA4.kext) (BSD 3-Clause, Copyright (c) 2026 Sunneva N. Mariu, `next` @ `dded456d`): Non-Lilu components adapted into Garnet, covering the Navi 44 `ODM_MEM_PWR_CTRL2` hazard exclusion, CoreDisplay `AMD*` framebuffer prefix recognition, DCN 4.01 128x128 hardware cursor programming, RADV `VK_EXT_metal_surface` WSI fallback and `radv-app.sh`, VMID exact TLB ack / quarantine / sparse page-table rollback, S3 sleep/wake and DP DDC-to-AUX pad recovery, and the `GarnetGLDrawable` surface bridge (`supportsUnalignedVertexFetch`, `commitAndHold`, `setLineWidth:`).
- [RDNA4FB](https://github.com/somestupidgirl/RDNA4FB) (BSD 3-Clause, Copyright (c) 2026 Sunneva N. Mariu): On-die IP discovery (`src/garnet/src/ipdiscovery.*`), BAR5 mapping, and indirect VRAM survey logic.
- [mac-amdgpu](https://github.com/lemonade-sdk/mac-amdgpu) (MIT, Copyright (c) 2026 Geramy Loveless): RDNA 4 IP-block initialization sequence references.
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa) (MIT), [metal2vulkan](https://github.com/steelbrain/metal2vulkan) (LGPL-3.0-or-later), and [NullMoth Systems](https://github.com/nullmoth/nvidia-macos-driver) (LGPL-3.0-or-later compiler adaptations noted in [third-party/metal2vulkan-rdna4/NOTICE](third-party/metal2vulkan-rdna4/NOTICE)).

Full third-party license texts and commit-level attributions are recorded in [third-party/THIRD-PARTY-LICENSES.txt](third-party/THIRD-PARTY-LICENSES.txt). This project is an independent open-source effort and is not affiliated with or endorsed by AMD, Apple, Cobalt64, or RX4Darwin.
