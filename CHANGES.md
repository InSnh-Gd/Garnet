# Garnet changes and snapshot provenance

Current standalone Garnet architecture: `Garnet` 0.0.673 (`src/garnet`, `org.garnet.driver`), `GarnetAccel` (`src/garnet-accel`, `org.garnet.accel`), `GarnetMetal` bundle 43 (`src/garnet-metal/Info.plist`, `org.garnet.metal`), 26 Mesa Darwin patches (`mesa-patches/`, including `VK_EXT_metal_surface`), and 65 `metal2vulkan` patches with `garnetxlate` (`third-party/metal2vulkan-rdna4/`), with `Lilu.kext` and `AMDRadeonRDNA4.kext` dependencies removed and selected non-Lilu display/cursor/WSI/VM/sleep/GL-drawable advancements absorbed from `RX4Darwin/AMDRadeonRDNA4.kext`.
Host suites run without a GPU; physical validation requires running on target hardware.

The baseline entries below preserve upstream Cobalt64's 5 October 2026 release notes as historical pre-Garnet context (when components used `tools/pc/`, `src/rdna4-bringup/`, `tools/native/rdna4metal/`, and `librdna4xlate`). Where upstream validated a stage on its Navi 48 bench, that belongs to upstream; local validation evidence lives under `logs/`.

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
