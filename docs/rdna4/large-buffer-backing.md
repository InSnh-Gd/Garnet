# Large buffer backing

The `'GRNT'` native user client uses a 48-bit GPU address space with 4095 usable
handles and 4096 map records per session. Multi-gigabyte Metal buffers work in
both 256 MiB Small-BAR and Full-BAR configurations without changing numeric
selectors or wire layouts.

## Page tables

Each client has a 32 MiB page-table quota. The global physical page-table
reserve is bounded by `min(64 MiB, visible_vram_pool / 4)`, and each new 16 KiB
block requires available visible VRAM. Idle reserve blocks stay physically
allocated and counted; reusing one consumes no new visible bytes. Neither
normal retirement nor rollback returns page-table blocks to the user VRAM
allocator.

An 8 GiB mapping with 4 KiB pages requires roughly 16 MiB of leaf tables plus
directory pages (2 MiB `PDB0` huge pages reduce leaf pressure when aligned
regions qualify). Thirty-two 256 MiB Private children, or 128 host children of
64 MiB, fit comfortably inside the handle and map limits alongside normal
desktop resources.

`MAP` validates the full physical range before writing leaves. On allocation
failure, the kernel clears attempted leaves, disconnects newly created
directories, scrubs newly added pages in any partial block, and restores the
session cursor while leaving existing mappings untouched. Public high-half GPU
VAs are canonical; the internal map ledger keeps the stripped form.

Before a rolled-back block can be reused, the kernel flushes HDP and the
session VMID under the shared allocator lock. If TLB invalidation times out,
those blocks remain counted and are removed from the reusable reserve while
`HUNG` handling keeps their physical backing pinned.

Host tests (`src/garnet/tests/`):

```sh
make -C src/garnet/tests native_large_pt_test native_pt_alloc_test
```

`native_pt_alloc_test` extracts `PtSnap` and `PtMem` directly from kernel
sources and stubs only the BAR and VRAM allocator boundaries under ASan/UBSan.

## Private composite allocations

Mesa patch `0017-radv-darwin-composite-private-backing.patch` backs native
Private allocations larger than `0xfffffffc` bytes with physical children under
a single contiguous GPU VA reservation. Children do not allocate duplicate VA
ranges or mappings. Each chunk starts at up to 256 MiB; physical `ENOMEM` can
step chunk size down to the alignment/page boundary, up to 2048 children.
Export and `MAP` failures abort immediately without fragmentation retries.

The parent BO is fully backed (not sparse/virtual), uses no PRT/CLEAR/REPLACE
operations, fans out submission handles and metadata writes to its physical
children, and counts child backing once. CPU mapping, external export, replay
addresses, and sparse rebinding are rejected. Allocations `<= 0xfffffffc` bytes
keep the single-BO path.

Teardown unmaps any optional read-only padding page and each child range,
destroys child handles in reverse order, and releases the single VA
reservation. Unmap or free failures enter native quarantine and retain budget
accounting until safe teardown.

Host test:

```sh
python3 mesa-patches/tests/test-composite-bo.py "$PATCHED_MESA" "$TMPDIR/composite-test"
```

## Shared and Managed host backing

Shared and Managed buffers larger than 64 MiB reserve one aligned anonymous CPU
range and import it via `VK_EXT_external_memory_host`. Fresh anonymous pages
start zero-filled. Both modes use coherent host memory so CPU and GPU share the
same pages (`didModifyRange:` requires no copy), bypassing the 64 MiB single-BO
GTT limit. Creation fails if the backend exposes no coherent host-import memory
type.

Mesa patch `0018` imports that CPU range in chunks of at most 64 MiB under one
GPU VA reservation and expands submission references across all child handles.
The backend borrows the caller's pointer and never unmaps it; allocation,
export, or `MAP` failures unwind completed children in reverse order with no
fragmentation retry.

