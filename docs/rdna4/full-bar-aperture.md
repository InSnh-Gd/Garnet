# RDNA4 Full PCIe BAR0 Aperture & VRAM Ownership

This document specifies the pre-PCI-configuration BAR0 compatibility layer (`GarnetPCIBarCompat.kext`), the single-owner VRAM and aperture boundaries across the Garnet kexts, and the physical verification checklist for RX 9060 XT 8 GB cards.

---

## 1. Pre-PCI BAR0 Compatibility (`GarnetPCIBarCompat.kext`)

### Why an Early Standalone `Root` Kext Is Needed

On macOS 26.x (`25G332` verified baseline), `com.apple.iokit.IOPCIFamily` (`726.100.6`, `LC_UUID 227771C6-ADC2-30C7-8408-263F723CF81C`) clamps every probed PCI BAR in `IOPCIConfigurator::probeBaseAddressRegister` (`hdr+0x3712e`) against `MAX_BAR_SIZE = 1 GiB` (`0x40000000`). A 16 GiB RDNA 4 BAR0 (`0xFFFFFFFC00000000`) is truncated via `mov ecx, eax` to low 32 bits = **`0`**, clearing `clean64` so `safeProbeBaseAddressRegister` frees BAR0 completely while Site 1 decrements `nextBarNum` from `2` to `1` and corrupts BAR1/BAR2 unless preserved at probe time.

`GarnetPCIBarCompat.kext` (`org.garnet.pcibarcompat`) declares `OSBundleRequired = Root`, matches `IOResources` (`IOResourceMatch = IOKit`), and links only `IOPCIFamily` (`1.1`) plus Apple KPIs (`iokit`, `libkern`, `mach`, `unsupported`) with no third-party kernel patcher dependency. Its `kmod_start` initializer executes synchronously in the prelinked boot kernel ~405 ms **before** `[ PCI configuration begin ]`. Late callbacks run after PCI configuration completes and cannot alter BAR range probing.

### Why Runtime Hot BAR Resize/Move Is Not Used

PCI device BAR ranges, upstream PCIe switch windows, and host-bridge PFM64 decode apertures are sized and committed once during `[ PCI configuration begin ]`. Resizing or relocating an active GPU BAR after `IOPCIFamily` enumeration risks MMIO decode conflicts, upstream bridge aperture invalidation, and stale `IOMemoryDescriptor` / DMA mappings. Instead:

1. Motherboard BIOS enables `Above 4G Decoding` and `Re-Size BAR Support` (providing a 64-bit host-bridge PFM64 window).
2. OpenCore passes the firmware-sized BAR through (`ResizeAppleGpuBars = -1`, `ResizeGpuBars = -1`).
3. `GarnetPCIBarCompat.kext` preserves the full 64-bit BAR0 `IOPCIRange` inside `probeBaseAddressRegister` on first enumeration and restores the 17-byte prologue in `_antimain` on unload.

---

## 2. Build Staging & OpenCore Integration

### Opt-In Payload Staging (`tools/rdna4/build-all.sh`)

- `RDNA4_BUILD_PCIBARCOMPAT=0` (default dev builds): `GarnetPCIBarCompat.kext` is not built or staged, and delivery manifests omit `PCIBARCOMPAT_*` fields.
- `RDNA4_BUILD_PCIBARCOMPAT=1` (enabled automatically when `RDNA4_RELEASE=1`): `build-all.sh` builds and signs `src/garnet-pci-compat`, validates it via `build_identity.py pcibarcompat`, stages it at `payload/optional/GarnetPCIBarCompat.kext` (never installed into `/Library/Extensions`), and writes `PCIBARCOMPAT_*` metadata into `MANIFEST.txt`.

### OpenCore `config.plist` Requirements

1. **`Kernel -> Add` Load Order**:
   - Place `GarnetPCIBarCompat.kext` enabled **after** `IOPCIFamily` and **before** `Garnet.kext` and `GarnetAccel.kext` so OpenCore prelinks it into `BootKernelExtensions` and `kmod_start` runs before `[ PCI configuration begin ]`.
2. **`Booter -> Quirks`**:
   - `ResizeAppleGpuBars = -1` (preserve firmware-sized BAR0; do not clamp to 256 MiB / 1 GiB).
   - `ResizeGpuBars = -1`.
   - `IncreasePciBarSize = False`.
3. **`NVRAM -> Add -> 7C436110-AB2A-4BBB-A880-FE41995C9F82 -> boot-args`**:
   - Remove `npci=0x2000`.
   - Add `garnet-full-bar=1` to enable Full-BAR ownership in `Garnet.kext`.

