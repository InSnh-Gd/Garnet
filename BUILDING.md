# Building the Garnet driver stack

> **WARNING.** Garnet.kext writes GPU registers directly (PCIe BARs, PSP, SMU, RLC, SDMA, CP,
> MES, display engine). It is experimental. Physical bring-up from this tree is unverified, and a
> wrong build or a different card can hang or restart the machine or corrupt display state. The only
> intended target is one x86_64 test bench with one RX 9060 XT (Navi 44). Do not run it on a machine
> you cannot afford to restart or recover. You run it entirely at your own risk; there is no
> warranty (see LICENSE).

## Prerequisites

- macOS with the Xcode Command Line Tools (clang, make, codesign, python3) for a loadable kext,
  or Linux with clang, `llvm-nm`, and MacKernelSDK plus a cctools-port `ld` that implements
  `-kext` for a kernel-only cross-build (see below).
- MacKernelSDK (pinned at `05094e5e88cec7caedbfb35e8449ed0db94bf95b`, not vendored here); pass its
  path as `MKSDK` below. (`Garnet.kext` and `GarnetAccel.kext` have no `Lilu.kext` or external
  display-plugin dependency.)
- AMD firmware from linux-firmware (not in this repo, AMD's own licence):

  ```
  git clone https://gitlab.com/kernel-firmware/linux-firmware.git
  tools/fetch-firmware.sh <path-to-that-checkout>
  ```

  This only copies files (no network) into `src/garnet/firmware/`, which is
  gitignored. The kext embeds these files from linux-firmware's `amdgpu/`, ten per chip:

  ```
  Navi 48 (RX 9070 / 9070 XT, AI PRO R9700; CHIPS=navi48):
  gc_12_0_1_imu.bin  gc_12_0_1_me.bin  gc_12_0_1_mec.bin  gc_12_0_1_pfp.bin
  gc_12_0_1_rlc.bin  gc_12_0_1_uni_mes.bin
  psp_14_0_3_sos.bin  psp_14_0_3_ta.bin
  sdma_7_0_1.bin  smu_14_0_3.bin

  Navi 44 (RX 9060 XT; CHIPS=navi44):
  gc_12_0_0_imu.bin  gc_12_0_0_me.bin  gc_12_0_0_mec.bin  gc_12_0_0_pfp.bin
  gc_12_0_0_rlc.bin  gc_12_0_0_uni_mes.bin
  psp_14_0_2_sos.bin  psp_14_0_2_ta.bin
  sdma_7_0_0.bin  smu_14_0_2.bin
  ```

  `make` builds both chips by default (`CHIPS="navi48 navi44"`); `make CHIPS=navi44` builds a
  kext with only the 9060 XT's blobs (the other chip's symbols become empty stubs and that chip is
  refused at probe). `CHIPS=navi44 tools/fetch-firmware.sh <path>` copies only that chip's set.

  The Makefile stops with a list if any is missing. `tools/fetch-firmware.sh` prints the
  sha256 of each copy against the revision the project was developed with; a different
  linux-firmware revision may work, but only that one has been run.

## Build

```
make -C src/garnet MKSDK=/path/to/MacKernelSDK all
```

The result is `src/garnet/build/Garnet.kext` (x86_64, ad-hoc signed, min macOS
11 ABI). Check it with: `dwarfdump --uuid build/Garnet.kext/Contents/MacOS/Garnet`
and `nm -u -arch x86_64 <that binary>` (only kernel, IOKit and KPI symbols).

## Linux kernel-only cross-build

The kernel extension can also be built with Linux clang, MacKernelSDK,
`llvm-nm` and a cctools-port ld64 linker that implements `-kext`:

```sh
CHIPS=navi44 bash tools/fetch-firmware.sh /usr/lib/firmware
make -C src/garnet all CHIPS=navi44 BUILD=/path/to/workspace/temp/scratch/garnet \
  MKSDK=/absolute/path/to/MacKernelSDK \
  KEXT_LD=/absolute/path/to/x86_64-apple-darwin-ld
```

