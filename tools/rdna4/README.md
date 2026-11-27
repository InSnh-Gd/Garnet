# tools/rdna4

Native payload build, verification, installation, and recovery scripts for
x86_64 macOS 26.x (`25G332` verified baseline). Every name, flag, and path
below matches the scripts directly.

## Scripts

- `deploy-common.sh`: sourced by both installers (bash 3.2). Defines standalone Garnet bundle identities
  (`GARNET_BUNDLE=Garnet.kext` / `org.garnet.driver`, `ACCEL_BUNDLE=GarnetAccel.kext` /
  `org.garnet.accel`, `METAL_BUNDLE=GarnetMetal.bundle` / `org.garnet.metal`),
  `DELIVERY_MANIFEST_FORMAT=garnet-delivery-v1`, `RDNA4_ARGS_BASE` (`garnet=1 garnet-interrupts=1`),
  `RDNA4_ARGS_NATIVE` (`garnet-stage=17 garnet-native=1 garnet-metal=1 garnet-metal-disp=1
  garnet-metal-ws=1 garnet-multisession=1 garnet-apps=1 amfi=0x80`), `RDNA4_ARGS_VM`
  (`garnet-stage=0`), `RDNA4_ARGS_CONTROLLED`; `strip_args` / `profile_args` / `migrate_args` /
  `legacy_args`; `LEGACY_DAEMONS` (`com.navi44.native com.navi44.translate com.navi48.translate
  com.navi44.desktop-test com.rdna4.translate com.rdna4.native com.rdna4.native-translate`);
  `legacy_moves` / `legacy_deletes` / `remove_legacy`; `AUX_OURS` (`org.garnet.accel` plus retired
  managed IDs `com.rdna4.accelprobe com.rdna4.bringup com.hackintosh.RDNA4FB
  com.hackintosh.AMDRadeonRDNA4 com.navi48.accelprobe org.garnet.driver`) and `rebuild_aux`.
  `AUX_OURS` names managed components to exclude when preserving foreign Aux KC extensions. The
  current delivery installs both `Garnet.kext` and `GarnetAccel.kext` into
  `/Library/Extensions` and adds both to the Aux KC (no `Lilu.kext` or `AMDRadeonRDNA4.kext`
  dependency). `Garnet.kext` inherits `IOFramebuffer` (`IOGraphicsFamily`), which exists only in
  `SystemKernelExtensions.kc` on macOS 26.x and cannot be prelinked into OpenCore's `BootKC`.
  Retired pre-Garnet `RDNA4*`/`Navi48*`/`AMDRadeonRDNA4`
  artefacts are backup-and-remove only after bundle identity verification. Never run directly.
- `build-radv.sh MESA_SRC OUT_DIR`: builds x86_64 `libvulkan_radeon.dylib` from Mesa
  `f5cb8ee` plus `mesa-patches/*.patch` (fails if a patch does not apply or legacy strings leak).
- `build-xlate.sh M2V_SRC OUT_DIR`: builds x86_64 `libgarnetxlate.dylib` from the
  `metal2vulkan` fork at `43c46ac` plus `third-party/metal2vulkan-rdna4/patches/*`.
- `build_identity.py`: verifies standalone Garnet/GarnetAccel bundle identities (including
  rejecting any `Lilu` dependency or legacy token), deterministic local tracked-plus-untracked
  source snapshots, and the `garnet-delivery-v1` manifest contract. `build-all.sh` invokes it;
  it uses only Python's standard library and local Git.
- `build-all.sh OUT_DIR`: builds one matched, signed standalone Garnet payload (`$PWD/payload`,
  outside the repo). Needs `MKSDK`, `GARNET_LAYOUT_OUT`, `GARNET_KC_WORK` dirs; `CHIPS`
  (default `navi44`, only `navi44`/`navi48`; `RDNA4_RELEASE=1` enforces `navi44` and implies
  `RDNA4_BUILD_PCIBARCOMPAT=1`); `RDNA4_RADV_LIB` or `RDNA4_MESA_SRC`;
  `RDNA4_XLATE_LIB` or `RDNA4_M2V_SRC`; `RDNA4_VK_INC` (default `-I$RDNA4_MESA_SRC/include`);
  `RDNA4_RELEASE` (`0` default dev layout, `1` Navi 44 release payload layout requiring
  `src/garnet/firmware/LICENSE.amdgpu` or `RDNA4_AMD_FIRMWARE_LICENSE`).
  `GARNET_KC_WORK` is the directory holding `kc/` and `kexts/` carved from the target OS, used by the
  accel trampoline generator. Verifies bundle ids, rejects `Lilu` dependencies and legacy tokens
  (`Navi48Bringup|Navi48Metal|RDNA4Bringup|RDNA4Accel|RDNA4Metal|AMDRadeonRDNA4|com.navi48|n48m`),
  and writes `MANIFEST.txt` plus `SHA256SUMS` starting with the `garnet-delivery-v1` installer
  contract plus machine-readable input/output identity keys (`SOURCE_IDENTITY_SHA256`,
  `INPUT_IDENTITIES_SHA256`, `REPOSITORY_DIFF_SHA256` / `REPOSITORY_STATUS_SHA256`,
  `OUTPUT_Garnet_*`, `OUTPUT_GarnetAccel_*`, `OUTPUT_METAL_*`, `OUTPUT_TOOL_*`) and local-source
  snapshots (`LOCAL-SOURCE-MANIFEST.txt`). A non-empty source `spvcache` with paired `.spv`/`.meta.json`
  files is mandatory, and the extracted bundle must contain the same counts. Never installs or
  loads anything.