### Independent Disable & Recovery

- **Boot-arg bypass**: Pass `garnet-rebar-compat=0` in `boot-args`. `GarnetPCIBarCompat` exits in `kmod_start` with zero writes (`GarnetPCIBarCompat,Status = disabled-by-boot-arg`).
- **OpenCore toggle**: Set `Enabled = False` on the `Kernel -> Add` entry for `GarnetPCIBarCompat.kext`.
- **Small-BAR recovery**: Set `ResizeAppleGpuBars = 0` / `ResizeGpuBars = 0` and omit `garnet-full-bar=1`. Standard 256 MiB Small-BAR mode in `Garnet.kext` remains unaffected, and any `<= 1 GiB` BAR falls through to unmodified Apple `probeBaseAddressRegister`.

---

## 3. Fail-Closed Validation & Invariants

Every precondition failure leaves `IOPCIFamily` unpatched (zero writes) or tail-calls unmodified Apple `probeBaseAddressRegister` (`passthrough`):

| Gate | Condition Checked | Failure Behavior |
| :--- | :--- | :--- |
| Boot-arg override | `garnet-rebar-compat=0` | `disabled-by-boot-arg`, zero writes |
| Late-load / wrong OC order | `IOPCIDevice` or `IOPCIConfigurator` instance count `> 0` | `refused-late-load` (`LateLoadRefused = true`), zero writes |
| `IOPCIFamily` identity | Backward page scan (`<= 16` pages from `IOPCIBridge::initialize`), `mach_header_64`, `__TEXT,__text`, non-zero `LC_UUID`; `exact-uuid` fast path for `25G332` (`227771C6-ADC2-30C7-8408-263F723CF81C`) or `structural` verifier for compatible `26.x` builds | `header-not-found` / `bad-macho` / `uuid-mismatch` / `struct-layout-mismatch`, zero writes |
| Unique signatures, struct displacements & calls | Unique (`count == 1`) 28 B prologue (`+0x0`), 14 B Site 1 (`+0x2b2`), 17 B Site 2 (`+0x3b7`), intra-function struct-displacement bytes (`0x2E`/`0x32`/`0x40`/`0x48`/`0x54`/`0x60`/`0x90`), plus 5 `e8 rel32` call sites (`exact-uuid`: pinned `25G332` targets; `structural`: 5 distinct in-`__text` helpers with `push rbp` prologue outside probe) | `pattern-*` / `struct-layout-mismatch` / `helper-call-mismatch`, zero writes |
| Device allowlist | Vendor `0x1002`, device ID in `{0x7590 (16 GiB), 0x7550 (16 GiB), 0x7551 (32 GiB)}`, Type-0 endpoint (`lastBarNum >= 5`) | `passthrough` to Apple handler |
| Bridge PFM64 capacity | `kIOPCIConfiguratorPFM64` (`0x2000`) set and root host-bridge PFM64 free/window capacity `>= size64` (lock-free bounded walk `<= 64` nodes) | `RefusedBridgeCount++`, `passthrough` to Apple handler |
| BAR0 geometry & BAR0-only step | 64-bit prefetchable (`(maskLo & 0xF) == 0xC`), power-of-two `size64 = -mask64`, `1 GiB < size64 <= deviceLimit`; advances `barNum` from `0 -> 2` | Invalid mask falls back to `passthrough`; BAR2..5 and Expansion ROM (`0x30`, display class `0x030000`) remain capped at `1 GiB` |

The detour executes under `mp_rendezvous_no_intrs` with interrupts disabled, performing zero locks, zero heap allocations, and zero `IOLog` calls. Read-only `IORegistry` diagnostics are published later in `GarnetPCIBarCompat::start`:
`GarnetPCIBarCompat,Status`, `MatchMode`, `IOPCIFamilyUUID`, `HookInstalled`, `TargetUUIDVerified`, `InterceptCount`, `PreservedBar0Bytes`, `DeviceID`, `PassthroughCount`, `RefusedBridgeCount`, and `LateLoadRefused`.

---

## 4. VRAM & Aperture Ownership Boundaries

### Single Device & VRAM Owner