Linux defaults to `SIGN=0`: the result is explicitly unsigned, intended for
OpenCore injection. This does not validate macOS loading or GPU behavior.
The macOS build still defaults to `SIGN=1` and requires its normal codesign
checks. A current MacKernelSDK only supplies libkmod; the older libkmodc++
is linked when present. Userspace tools still need a full macOS SDK and are
not built by this kernel-only command.

Do not substitute ld64.lld: the tested LLVM 23 linker ignores `-kext` and
attempts to produce an executable. Check the output with `file` or
`llvm-objdump --macho --private-header`; it must be an x86_64 KEXTBUNDLE.

## Where generated files land

- `firmware/*.bin` inputs live in `src/garnet/firmware/` (gitignored). The Makefile
  generates the embedding C sources and their objects under `$(BUILD)/firmware/`
  (`FW_GEN := $(BUILD)/firmware`), so the default `src/` tree stays free of generated files.
  `BUILD` defaults to `src/garnet/build`; pass another `BUILD=` for candidate builds
  (for example a shader-cache experiment), never the default build directory.
- Compiled objects also land under `$(BUILD)/` (`dcn/*.o`, `amd/*.o`).
- The `src/garnet/firmware/` directory, `build/` and `build-*/`, `*.kext/`, `*.o`,
  `*.log`, and `runs/` are all gitignored (see `.gitignore`). Never commit build outputs,
  logs, or run archives.
- The aux kext host tests (`src/garnet-accel/tests/Makefile`) honor `BUILD` and
  `TMPDIR`: `BUILD ?= $(if $(TMPDIR),$(TMPDIR),/tmp)/garnet-accel-tests`. Work logs for the
  RDNA4 refactor itself go under the workspace `logs/rdna4-refactor/` directory, not `/tmp`
  and not into the repo.

## Runtime shader cache and ABI notes

The native stack (Mesa RADV plus `GarnetMetal.bundle` in `src/garnet-metal`) does not use
Apple shader-substitution caches. Compiled compute test shaders remain in `src/garnet/shaders/`.
Boot-args and loading are described in the source comments; this file only covers building.

## Rebuild cooperating components together

The standalone Garnet stack uses `Garnet` (`org.garnet.driver`), `GarnetAccel`
(`org.garnet.accel`), `GarnetMetal` (`org.garnet.metal`), `libgarnetxlate.dylib`,
`garnettest`, and `garnet-*` boot arguments. Rebuild the kernel and user-space
components together and update service names, bundle paths and boot arguments before a later
deployment. Numeric ABI values, selectors, structure layouts and serialized signatures stay
unchanged. Actual chip selectors remain `navi44` and `navi48`.

## Full payload (`Garnet`, `GarnetAccel`, `GarnetMetal`)

`tools/rdna4/build-all.sh OUT_DIR` builds one matched, signed standalone Garnet payload for
`tools/rdna4/install-native.sh` (scripts, layout, env vars, and safety notes: see
`tools/rdna4/README.md`). Pass `MKSDK`, `C64_LAYOUT_OUT`, and `C64_WORK` (plus
`RDNA4_RADV_LIB`/`RDNA4_MESA_SRC` and `RDNA4_XLATE_LIB`/`RDNA4_M2V_SRC`); no
`AMDRadeonRDNA4.kext` or `Lilu.kext` input is required or accepted. Before compiling,
`build-all.sh` writes `MANIFEST.txt` (`garnet-delivery-v1`) and `LOCAL-SOURCE-MANIFEST.txt`
covering tracked and untracked local build inputs plus declared ignored inputs
(`src/garnet/firmware/` and `src/garnet-metal/spvcache/`); generated build outputs are
excluded. Each kext is strict-verified, signed ad hoc if not already signed, and strict-verified
again. The script never installs or loads anything on the target system.
