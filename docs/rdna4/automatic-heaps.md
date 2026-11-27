# Automatic and placement heaps

`newHeapWithDescriptor:` reserves capacity without opening RADV or allocating
GPU memory. Both `MTLHeapTypeAutomatic` and `MTLHeapTypePlacement` support
Private, Shared, and Managed storage; Memoryless heaps remain unsupported.
Resource storage, cache, and hazard modes must match the heap. The default
hazard mode is Untracked, so applications must encode the required
dependencies.

Query the device for each resource's `MTLSizeAndAlign` before sizing a heap.
These queries open RADV, create a temporary unbound Vulkan resource, read its
`VkMemoryRequirements2`, and destroy it without allocating backing. Queries and
constructors use the same buffer/image descriptions. Missing requirements or
unsupported descriptors return zero size and alignment; creation returns `nil`.

```objc
MTLResourceOptions options = MTLResourceStorageModePrivate;
MTLSizeAndAlign sizing = [device heapBufferSizeAndAlignWithLength:length
                                                          options:options];
if (!sizing.size || !sizing.align) return;
MTLHeapDescriptor *descriptor = [MTLHeapDescriptor new];
descriptor.storageMode = MTLStorageModePrivate;
descriptor.size = sizing.size;
id<MTLHeap> heap = [device newHeapWithDescriptor:descriptor];
id<MTLBuffer> buffer = [heap newBufferWithLength:length options:options];
if (!buffer) return;
```

For multiple resources, round each placement up to its reported alignment and
leave headroom for separate memory types, dedicated allocations, and
fragmentation. A nonzero sizing result describes a resource's requirements; it
does not guarantee that current budget or a given heap can fit it.

## Backing and reports

Resources bind real `VkDeviceMemory` blocks at aligned offsets. The allocator
checks `memoryTypeBits`, `bufferImageGranularity`, and required or preferred
dedicated allocations. A partial slab uses the requested extent when the default
4 MiB slab would exceed remaining capacity, leaving room for other backing
classes. Backing never exceeds the heap's capacity.

Shared and Managed buffers use coherent host mappings. Device-owned buffers
larger than 64 MiB use the owned host-import path; NoCopy ownership rules stay
unchanged. Optimal images retain device-local backing and their CPU staging
path, including images in Shared or Managed heaps.

| Query | Meaning |
| --- | --- |
| `heap.size`, `heap.currentAllocatedSize` | Reserved capacity, rounded up to 16 KiB |
| `resource.allocatedSize`, `heap.usedSize` | Vulkan requirement bytes charged to live, non-aliasable resources |
| `MTLSizeAndAlign.size` | Placement extent including required granularity and alignment padding; may exceed `allocatedSize` |
| `heap.maxAvailableSizeWithAlignment:` | Largest currently reusable range, bounded by logical gaps, backing classes, and allocation budget |
| `device.currentAllocatedSize` | Counts each heap's reserved capacity once; heap resources do not add to it again |

Automatic resources report `heapOffset == 0` (not their internal Vulkan bind
offset). Placement resources report the explicit `heapOffset` requested at
creation, and texture views of a placed texture expose their root's
`heapOffset`. Purgeable state is tracked as a hint and does not discard
contents. Texture views retain their root and expose its heap metadata; call
`makeAliasable` on the root once it is safe to retire its allocation.

## Placement heaps (`MTLHeapTypePlacement`)

Placement heaps accept `newBufferWithLength:options:offset:` and
`newTextureWithDescriptor:offset:` (returning `nil` for the non-offset
automatic selectors; automatic heaps return `nil` for offset selectors). Each
placement heap allocates one contiguous heap-sized `VkDeviceMemory` block on
first use, retained for the heap's lifetime and freed on teardown, so resources
bind directly at their requested byte offset:

- Offsets must satisfy the alignment and `bufferImageGranularity` reported by
  `heapBufferSizeAndAlignWithLength:options:` or
  `heapTextureSizeAndAlignWithDescriptor:`, and `[offset, offset + extent)` must
  fit within `heap.size` without overflow. Misaligned, out-of-bounds,
  dedicated-required, or mismatched-option requests return `nil` without
  mutating accounting.
- Overlapping explicit offsets are permitted; applications must synchronize GPU
  uses across aliased ranges with fences, events, or command-buffer ordering.
  `doesAliasResource:` reflects actual backing-range overlap.
- `heap.usedSize` reports the merged union length of active (non-aliasable)
  placed ranges (bounded by `heap.size`, never double-counting overlapping
  aliases), and `maxAvailableSizeWithAlignment:` reports the largest aligned
  uncovered gap bounded by device budget. `makeAliasable` retires logical
  accounting immediately while deferred lease cleanup waits for GPU completion.
- `device.currentAllocatedSize` counts a placement heap's reserved capacity
  once at creation and releases it on teardown.

## Reuse and lifetime

Call `makeAliasable` after the resource's last use, and do not use that
resource again. Logical usage returns immediately while physical backing stays
reserved until previously encoded GPU work retires. Completion, resource
references, and physical backing lifetime are tracked independently: an empty
block can be reclaimed once no resource handle references it.

For predictable CPU-driven reuse, wait for command-buffer completion before
making the old resource aliasable and allocating its replacement. Replacements
can still return `nil` while preceding work is in flight even after `usedSize`
drops; check `maxAvailableSizeWithAlignment:` and handle allocation failure.

Allocation, bind, and view failures roll back both logical and physical
ownership. Unresolved submitted work keeps its backing quarantined; releasing
the application's reference never frees in-flight GPU backing.

## Verification

- **Host C suites** (`make -C src/garnet-metal/tests host`):
  `test-host-memory.c` validates automatic heap allocation policy and
  `test-swizzle-placement.c` validates placement bounds, overlap aliasing,
  interval accounting, and source wiring under ASan/UBSan.
- **Mocked Vulkan execution** (`bash src/garnet-metal/tests/run-metal-test.sh test-heap-execution "$OUT"`):
  runs production resource constructors against simulated Vulkan boundaries
  (requirements2, dedicated allocations, memory types, failure rollback,
  pool-off retirement, views, host imports, and quarantine).
- **GPU execution oracles** (on macOS with a GPU or isolated VM):
  `run-metal-test.sh test-heap-gpu "$OUT"` covers mixed buffer/BGRA8/RGBA16Float
  data, byte-tail accesses, capacity refusal, shared-event in-flight protection,
  and repeated alias cycles; `test-metal3-placement-heap.m` covers explicit
  placement-heap execution (see [TESTING.md](../../src/garnet-metal/tests/TESTING.md)).