- **`Garnet.kext` (`org.garnet.driver`)**: Sole hardware owner of the RDNA 4 `IOPCIDevice`, PCI BAR0 aperture, GART/VM page tables, GMC/IH/SDMA/GFX/VCN rings, and display engine. All VRAM partition sizing, aperture clamping, and protected reserves are enforced exclusively inside `Garnet.kext`.
- **`GarnetAccel.kext` (`org.garnet.accel`)**: Thin accelerator client communicating strictly through `rdna4.display.peer.v1`. Holds zero direct BAR mappings, `writeRegister` calls, or raw descriptor ownership over the GPU aperture.
- **`GarnetPCIBarCompat.kext` (`org.garnet.pcibarcompat`)**: Operates strictly at PCI enumeration (`IOPCIFamily` `IOPCIRange` sizing) and owns zero VRAM allocations, MMIO mappings, or runtime GPU state.

### Full-BAR Aperture (8 / 16 GiB) vs. Validated VRAM vs. Reserves

```text
0x0000_0000_0000 ──┬─────────────────────────────────────────────────────────┐
                   │ Boot Console / Early Framebuffer Scanout                │
                   ├─────────────────────────────────────────────────────────┤
                   │ Garnet VRAM Arena (allocatable via rdna4.display.peer)  │
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

- **BAR0 aperture (`8 GiB` on 8 GB / `16 GiB` on 16 GB)**: Full PCIe MMIO decode range exposed by `probeBaseAddressRegister` on RX 9060 XT (`0x7590`) / RX 9070 (`0x7550`), or up to `32 GiB` on `0x7551`.
- **Validated physical VRAM (~`8112 MiB` on RX 9060 XT 8 GB, ~`16304 MiB` on RX 9060 XT 16 GB)**: Firmware-reported/backed VRAM ceiling within BAR0. Addresses above validated VRAM form an unbacked high tail excluded from host and GPU allocation.
- **`vramLimit` & protected regions**: `Garnet.kext` reserves early boot console/framebuffer spans, firmware/PSP/SMU/GART regions, and the high-tail reserves (`kVramHiTotalReserve = 768 MiB`: `512 MiB` high tail + `256 MiB` Apple arena carveout), exposing only the verified allocatable span up to `vramLimit`.

---

## 5. RX 9060 XT 8 GB Physical Verification Checklist

> **Current Status**: RX 9060 XT **16 GB** (`1002:7590`) is physically verified. RX 9060 XT **8 GB** (`1002:7590`, Navi 44 XT, 8 GiB VRAM / 8 GiB Full-BAR) is **code-ready** (host suites pass) and **pending physical hardware verification**.

Before accepting live 8 GB operation on a physical RX 9060 XT 8 GB card:

1. **Kernel VRAM Size & `Garnet,MemoryCaps`**:
   - Confirm `RCC_CONFIG_MEMSIZE` / `gmc.real_vram_size` (~`8112–8192 MiB`) and verify `Garnet,MemoryCaps` publishes `physical >= pool`, `pool = visible + high` (~`7280–7360 MiB`), and zero reserve overlap (`garnettest accel vramstat`).
2. **ReBAR BAR0 & `GarnetPCIBarCompat`**:
   - Verify firmware ReBAR reports `8 GiB` (ReBAR code `13`) and `GarnetPCIBarCompat` reports `PreservedBar0Bytes = 0x200000000` (`8 GiB`) alongside intact BAR2 doorbell probing in Full-BAR (`--mode full`) boots.
   - Verify `--mode small` (`256 MiB` Small-BAR) recovery boot exposes usable `visible` + `high` VRAM without `GarnetPCIBarCompat.kext`.
3. **Direct CPU BAR <-> GPU Transfers**:
   - In 8 GiB Full-BAR mode, run `garnettest accel bartest 0` and verify both targets (`4 GiB - 64 KiB` straddling block and top block below `vramLimit`) pass bidirectional CPU/GPU checks with intact guards and low-address canary.
   - In Small-BAR mode, verify `garnettest accel bartest 1` passes.
4. **Hardware `GB_ADDR_CONFIG` Register Check (Mesa RADV)**:
   - Mesa RADV Darwin enforces `GB_ADDR_CONFIG == 0x08200545` (128-bit / 4-channel Navi 44). If an 8 GB SKU boots with a different `GB_ADDR_CONFIG`, record the readback from `Garnet` / RADV logs and diagnose with `RADV_DARWIN_ALLOW_ADDRCFG=1` before patching Mesa.
5. **GarnetMetal Pre-Open vs. Full-Open Crosscheck**:
   - Confirm on first RADV open that `argument limits: full-open identity and all raw limits MATCH the pre-open snapshot` (aligning `nativeMemory->pool`, ~`7424 MiB`, with zero `CROSSCHECK_LIMIT_MISMATCH`) so `gpuArgumentTables` and Tier 2 argument samplers stay enabled.