- `build-native-tools.sh OUT_DIR`: builds the signed x86_64 userspace tools
  (`native-translate`, `native-bundle-check`, `publish-metal`, `mtlprobe-direct`,
  `garnettest`) without writing into the repo.
- `install-native.sh [--check] [--controlled|--vm-load-only] ESP_MOUNT`: installs a standalone
  `build-all.sh` payload. `--check` prints `PREFLIGHT PASS`, `BOOT_ARGS`,
  `LEGACY_BOOT_ARGS_DROPPED` and `LEGACY_REMOVE` lines and changes nothing.
  Both `Garnet.kext` (`org.garnet.driver`) and `GarnetAccel.kext` (`org.garnet.accel`) install into
  `/Library/Extensions` and join the Aux KC without requiring `Lilu.kext`, while any retired
  ESP copy of `Garnet.kext` is removed from `EFI/OC/Kexts/` and `Kernel.Add`. Foreign Aux KC
  extensions are preserved. The installer copies `/Library/GPUBundles/GarnetMetal.bundle` and
  `garnettest`/tools,
  refuses to overwrite mismatched same-name bundles, backs up and retires legacy
  `RDNA4*`/`AMDRadeonRDNA4`/`Navi48*` components after verifying their identities, sets
  `boot-args` and `csr-active-config` (`AwgAAA==` = 0x803), and keeps both in `NVRAM.Delete`.
  The default `native` profile installs `com.garnet.native` and `com.garnet.native-translate`;
  `--controlled` omits/removes those boot jobs and uses `RDNA4_ARGS_CONTROLLED`; `--vm-load-only`
  is preflight-only in a VM (refuses bare metal).
- `uninstall-native.sh [--check] [--legacy-only] ESP_MOUNT`: removes the install
  (everything into `/Library/Application Support/Garnet/backups/uninstall.*`; removes any
  `/Library/Extensions/Garnet*.kext` and residual ESP `Garnet.kext`). `--legacy-only` removes
  only retired artefacts (`Navi48*`/`RDNA4*`/`AMDRadeonRDNA4`) and migrates legacy boot-args to
  `garnet-*`, preserving standalone Garnet.
- `activate-native.sh`: runs at boot from `/Library/Application Support/Garnet/native/`.
  Requires `org.garnet.driver` to be boot-loaded, rejects legacy conflicting kexts, and late-loads
  `org.garnet.accel`. It checks `garnet-native/metal/metal-disp/metal-ws/apps/multisession` flags,
  the translator, `GarnetMetal.bundle`, compute/quad/sysdraw probes and `garnettest` steps
  (`native` and `vm-load-only` profiles only).

## Payload layout (`build-all.sh` output)

`Garnet.kext`, `GarnetAccel.kext`, `GarnetMetal.bundle.tar`,
`native-translate`, `native-bundle-check`, `publish-metal`, `mtlprobe-direct`,
`garnettest`, `install-native.sh`, `uninstall-native.sh`, `deploy-common.sh`,
`activate-native.sh`, `collect-installed.sh`,
`com.garnet.native.plist`, `com.garnet.native-translate.plist`,
`LOCAL-SOURCE-MANIFEST.txt`, `MANIFEST.txt`, `SHA256SUMS`.

The Metal bundle carries `libvulkan_radeon.dylib` and `libgarnetxlate.dylib`; the default dev build
(`RDNA4_RELEASE=0`) does not supply a Vulkan loader, `vulkaninfo`, or an ICD JSON manifest.

### Release payload layout (`RDNA4_RELEASE=1 build-all.sh OUT_DIR`)

Adds release provenance and layout selection assets while placing `GarnetAccel.kext` under a single
variant path (`SHA256SUMS` covers 100% of payload files and `build_identity.py verify-release-payload`
enforces completeness):

- `Garnet.kext` (`CHIPS=navi44` only, embedded Navi 44 firmware) + `LICENSE.amdgpu`
- `variants/index.json` (`garnet-accel-variants-v1`), top-level `layout-fingerprint.json`, and
  `variants/<shortId>/{GarnetAccel.kext,layout-fingerprint.json}`
- `optional/GarnetPCIBarCompat.kext` (users install into OpenCore `EFI/OC/Kexts/` for Full-BAR (8/16 GiB))
- `optional/vulkan/radeon_icd.x86_64.json` and `optional/vulkan/radv-app.sh` (optional, default OFF)
- `default-app-denylist.txt` (template initialized to `/Library/Application Support/Garnet/native/app-denylist.txt`)
- `MANIFEST.txt` includes `RELEASE_MODE=1`, `TARGET_SILICON=navi44`, `TARGET_PCI_IDS=1002:7590`,
  `DEFAULT_LAYOUT_ID=<sha256>`, and `AMD_FIRMWARE_LICENSE=LICENSE.amdgpu`.

## Build identity tests

`python3 tools/rdna4/tests/build-identity-test.py -v` uses local Git, plist and
object fixtures to verify standalone `Garnet`/`GarnetAccel` identity rules, `Lilu`-dependency
and legacy-token rejection, the `garnet-delivery-v1` manifest contract, and local-source
detection of dirty/untracked inputs while ignoring build outputs. The tests do not build or load a
kext.

## Safety

- Installers need root, a supported x86_64 macOS 26.x build, and a verified payload
  (`shasum -a 256 -c SHA256SUMS`); they touch the ESP `config.plist` only after
  copying `config-before.plist` into the backup, and roll the EFI, Aux KC and files
  back on failure.
- Never run in a VM except `--check` / `--vm-load-only` (`--vm-load-only` refuses
  bare metal). Reboot afterwards; a kext is never loaded by the installer.
