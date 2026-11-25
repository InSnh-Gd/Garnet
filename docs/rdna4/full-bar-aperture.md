# RDNA4 Full PCIe BAR0 Aperture & VRAM Ownership

This document specifies the pre-PCI-configuration BAR0 compatibility layer (`GarnetPCIBarCompat.kext`, Task 2.2), the single-owner VRAM / aperture boundaries (Task 3.3), and RX 9060 XT 8GB bring-up verification requirements.

---

## 1. Pre-PCI BAR0 Compatibility (`GarnetPCIBarCompat.kext`)

### Why an Early Standalone `Root` Kext Is Needed

On macOS 26.7.2 (`25G332`), `com.apple.iokit.IOPCIFamily` (`726.100.6`, `LC_UUID 227771C6-ADC2-30C7-8408-263F723CF81C`) clamps every probed PCI BAR in `IOPCIConfigurator::probeBaseAddressRegister` (`hdr+0x3712e`) against `MAX_BAR_SIZE = 1 GiB` (`0x40000000`). A 16 GiB RDNA4 BAR0 (`0xFFFFFFFC00000000`) is truncated via `mov ecx, eax` to its low 32 bits = **`0`** (clearing `clean64`, so `safeProbeBaseAddressRegister` frees/erases BAR0 completely while Site 1 decrements `nextBarNum` from `2` to `1` and corrupts BAR1/BAR2) unless preserved at probe time.

`GarnetPCIBarCompat.kext` (`org.garnet.pcibarcompat`) declares `OSBundleRequired = Root`, matches `IOResources` (`IOResourceMatch = IOKit`), and links only `IOPCIFamily` (`1.1`) plus Apple KPIs (`iokit`, `libkern`, `mach`, `unsupported`) — with no third-party kernel patcher dependency. Its `kmod_start` initializer executes synchronously in the prelinked boot kernel ~405 ms **before** `[ PCI configuration begin ]`. Late kernel extension callbacks (such as screen-release callbacks firing ~380 ms **after** PCI configuration completes) run too late to affect BAR range probing.

### Why Runtime Hot BAR Resize/Move Is Not Used

PCI device BAR ranges, upstream PCIe switch windows, and host-bridge PFM64 decode apertures are sized and committed once during `[ PCI configuration begin ]`. Attempting to resize or relocate an active GPU BAR at runtime after `IOPCIFamily` enumeration risks MMIO decode conflicts, invalidating upstream bridge apertures, and breaking already-mapped `IOMemoryDescriptor` / DMA state. Instead:

1. Firmware enables Resizable BAR / Above 4G Decoding (e.g., B650M `_CRS` PFM64 window `[0x4_6000_0000 .. 0xFC_FFFF_FFFF]`, 994.5 GiB).
2. OpenCore passes the full firmware-sized BAR through (`ResizeAppleGpuBars = -1`, `ResizeGpuBars = -1`).
3. `GarnetPCIBarCompat.kext` preserves the full 64-bit BAR0 `IOPCIRange` cleanly inside `probeBaseAddressRegister` on first boot enumeration, and restores the stolen 17-byte prologue in `_antimain` on unload.

---

## 2. Build Staging & OpenCore Integration

### Opt-In Payload Staging (`tools/rdna4/build-all.sh`)

`GarnetPCIBarCompat.kext` is strictly opt-in so standard small-BAR builds and `install-native.sh` remain unchanged:

- `RDNA4_BUILD_PCIBARCOMPAT=0` (default): `GarnetPCIBarCompat.kext` is not built or staged, and delivery manifests omit `PCIBARCOMPAT_*` fields.
- `RDNA4_BUILD_PCIBARCOMPAT=1`: `build-all.sh` builds `src/garnet-pci-compat`, signs the bundle, validates `build_identity.py pcibarcompat` and `check_legacy` cleanly, stages it at `payload/optional/GarnetPCIBarCompat.kext` (never installed automatically into `/Library/Extensions`), emits `PCIBARCOMPAT_*` into `MANIFEST.txt`, and records load-order and disable-flag provenance (`LOAD_ORDER=after-IOPCIFamily-before-PCI-config-and-Garnet-via-OpenCore-Kernel-Add-Root`, `DISABLE_BOOT_ARG=garnet-rebar-compat=0`).

### OpenCore `config.plist` Requirements

1. **`Kernel -> Add` Load Order**:
   - Place `GarnetPCIBarCompat.kext` enabled **after** `IOPCIFamily` (its `OSBundleLibraries` dependency) and **before** `Garnet.kext` and `GarnetAccel.kext` so OpenCore prelinks it into `BootKernelExtensions` and `kmod_start` runs after `IOPCIFamily` loads and before `[ PCI configuration begin ]`.
2. **`Booter -> Quirks`**:
   - `ResizeAppleGpuBars = -1` (preserve firmware-sized BAR0; do not downsize to 256 MiB / 1 GiB).
   - `ResizeGpuBars = -1`.
   - `IncreasePciBarSize = False`.
