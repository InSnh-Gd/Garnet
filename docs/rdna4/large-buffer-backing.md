# Large buffer backing

The native client uses its existing 48-bit GPU address space. Numeric selectors,
wire layouts and the 4095 usable handles / 4096 map records remain unchanged.
This work does not resize BAR0.

## Page tables

One client still has a 32 MiB page-table limit. The global physical reserve is
bounded by the smaller of 64 MiB and one quarter of the visible VRAM pool, and
each new 16 KiB block also requires actual visible free space. Idle reserve
blocks remain physically allocated and counted; reusing one consumes no new
visible bytes. Neither normal retirement nor allocation rollback returns these
blocks to the user VRAM allocator.

An 8 GiB mapping with 4 KiB pages needs about 16 MiB of leaf tables plus directory
pages. Thirty-two 256 MiB Private children, or 128 host children of 64 MiB, fit
the existing handle and map counts. Other resources and fragmentation still
consume the same limits, so this accounting does not guarantee an allocation.

MAP validates the full physical range before writing leaves. A failed allocation
clears its attempted leaves, disconnects newly created directories, scrubs new
pages in the old partial block and restores the session cursor. Existing mappings
outside the attempted range remain intact. Public high-half VAs are canonical;
the map ledger retains its existing stripped representation.

The kernel flushes HDP and the session VMID while holding the shared allocator
lock before a rolled-back block can be reused. If the invalidate times out, the
blocks remain counted and are removed from the reusable reserve; the existing
HUNG handling prevents their physical backing from being released.

Run the targeted host checks from the repository root, with `TMPDIR` pointing
inside the workspace scratch directory:

```sh
make -C src/rdna4-bringup/tests large-pt BUILD="$TMPDIR/large-pt"
```

The production allocator test extracts `PtSnap` and `PtMem` from the actual
kernel source at build time. Only its external BAR and VRAM allocator boundaries
are stubbed. The generated include and binaries stay in the selected build
directory.

Reviewed evidence: `logs/rdna4-metal2/20261105-110230/large-pt/`.
`reviewed-regressions-v2.log` records 126 walker/transaction checks, 520 existing
native checks, 947 host-import checks and 53 actual allocator checks, all passing
with ASan/UBSan. `kernel-reviewed-v2.json` records the successful x86_64 macOS
kernel object compilation. These checks prove mapping and rollback behavior with
synthetic physical addresses; actual 8 GiB backing and target GPU execution
remain in the final hardware acceptance group.

## Private composite allocations

Mesa patch `0017-radv-darwin-composite-private-backing.patch` extends the fixed
`f5cb8ee032adabef599ae892ec4e42d796b84da9` tree after patches 0001 through 0016.
Native Private allocations above `0xfffffffc` bytes reserve one continuous GPU
VA and map physical children directly into it. Children do not reserve duplicate
VA ranges or duplicate mappings. Each chunk starts at no more than 256 MiB;
physical-allocation `ENOMEM` can reduce that size down to the alignment/page
boundary, with at most 2048 children. Export and MAP failures abort creation
without being retried as fragmentation.

The parent is fully backed and does not carry the sparse/virtual BO flag.
Creating it uses no PRT, CLEAR or REPLACE operation. Submission references expand
to all physical child handles, metadata writes fan out to the children, and
ordinary allocation counters count the child backing once. Private CPU mapping,
external export, replay addresses and sparse rebinding are rejected. Existing
small allocations keep their original path.

Teardown unmaps any optional read-only padding page and each exact child range,
destroys child handles in reverse order, then releases the single VA reservation.
UNMAP/free errors are logged and passed through the existing native free path;
kernel resources that cannot safely retire remain subject to native quarantine
and its budget accounting.

```sh
python3 mesa-patches/test-composite-bo.py "$PATCHED_MESA" "$TMPDIR/composite-test"
```

The test extracts and compiles the actual creation, destruction, submission and
metadata functions. Its DRM transport boundaries are stubs. The 351 passing
ASan/UBSan checks cover small chunks, synthetic 8 GiB layout, fragmentation,
allocation/export/MAP/padding failures, child quotas, readonly flags, submission
list growth failure, metadata, export/map rejection and error reporting. This is
not evidence of actual 8 GiB physical backing.

