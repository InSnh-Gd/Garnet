# Native memory reports

The memory work in `complete-rdna4-metal2-large-memory` keeps distinct values:

| Value | Meaning |
| --- | --- |
| Physical VRAM | ASIC memory capacity, including fixed reserved ranges |
| Pool capacity | Visible plus high allocator capacity after fixed carve-outs |
| CPU-visible capacity | The visible allocator, already part of pool capacity |
| Non-WS budget cap | Shared allowance after internal/PT and WindowServer reservations |
| Available allowance | Remaining shared allowance, also bounded by real pool free bytes |
| Recommended working set | Performance recommendation, currently at most 12 GiB |
| Allocation budget | Capacity used for allocation admission; independent of that recommendation |
| Buffer length | Implemented logical-resource limit; separate from aggregate capacity |
| Descriptor range | The backend's UBO/SSBO view limit; large logical resources need a separate address path |

Fixed console/firmware carve-outs are excluded by the pools before budgeting.
The budget does not deduct them again. A mapped BAR is not additional VRAM.
An application's memory and any quarantined allocations remain accounted until
the backing is actually safe to free. Growing WindowServer occupancy changes
new admission, not the validity of existing resources.

## Discovery before opening RADV

The native Metal nub publishes an immutable `RDNA4,MemoryCaps` dictionary before
registering its accelerator. Schema version 1 has unsigned integer fields:
`version`, `physical`, `pool`, `visible`, `applicationBudget`, `maxBuffer`.
The optional `compositeMaxBuffer` field is additive within schema 1: older
bundles ignore it and continue using `maxBuffer`.
`applicationBudget` is a static ceiling with the same internal/PT and WS growth
reserve as admission, assuming no current WS use. It is not a free-memory
promise. Live QueryInfo remains authoritative after opening a native session.

Metal reads this dictionary once while initializing the device. Capability
queries do not open RADV or initialize hardware. Unknown versions, missing or
invalid fields, negative/floating values and inconsistent capacities use the
existing conservative fallback. A valid zero application allowance stays zero.
After RADV opens, measured backend limits supersede the static snapshot.
The logical buffer limit is independent of the allocation allowance in both
paths. An exhausted allowance does not change `maxBufferLength`; allocation
admission can still reject a request within that length.

RADV reports disjoint high and visible local heaps. Metal sums those heaps once
for aggregate capacity, excluding the host heap. A single Vulkan allocation
still belongs to one heap. When `VK_EXT_memory_budget` is available, the sum of
local heap budgets bounds Metal's allocation budget; a valid zero stays zero.
RADV shares native available headroom across the local heaps once and preserves
its actual application allocation counters. It does not report `cap - available`
as physical usage. The existing native 64 MiB application-visible sublimit stays
enforced by the kernel; the current ABI does not report a separate visible
allowance or per-session VRAM usage.

The legacy `maxBuffer` remains `0xFFFFFFFC` or the smaller largest local heap.
The current bundle also reads `compositeMaxBuffer`, bounded by that heap and
the implemented 8 GiB logical limit. Missing, malformed or inconsistent optional
values preserve the legacy limit. Backend limits remain authoritative after
opening RADV. These queries do not prove real allocation or GPU execution above
4 GiB; that remains a separate hardware acceptance requirement.

## Current verification boundary

The host devcaps tests verify budget/recommendation separation and malformed
snapshots. The existing-VM device identity test executes the production Metal
initializer/getters with simulated registry properties and checks that no RADV
open occurred. Its 13 GiB heap object is a budget object without GPU backing;
it verifies admission semantics only. It does not prove a large allocation.

Evidence: `logs/rdna4-metal2/20261105-110230/devcaps/`. The legacy
`native_metal_test` has eight unchanged failures when run against both the
inherited snapshot and this candidate (stale selector/build-version pins and
historical relocated tool/doc paths). This comparison does not count those
failures as passes. The new nub and the budget kernel object cross-compile for
x86_64 macOS with the pinned kernel SDK.

Mesa patches 0001 through 0016 replay without fuzz. The pure budget tests pass
331 checks, actual Darwin backend runtime tests pass 353 checks, and the submit
regression passes 72 checks, all with address/undefined-behavior sanitizers.
These use synthetic QueryInfo and simulated submission acceptance. Evidence is
under `logs/rdna4-metal2/20261105-110230/mesa-budget/`; they do not demonstrate
physical VRAM allocation or GPU execution. The additive composite discovery
checks are in `logs/rdna4-metal2/20261106-memory-discovery/`: 64 host capability
checks and the updated production initializer/getter fixture passed, including
all 17 memory snapshots without opening RADV. Current Mesa budget/runtime checks
also pass. The existing VM ran alone with 3 GiB guest RAM and a 3.5 GiB host
cgroup limit; these remain software observations.

## Host-system admission

Large Shared/Managed buffers and NoCopy imports consume system RAM, independently
of the VRAM budget. With native multi-session mode enabled, non-WindowServer
imports share a cap derived from installed host RAM minus a 2 GiB reserve.
Unknown RAM size uses the bounded legacy 6 GiB cap. The old 64 MiB per-import
limit remains; WindowServer and single-client mode retain their legacy import
rules. GTT and imports use the same host-system ledger.

The legacy QueryInfo `gtt_cap`, `gtt_used` and `gtt_max_bo` fields continue to
describe ordinary GTT BOs (512 MiB per session and 64 MiB per BO). They do not
include external host imports. Their numeric ABI and the corresponding RADV
host heap keep that meaning; neither is a report of total pinned system pages.
The shared host-system ledger separately accounts for both classes, including
pending imports and quarantined pins, when admitting new memory.

Admission reserves pending bytes under the shared allocator lock before the
descriptor pins pages. The lock is released during `prepare()`. Successful
publication moves those bytes from pending to held once; failures complete and
release any pinned descriptor before returning the reservation. A session change
or missing reservation fails before mapping. Normal retirement releases held
bytes, while HUNG/quarantine keeps both the pinned pages and charge until reboot.

The reviewed host-budget tests pass 91 system-memory checks, 952 import checks,
8 actual descriptor-prepare failure cases, 301 G2 checks, 295199 G4 checks and
520 existing native checks. They use synthetic physical-memory capacities and
simulated descriptor/transport boundaries. The kernel object also compiles with
the pinned MacKernelSDK. Evidence is under
`logs/rdna4-metal2/20261105-110230/host-budget/review-cleanup/` and the preceding
`review-fix/kernel-compile.json`; cleanup changed comments and removed unused
test-only helpers, without changing the compiled kernel behavior. These results
do not prove a real greater-than-4-GiB pinned allocation or GPU execution.