`GarnetMetal` releases the owned CPU range only after destroying the Vulkan
buffer and freeing the imported memory. A quarantined kernel descriptor keeps
its UPL and physical pages pinned even if the user mapping disappears. NoCopy
imports retain their 64 MiB per-import limit, borrow the caller's pointer
without copying, and invoke the caller's deallocator once after ownership
transfer succeeds (never on failed creation).

Device-owned imported backing counts toward `currentAllocatedSize` using its
aligned span, separate from the borrowed NoCopy import ledger. Heap resources
bind their heap's backing without adding a second device charge (see
[automatic-heaps.md](automatic-heaps.md)).

## Logical lengths and binding windows

Mesa patch `0019` raises the native RADV logical buffer/allocation ceiling to
8 GiB (bounded by VRAM capacity) without changing UBO/SSBO descriptor range
limits. `GarnetMetal` exposes the `maintenance4` logical limit only when 64-bit
shader addressing is active, bounded by the allocation limit and largest local
heap.

Descriptor bindings relocate the base by the full 64-bit logical offset with an
explicit bounded range (ordinary UBO/SSBO bindings never use `VK_WHOLE_SIZE`).
Vertex bindings preserve the 64-bit base while saturating the hardware byte
window at `UINT32_MAX`; index windows saturate at `UINT32_MAX` elements. The
single-handle native transfer bridge rejects composite BOs.

Buffer address-table version 2 (`768` bytes) keeps 32 `uint64` base addresses at
offset `0`, adds 32 remaining logical lengths at offset `256`, and adds 32
access masks (`read=1`, `write=2`) at offset `512`. Metadata must declare both
version 2 and 768 bytes; metadata omitting both falls back to the 256-byte v1
table, while incomplete or malformed layouts fail pipeline creation.

## 64-bit shader addressing

Translator patch `0004` (`third-party/metal2vulkan-rdna4/patches/`) materializes
physical byte addresses in 64 bits across runtime GEPs, pointer/integer round
trips, selected and loop-carried pointers, and local pointer arrays. Checked
pointers carry base, remaining length, and access mask through these operations,
and local pointer tables keep parallel bounds metadata at matching indices.
Under-aligned accesses lower to byte operations; `vec3` accesses check their
12-byte memory width while keeping 16-byte GEP stride. Unsupported recursive
bounds provenance fails translation cleanly.

## Transfers and Private initialization

Mesa patch `0020` splits Darwin copy/fill requests before entering 32-bit
compute kernels: aligned chunks are at most `0xfffffff0` bytes, and byte-copy
chunks are at most `0xffffffc0` bytes so workgroup-width rounding cannot
overflow. Addresses, remaining lengths, and cursor advances remain 64-bit, with
existing compute/CP-DMA selection for tails and immediate stop on recording
errors. Transfer queues keep their 64-bit SDMA path.

Large Private buffers are GPU-zeroed before publication, using a zero staging
buffer for any unaligned 1–3 byte tail. Unresolved one-shot submissions retain
both buffer and staging resources in quarantine so failed creation cannot free
in-flight backing.

## Verification

- **Kernel page tables & allocator**: `make -C src/garnet/tests all`
- **Mesa composite BO & transfer splitting**:
  `python3 mesa-patches/tests/test-composite-bo.py "$PATCHED_MESA" "$OUT"` and
  `python3 mesa-patches/tests/test-large-buffer-transfer.py "$PATCHED_MESA" "$OUT"`
- **Metal address-table, Private init & lifetime fixtures** (macOS):
  `run-metal-test.sh test-buffer-address-table "$OUT"`,
  `run-metal-test.sh test-private-buffer-init "$OUT"`,
  `run-metal-test.sh test-large-buffer-addressing "$OUT"`, and
  `run-compatibility.py live ... --only large-memory-over4g` on physical
  hardware (the runner refuses 8 GiB modes in VMs; see
  [TESTING.md](../../src/garnet-metal/tests/TESTING.md)).
