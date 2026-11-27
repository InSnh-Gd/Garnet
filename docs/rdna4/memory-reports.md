# Native memory reports

Garnet separates physical memory, allocator pools, admission budgets, and
logical resource limits into distinct reported values:

| Value | Meaning |
| --- | --- |
| Physical VRAM | ASIC memory capacity, including fixed reserved ranges |
| Pool capacity | Visible plus high allocator capacity after fixed carve-outs |
| CPU-visible capacity | Visible allocator span (already counted inside pool capacity) |
| Non-WS budget cap | Shared allowance after internal/PT and WindowServer reservations |
| Available allowance | Remaining shared allowance, bounded by actual free pool bytes |
| Recommended working set | Performance recommendation (currently capped at 12 GiB) |
| Allocation budget | Capacity used for allocation admission; independent of recommended working set |
| Buffer length | Implemented logical-resource limit; separate from aggregate capacity |
| Descriptor range | Backend UBO/SSBO view limit; multi-gigabyte resources use 64-bit BDA |

Fixed console and firmware carve-outs are deducted before pool sizing and are
not subtracted again during budgeting. A mapped PCIe BAR is never counted as
extra VRAM. Active application memory and quarantined allocations remain
accounted until their backing is safe to free. Growing WindowServer occupancy
affects new admission, not the validity of existing resources.

## Discovery before opening RADV

`GarnetMetalNub` publishes an immutable `RDNA4,MemoryCaps` dictionary (and
`Garnet,MemoryCaps`) before registering its accelerator. Schema version 1 holds
unsigned integer fields: `version`, `physical`, `pool`, `visible`,
`applicationBudget`, `maxBuffer`, and additive `compositeMaxBuffer` (ignored by
older bundles). `applicationBudget` is a static upper bound reflecting the same
internal/PT and WindowServer reserves as admission assuming zero current
WindowServer use — it is not a free-memory guarantee. Live `QueryInfo` becomes
authoritative once a native session opens.

`GarnetMetal` reads this dictionary once during device initialization so
capability queries never open RADV or touch GPU hardware. Unknown versions,
missing/invalid fields, negative/floating values, or inconsistent capacities
fall back to conservative defaults; a valid zero allowance stays zero. After
RADV opens, measured backend limits supersede the static snapshot. Logical
buffer length stays independent of current free allowance: exhausting budget
does not shrink `maxBufferLength`, though admission can still refuse new
allocations.

RADV exposes disjoint high and CPU-visible device-local heaps. `GarnetMetal`
sums those local heaps once for aggregate capacity (excluding the host heap),
while each Vulkan allocation still belongs to one heap. When
`VK_EXT_memory_budget` is available, the sum of local heap budgets bounds Metal
admission (preserving a valid zero). RADV distributes native available headroom
once across local heaps and keeps actual application allocation counters rather
than synthesizing usage as `cap - available`. The kernel's 64 MiB
application-visible sublimit remains enforced in the kernel.

Legacy `maxBuffer` stays `0xFFFFFFFC` (or the largest local heap if smaller).
Current bundles read `compositeMaxBuffer`, bounded by that heap and the 8 GiB
logical buffer limit.

## Host-system admission

Large Shared/Managed buffers and NoCopy imports consume system RAM independently
of the VRAM budget. Under native multi-session mode, non-WindowServer imports
share a cap equal to installed host RAM minus a 2 GiB reserve (falling back to
a 6 GiB cap if host RAM size is unavailable). The 64 MiB per-import limit
remains; WindowServer and single-client mode keep their existing rules. Ordinary
GTT BOs and host imports share the same host-system ledger.

`QueryInfo`'s `gtt_cap`, `gtt_used`, and `gtt_max_bo` fields describe ordinary
GTT BOs (512 MiB per session, 64 MiB per BO) and do not include external host
imports. The shared host-system ledger accounts for both classes — including
pending imports and quarantined pins — when admitting new allocations.

Admission reserves pending bytes under the shared allocator lock before
`prepare()` pins pages, releases the lock during `prepare()`, and promotes
pending bytes to held once publication succeeds. Failed preparations release
pinned descriptors before returning the reservation. Normal retirement frees
held bytes, while `HUNG`/quarantine retains both pinned pages and accounting
until reboot.

## Verification

- **Kernel memory & host-budget tests**: `make -C src/garnet/tests all` (covers
  `native_g2_test`, `native_g4_test`, `native_hostimport_test`, and
  `native_sysmem_budget_test` under ASan/UBSan).
- **Mesa RADV budget/runtime tests**:
  `bash mesa-patches/tests/test-gfx1200-runtime.sh "$PATCHED_MESA"` (covers
  memory queries, shared headroom distribution, and live pool/budget checks).
- **Metal device-caps & identity fixtures**: `make -C src/garnet-metal/tests host`
  (`test-host-runtime.c`) and macOS `run-metal-test.sh --iokit test-device-identity "$OUT"`
  verifying static `MemoryCaps` parsing without opening RADV.
