// ---------------------------------------------------------------------------------------------------------------
// m11 H1: MTLHeap (automatic heaps). SkyLight's MetalCompositor sends -[MTLDevice newHeapWithDescriptor:] through MPSSupportsMTLDevice -> MPSDevice::MPSDevice
// (MPSCore 0x7ff810821b40: setSize: / setStorageMode: / setCpuCacheMode: / setResourceOptions: / setProtectionOptions: / newHeapWithDescriptor:, then
// -setPurgeableState:3 and -size on the heap; CONFIRMED against the carved MPSCore and the WindowServer crash report of an earlier run).
// Design: automatic heaps use a first-fit logical allocator and on-demand
// physical backing blocks; placement heaps (MTLHeapTypePlacement) bind
// resources directly at caller-specified offsets inside a contiguous
// heap-sized physical block, allowing explicit-offset aliasing. Every
// buffer/texture is still an ordinary RDNA4Buffer / RDNA4Texture, validated
// against its real VkMemoryRequirements and bufferImageGranularity. The heap's
// size is added to currentAllocatedSize at creation and its resources are not
// counted again.
// Values below were measured on an Apple-silicon Mac first (mtlprobe heap): size rounds up to 16 KiB; usedSize = sum of the requested sizes (not rounded);
// maxAvailableSizeWithAlignment = largest free gap after aligning its start; automatic heaps return nil for the offset variants; hazardTrackingMode
// Default becomes Untracked; resourceOptions = storage << 4 | cpuCache | hazard << 8; setPurgeableState: returns the previous state (initially NonVolatile).
// Bundle-specific refusals (an Apple-silicon Mac aborts in its validation layer on these, so they are PC-only tests): resource storage/cpu-cache mode different from the
// heap's, a Tracked/Untracked request that differs from the heap's, Memoryless/placement/sparse heaps.
// ---------------------------------------------------------------------------------------------------------------
static NSUInteger rdna4_alup(NSUInteger v, NSUInteger a) { return (NSUInteger)rdna4ha_alup(v, a); }
static BOOL rdna4_heap_reserve_capacity(uint64_t size, uint64_t limit) {
    uint64_t old = atomic_load_explicit(&rdna4_heap_reserved, memory_order_relaxed);
    for (;;) {
        if (old > limit || size > limit - old) return NO;
        if (atomic_compare_exchange_weak_explicit(&rdna4_heap_reserved, &old, old + size,
                                                  memory_order_acq_rel, memory_order_relaxed)) return YES;
    }
}
static uint64_t rdna4_heap_release_serial(void) {
    pthread_mutex_lock(&RDNA4P.mu);
    uint64_t serial = RDNA4HT.ready ? RDNA4HT.p.submitted : 0;
    pthread_mutex_unlock(&RDNA4P.mu);
    return serial;
}
static uint64_t rdna4_heap_completed_serial(void) {
    pthread_mutex_lock(&RDNA4P.mu);
    uint64_t serial = RDNA4HT.ready ? RDNA4HT.p.completed : 0;
    pthread_mutex_unlock(&RDNA4P.mu);
    return serial;
}
@implementation RDNA4Heap {
    id _dev; NSString *_lbl; NSUInteger _size, _purge; MTLHeapType _type; unsigned _st, _cc, _hz; RDNA4HeapAlloc _ha; rdna4hb_heap _hb;
    NSHashTable *_members;
}
- (instancetype)initWithDevice:(id)dev descriptor:(MTLHeapDescriptor *)d {
    self = [super init]; if (!self) return nil;
    NSUInteger sz = d.size; unsigned st = (unsigned)d.storageMode, cc = (unsigned)d.cpuCacheMode, hz = (unsigned)d.hazardTrackingMode;
    #define HFAIL(...) do { RDNA4LOG("newHeapWithDescriptor: NIL: " __VA_ARGS__); return nil; } while (0)
    if (d.type != MTLHeapTypeAutomatic && d.type != MTLHeapTypePlacement) HFAIL("heap type %lu: only automatic and placement heaps are supported", (unsigned long)d.type);
    if (!sz) HFAIL("size 0");
    if (st > 2) HFAIL("storage mode %u (Memoryless) not supported", st);
    if (hz > 2) HFAIL("hazard tracking mode %u", hz);
    rdna4dc_caps caps = [(GarnetDevice *)dev rdna4MemoryCaps];
    sz = (NSUInteger)rdna4dc_heap_size(sz, caps.allocation_budget);
    if (!sz) HFAIL("size %lu exceeds the rounded Vulkan heap budget %llu", (unsigned long)d.size, (unsigned long long)caps.allocation_budget);
    if (!rdna4_heap_reserve_capacity(sz, caps.allocation_budget)) HFAIL("size %lu exceeds the capacity not already promised to live heaps (budget %llu)", (unsigned long)sz, (unsigned long long)caps.allocation_budget);
    #undef HFAIL
    _dev = dev; _size = sz; _type = d.type; _st = st; _cc = cc; _hz = hz ? hz : 1; _purge = 2; rdna4ha_init(&_ha, sz); rdna4hb_init(&_hb, sz, RDNA4HB_MAX_NORMAL);
    _members = [NSHashTable hashTableWithOptions:NSPointerFunctionsWeakMemory | NSPointerFunctionsObjectPointerPersonality];
    atomic_fetch_add(&rdna4_alloc_total, (uint64_t)_size);
    RDNA4LOG("RDNA4Heap %p: %lu bytes type %lu storage %u cache %u hazard %u (reserved capacity; VkDeviceMemory created on demand)", (__bridge void *)self, (unsigned long)_size, (unsigned long)_type, _st, _cc, _hz);
    return self;
}
- (void)dealloc {
    for (size_t i = 0; i < _hb.nlease; i++) {
        void *owner = _hb.lease[i].owner;
        _hb.lease[i].owner = NULL;
        if (!owner) continue;
        id backing = (__bridge_transfer id)owner;
        if ([backing respondsToSelector:@selector(destroyHandles)]) [backing destroyHandles];
    }
    rdna4hb_reclaim block;
    while (rdna4hb_take_block(&_hb, &block)) {
        if (RDNA4R.ok && block.host_mapped && !block.owned_host && block.map && block.mem)
            vkUnmapMemory(RDNA4R.dev, (VkDeviceMemory)block.mem);
        if (RDNA4R.ok && block.mem) vkFreeMemory(RDNA4R.dev, (VkDeviceMemory)block.mem, NULL);
        if (block.owned_host) {
            if (block.map) (void)munmap(block.map, (size_t)block.size);
            @synchronized ([GarnetDevice class]) { RDNA4R.impTotal -= block.size; }
        }
    }
    rdna4hb_destroy(&_hb);
    if (_size) {
        atomic_fetch_sub(&rdna4_heap_reserved, (uint64_t)_size);
        atomic_fetch_sub(&rdna4_alloc_total, (uint64_t)_size);
    }
    rdna4ha_destroy(&_ha);
}
- (void)doesNotRecognizeSelector:(SEL)sel {
    RDNA4LOG("UNRECOGNIZED selector %s on RDNA4Heap", sel_getName(sel));
    [super doesNotRecognizeSelector:sel];
}
// Logical accounting is driven by the resource's real VkMemoryRequirements. The
// charged size is mr.size; the placement extent also includes bufferImageGranularity.
- (BOOL)rdna4ReserveRequirements:(VkMemoryRequirements)mr offset:(NSUInteger *)off id:(uint64_t *)bid {
    if (!off || !bid || !mr.size || !mr.alignment) return NO;
    NSError *e = nil;
    if (!rdna4_radv_open(&e)) { RDNA4LOG("heap resource: RADV open failed: %s", e.localizedDescription.UTF8String); return NO; }
    @synchronized (self) {
        uint64_t gran = RDNA4R.lim.bufferImageGranularity ? RDNA4R.lim.bufferImageGranularity : 1;
        uint64_t extent = rdna4_alup((NSUInteger)mr.size, (NSUInteger)gran), align = MAX((uint64_t)mr.alignment, gran);
        if (extent == UINT64_MAX || extent < mr.size || align < gran) return NO;
        rdna4dc_caps caps = rdna4_device_caps();
        if (mr.size > rdna4dc_heap_available(&caps, _ha.used)) return NO;
        uint64_t o;
        if (!rdna4ha_alloc_ext(&_ha, mr.size, extent, align, &o, bid)) return NO;
        *off = (NSUInteger)o;
        return YES;
    }
}
- (void)rdna4Free:(uint64_t)bid { @synchronized (self) { rdna4ha_free(&_ha, bid); } }
- (void)rdna4FreeBlockLocked:(rdna4hb_reclaim)block {
    if (RDNA4R.ok && block.host_mapped && !block.owned_host && block.map && block.mem)
        vkUnmapMemory(RDNA4R.dev, (VkDeviceMemory)block.mem);
    if (RDNA4R.ok && block.mem) vkFreeMemory(RDNA4R.dev, (VkDeviceMemory)block.mem, NULL);
    if (block.owned_host) {
        if (block.map) (void)munmap(block.map, (size_t)block.size);
        @synchronized ([GarnetDevice class]) { RDNA4R.impTotal -= block.size; }
    }
}
- (void)rdna4CollectLocked {
    rdna4hb_reap(&_hb, rdna4_heap_completed_serial());
    for (;;) {
        uint64_t lease = 0, block = 0; void *owner = NULL;
        if (!rdna4hb_finalize_candidate(&_hb, &lease, &block, &owner)) break;
        id backing = (__bridge_transfer id)owner;
        if ([backing respondsToSelector:@selector(destroyHandles)]) [backing destroyHandles];
        rdna4hb_handle_close(&_hb, block);
        rdna4hb_finalized(&_hb, lease);
    }
    // A lease abandoned before bind has no owner, and an object that already destroyed
    // its handle may have been collected before the central reaper ran.
    for (size_t i = 0; i < _hb.nlease;) {
        rdna4hb_lease *lease = &_hb.lease[i];
        const rdna4hb_block *block = rdna4hb_find_block_const(&_hb, lease->block);
        if (lease->dedicated && lease->retired && !lease->owner && block && !block->handles) {
            uint64_t lease_id = lease->id;
            rdna4hb_finalized(&_hb, lease_id);
            i = 0;
        } else i++;
    }
    rdna4hb_reclaim block;
    while (rdna4hb_take_reclaimable(&_hb, &block)) [self rdna4FreeBlockLocked:block];
}
- (BOOL)rdna4AcquireBackingForBuffer:(VkBuffer)buffer image:(VkImage)image requirements:(VkMemoryRequirements)mr
                           dedicated:(BOOL)dedicated hostImport:(BOOL)hostImport
                                 mem:(VkDeviceMemory *)mem offset:(uint64_t *)offset map:(uint8_t **)map
                               block:(uint64_t *)block lease:(uint64_t *)lease rangeSize:(uint64_t *)rangeSize
                               error:(NSError **)err {
    if (!mem || !offset || !map || !block || !lease || !rangeSize || (!buffer && !image) || (buffer && image) || !mr.size ||
        (hostImport && !buffer)) {
        if (err) *err = rdna4_err(22, @"invalid heap backing request");
        return NO;
    }
    @synchronized (self) {
        // A completion callback cannot reach the heap, so reap finished retirement
        // before deciding that capacity is exhausted.
        [self rdna4CollectLocked];
        // Optimal images always keep the old DEVICE_LOCAL route and use the
        // texture CPU staging path. Only Shared/Managed heap buffers are mapped.
        const BOOL hostMapped = buffer && _st < 2;
        uint64_t granularity = RDNA4R.lim.bufferImageGranularity ? RDNA4R.lim.bufferImageGranularity : 1;
        for (int attempt = 0; attempt < 5; attempt++) {
            rdna4hb_res res = {0};
            int action = rdna4hb_alloc(&_hb, mr.size, mr.alignment, granularity, mr.memoryTypeBits,
                                       hostMapped, hostImport, dedicated, NULL, &res);
            if (action == RDNA4HB_R_OK) {
                rdna4hb_block *found = rdna4hb_find_block(&_hb, res.block);
                if (!found) {
                    rdna4hb_release(&_hb, res.lease, 0);
                    rdna4hb_reap(&_hb, 0);
                    [self rdna4CollectLocked];
                    if (err) *err = rdna4_err(22, @"heap backing block disappeared");
                    return NO;
                }
                if (found->host_mapped != hostMapped || (hostImport && !found->owned_host) ||
                    (hostMapped && !hostImport && found->owned_host)) {
                    rdna4hb_release(&_hb, res.lease, 0);
                    rdna4hb_reap(&_hb, 0);
                    [self rdna4CollectLocked];
                    if (err) *err = rdna4_err(22, @"heap backing class mismatch");
                    return NO;
                }
                *mem = (VkDeviceMemory)found->mem;
                *map = (uint8_t *)found->map;
                *offset = res.offset;
                *block = res.block;
                *lease = res.lease;
                *rangeSize = res.range_size;
                return YES;
            }
            if (action == RDNA4HB_R_RETRY) { [self rdna4CollectLocked]; continue; }
            if (action != RDNA4HB_R_NEED_BLOCK) {
                if (err) *err = rdna4_err(22, [NSString stringWithFormat:@"no physical heap backing for %llu bytes (backing %llu of %llu)",
                                               (unsigned long long)mr.size, (unsigned long long)_hb.backing, (unsigned long long)_hb.size]);
                return NO;
            }

            uint64_t blockSize = res.block_size;
            if (!blockSize || blockSize > _hb.size - _hb.backing) {
                [self rdna4CollectLocked];
                blockSize = res.block_size;
                if (!blockSize || blockSize > _hb.size - _hb.backing) {
                    if (err) *err = rdna4_err(22, @"physical heap backing capacity exhausted");
                    return NO;
                }
            }

            VkDeviceMemory newMemory = VK_NULL_HANDLE;
            uint8_t *newMap = NULL;
            void *ownedHost = NULL;
            uint64_t allocationSize = blockSize;
            uint32_t memoryType = 0;
            VkMemoryHostPointerPropertiesEXT hostProps = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
            if (hostImport) {
                uint64_t hostSpan = 0;
                ownedHost = rdna4nc_owned_allocate(blockSize, RDNA4R.hostAlign, &hostSpan);
                if (!ownedHost || !hostSpan) {
                    if (err) *err = rdna4_err(22, @"cannot reserve owned CPU memory for a heap backing block");
                    return NO;
                }
                if (hostSpan > _hb.size - _hb.backing) {
                    (void)munmap(ownedHost, (size_t)hostSpan);
                    if (err) *err = rdna4_err(22, @"aligned heap import block exceeds its remaining capacity");
                    return NO;
                }
                allocationSize = hostSpan;
                VkResult hr = rdna4_vkGetMHPP(RDNA4R.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ownedHost, &hostProps);
                int mt = hr == VK_SUCCESS && hostProps.memoryTypeBits
                    ? rdna4_find_mem(hostProps.memoryTypeBits & mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                    : -1;
                if (mt < 0) {
                    (void)munmap(ownedHost, (size_t)hostSpan);
                    if (err) *err = rdna4_err(22, [NSString stringWithFormat:@"host import has no coherent heap memory type (vk %d, bits 0x%x / 0x%x)",
                                                   hr, hostProps.memoryTypeBits, mr.memoryTypeBits]);
                    return NO;
                }
                memoryType = (uint32_t)mt;
                newMap = ownedHost;
            } else {
                int mt = rdna4_find_mem(mr.memoryTypeBits,
                    hostMapped ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                               : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (mt < 0) {
                    if (err) *err = rdna4_err(22, hostMapped ? @"no host-visible coherent memory type for the heap block"
                                                             : @"no device-local memory type for the heap block");
                    return NO;
                }
                memoryType = (uint32_t)mt;
            }

            VkMemoryDedicatedAllocateInfo dedicatedInfo = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                .buffer = buffer, .image = image };
            VkImportMemoryHostPointerInfoEXT importInfo = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = ownedHost };
            VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                .flags = RDNA4R.bufferAddress ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0 };
            if (dedicated) {
                dedicatedInfo.pNext = hostImport ? &importInfo : NULL;
                flags.pNext = &dedicatedInfo;
            } else if (hostImport) {
                flags.pNext = &importInfo;
            }
            VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &flags,
                .allocationSize = allocationSize, .memoryTypeIndex = memoryType };
            VkResult vr = vkAllocateMemory(RDNA4R.dev, &allocation, NULL, &newMemory);
            if (vr != VK_SUCCESS) {
                if (ownedHost) (void)munmap(ownedHost, (size_t)allocationSize);
                if (err) *err = rdna4_err(22, [NSString stringWithFormat:@"vkAllocateMemory(heap block %llu) = %d",
                                               (unsigned long long)allocationSize, vr]);
                return NO;
            }
            if (hostMapped && !hostImport) {
                vr = vkMapMemory(RDNA4R.dev, newMemory, 0, VK_WHOLE_SIZE, 0, (void **)&newMap);
                if (vr != VK_SUCCESS) {
                    vkFreeMemory(RDNA4R.dev, newMemory, NULL);
                    if (err) *err = rdna4_err(24, [NSString stringWithFormat:@"vkMapMemory(heap block) = %d", vr]);
                    return NO;
                }
            }
            uint64_t newBlock = 0;
            if (!rdna4hb_add_block(&_hb, allocationSize, memoryType, hostMapped, dedicated,
                                   res.exclusive, hostImport, newMemory, newMap, &newBlock)) {
                if (hostMapped && !hostImport) vkUnmapMemory(RDNA4R.dev, newMemory);
                vkFreeMemory(RDNA4R.dev, newMemory, NULL);
                if (ownedHost) (void)munmap(ownedHost, (size_t)allocationSize);
                if (err) *err = rdna4_err(22, @"heap backing metadata rejected the Vulkan allocation");
                return NO;
            }
            if (hostImport) @synchronized ([GarnetDevice class]) { RDNA4R.impTotal += allocationSize; }
        }
        if (err) *err = rdna4_err(22, @"heap backing allocator did not converge");
        return NO;
    }
}
- (BOOL)rdna4AcquirePlacementBackingForBuffer:(VkBuffer)buffer image:(VkImage)image requirements:(VkMemoryRequirements)mr
                                       offset:(NSUInteger)placeOffset hostImport:(BOOL)hostImport
                            requiresDedicated:(BOOL)requiresDedicated
                                          mem:(VkDeviceMemory *)mem offset:(uint64_t *)offset map:(uint8_t **)map
                                        block:(uint64_t *)block lease:(uint64_t *)lease rangeSize:(uint64_t *)rangeSize
                                        error:(NSError **)err {
    if (!mem || !offset || !map || !block || !lease || !rangeSize || (!buffer && !image) || (buffer && image) || !mr.size ||
        (hostImport && !buffer) || _type != MTLHeapTypePlacement) {
        if (err) *err = rdna4_err(22, @"invalid placement heap backing request");
        return NO;
    }
    @synchronized (self) {
        [self rdna4CollectLocked];
        const BOOL needHostMap = buffer && _st < 2;
        uint64_t granularity = RDNA4R.lim.bufferImageGranularity ? RDNA4R.lim.bufferImageGranularity : 1;
        for (int attempt = 0; attempt < 2; attempt++) {
            rdna4hb_res res = {0};
            int action = rdna4hb_place(&_hb, (uint64_t)placeOffset, mr.size, mr.alignment, granularity,
                                       mr.memoryTypeBits, needHostMap ? 1 : 0, hostImport ? 1 : 0,
                                       requiresDedicated ? 1 : 0, &res);
            if (action == RDNA4HB_R_OK) {
                rdna4hb_block *found = rdna4hb_find_block(&_hb, res.block);
                if (!found || (needHostMap && !found->host_mapped) || (hostImport ? !found->owned_host : found->owned_host)) {
                    rdna4hb_release(&_hb, res.lease, 0);
                    rdna4hb_reap(&_hb, 0);
                    if (err) *err = rdna4_err(22, @"placement heap backing class mismatch");
                    return NO;
                }
                *mem = (VkDeviceMemory)found->mem;
                *map = needHostMap ? (uint8_t *)found->map : NULL;
                *offset = res.offset;
                *block = res.block;
                *lease = res.lease;
                *rangeSize = res.range_size;
                return YES;
            }
            if (action != RDNA4HB_R_NEED_BLOCK) {
                if (err) *err = rdna4_err(22, action == RDNA4HB_R_INVALID
                    ? [NSString stringWithFormat:@"placement offset %lu violates alignment %llu / granularity %llu or dedicated requirements",
                       (unsigned long)placeOffset, (unsigned long long)mr.alignment, (unsigned long long)granularity]
                    : [NSString stringWithFormat:@"placement range [%lu, +%llu) exceeds heap size %llu or incompatible backing",
                       (unsigned long)placeOffset, (unsigned long long)mr.size, (unsigned long long)_hb.size]);
                return NO;
            }

            uint64_t blockSize = res.block_size;
            VkDeviceMemory newMemory = VK_NULL_HANDLE;
            uint8_t *newMap = NULL;
            void *ownedHost = NULL;
            uint64_t allocationSize = blockSize;
            uint32_t memoryType = 0;
            int fallbackMt = -1;
            BOOL blockHostMapped = NO;
            VkMemoryHostPointerPropertiesEXT hostProps = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
            if (hostImport) {
                uint64_t hostSpan = 0;
                ownedHost = rdna4nc_owned_allocate(blockSize, RDNA4R.hostAlign, &hostSpan);
                if (!ownedHost || !hostSpan || hostSpan > _hb.size - _hb.backing) {
                    if (ownedHost && hostSpan) (void)munmap(ownedHost, (size_t)hostSpan);
                    if (err) *err = rdna4_err(22, @"cannot reserve owned CPU memory for placement heap backing");
                    return NO;
                }
                allocationSize = hostSpan;
                VkResult hr = rdna4_vkGetMHPP(RDNA4R.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ownedHost, &hostProps);
                int mt = hr == VK_SUCCESS && hostProps.memoryTypeBits
                    ? rdna4_find_mem(hostProps.memoryTypeBits & mr.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                    : -1;
                if (mt < 0) {
                    (void)munmap(ownedHost, (size_t)hostSpan);
                    if (err) *err = rdna4_err(22, @"host import has no coherent memory type for placement heap");
                    return NO;
                }
                memoryType = (uint32_t)mt;
                newMap = ownedHost;
                blockHostMapped = YES;
            } else if (_st < 2) {
                int mt = rdna4_find_mem(mr.memoryTypeBits,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                int hostMt = rdna4_find_mem(mr.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                if (mt >= 0 && hostMt >= 0 && hostMt != mt) fallbackMt = hostMt;
                if (mt < 0) mt = hostMt;
                blockHostMapped = mt >= 0;
                if (mt < 0 && !needHostMap) mt = rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (mt < 0) {
                    if (err) *err = rdna4_err(22, @"no compatible host-visible or device-local memory type for placement heap");
                    return NO;
                }
                memoryType = (uint32_t)mt;
            } else {
                int mt = rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                if (mt < 0) {
                    if (err) *err = rdna4_err(22, @"no device-local memory type for placement heap");
                    return NO;
                }
                memoryType = (uint32_t)mt;
            }

            VkImportMemoryHostPointerInfoEXT importInfo = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = ownedHost };
            VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                .pNext = hostImport ? &importInfo : NULL,
                .flags = RDNA4R.bufferAddress ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0 };
            VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &flags,
                .allocationSize = allocationSize, .memoryTypeIndex = memoryType };
            VkResult vr = vkAllocateMemory(RDNA4R.dev, &allocation, NULL, &newMemory);
            if (vr != VK_SUCCESS && fallbackMt >= 0) {
                memoryType = (uint32_t)fallbackMt;
                allocation.memoryTypeIndex = memoryType;
                vr = vkAllocateMemory(RDNA4R.dev, &allocation, NULL, &newMemory);
            }
            if (vr != VK_SUCCESS) {
                if (ownedHost) (void)munmap(ownedHost, (size_t)allocationSize);
                if (err) *err = rdna4_err(22, [NSString stringWithFormat:@"vkAllocateMemory(placement heap %llu) = %d",
                                               (unsigned long long)allocationSize, vr]);
                return NO;
            }
            if (blockHostMapped && !hostImport) {
                vr = vkMapMemory(RDNA4R.dev, newMemory, 0, VK_WHOLE_SIZE, 0, (void **)&newMap);
                if (vr != VK_SUCCESS) {
                    vkFreeMemory(RDNA4R.dev, newMemory, NULL);
                    if (err) *err = rdna4_err(24, [NSString stringWithFormat:@"vkMapMemory(placement heap) = %d", vr]);
                    return NO;
                }
            }
            uint64_t newBlock = 0;
            if (!rdna4hb_add_placement_block(&_hb, allocationSize, memoryType, blockHostMapped ? 1 : 0,
                                             hostImport ? 1 : 0, newMemory, newMap, &newBlock)) {
                if (blockHostMapped && !hostImport) vkUnmapMemory(RDNA4R.dev, newMemory);
                vkFreeMemory(RDNA4R.dev, newMemory, NULL);
                if (ownedHost) (void)munmap(ownedHost, (size_t)allocationSize);
                if (err) *err = rdna4_err(22, @"placement heap metadata rejected the Vulkan allocation");
                return NO;
            }
            if (hostImport) @synchronized ([GarnetDevice class]) { RDNA4R.impTotal += allocationSize; }
        }
        if (err) *err = rdna4_err(22, @"placement heap backing allocator did not converge");
        return NO;
    }
}
- (BOOL)rdna4BindSucceeded:(uint64_t)block lease:(uint64_t)lease owner:(id)owner {
    @synchronized (self) {
        if (!rdna4hb_handle_open(&_hb, block)) {
            RDNA4LOG("heap: failed to retain backing block %llu", (unsigned long long)block);
            return NO;
        }
        rdna4hb_lease *found = rdna4hb_find_lease(&_hb, lease);
        if (!found || found->block != block) {
            rdna4hb_handle_close(&_hb, block);
            RDNA4LOG("heap: backing lease %llu disappeared before bind", (unsigned long long)lease);
            return NO;
        }
        if (!found->dedicated) return YES;
        if (!owner) {
            rdna4hb_handle_close(&_hb, block);
            RDNA4LOG("heap: dedicated block %llu has no lifecycle owner", (unsigned long long)block);
            return NO;
        }
        void *retained = (__bridge_retained void *)owner;
        if (!rdna4hb_lease_owner(&_hb, lease, retained)) {
            id released = (__bridge_transfer id)retained;
            (void)released;
            rdna4hb_handle_close(&_hb, block);
            RDNA4LOG("heap: failed to attach the lifecycle owner to lease %llu", (unsigned long long)lease);
            return NO;
        }
        return YES;
    }
}
- (void)rdna4AbandonBacking:(uint64_t)lease {
    if (!lease) return;
    @synchronized (self) {
        rdna4hb_release(&_hb, lease, 0);
        rdna4hb_reap(&_hb, 0);
        [self rdna4CollectLocked];
    }
}
- (void)rdna4CloseBackingHandle:(uint64_t)block {
    @synchronized (self) {
        rdna4hb_handle_close(&_hb, block);
        [self rdna4CollectLocked];
    }
}
- (void)rdna4DetachBacking:(RDNA4HeapBacking *)backing block:(uint64_t)block lease:(uint64_t)lease {
    @synchronized (self) {
        rdna4hb_lease *found = rdna4hb_find_lease(&_hb, lease);
        BOOL deferred = found && found->dedicated && found->owner == (__bridge void *)backing;
        if (deferred) {
            // The lease already retains the handle owner. The collector destroys
            // the Vulkan handles and closes the block reference only after the
            // retirement serial clears.
            if (!found->retired) rdna4hb_release(&_hb, lease, rdna4_heap_release_serial());
            [self rdna4CollectLocked];
            return;
        }
        [backing destroyHandles];
        if (block) rdna4hb_handle_close(&_hb, block);
        found = rdna4hb_find_lease(&_hb, lease);
        void *owner = found ? found->owner : NULL;
        if (owner && rdna4hb_lease_owner_clear(&_hb, lease, (__bridge void *)backing)) {
            id retained = (__bridge_transfer id)owner;
            if ([retained respondsToSelector:@selector(destroyHandles)]) [retained destroyHandles];
        }
        [self rdna4CollectLocked];
    }
}
- (void)rdna4RetireResource:(id)resource logical:(uint64_t)bid lease:(uint64_t)lease {
    (void)resource;
    @synchronized (self) {
        if (bid) rdna4ha_free(&_ha, bid);
        if (lease) rdna4hb_release(&_hb, lease, rdna4_heap_release_serial());
        [self rdna4CollectLocked];
    }
}
- (VkDeviceMemory)rdna4BackingMemory:(uint64_t)block {
    @synchronized (self) {
        const rdna4hb_block *found = rdna4hb_find_block_const(&_hb, block);
        return found ? (VkDeviceMemory)found->mem : VK_NULL_HANDLE;
    }
}
- (uint8_t *)rdna4BackingMap:(uint64_t)block {
    @synchronized (self) {
        const rdna4hb_block *found = rdna4hb_find_block_const(&_hb, block);
        return found ? (uint8_t *)found->map : NULL;
    }
}
- (BOOL)rdna4Block:(uint64_t)block offset:(uint64_t)offset size:(uint64_t)size aliases:(id)other {
    if (!block || !size || !other || ![other respondsToSelector:@selector(rdna4BackingBlock)] ||
        ![other respondsToSelector:@selector(rdna4BackingOffset)] || ![other respondsToSelector:@selector(rdna4BackingSize)] ||
        ![other respondsToSelector:@selector(heap)] || [other heap] != self) return NO;
    uint64_t otherBlock = [other rdna4BackingBlock], otherOffset = [other rdna4BackingOffset], otherSize = [other rdna4BackingSize];
    if (otherBlock != block || !otherSize) return NO;
    return offset < otherOffset ? otherOffset - offset < size : offset - otherOffset < otherSize;
}
- (uint64_t)rdna4BackingBytes { @synchronized (self) { return _hb.backing; } }
- (uint64_t)rdna4BackingInUse { @synchronized (self) { return rdna4hb_live_bytes(&_hb); } }
- (void)rdna4Reap { @synchronized (self) { [self rdna4CollectLocked]; } }
- (BOOL)rdna4OptionsOK:(NSUInteger)o what:(const char *)what {
    unsigned st = (unsigned)((o >> 4) & 0xF), cc = (unsigned)(o & 0xF), hz = (unsigned)((o >> 8) & 3);
    if (st != _st || cc != _cc || (hz && hz != _hz)) {
        RDNA4LOG("heap %s NIL: requested storage %u cache %u hazard %u does not match the heap's %u / %u / %u (Apple's validation layer aborts on this)", what, st, cc, hz, _st, _cc, _hz); return NO; }
    return YES;
}
- (id)newBufferWithLength:(NSUInteger)len options:(MTLResourceOptions)o {
    if (_type != MTLHeapTypeAutomatic) { RDNA4_ONCE("heap newBufferWithLength:options: on a placement heap: nil (use offset:)"); return nil; }
    if (!len || len > _size || ![self rdna4OptionsOK:o what:"newBufferWithLength:options:"]) { if (!len || len > _size) RDNA4LOG("heap newBufferWithLength: NIL: length %lu (heap %lu)", (unsigned long)len, (unsigned long)_size); return nil; }
    NSError *e = nil; NSUInteger ro = ((NSUInteger)_st << 4) | _cc | ((NSUInteger)_hz << 8);
    RDNA4Buffer *b = [[RDNA4Buffer alloc] initWithDevice:_dev length:len options:ro heap:self error:&e];
    if (!b) { RDNA4LOG("heap newBuffer: failed: %s", e.localizedDescription.UTF8String); return nil; }
    [self rdna4TrackResource:b];
    return b;
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)td {
    if (_type != MTLHeapTypeAutomatic) { RDNA4_ONCE("heap newTextureWithDescriptor: on a placement heap: nil (use offset:)"); return nil; }
    if (!td || ![self rdna4OptionsOK:((NSUInteger)td.storageMode << 4) | (NSUInteger)td.cpuCacheMode | ((NSUInteger)td.hazardTrackingMode << 8) what:"newTextureWithDescriptor:"]) return nil;
    MTLTextureDescriptor *c = [td copy]; c.hazardTrackingMode = (MTLHazardTrackingMode)_hz;
    NSError *e = nil; RDNA4Texture *t = [[RDNA4Texture alloc] initWithDevice:_dev descriptor:c heap:self
                                                                             error:&e];
    if (!t) { RDNA4LOG("heap newTextureWithDescriptor NIL: pf %lu %lux%lu (%s)", (unsigned long)td.pixelFormat, (unsigned long)td.width, (unsigned long)td.height, e.localizedDescription.UTF8String); return nil; }
    [self rdna4TrackResource:t];
    return t;
}
// offset (placement) variants: valid only on MTLHeapTypePlacement; nil on MTLHeapTypeAutomatic.
- (id)newBufferWithLength:(NSUInteger)l options:(MTLResourceOptions)o offset:(NSUInteger)off {
    if (_type != MTLHeapTypePlacement) { RDNA4_ONCE("heap newBufferWithLength:options:offset: on an automatic heap: nil (as an Apple-silicon Mac)"); return nil; }
    if (!l || off >= _size || l > _size - off || ![self rdna4OptionsOK:o what:"newBufferWithLength:options:offset:"]) {
        if (!l || off >= _size || l > _size - off) RDNA4LOG("placement heap newBuffer: NIL: length %lu offset %lu (heap %lu)", (unsigned long)l, (unsigned long)off, (unsigned long)_size);
        return nil;
    }
    NSError *e = nil; NSUInteger ro = ((NSUInteger)_st << 4) | _cc | ((NSUInteger)_hz << 8);
    RDNA4Buffer *b = [[RDNA4Buffer alloc] initWithDevice:_dev length:l options:ro heap:self offset:off error:&e];
    if (!b) { RDNA4LOG("placement heap newBuffer: failed: %s", e.localizedDescription.UTF8String); return nil; }
    [self rdna4TrackResource:b];
    return b;
}
- (id)newBufferWithLength:(NSUInteger)l options:(MTLResourceOptions)o atOffset:(NSUInteger)off { return [self newBufferWithLength:l options:o offset:off]; }
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off {
    if (_type != MTLHeapTypePlacement) { RDNA4_ONCE("heap newTextureWithDescriptor:offset: on an automatic heap: nil (as an Apple-silicon Mac)"); return nil; }
    if (!d || off >= _size || ![self rdna4OptionsOK:((NSUInteger)d.storageMode << 4) | (NSUInteger)d.cpuCacheMode | ((NSUInteger)d.hazardTrackingMode << 8) what:"newTextureWithDescriptor:offset:"]) {
        if (!d || off >= _size) RDNA4LOG("placement heap newTexture: NIL: offset %lu (heap %lu)", (unsigned long)off, (unsigned long)_size);
        return nil;
    }
    MTLTextureDescriptor *c = [d copy]; c.hazardTrackingMode = (MTLHazardTrackingMode)_hz;
    NSError *e = nil; RDNA4Texture *t = [[RDNA4Texture alloc] initWithDevice:_dev descriptor:c heap:self offset:off error:&e];
    if (!t) { RDNA4LOG("placement heap newTexture NIL: pf %lu %lux%lu offset %lu (%s)", (unsigned long)d.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)off, e.localizedDescription.UTF8String); return nil; }
    [self rdna4TrackResource:t];
    return t;
}
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d atOffset:(NSUInteger)off { return [self newTextureWithDescriptor:d offset:off]; }
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)al {
    @synchronized (self) {
        // Query correctness is part of the allocation contract: retire cleared
        // ranges and central-block reclamation first, then bound the logical
        // answer by the physical ranges that could actually satisfy a request.
        [self rdna4CollectLocked];
        rdna4dc_caps caps = rdna4_device_caps();
        uint64_t granularity = RDNA4R.lim.bufferImageGranularity ? RDNA4R.lim.bufferImageGranularity : 1;
        if (_type == MTLHeapTypePlacement) {
            uint64_t avail = rdna4hb_placement_max_available(&_hb, al, granularity);
            uint64_t budget = rdna4dc_heap_available(&caps, rdna4hb_placement_used(&_hb));
            return (NSUInteger)rdna4dc_min(avail, budget);
        }
        uint64_t logical = rdna4ha_maxavail(&_ha, al);
        uint64_t budget = rdna4dc_heap_available(&caps, _ha.used);
        // A Shared/Managed heap has two backing classes: host-mapped buffers and
        // DEVICE_LOCAL optimal images. The query has no resource kind, so report
        // the largest range that either class can satisfy.
        uint64_t physical = rdna4hb_max_available(&_hb, al, granularity, 0);
        if (_st < 2) physical = MAX(physical, rdna4hb_max_available(&_hb, al, granularity, 1));
        return (NSUInteger)rdna4dc_min(rdna4dc_min(logical, budget), physical);
    }
}
- (NSUInteger)setPurgeableState:(NSUInteger)st {
    @synchronized (self) { NSUInteger prev = _purge; if (st >= 2 && st <= 4) _purge = st; RDNA4_ONCE("heap setPurgeableState: tracked as a flag only (no memory is ever reclaimed)"); return prev; }
}
- (void)rdna4TrackResource:(id)resource { @synchronized (self) { if (resource) [_members addObject:resource]; } }
- (NSArray *)rdna4LiveResources { @synchronized (self) { return _members.allObjects; } }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)size { return _size; }
- (NSUInteger)usedSize { @synchronized (self) { return _type == MTLHeapTypePlacement ? (NSUInteger)rdna4hb_placement_used(&_hb) : (NSUInteger)_ha.used; } }
- (NSUInteger)currentAllocatedSize { return _size; }
- (NSUInteger)allocatedSize { return _size; }
- (MTLStorageMode)storageMode { return (MTLStorageMode)_st; }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)_cc; }
- (MTLHazardTrackingMode)hazardTrackingMode { return (MTLHazardTrackingMode)_hz; }
- (NSUInteger)resourceOptions { return ((NSUInteger)_st << 4) | _cc | ((NSUInteger)_hz << 8); }
- (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; }
- (MTLHeapType)type { return _type; }
- (NSUInteger)protectionOptions { return 0; }
- (uint64_t)gpuAddress { RDNA4_ONCE("heap gpuAddress: no GPU virtual address is exported; 0"); return 0; }
- (uint64_t)memoryPoolId { return 0; }
// -[_MTLHeap description] calls -formattedDescription: (found on an Apple-silicon Mac: it sends -usedSize and more); a plain string here, NOT [self description] (recursion)
- (NSString *)formattedDescription:(NSUInteger)indent { (void)indent; return [NSString stringWithFormat:@"<RDNA4Heap: %p> size %lu used %lu type %lu storage %u cache %u hazard %u label %@", (__bridge void *)self, (unsigned long)_size, (unsigned long)[self usedSize], (unsigned long)_type, _st, _cc, _hz, _lbl]; }
@end