Evidence: `logs/rdna4-metal2/20261105-110230/composite/`. All 17 patches replay
with zero fuzz and produce the same tested BO/CS source; the changed BO and CS
objects also compile against the actual generated Mesa headers in the existing
macOS VM. Overall Metal `maxBufferLength` stays at its old limit until the
addressing, storage-mode and transfer work is ready.

## Shared and Managed host backing

Metal allocations above 64 MiB in Shared or Managed storage reserve one aligned,
anonymous CPU range and import it through `VK_EXT_external_memory_host`. Fresh
anonymous pages start at zero. Both modes use coherent host memory; the CPU and
GPU refer to the same pages, so `didModifyRange:` requires no copy. Creation fails
if the backend has no coherent host-import memory type. The owned range bypasses
the ordinary GTT BO path and its 64 MiB single-BO limit.

Patch 0018 maps that range with at most 64 MiB per native import. Its fully
backed parent keeps the caller's pointer, owns one GPU VA reservation, and expands
submission references to all child handles. It never unmaps the borrowed CPU
range. Allocation, export or MAP failure retires the successful children and the
single reservation; imports have no fragmentation retry.

Metal releases its owned CPU range after destroying the Vulkan buffer and
freeing the imported memory. A quarantined kernel descriptor keeps its UPL and
physical pages pinned even after the user mapping disappears. This preserves
the existing fault-containment rule. NoCopy retains its separate public limit of
64 MiB per aligned import: it borrows the original pointer without copying and
calls the original deallocator once after successful ownership transfer. Failed
creation does not call that deallocator.

Device-owned imported backing is included in `currentAllocatedSize`, using its
aligned allocation span. Import-ledger accounting remains separate. Heap
resources bind the heap's own backing and do not add a second device charge;
borrowed NoCopy memory retains its existing import-ledger accounting. See
`automatic-heaps.md` for the reserved-capacity and physical-backing distinction.
Allocation/bind/address failures reverse the same charges before returning nil.

The current software checks exercise actual production Metal initializers with
simulated Vulkan boundaries, including every creation failure and callback
ownership. The 8 GiB case reserves CPU VA and touches only three selected pages;
it is not a large pinned allocation or GPU execution result. The actual Mesa
host-composite functions pass 2969 checks with ASan/UBSan, including small real
host backing, synthetic 8 GiB layout, exact segment unmaps and failure rollback.
Private composite regression remains 351 checks. All 19 patches replay with
zero fuzz, and the final BO/CS sources compile in the existing macOS VM.

Evidence: `logs/rdna4-metal2/20261105-110230/large-api/`, especially
`replay-19.json`, `replayed-host-composite.log`, `replayed-private-composite.log`,
`vm-replayed-bo-metal.log` and `vm-host-memory-metadata.log`.

The subsequent `vm-host-memory-accounting.log` checks the actual owned-import
initializer, failure cleanup and heap-adoption accounting with simulated Vulkan.
`vm-memory-shared-managed.log` and its artifacts record three VM GPU cycles for
each of Shared and Managed: a 78,774,276-byte buffer crosses the 64 MiB backing
boundary, exchanges CPU/GPU data, verifies every payload word through bounded
staging copies, and preserves both guards. The 78,778,368-byte aligned span is
reported while live and returns to zero after each cycle. Existing reference and
bundle NoCopy/readonly checks also pass in `vm-memory-accounting.log`. These
small GPU runs do not establish native 8 GiB pinned capacity.

## Logical lengths and binding windows

Patch 0019 raises the native RADV logical buffer/allocation ceiling to 8 GiB or
the smaller VRAM capacity without changing the UBO/SSBO descriptor limits.
Metal uses the maintenance4 logical limit only when 64-bit shader addresses are
enabled, and also bounds it by the allocation limit and largest local heap.
Unmatched or limited backends retain their measured smaller limits.

