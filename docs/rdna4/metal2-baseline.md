# Metal 2 runtime baseline and fallback contracts

This note records the capability baseline, null-descriptor fallback rules, and
heap-accounting contracts shared across `GarnetMetal.bundle` and the offline
regression suites. For detailed feature-by-feature requirements, see
[metal2-matrix.md](metal2-matrix.md) and [metal3-matrix.md](metal3-matrix.md).

## Capability contract evolution

`GarnetMetal.bundle` advertises `Mac2` (`MTLFeatureSet_macOS_GPUFamily2_v1` /
`MTLGPUFamilyMac2`) and `MTLGPUFamilyMetal3` only after runtime backend probing
verifies the required Vulkan extensions and limits. Prior to opening RADV,
static discovery stays fail-closed:

| Area | Baseline behavior |
| --- | --- |
| Non-WS VRAM cap | Pool capacity minus internal/PT reserves, active WindowServer occupancy, and WS growth reserve |
| Pre-open buffer limit | Read-only negotiated device snapshot (`RDNA4,MemoryCaps`) without opening RADV |
| Multi-gigabyte buffers | 64-bit BDA (`buffer_address_table_version = 2`) with explicit bounded descriptor windows (see [large-buffer-backing.md](large-buffer-backing.md)) |
| Host-system imports | Independent bounded system-RAM ledger (installed RAM minus 2 GiB reserve), preserving 64 MiB per-import NoCopy rules (see [memory-reports.md](memory-reports.md)) |
| Argument buffers | Tier 2 layout/reflection, all-stage dynamic arrays, and GPU-produced resource references |
| Tessellation & ICBs | Hardware triangle/quad post-tessellation pipelines plus CPU and GPU render/compute ICB execution |
| Heaps & sync | Real `VkDeviceMemory`-backed automatic and placement heaps, segment-split events, and checkpoint fences (see [automatic-heaps.md](automatic-heaps.md), [command-events.md](command-events.md)) |
| Metal 3 & hardware RT | Runtime-gated capabilities (mesh/object shaders, ray queries, acceleration structures, IFT/VFT, binary archives, dynamic libraries, residency sets, sparse/PRT) documented in [metal3-matrix.md](metal3-matrix.md) |

## Null-descriptor fallback

Tier 2 argument tables do not require Vulkan `nullDescriptor`. On backends that
report `nullDescriptor=false` (such as MoltenVK/Paravirtual VMs) while still
supporting runtime descriptor arrays, partially bound update-after-bind
descriptors, and nonuniform indexing, binding `VK_NULL_HANDLE` into a
sampled-image slot is invalid.

Garnet's fallback keeps the token tables intact, binds a valid shape-matching
dummy descriptor for missing tokens, and selects an all-zero result in the
shader (guarding texel reads and writes before descriptor loads). This preserves
original sample/gather operands and implicit-LOD control flow without falsely
claiming hardware `nullDescriptor` support.

Two constraints apply to this fallback:

1. Backends that report `quadDivergentImplicitLod=false` still have undefined
   implicit-LOD behavior when a fragment shader's image or sampler selection
   diverges across a quad; the fallback does not alter quad convergence.
2. Every admitted sampled-image dimensionality and view form (1D/2D/3D/cube,
   arrayed, depth, multisampled) requires a matching valid dummy descriptor or
   an explicit shader-side zero path.

## Automatic-heap accounting behavior

Automatic heaps separate placement extent (`MTLSizeAndAlign.size`, including
alignment and `bufferImageGranularity` padding) from charged Vulkan requirement
bytes (`resource.allocatedSize` / `heap.usedSize`). For example, a 1000-byte
buffer can report a 1008-byte 16-aligned placement extent while charging 1000
bytes to `usedSize`. Tests should query `maxAvailableSizeWithAlignment:` rather
than assuming a budget-only contiguous remainder. `A8Unorm` sampled heap
textures share the normal texture path; `A8Unorm` render targets remain
rejected.

## Regression suites

- Offline host C11 (ASan/UBSan), assembled-device source-pin, and Python suites:
  `make -C src/garnet-metal/tests all`
- Tier 2 macOS compatibility and execution harness:
  `python3 src/garnet-metal/tests/run-compatibility.py` (supports `vm` and
  `live` target modes; see [TESTING.md](../../src/garnet-metal/tests/TESTING.md))
