# Automatic heaps

`newHeapWithDescriptor:` reserves capacity without opening RADV or allocating
GPU memory. Automatic heaps support Private, Shared and Managed storage;
placement, sparse and Memoryless heaps remain unsupported. Resource storage,
cache and hazard modes must match the heap. The default hazard mode becomes
Untracked, so applications must encode the required dependencies.

Ask the device for each resource's `MTLSizeAndAlign` before choosing heap
capacity. These queries open RADV, create a temporary unbound Vulkan resource,
read its requirements2, and destroy it without allocating backing. Queries and
constructors use the same buffer/image descriptions. Missing requirements2 or
unsupported descriptors return zero size/alignment; creation returns nil.

For example, create a Private buffer as follows:

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

For multiple resources, round each placement up to its reported alignment.
Allow additional capacity for separate memory types, dedicated allocations and
fragmentation. A nonzero sizing result describes a resource's requirements; it
does not promise that current budget or a particular heap can accommodate it.

## Backing and reports

Resources bind real `VkDeviceMemory` blocks at aligned offsets. The allocator
checks `memoryTypeBits`, `bufferImageGranularity`, and required or preferred
dedicated allocations. A partial slab uses the requested extent when the usual
4 MiB slab would exceed remaining capacity, preserving room for other backing
classes. Backing never exceeds the heap's capacity.

Shared/Managed buffers use coherent host mappings. Device-owned buffers larger
than 64 MiB use the existing owned host-import path; NoCopy ownership rules do
not change. Optimal images retain device-local backing and their existing CPU
staging path, including images in Shared/Managed heaps.

| Query | Meaning |
| --- | --- |
| `heap.size`, `heap.currentAllocatedSize` | Reserved capacity, rounded to 16 KiB |
| `resource.allocatedSize`, `heap.usedSize` | Vulkan requirement bytes charged to live, non-aliasable resources |
| `MTLSizeAndAlign.size` | Placement extent including required granularity/alignment padding; may exceed `allocatedSize` |
| `heap.maxAvailableSizeWithAlignment:` | Largest currently reusable range, bounded by logical gaps, backing classes and allocation budget |
| `device.currentAllocatedSize` | Counts each heap's reserved capacity once; heap resources do not add it again |

Automatic resources report `heapOffset == 0`; this is not their internal Vulkan
bind offset. Private probes used by tests distinguish actual backing bytes from
reserved capacity. Purgeable state remains a tracked hint and does not discard
resource contents. Texture views retain their root and expose its heap metadata;
make the root aliasable when it is safe to retire its allocation.

## Reuse and lifetime

After the last use, call `makeAliasable` on the resource. Do not use that resource
again. Logical usage returns immediately, while its physical range remains
unavailable until the previously encoded GPU work retires. The heap also tracks
retirement with the general resource pool disabled. Completion, resource
references, and physical backing lifetime are separate: an empty block may be
reclaimed once no resource handle needs it.

For predictable CPU-driven reuse, wait for command-buffer completion before
making the old resource aliasable and allocating its replacement. A replacement
can return nil while preceding work remains in flight, even after `usedSize`
decreases. Use `maxAvailableSizeWithAlignment:` and handle allocation failure.
Completed alias cycles can reuse the same block while old aliasable handles
remain alive. Dropping every handle can reclaim that block instead. Both cases
remain within the same heap capacity.

Allocation, bind and view failures roll back logical and physical ownership.
Unresolved submitted work keeps its backing quarantined; it is not returned to
the heap merely because the application releases its reference.

## Verification

Build the GPU oracle in the existing macOS VM, then run the reference and
candidate serially:

```sh
bash tools/native/rdna4metal/test-heap-gpu.sh "$TOOLS_OUTPUT"
"$TOOLS_OUTPUT/test-heap-gpu" --reference
sudo env RDNA4M_TEST_INPROC=1 MVK_CONFIG_USE_MTLHEAP=0 \
    "$TOOLS_OUTPUT/test-heap-gpu" "$VM_BUNDLE"
```

The four cases check mixed buffer/BGRA8/RGBA16Float data, a one-byte tail through
legal compute accesses, full-capacity refusal and completed reuse, shared-event
gated in-flight protection, and repeated alias cycles without allocation growth.
`HEAP_GPU_LAYOUT` records backend-specific query and accounting values.
Reference mode uses public APIs and does not impose the candidate's private
backing policy on system Metal.

`test-heap-execution.sh` additionally executes production resource constructors
with simulated Vulkan boundaries: requirements2, dedicated allocations, memory
types, failure rollback, pool-off retirement, views, host imports and quarantine.
`test-heap-backing.c` checks allocation policy with host sanitizers. These checks
do not establish target-RDNA4 GPU execution or physical large-memory capacity.
Evidence is under `logs/rdna4-metal2/20261105-110230/heaps/`; real hardware
acceptance remains in the final task group.