Descriptor bindings relocate the base by the full 64-bit logical offset and
use an explicit bounded range. No ordinary UBO/SSBO uses `VK_WHOLE_SIZE`.
Vertex bindings preserve the 64-bit base while RADV saturates the hardware byte
window at `UINT32_MAX`; index count windows saturate at `UINT32_MAX` elements.
Applications access another part of a large buffer by binding that part's base.
The single-handle native bridge rejects composite backing before accepting work.

Address-table version 2 preserves the original 32 uint64 addresses at byte 0,
adds 32 remaining logical lengths at byte 256, and 32 access masks at byte 512
(read=1, write=2), for 768 bytes. Metadata must declare both version 2 and 768
bytes. Metadata with neither field retains the old 256-byte address table;
incomplete, fractional or unsupported layouts fail pipeline creation. The
translation cache namespace is `rdna4x-2-bda64`; the old namespace produces a
different key for otherwise identical inputs.

Binding metadata must contain dictionary entries in an array. Address-table
binding/count fields must be unsigned integers and match one reflected storage
descriptor; duplicate table declarations fail before creating a Vulkan layout.
`test-buffer-address-table.m` exercises the production parser with simulated
layout creation (65 checks, legacy v1 and v2 accepted). The current full Metal
source also builds and passes all eight small-backing addressing cases (16
dispatches) in the existing VM. Bounds/window tests and extracted production
RADV functions pass with ASan/UBSan; device identity tests confirm that smaller
measured backend limits supersede discovery values. Evidence:
`logs/rdna4-metal2/20261106-logical-buffer/` and
`logs/rdna4-metal2/20261106-memory-discovery/`.

These software API/binding checks do not close the shader execution or transfer
tasks. The native nub keeps the old `maxBuffer` for older bundles and adds
`compositeMaxBuffer` for the current 64-bit path, without opening RADV during
discovery. No actual 8 GiB capacity or target-GPU cross-boundary result is claimed.

## 64-bit shader addressing

Translator patch 0004 materializes physical byte addresses in 64 bits, including
runtime GEPs, pointer/integer round trips, selected/loop-carried pointers, and
local pointer arrays. Checked pointers carry the selected base, remaining
length and access mask through these operations. Local pointer tables store
parallel bounds metadata at the same runtime indices as their address payloads.
Under-aligned accesses use byte operations; a vec3 access checks its 12-byte
memory size while GEP keeps the 16-byte allocation stride. Unsupported recursive
bounds provenance returns a translation error.

Build the small-backing execution fixture on macOS, then run the reference and
candidate serially:

```sh
bash tools/native/rdna4metal/test-large-buffer-addressing.sh /path/to/new-output
/path/to/new-output/test-large-buffer-addressing --reference
sudo env RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 \
  /path/to/new-output/test-large-buffer-addressing /path/to/RDNA4Metal.bundle
```

The MSL 2.1 fixture passes runtime offsets as two 32-bit words and combines them
as `size_t` in the shader. MSL 2.1 does not allow `ulong` fields in a buffer
structure. Its eight cases each execute two single-invocation rounds, covering
runtime 64-bit indexing, constant-address-space reads, a helper, nullable/loop
pointers, pointer/integer arithmetic, atomics, unaligned bytes and a local 2D
pointer table. A blit in the same command buffer preserves complete data,
output, control and guard images in separate readback buffers; application
references to the shader buffers are released before submission.

The original `mac` VM passed both reference and candidate execution on macOS
26.7.2 / SDK 26.5. All eight generated candidate shaders used physical addressing
and address-table v2, and passed `spirv-val --target-env vulkan1.2`. The 26 authored
release-mode translator cases also passed with 20-second and 500-MiB worker
limits. Evidence and exact candidate identities are under
`logs/rdna4-metal2/20261105-110230/bda/root-r3/`.

These execution results use a 256-KiB data window in the isolated VM backend.
They do not verify an allocation or offset above 4 GiB, target-GPU execution,
or preservation of helper/local-array structure after Apple's optimization.
Authored LLVM fixtures cover the latter structures separately. The physical
capacity and cross-4-GiB execution requirements remain open.

## Transfers and Private initialization