3. **`NVRAM -> Add -> 7C436110-AB2A-4BBB-A880-FE41995C9F82 -> boot-args`**:
   - Remove `npci=0x2000`.
   - Add `garnet-full-bar=1` to enable full-BAR ownership in `Garnet.kext`.

### Independent Disable & Recovery

- **Boot-arg bypass**: Pass `garnet-rebar-compat=0` in `boot-args`. `GarnetPCIBarCompat` stops in `kmod_start` with zero memory writes (`GarnetPCIBarCompat,Status = disabled-by-boot-arg`).
- **OpenCore toggle**: Set `Enabled = False` on the `Kernel -> Add` entry for `GarnetPCIBarCompat.kext`.
- **Small-BAR recovery**: Boot with `config-small-bar-recovery.plist` (or `ResizeAppleGpuBars = 0` / `ResizeGpuBars = 0` and omit `garnet-full-bar=1`). Standard 256 MiB small-BAR behavior in `Garnet.kext` remains unchanged, and any `<= 1 GiB` BAR passthrough calls unmodified Apple `probeBaseAddressRegister`.

---

## 3. Fail-Closed Validation & Invariants

Every precondition failure leaves `IOPCIFamily` unpatched (zero writes) or tail-calls unmodified Apple `probeBaseAddressRegister` (`passthrough`):

| Gate | Condition Checked | Failure Behavior |
| :--- | :--- | :--- |
| Boot-arg override | `garnet-rebar-compat=0` | `disabled-by-boot-arg`, zero writes |
| Late-load / wrong OC order | `IOPCIDevice` or `IOPCIConfigurator` instance count `> 0` | `refused-late-load` (`LateLoadRefused = true`), zero writes, never forges late descriptors |
| `IOPCIFamily` identity | Backward page scan (`<= 16` pages from `IOPCIBridge::initialize`), `mach_header_64`, `__TEXT,__text`, non-zero `LC_UUID`; `exact-uuid` fast path for `25G332` (`227771C6-ADC2-30C7-8408-263F723CF81C`) or `structural` verifier for compatible `26.x` builds | `header-not-found` / `bad-macho` / `uuid-mismatch` / `struct-layout-mismatch`, zero writes |
| Unique signatures, struct displacements & calls | Unique (`count == 1`) 28 B prologue (`+0x0`), 14 B Site 1 (`+0x2b2`), 17 B Site 2 (`+0x3b7`), intra-function struct-displacement bytes (`0x2E`/`0x32`/`0x40`/`0x48`/`0x54`/`0x60`/`0x90`), plus 5 `e8 rel32` call sites (`exact-uuid`: pinned `25G332` targets; `structural`: 5 distinct in-`__text` helpers with `push rbp` prologue outside probe) | `pattern-*` / `struct-layout-mismatch` / `helper-call-mismatch`, zero writes |
| Device allowlist | Vendor `0x1002`, device ID in `{0x7590 (16 GiB), 0x7550 (16 GiB), 0x7551 (32 GiB)}`, Type-0 endpoint (`lastBarNum >= 5`) | `passthrough` to Apple handler |
| Bridge PFM64 capacity | `kIOPCIConfiguratorPFM64` (`0x2000`) set and root host-bridge PFM64 free/window capacity `>= size64` (lock-free bounded walk `<= 64` nodes) | `RefusedBridgeCount++`, `passthrough` to Apple handler |
| BAR0 geometry & BAR0-only step | 64-bit prefetchable (`(maskLo & 0xF) == 0xC`), power-of-two `size64 = -mask64`, `1 GiB < size64 <= deviceLimit`; advances `barNum` from `0 -> 2` | Invalid mask falls back to `passthrough`; BAR2..5 and Expansion ROM (`0x30`, display class `0x030000`) remain capped at `1 GiB` |

The detour handler executes under `mp_rendezvous_no_intrs` with interrupts disabled, so the hook path performs zero locks, zero heap allocations, and zero `IOLog` calls. Read-only `IORegistry` diagnostics are published later in `GarnetPCIBarCompat::start`:
`GarnetPCIBarCompat,Status`, `MatchMode`, `IOPCIFamilyUUID`, `HookInstalled`, `TargetUUIDVerified`, `InterceptCount`, `PreservedBar0Bytes`, `DeviceID`, `PassthroughCount`, `RefusedBridgeCount`, and `LateLoadRefused`.

---

## 4. VRAM & Aperture Ownership Boundaries (Task 3.3)

### Single Device & VRAM Owner

- **`Garnet.kext` (`org.garnet.driver`)**: Sole hardware owner of the RDNA4 `IOPCIDevice`, PCI BAR0 aperture, GART/VM page tables, GMC/IH/SDMA/GFX/VCN rings, and display engine. All VRAM partition sizing, aperture clamping, and protected reserves are enforced exclusively inside `Garnet.kext`.
- **`GarnetAccel.kext` (`org.garnet.accel`)**: Thin client communicating strictly through `rdna4.display.peer.v1`. Holds zero direct BAR mappings, `writeRegister` calls, or raw descriptor ownership over the GPU aperture.
- **`GarnetPCIBarCompat.kext` (`org.garnet.pcibarcompat`)**: Operates strictly at PCI enumeration (`IOPCIFamily` `IOPCIRange` sizing) and owns zero VRAM allocations, MMIO mappings, or runtime GPU state.

