# Building the Garnet driver stack

> [!WARNING]
> `Garnet.kext` writes GPU registers directly (PCIe BARs, PSP, SMU, RLC, SDMA, CP, MES, and the DCN display engine). An incompatible build, GPU, or macOS version can freeze or restart the machine or corrupt display output. Run builds only on a dedicated, recoverable test machine at your own risk; see [LICENSE](LICENSE).

## Prerequisites

- **macOS host** with Xcode Command Line Tools (`clang`, `make`, `codesign`, `python3`) for full signed payloads, or **Linux host** with `clang`, `llvm-nm`, `MacKernelSDK`, and a `cctools-port` `ld64` linker implementing `-kext` for kernel-only cross-builds.
- **MacKernelSDK** pinned at `05094e5e88cec7caedbfb35e8449ed0db94bf95b` (not vendored here); pass its path as `MKSDK`. `Garnet.kext` and `GarnetAccel.kext` have no `Lilu.kext` or external display-plugin dependency.
- **AMD firmware** from `linux-firmware` (not tracked in git; distributed under AMD's firmware license):

  ```sh
  git clone https://gitlab.com/kernel-firmware/linux-firmware.git
  CHIPS=navi44 bash tools/fetch-firmware.sh /path/to/linux-firmware
  ```

  `fetch-firmware.sh` only copies local files (no network) into gitignored `src/garnet/firmware/`. Ten blobs are embedded per chip:

  ```text
  Navi 44 (RX 9060 XT; CHIPS=navi44):
  gc_12_0_0_imu.bin  gc_12_0_0_me.bin  gc_12_0_0_mec.bin  gc_12_0_0_pfp.bin
  gc_12_0_0_rlc.bin  gc_12_0_0_uni_mes.bin
  psp_14_0_2_sos.bin  psp_14_0_2_ta.bin
  sdma_7_0_0.bin  smu_14_0_2.bin

  Navi 48 (RX 9070 / RX 9070 XT / AI PRO R9700; CHIPS=navi48):
  gc_12_0_1_imu.bin  gc_12_0_1_me.bin  gc_12_0_1_mec.bin  gc_12_0_1_pfp.bin
  gc_12_0_1_rlc.bin  gc_12_0_1_uni_mes.bin
  psp_14_0_3_sos.bin  psp_14_0_3_ta.bin
  sdma_7_0_1.bin  smu_14_0_3.bin
  ```

  `make` embeds both chips by default (`CHIPS="navi48 navi44"`). Setting `CHIPS=navi44` builds a kext containing only Navi 44 firmware; unsupported chips are stubbed out and refused at probe time. The Makefile stops with a list if any required blob is missing, and `fetch-firmware.sh` verifies SHA-256 digests against the tested revision.

## Build `Garnet.kext` on macOS

```sh
make -C src/garnet CHIPS=navi44 MKSDK=/path/to/MacKernelSDK all
```

The output lands in `src/garnet/build/Garnet.kext` (`x86_64`, ad-hoc signed, macOS 11 minimum ABI). Verify it with:

```sh
dwarfdump --uuid src/garnet/build/Garnet.kext/Contents/MacOS/Garnet
nm -u -arch x86_64 src/garnet/build/Garnet.kext/Contents/MacOS/Garnet
```

Only kernel, IOKit, and KPI symbols should appear as undefined imports.

## Linux kernel-only cross-build

`Garnet.kext` can also be cross-compiled on Linux with `clang`, `MacKernelSDK`, `llvm-nm`, and a `cctools-port` `ld64` supporting `-kext`:

```sh
CHIPS=navi44 bash tools/fetch-firmware.sh /usr/lib/firmware
make -C src/garnet all CHIPS=navi44 BUILD=/tmp/garnet-build \
  MKSDK=/absolute/path/to/MacKernelSDK \
  KEXT_LD=/absolute/path/to/x86_64-apple-darwin-ld
```

Linux defaults to `SIGN=0` (unsigned kext for OpenCore injection or offline inspection); macOS defaults to `SIGN=1` and runs `codesign`. Recent `MacKernelSDK` snapshots provide `libkmod`; `libkmodc++` is linked automatically when present. Userspace components (`GarnetMetal.bundle`, `Garnet.app`, `libvulkan_radeon.dylib`) require a macOS SDK and are not built by this kernel-only target.

Do not substitute `ld64.lld`: LLVM's `ld64.lld` ignores `-kext` and emits an executable instead of a `KEXTBUNDLE`. Verify the output with `file` or `llvm-objdump --macho --private-header`.

## Generated files and build directories

- `src/garnet/firmware/*.bin` is gitignored. Generated firmware C sources and objects land under `$(BUILD)/firmware/` (`FW_GEN := $(BUILD)/firmware`), keeping `src/` clean.
- Override `BUILD=` for candidate or experiment builds instead of reusing `src/garnet/build`.
- Build outputs (`build/`, `build-*/`, `*.kext/`, `*.o`, `*.log`, `runs/`) are gitignored; do not commit generated artifacts.
- `src/garnet-accel/tests/Makefile` honors `BUILD` and `TMPDIR` (defaulting to `${TMPDIR:-/tmp}/garnet-accel-tests`).

## Runtime shader cache and ABI notes

The native stack (Mesa RADV plus `GarnetMetal.bundle` in `src/garnet-metal/`) translates Metal AIR to SPIR-V directly and does not use Apple shader-substitution caches. Compiled compute test shaders remain in `src/garnet/shaders/`.

## Rebuilding matching components together

All layers share the standalone Garnet identity (`Garnet` / `org.garnet.driver`, `GarnetAccel` / `org.garnet.accel`, `GarnetMetal` / `org.garnet.metal`, `libgarnetxlate.dylib`, `garnettest`, and `garnet-*` boot arguments). Rebuild kernel and userspace components together whenever ABI headers or layouts change. Chip selectors remain `navi44` and `navi48`.

## Full matched payload (`Garnet`, `GarnetAccel`, `GarnetMetal`, `GarnetRecover.efi`)

`tools/rdna4/build-all.sh OUT_DIR` builds a single matched, signed standalone Garnet payload for `tools/rdna4/install-native.sh` (see [tools/rdna4/README.md](tools/rdna4/README.md) for full options and safety notes). Pass:

- `MKSDK`: `MacKernelSDK` directory
- `GARNET_KC_WORK`: directory holding `kc/` (`BootKernelExtensions.kc`, `SystemKernelExtensions.kc`) and carved `kexts/` from the target macOS build
- `GARNET_LAYOUT_OUT`: output directory generated by `tools/ioaccel-layout/`
- `RDNA4_RADV_LIB` (or `RDNA4_MESA_SRC`) and `RDNA4_XLATE_LIB` (or `RDNA4_M2V_SRC`)
- Optional `RDNA4_RECOVER_EFI`: prebuilt x86_64 PE32+ `GarnetRecover.efi` (built from `src/garnet-recover-efi/` with `clang -target x86_64-unknown-windows` + `lld-link`)
- Optional `GARNET_SIGN_CA_DIR` (created outside the repo via `tools/rdna4/make-signing-ca.sh`) and `GARNET_AMFIANCHOR_KEXT`: sign userspace Mach-Os with a dedicated CA and bundle codeless Schema-1 `GarnetAnchorInjector.kext` for `AMFIAnchor` (removing the need for `amfi=0x80`; requires `rcodesign`)

Before compiling, `build-all.sh` records `MANIFEST.txt` (`garnet-delivery-v1`) and `LOCAL-SOURCE-MANIFEST.txt` covering tracked and untracked local inputs plus declared ignored inputs (`src/garnet/firmware/` and `src/garnet-metal/spvcache/`). Each kext is strict-verified, ad-hoc signed if needed, and verified again. The script never installs or loads anything on the host.

## Offline host tests

Run the C/C++ and Python offline suites on Linux or macOS without a GPU:

```sh
make -C src/garnet/tests all
make -C src/garnet-metal/tests all
make -C src/garnet-recover-efi test
python3 tools/rdna4/tests/build-identity-test.py -v
```