Patch 0020 splits Darwin copy/fill requests before the existing compute kernels,
whose push-constant ranges remain 32-bit. Aligned chunks are at most
`0xfffffff0` bytes. The byte-copy path uses at most `0xffffffc0` bytes so rounding
the dispatch width to a workgroup cannot overflow. Source/destination addresses,
remaining lengths and advancement stay 64-bit. Each tail uses the original
compute/CP-DMA selection. Recording errors stop subsequent chunks; already
recorded work is never replayed. Transfer queues retain their existing 64-bit
SDMA path.

Large Private buffers receive a GPU zero fill before publication. An unaligned
tail uses a zero staging buffer for its last one to three bytes. The existing
bounded one-shot submit/wait retains the buffer and staging resources on an
unresolved submission, so failed creation cannot release in-flight backing.

```sh
python3 mesa-patches/test-large-buffer-transfer.py "$PATCHED_MESA" "$TMPDIR/transfer-test"
bash tools/native/rdna4metal/test-private-buffer-init.sh "$VULKAN_INCLUDE" "$TMPDIR/private-init"
```

The transfer test extracts production functions and chunk constants. It checks
real small CPU backing with guards, synthetic 4/8 GiB ranges and high addresses,
dispatch limits, tails and second-chunk failures under ASan/UBSan. The Private
initializer test runs the actual Objective-C initializer against simulated
Vulkan allocation/recording/retirement boundaries, including bounded timeout
and quarantine retention. Neither test executes a large GPU allocation.

Evidence: `logs/rdna4-metal2/20261105-110230/large-api/`, including
`replay-20.json`, `reviewed-transfer.log`, `vm-compile-transfer.json` and
`vm-private-init.log`. All 20 Mesa patches replay with zero fuzz; the actual
`radv_meta_buffer.c` object compiles in the existing macOS VM. Updated Metal
passes the 13 original buffer-blit cases against the accepted isolated VM
backend, including 96 byte-fill boundaries. All 19 native-buffer-route cases
and the production-export completion check also pass with their simulated
transport. Their logs are `vm-small-regression.log` and
`vm-native-route-regression.log`; this does not replace the final matched-stack
or physical-capacity acceptance.

## Named memory acceptance

`run-compatibility.py --only memory` selects the new group explicitly; the
default 47-case selection is unchanged. It first runs the production resource
lifetime paths with simulated Vulkan calls: early application release,
allocation failure rollback, repeated allocation/free, bounded hang/device-loss
quarantine, and child-process exit. The test does not simulate kernel teardown
or claim that real pinned pages were released. Small mode then runs Shared and Managed at
less than 128 MiB, three allocation/execution/release cycles by default. The
runner refuses large modes in a VM.

```sh
sudo python3 tools/native/rdna4metal/run-compatibility.py vm "$VM_BUNDLE" \
    "$VULKAN_INCLUDE" "$OUTPUT" --only memory
```

Build the acceptance executable without running GPU work:

```sh
bash tools/native/rdna4metal/test-large-memory.sh "$TMPDIR/memory-tools"
```

The explicit `private`, `shared` and `managed` modes request one exact 8 GiB
buffer, including two 64 KiB guards. A single invocation reads and writes four
runtime `ulong` offsets at the beginning, both sides of the resource's 4 GiB
boundary and the far end; full-range transforms and chunked readback then check
every word. `nocopy-total` retains 65 simultaneous 64 MiB imports, reads each,
and verifies original callback pointers/lengths, exactly-once retirement, a
read-only import and rejection of an oversized single import. Large modes have
been compiled but not executed. They belong to final physical acceptance.

The reviewed memory group passes on the event-segmentation source snapshot
`dc798b91869708ccdb0648eb4fef19dbc7b0ac787704f677f8d000f730e14f68`.
Evidence is in `logs/rdna4-metal2/20261105-110230/large-api/lifetime-final/`
and `memory-runner-final/`: 144 allocation failures, 16 successful allocation
cycles, three retained-resource cycles, four bounded child exits, and six
Shared/Managed GPU cycles. The GPU cycles use an isolated VM backend and small
real backing; they do not establish greater-than-4-GiB or target-GPU execution.