### Full-BAR Aperture (8 / 16 GiB) vs. Validated VRAM vs. Reserves

```text
0x0000_0000_0000 ──┬─────────────────────────────────────────────────────────┐
                   │ Boot Console / Early Framebuffer Scanout                │
                   ├─────────────────────────────────────────────────────────┤
                   │ Garnet VRAM Arena (allocatable via rdna4.display.peer) │
                   │   - GART / VM / Fence / Firmware Low Reserves           │
                   │   - Compute / Metal / RADV Buffer & Surface Pools       │
                   ├─────────────────────────────────────────────────────────┤
     vramLimit ────┤ Protected High-Tail Reserves (amdgpu_gmc.h)             │
                   │   - kVramHiTailReserve       = 512 MiB                  │
                   │   - kAppleArenaCarveReserve  = 256 MiB                  │
                   │   - kVramHiTotalReserve      = 768 MiB                  │
~8112 / 16304 MiB ─┼─────────────────────────────────────────────────────────┤
                   │ Unbacked High Aperture Tail (clamped / excluded)        │
8 / 16 GiB (End) ──┴─────────────────────────────────────────────────────────┘
```

- **BAR0 aperture (`8 GiB` on 8GB / `16 GiB` on 16GB)**: Full PCIe MMIO decode range exposed by `probeBaseAddressRegister` on RX 9060 XT (`0x7590`) / RX 9070 (`0x7550`), or up to `32 GiB` on `0x7551`.
- **Validated physical VRAM (`~8112 MiB` on RX 9060 XT 8GB, `~16304 MiB` on RX 9060 XT 16GB)**: Firmware-reported/backed VRAM ceiling within BAR0. Addresses above validated VRAM constitute an unbacked high tail and must be excluded from host/GPU allocation.
- **`vramLimit` & protected regions**: `Garnet.kext` preserves early boot console/framebuffer regions, firmware/PSP/SMU/GART reserves, and the high-tail reserves (`kVramHiTotalReserve = 768 MiB`: `512 MiB` high tail + `256 MiB` Apple arena carveout), exposing only the verified allocatable span up to `vramLimit` to `GarnetAccel.kext` and userland clients.

---

## 5. RX 9060 XT 8GB Physical Verification & Diagnostic Checklist

> **Current Status**: RX 9060 XT **16GB** (`1002:7590`) is physically verified. RX 9060 XT **8GB** (`1002:7590`, Navi 44 XT, 8 GiB VRAM / 8 GiB Full-BAR) is **code-ready** (verified via offline/isolated host suites and compilation) and **pending physical hardware verification** — no live 8GB execution is claimed until this checklist completes on real hardware.

Before accepting live 8GB operation on a physical RX 9060 XT 8GB card:

1. **Kernel VRAM Size & `Garnet,MemoryCaps`**:
   - Confirm `RCC_CONFIG_MEMSIZE` / `gmc.real_vram_size` (`~8112–8192 MiB`) and verify `Garnet,MemoryCaps` publishes `physical >= pool`, `pool = visible + high` (~`7280–7360 MiB`), and zero reserve overlap (`garnettest accel vramstat`).
2. **ReBAR BAR0 & `GarnetPCIBarCompat`**:
   - Verify firmware ReBAR census reports `8 GiB` (ReBAR code `13`) and `GarnetPCIBarCompat` reports `PreservedBar0Bytes = 0x200000000` (`8 GiB`) alongside intact BAR2 doorbell probing in Full-BAR (`--mode full`) boots.
   - Verify `--mode small` (`256 MiB` Small-BAR) recovery boot still exposes usable `visible` + `high` VRAM without `GarnetPCIBarCompat.kext`.
3. **Direct CPU BAR <-> GPU Transfers**:
   - In 8 GiB Full-BAR mode, run `garnettest accel bartest 0` and verify all **2 targets** (`4 GiB - 64 KiB` straddling block and top block below `vramLimit`) pass bidirectional CPU/GPU checks with intact guards and low-address canary.
   - In Small-BAR mode, verify `garnettest accel bartest 1` passes.
4. **Hardware `GB_ADDR_CONFIG` Register Triage (Mesa RADV)**:
   - Mesa RADV Darwin enforces `GB_ADDR_CONFIG == 0x08200545` (matching 128-bit / 4-channel Navi 44). If an 8GB board SKU boots with a different `GB_ADDR_CONFIG` value, record the exact register readback from `Garnet` / RADV logs and run initial diagnostics with `RADV_DARWIN_ALLOW_ADDRCFG=1` before making any Mesa patch decision.
5. **GarnetMetal Pre-Open vs. Full-Open Crosscheck**:
   - Inspect unified/Metal logs on first RADV open and confirm `argument limits: full-open identity and all raw limits MATCH the pre-open snapshot` (aligning `nativeMemory->pool`, ~`7424 MiB`, with zero `CROSSCHECK_LIMIT_MISMATCH`) so `gpuArgumentTables` and Tier 2 argument samplers stay enabled.
