// ---------------------------------------------------------------------------------------------------------------
// Metal API gap census fixes (an internal design note). Shared pieces:
//   RDNA4_ONCE          one log line per call site per process (no-op stubs are called thousands of times a second by the compositor)
//   RDNA4_RES_IVARS /   state + the public/SPI MTLResource surface that QuartzCore / SkyLight send to every texture and buffer
//   RDNA4_RESOURCE_SPI  (census: -setResponsibleProcess: 7450, -protectionOptions 6741, -heap 1397, -virtualAddress 1092 in 45 s)
// ---------------------------------------------------------------------------------------------------------------
static _Atomic uint64_t rdna4_uid_ctr;
static uint64_t rdna4_resource_uid(_Atomic uint64_t *uid) {
    uint64_t current = atomic_load_explicit(uid, memory_order_relaxed);
    if (current) return current;
    uint64_t candidate = atomic_fetch_add_explicit(&rdna4_uid_ctr, 1, memory_order_relaxed) + 1;
    uint64_t expected = 0;
    return atomic_compare_exchange_strong_explicit(uid, &expected, candidate,
        memory_order_relaxed, memory_order_relaxed) ? candidate : expected;
}
static _Atomic uint64_t rdna4_alloc_total;   // bytes of VkDeviceMemory held by live buffers and textures (-[MTLDevice currentAllocatedSize])
static _Atomic uint64_t rdna4_heap_reserved;   // heap capacity promises; not device memory and not deducted from rdna4_alloc_total
// Heap membership (m11 H1): a resource made by -[RDNA4Heap newBuffer.../newTexture...] is a normal VkBuffer/VkImage that carries its heap, the offset the heap
// assigned, the bytes it was charged and the block id; the budget goes back to the heap at -makeAliasable or at dealloc, whichever is first.
@interface RDNA4HeapBacking : NSObject
- (instancetype)initWithBuffer:(VkBuffer)buffer image:(VkImage)image view:(VkImageView)view;
- (void)addView:(VkImageView)view;
- (void)destroyHandles;
@end
@interface RDNA4Heap : _MTLHeap
- (void)rdna4Free:(uint64_t)bid;
- (BOOL)rdna4AcquireBackingForBuffer:(VkBuffer)buffer image:(VkImage)image requirements:(VkMemoryRequirements)mr
                           dedicated:(BOOL)dedicated hostImport:(BOOL)hostImport
                                 mem:(VkDeviceMemory *)mem offset:(uint64_t *)offset map:(uint8_t **)map
                               block:(uint64_t *)block lease:(uint64_t *)lease rangeSize:(uint64_t *)rangeSize
                               error:(NSError **)err;
- (BOOL)rdna4BindSucceeded:(uint64_t)block lease:(uint64_t)lease owner:(id)owner;
- (void)rdna4AbandonBacking:(uint64_t)lease;
- (void)rdna4CloseBackingHandle:(uint64_t)block;
- (void)rdna4DetachBacking:(RDNA4HeapBacking *)backing block:(uint64_t)block lease:(uint64_t)lease;
- (void)rdna4RetireResource:(id)resource logical:(uint64_t)bid lease:(uint64_t)lease;
- (BOOL)rdna4ReserveRequirements:(VkMemoryRequirements)mr offset:(NSUInteger *)offset id:(uint64_t *)bid;
- (VkDeviceMemory)rdna4BackingMemory:(uint64_t)block;
- (uint8_t *)rdna4BackingMap:(uint64_t)block;
- (BOOL)rdna4Block:(uint64_t)block offset:(uint64_t)offset size:(uint64_t)size aliases:(id)other;
- (uint64_t)rdna4BackingBytes;
- (uint64_t)rdna4BackingInUse;
- (void)rdna4Reap;
- (void)rdna4TrackResource:(id)resource;
- (NSArray *)rdna4LiveResources;
@end
#define RDNA4_RES_IVARS NSUInteger _prot; int _respPid; BOOL _respSet; _Atomic uint64_t _uid; RDNA4Heap *_hpHeap; NSUInteger _hpOff, _hpSize; uint64_t _hpId; BOOL _hpAl; \
    uint64_t _hpBlk, _hpLease, _hpBindOff, _hpPhysSize; BOOL _hpDedicated; RDNA4HeapBacking *_hpBacking;
#define RDNA4_HEAP_RETIRE do { if (_hpHeap && _hpId && !_hpAl) { _hpAl = YES; [_hpHeap rdna4RetireResource:self logical:_hpId lease:_hpLease]; } } while (0)
// Detach decides whether Vulkan handles can die now (shared block) or must stay
// with the dedicated lease's retained owner until its retirement completes.
#define RDNA4_HEAP_DEALLOC do { \
    RDNA4_HEAP_RETIRE; \
    if (_hpHeap && _hpBlk) { \
        uint64_t b_ = _hpBlk; \
        [_hpHeap rdna4DetachBacking:_hpBacking block:b_ lease:_hpLease]; \
        _hpBacking = nil; \
        _hpBlk = _hpLease = _hpBindOff = _hpPhysSize = 0; _hpDedicated = NO; \
    } \
} while (0)
#define RDNA4_RESOURCE_SPI \
    - (id)heap { return [self rdna4ResourceHeap]; } \
    - (NSUInteger)heapOffset { return 0; } /* Apple-silicon: 0 for every resource of an automatic heap, although gpuAddress differs by the real offsets (measured) */ \
    - (BOOL)isAliasable { return [self rdna4ResourceAliasable]; } \
    - (void)makeAliasable { [self rdna4ResourceMakeAliasable]; } \
    - (NSUInteger)setPurgeableState:(NSUInteger)st { (void)st; return 2; } \
    - (BOOL)isPurgeable { return NO; } \
    - (int)setOwnerWithIdentity:(uint32_t)o { (void)o; return (int)0xE00002C2; } \
    - (BOOL)doesAliasResource:(id)o { return [self rdna4ResourceAliases:o]; } \
    - (BOOL)doesAliasAnyResources:(const id __unsafe_unretained *)o count:(NSUInteger)n { \
        if (!o) return NO; for (NSUInteger i_ = 0; i_ < n; i_++) if ([self doesAliasResource:o[i_]]) return YES; return NO; } \
    - (BOOL)doesAliasAllResources:(const id __unsafe_unretained *)o count:(NSUInteger)n { \
        if (!o || !n) return NO; for (NSUInteger i_ = 0; i_ < n; i_++) if (![self doesAliasResource:o[i_]]) return NO; return YES; } \
    - (BOOL)isComplete { return YES; } \
    - (BOOL)isWriteComplete { return YES; } \
    - (void)waitUntilComplete {} \
    - (NSUInteger)protectionOptions { return _prot; } \
    - (int)responsibleProcess { return _respSet ? _respPid : (int)getpid(); } \
    - (void)setResponsibleProcess:(int)p { _respPid = p; _respSet = YES; } \
    - (NSUInteger)unfilteredResourceOptions { return [self resourceOptions]; } \
    - (uint64_t)uniqueIdentifier { return rdna4_resource_uid(&_uid); } \
    - (MTLResourceID)gpuResourceID API_AVAILABLE(macos(13.0)) { MTLResourceID identifier; identifier._impl = self.uniqueIdentifier; return identifier; }

// ---------------------------------------------------------------------------------------------------------------
// RDNA4Buffer: minimal MTLBuffer (10a: Shared only) = VkBuffer + host-visible coherent VkDeviceMemory, mapped once.
// ---------------------------------------------------------------------------------------------------------------
@interface RDNA4Buffer : _MTLResource {
    id _dev; NSUInteger _len; void *_ptr; VkBuffer _vkbuf; VkDeviceMemory _vkmem; NSString *_lbl; NSUInteger _opts; NSUInteger _asz; uint64_t _gpuAddress;
    VkDeviceSize _vkoff; uint64_t _impSpan, _ownedHostSpan;   // client range within the imported buffer; never a pool/heap memory-binding offset
    RDNA4_RES_IVARS
    BOOL _noacct;   // heap resource: the heap's size is in rdna4_alloc_total, not this buffer's memory
    RDNA4Mem _pm; BOOL _pooled;   // P1: the memory comes from the pool (slab range / recycled allocation / own allocation made through rdna4_mem_alloc); _vkmem is then NULL
    void (^_deallocator)(void *, NSUInteger); BOOL _imported; BOOL _hostReadOnly;   // no-copy buffer: client memory imported as host memory; the block is called once, at release
}
- (instancetype)initWithDevice:(id)dev length:(NSUInteger)len options:(NSUInteger)opts error:(NSError **)err;
- (instancetype)initWithDevice:(id)dev length:(NSUInteger)len options:(NSUInteger)opts heap:(RDNA4Heap *)heap error:(NSError **)err;
- (RDNA4Heap *)rdna4ResourceHeap;
- (BOOL)rdna4ResourceAliasable;
- (void)rdna4ResourceMakeAliasable;
- (BOOL)rdna4ResourceAliases:(id)other;
- (instancetype)initWithDevice:(id)dev bytesNoCopy:(void *)p length:(NSUInteger)len options:(NSUInteger)opts deallocator:(void (^)(void *, NSUInteger))d error:(NSError **)err;
- (instancetype)rdna4ImportHostPointer:(void *)p length:(NSUInteger)len ownedSpan:(uint64_t)ownedSpan deallocator:(void (^)(void *, NSUInteger))d error:(NSError **)err;
- (BOOL)rdna4Imported;
- (BOOL)rdna4HostReadOnly;
- (void)rdna4CheckHostReadOnly:(const void *)p length:(NSUInteger)len;
- (VkDeviceMemory)rdna4NativeMemory:(uint64_t *)offset;
- (void)rdna4AdoptHeap:(RDNA4Heap *)h offset:(NSUInteger)o size:(NSUInteger)sz bid:(uint64_t)bid;
- (VkBuffer)vkBuffer;
- (VkDeviceSize)rdna4VkOffset;
- (uint64_t)gpuAddress;
- (uint64_t)rdna4BackingBlock;
- (uint64_t)rdna4BackingOffset;
- (uint64_t)rdna4BackingSize;
- (void)rdna4AbortHeapConstruction;
- (void)rdna4DestroyRetiredBacking;
- (BOOL)rdna4ZeroLargePrivate:(NSUInteger)len unresolved:(BOOL *)unresolved error:(NSError **)err;
@end

enum { RDNA4NC_ALIGN_STORAGE, RDNA4NC_ALIGN_UNIFORM, RDNA4NC_ALIGN_INDEX, RDNA4NC_ALIGN_INDIRECT, RDNA4NC_ALIGN_IMAGE };
static BOOL rdna4nc_vk_offset_ok(VkDeviceSize off, VkDeviceSize align, unsigned use) {
    if (!align || off % align == 0) return YES;
    static _Atomic unsigned warned;
    static const char *const names[] = { "storage descriptor", "uniform descriptor", "index buffer", "indirect command", "buffer/image copy" };
    if (!(atomic_fetch_or(&warned, 1u << use) & (1u << use)))
        RDNA4LOG("NoCopy Vulkan alignment limit: %s offset %llu must be a multiple of %llu; operation refused before encoding", names[use], (unsigned long long)off, (unsigned long long)align);
    return NO;
}

static void rdna4_full_barrier(VkCommandBuffer cb);
// The heap requires requirements2 so dedicated requirements are never guessed.
// Non-heap callers retain the Vulkan 1.0 fallback for an old loader, where they
// cannot choose a dedicated allocation anyway.
static BOOL rdna4_requirements2(VkBuffer buffer, VkImage image, VkMemoryRequirements *mr,
                                BOOL *prefersDedicated, BOOL *requiresDedicated,
                                BOOL requireDedicatedInfo) {
    if (requireDedicatedInfo &&
        ((buffer && !RDNA4P.bmr2) || (image && !RDNA4P.imr2))) return NO;
    VkMemoryDedicatedRequirements dedicated = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
    VkMemoryRequirements2 req = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated };
    if (buffer && RDNA4P.bmr2) {
        VkBufferMemoryRequirementsInfo2 info = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = buffer };
        RDNA4P.bmr2(RDNA4R.dev, &info, &req);
    } else if (image && RDNA4P.imr2) {
        VkImageMemoryRequirementsInfo2 info = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = image };
        RDNA4P.imr2(RDNA4R.dev, &info, &req);
    } else {
        if (buffer) vkGetBufferMemoryRequirements(RDNA4R.dev, buffer, &req.memoryRequirements);
        else if (image) vkGetImageMemoryRequirements(RDNA4R.dev, image, &req.memoryRequirements);
        memset(&dedicated, 0, sizeof dedicated);
    }
    if (mr) *mr = req.memoryRequirements;
    if (prefersDedicated) *prefersDedicated = dedicated.prefersDedicatedAllocation != VK_FALSE;
    if (requiresDedicated) *requiresDedicated = dedicated.requiresDedicatedAllocation != VK_FALSE;
    return YES;
}
// One source of truth for the heap buffer's create info. `external` is non-NULL
// only for the >64 MiB Shared/Managed heap import path.
static VkBufferCreateInfo rdna4_heap_buffer_create_info(NSUInteger len, VkExternalMemoryBufferCreateInfo *external) {
    return (VkBufferCreateInfo){ .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = len,
        .pNext = external,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                 (RDNA4R.bufferAddress ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0) };
}
static void *rdna4nc_owned_allocate(uint64_t len, uint64_t hostAlign, uint64_t *span) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || !hostAlign || (hostAlign & (hostAlign - 1))) return NULL;
    uint64_t align = MAX(hostAlign, (uint64_t)page), base, delta, size;
    if (rdna4nc_import_range(0, len, align, &base, &size, &delta) || size > SIZE_MAX - align) return NULL;
    size_t reserved = (size_t)(size + align);
    void *mapping = mmap(NULL, reserved, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) return NULL;
    uintptr_t start = ((uintptr_t)mapping + align - 1) & ~(uintptr_t)(align - 1);
    size_t prefix = start - (uintptr_t)mapping, suffix = reserved - prefix - (size_t)size;
    if (prefix) (void)munmap(mapping, prefix);
    if (suffix) (void)munmap((void *)(start + size), suffix);
    *span = size;
    return (void *)start;
}

// The heap owns physical memory, not the Metal resource object. A dedicated block must
// outlive the resource's Vulkan handles until the completion point, so retain this small
// handle owner in the lease instead of a raw RDNA4Buffer/RDNA4Texture pointer.
@implementation RDNA4HeapBacking {
    VkBuffer _buffer;
    VkImage _image;
    NSMutableArray<NSNumber *> *_views;
    BOOL _destroyed;
}
- (instancetype)initWithBuffer:(VkBuffer)buffer image:(VkImage)image view:(VkImageView)view {
    self = [super init];
    if (!self) return nil;
    _buffer = buffer;
    _image = image;
    _views = [NSMutableArray array];
    if (view) [_views addObject:@((uintptr_t)view)];
    return self;
}
- (void)addView:(VkImageView)view {
    if (!view) return;
    @synchronized (self) {
        if (_destroyed) {
            if (RDNA4R.ok) vkDestroyImageView(RDNA4R.dev, view, NULL);
            return;
        }
        [_views addObject:@((uintptr_t)view)];
    }
}
- (void)destroyHandles {
    @synchronized (self) {
        if (_destroyed) return;
        _destroyed = YES;
        if (RDNA4R.ok) {
            for (NSNumber *v in _views) vkDestroyImageView(RDNA4R.dev, (VkImageView)(uintptr_t)v.unsignedLongLongValue, NULL);
            if (_image) vkDestroyImage(RDNA4R.dev, _image, NULL);
            if (_buffer) vkDestroyBuffer(RDNA4R.dev, _buffer, NULL);
        }
        _views = nil;
        _buffer = VK_NULL_HANDLE;
        _image = VK_NULL_HANDLE;
    }
}
- (void)dealloc {
    [self destroyHandles];
}
@end

@implementation RDNA4Buffer
- (instancetype)initWithDevice:(id)dev length:(NSUInteger)len options:(NSUInteger)opts error:(NSError **)err {
    return [self initWithDevice:dev length:len options:opts heap:nil error:err];
}
- (instancetype)initWithDevice:(id)dev length:(NSUInteger)len options:(NSUInteger)opts heap:(RDNA4Heap *)heap error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    _dev = dev; _len = len; BOOL nozero = (opts & RDNA4_OPT_NOZERO) != 0; _opts = opts & ~RDNA4_OPT_NOZERO;   // P1: the marker never leaves this init
    opts = _opts;
    _hpHeap = heap;
    if (!rdna4_radv_open(err)) return nil;
    rdna4dc_caps caps = rdna4_device_caps();
    if (!rdna4dc_buffer_ok(&caps, len)) { if (err) *err = rdna4_err(25, @"buffer length exceeds the implemented logical allocation limit"); return nil; }
    unsigned sm = (unsigned)((opts >> 4) & 0xF);
    if (sm > 2) { if (err) *err = rdna4_err(25, @"buffer storage mode Memoryless is not supported"); return nil; }
    if (!heap && sm < 2 && len > (64ull << 20)) {
        if (!RDNA4R.hostExt || !rdna4_vkGetMHPP) { if (err) *err = rdna4_err(112, @"large Shared/Managed buffers require coherent host import"); return nil; }
        _ptr = rdna4nc_owned_allocate(len, RDNA4R.hostAlign, &_ownedHostSpan);
        if (!_ptr) { if (err) *err = rdna4_err(22, @"cannot reserve aligned CPU memory for the shared buffer"); return nil; }
        return [self rdna4ImportHostPointer:_ptr length:len ownedSpan:_ownedHostSpan deallocator:nil error:err];
    }
    BOOL heapHost = heap && sm < 2;
    BOOL heapImport = heapHost && len > (64ull << 20);
    if (heapImport && (!RDNA4R.hostExt || !rdna4_vkGetMHPP)) { if (err) *err = rdna4_err(112, @"large Shared/Managed heap buffers require coherent host import"); return nil; }
    VkExternalMemoryBufferCreateInfo external = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo bc = rdna4_heap_buffer_create_info(len, heapImport ? &external : NULL);
    VkResult r = vkCreateBuffer(RDNA4R.dev, &bc, NULL, &_vkbuf);
    if (r != VK_SUCCESS) { if (err) *err = rdna4_err(20, [NSString stringWithFormat:@"vkCreateBuffer = %d", r]); return nil; }
    VkMemoryRequirements mr; BOOL prefersDedicated = NO, requiresDedicated = NO;
    if (!rdna4_requirements2(_vkbuf, VK_NULL_HANDLE, &mr, &prefersDedicated, &requiresDedicated, heap != nil)) {
        if (err) *err = rdna4_err(22, @"heap buffers require vkGetBufferMemoryRequirements2 and dedicated requirements");
        [self rdna4AbortHeapConstruction];
        return nil;
    }
    BOOL dedicated = prefersDedicated || requiresDedicated;
    _asz = (NSUInteger)mr.size;
    if (heap) {
        if (![heap rdna4ReserveRequirements:mr offset:&_hpOff id:&_hpId]) {
            if (err) *err = rdna4_err(22, [NSString stringWithFormat:@"heap has no logical range for a %llu-byte buffer requirement", (unsigned long long)mr.size]);
            [self rdna4AbortHeapConstruction];
            return nil;
        }
        _hpSize = (NSUInteger)mr.size;
        VkDeviceMemory hmem = VK_NULL_HANDLE; uint64_t bindOff = 0, rangeSize = 0; uint8_t *map = NULL;
        if (![heap rdna4AcquireBackingForBuffer:_vkbuf image:VK_NULL_HANDLE requirements:mr dedicated:dedicated hostImport:heapImport
                                            mem:&hmem offset:&bindOff map:&map block:&_hpBlk lease:&_hpLease rangeSize:&rangeSize error:err]) {
            [self rdna4AbortHeapConstruction];
            return nil;
        }
        _hpBindOff = bindOff; _hpPhysSize = rangeSize; _hpDedicated = dedicated; _noacct = YES;
        r = vkBindBufferMemory(RDNA4R.dev, _vkbuf, hmem, bindOff);
        if (r != VK_SUCCESS) {
            if (err) *err = rdna4_err(23, [NSString stringWithFormat:@"vkBindBufferMemory = %d (heap block, offset %llu)", r, (unsigned long long)bindOff]);
            [self rdna4AbortHeapConstruction];
            return nil;
        }
        if (map) _ptr = map + bindOff;
        if (RDNA4R.bufferAddress) {
            VkBufferDeviceAddressInfo address = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = _vkbuf };
            _gpuAddress = rdna4_vkGetBufferAddress(RDNA4R.dev, &address);
            if (!_gpuAddress) {
                if (err) *err = rdna4_err(26, @"Vulkan returned a zero GPU address for the heap buffer");
                [self rdna4AbortHeapConstruction];
                return nil;
            }
        }
        _hpBacking = [[RDNA4HeapBacking alloc] initWithBuffer:_vkbuf image:VK_NULL_HANDLE view:VK_NULL_HANDLE];
        if (!_hpBacking || ![heap rdna4BindSucceeded:_hpBlk lease:_hpLease owner:_hpBacking]) {
            if (err) *err = rdna4_err(22, @"cannot retain the heap buffer's lifecycle backing");
            [self rdna4AbortHeapConstruction];
            return nil;
        }
        if (sm == 2 && len > RDNA4_BUFFER_LEGACY_MAX && !nozero) {
            BOOL unresolved = NO;
            if (![self rdna4ZeroLargePrivate:len unresolved:&unresolved error:err]) {
                if (!unresolved) [self rdna4AbortHeapConstruction];
                return nil;
            }
        }
        RDNA4LOGR("RDNA4Buffer %p: heap %lu bytes (req %llu, block %llu, bind %llu) mapped at %p", (__bridge void *)self, (unsigned long)len,
               (unsigned long long)mr.size, (unsigned long long)_hpBlk, (unsigned long long)_hpBindOff, _ptr);
        return self;
    }
    // 11e: Shared (0) and Managed (1) are host-visible coherent memory (the CPU and GPU copies are one, so didModifyRange: is a no-op);
    // Private (2) is DEVICE_LOCAL with no CPU mapping (contents = NULL). Memoryless (3) is refused.
    int mt = sm == 2 ? rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                     : rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) { if (err) *err = rdna4_err(21, sm == 2 ? @"no device-local memory type" : @"no host-visible coherent memory type"); return nil; }
    VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = RDNA4R.bufferAddress ? &flags : NULL,
        .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    if (RDNA4P.on) {   // P1: slab range / recycled allocation; the host-visible memory is mapped once by the pool (zeroed here unless the caller said it will overwrite it)
        r = rdna4_mem_alloc((uint32_t)mt, mr, sm != 2, sm != 2 && !nozero, _vkbuf, VK_NULL_HANDLE, &_pm);
        if (r == VK_SUCCESS) {
            _pooled = YES; atomic_fetch_add(&rdna4_alloc_total, (uint64_t)_asz);
            r = vkBindBufferMemory(RDNA4R.dev, _vkbuf, _pm.mem, _pm.off);
            if (r != VK_SUCCESS) { if (err) *err = rdna4_err(23, [NSString stringWithFormat:@"vkBindBufferMemory = %d (pool, offset %llu)", r, (unsigned long long)_pm.off]); return nil; }
            if (sm != 2) _ptr = _pm.map;
        }
    } else {
        r = vkAllocateMemory(RDNA4R.dev, &ma, NULL, &_vkmem);
        if (r == VK_SUCCESS) {
            atomic_fetch_add(&rdna4_alloc_total, (uint64_t)_asz);
            r = vkBindBufferMemory(RDNA4R.dev, _vkbuf, _vkmem, 0);
            if (r != VK_SUCCESS) { if (err) *err = rdna4_err(23, [NSString stringWithFormat:@"vkBindBufferMemory = %d", r]); return nil; }
            if (sm != 2) {
                r = vkMapMemory(RDNA4R.dev, _vkmem, 0, VK_WHOLE_SIZE, 0, &_ptr);
                if (r != VK_SUCCESS) { if (err) *err = rdna4_err(24, [NSString stringWithFormat:@"vkMapMemory = %d", r]); return nil; }
            }
        }
    }
    if (r != VK_SUCCESS) {
        if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY && !heap && sm < 2 && RDNA4R.hostExt && rdna4_vkGetMHPP) {
            void *owned = rdna4nc_owned_allocate(len, RDNA4R.hostAlign, &_ownedHostSpan);
            if (owned) {
                vkDestroyBuffer(RDNA4R.dev, _vkbuf, NULL);
                _vkbuf = VK_NULL_HANDLE;
                _ptr = owned;
                return [self rdna4ImportHostPointer:owned length:len ownedSpan:_ownedHostSpan deallocator:nil error:err];
            }
        }
        if (err) *err = rdna4_err(22, [NSString stringWithFormat:RDNA4P.on ? @"vkAllocateMemory = %d (pool)" : @"vkAllocateMemory = %d", r]);
        return nil;
    }
    if (RDNA4R.bufferAddress) {
        VkBufferDeviceAddressInfo address = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = _vkbuf };
        _gpuAddress = rdna4_vkGetBufferAddress(RDNA4R.dev, &address);
        if (!_gpuAddress) { if (err) *err = rdna4_err(26, @"Vulkan returned a zero GPU address for the bound buffer"); return nil; }
    }
    if (sm == 2 && len > RDNA4_BUFFER_LEGACY_MAX && !nozero &&
        ![self rdna4ZeroLargePrivate:len unresolved:NULL error:err]) return nil;
    RDNA4LOGR("RDNA4Buffer %p: %lu bytes (alloc %llu, memtype %d) mapped at %p", (__bridge void *)self, (unsigned long)len,
           (unsigned long long)mr.size, mt, _ptr);
    return self;
}
- (BOOL)rdna4ZeroLargePrivate:(NSUInteger)len unresolved:(BOOL *)unresolved error:(NSError **)err {
    if (unresolved) *unresolved = NO;
    RDNA4Buffer *edge = nil;
    NSUInteger whole = len & ~(NSUInteger)3, tail = len - whole;
    if (tail) {
        edge = [[RDNA4Buffer alloc] initWithDevice:_dev length:4 options:0 error:err];
        if (!edge || !edge.contents) return NO;
        memset(edge.contents, 0, 4);
    }
    // High-pool children are not CPU mapped or cleared by the kernel. The
    // recording block retains self and the edge so an unresolved submit cannot
    // tear down either resource while the GPU may still use it.
    return rdna4_oneshot(err, ^(VkCommandBuffer cmd) {
        (void)self;
        if (whole) vkCmdFillBuffer(cmd, self->_vkbuf, 0, whole, 0);
        if (tail) {
            VkBufferCopy copy = { [edge rdna4VkOffset], whole, tail };
            vkCmdCopyBuffer(cmd, [edge vkBuffer], self->_vkbuf, 1, &copy);
        }
        rdna4_full_barrier(cmd);
    }, unresolved);
}
// 12 (no-copy): the enclosing aligned range of client memory is imported with VK_EXT_external_memory_host (the IOSurface texture path, 11h.6). The CPU pointer
// IS the client's (contents == p); GPU writes are visible to the CPU after the writing command buffer completed, CPU writes before commit are seen by the GPU
// (host-visible import: the memory type is host coherent). Private storage and import spans > 64 MiB return nil.
#include <mach/vm_region.h>
static uint32_t rdna4nc_mach_prot(vm_prot_t p) {
    uint32_t flags = 0;
    if (p & VM_PROT_READ) flags |= RDNA4NC_PROT_READ;
    if (p & VM_PROT_WRITE) flags |= RDNA4NC_PROT_WRITE;
    if (p & VM_PROT_EXECUTE) flags |= RDNA4NC_PROT_EXEC;
    return flags;
}
static int rdna4nc_host_readonly(const void *p, NSUInteger len) {
    mach_vm_address_t cursor = (mach_vm_address_t)(uintptr_t)p;
    if ((uint64_t)len > UINT64_MAX - cursor) return -1;
    const mach_vm_address_t end = cursor + (uint64_t)len;
    int readonly = 0;
    while (cursor < end) {
        mach_vm_address_t address = cursor; mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t)&info, &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS || !size || address > cursor || cursor - address >= size) return -1;
        uint64_t part = MIN((uint64_t)size - (cursor - address), end - cursor);
        struct rdna4nc_region region = { address, size, rdna4nc_mach_prot(info.protection) };
        int verdict = rdna4nc_range_readonly(&region, 1, cursor, part);
        if (verdict < 0) return -1;
        readonly |= verdict;
        cursor += part;
    }
    return readonly;
}
static void rdna4nc_region_summary(void *p, NSUInteger len, char *out, size_t cap) {
    if (!cap) return;
    out[0] = '\0';
    mach_vm_address_t cursor = (mach_vm_address_t)(uintptr_t)p;
    if ((mach_vm_size_t)len > UINT64_MAX - cursor) { snprintf(out, cap, "range-overflow; any_page_not_writable=unknown"); return; }
    const mach_vm_address_t end = cursor + (mach_vm_size_t)len;
    size_t used = 0; unsigned nregions = 0; int anyNotWritable = 0, unknown = 0;
    while (cursor < end) {
        mach_vm_address_t address = cursor; mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t)&info, &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS || !size || address >= end) { unknown = 1; break; }
        if (address > cursor) { unknown = 1; cursor = address; }
        mach_vm_address_t regionEnd = size > UINT64_MAX - address ? end : address + size;
        if (!(info.protection & VM_PROT_WRITE)) anyNotWritable = 1;
        if (nregions < 4 && used < cap) {
            char prot[4], maxProt[4];
            rdna4nc_protection_text(rdna4nc_mach_prot(info.protection), prot);
            rdna4nc_protection_text(rdna4nc_mach_prot(info.max_protection), maxProt);
            int wrote = snprintf(out + used, cap - used, "%s%s/%s-%s", nregions ? "," : "",
                                 prot, maxProt, info.shared ? "shared" : "private");
            if (wrote > 0) used += (size_t)wrote < cap - used ? (size_t)wrote : cap - used - 1;
        }
        nregions++;
        if (regionEnd <= cursor) { unknown = 1; break; }
        cursor = regionEnd;
    }
    if (!nregions) snprintf(out, cap, "unavailable");
    else if (nregions > 4 && used < cap) (void)snprintf(out + used, cap - used, ",...");
    size_t usedNow = strnlen(out, cap);
    if (usedNow < cap) (void)snprintf(out + usedNow, cap - usedNow, "; any_page_not_writable=%s",
                                      anyNotWritable ? "yes" : unknown ? "unknown" : "no");
}
- (instancetype)initWithDevice:(id)dev bytesNoCopy:(void *)p length:(NSUInteger)len options:(NSUInteger)opts deallocator:(void (^)(void *, NSUInteger))d error:(NSError **)err {
    self = [super init]; if (!self) return nil;
    _dev = dev; _len = len; _opts = opts;
    return [self rdna4ImportHostPointer:p length:len ownedSpan:0 deallocator:d error:err];
}
- (instancetype)rdna4ImportHostPointer:(void *)p length:(NSUInteger)len ownedSpan:(uint64_t)ownedSpan deallocator:(void (^)(void *, NSUInteger))d error:(NSError **)err {
    #define NCFAIL(code, ...) do { NSString *m_ = [NSString stringWithFormat:__VA_ARGS__]; RDNA4LOG("newBufferWithBytesNoCopy: refused: %s", m_.UTF8String); if (err) *err = rdna4_err(code, m_); return nil; } while (0)
    if (!p || !len) NCFAIL(110, @"NULL pointer or zero length");
    if (((_opts >> 4) & 0xF) > 1) NCFAIL(111, @"storage mode %lu: only Shared and Managed client memory can be wrapped", (unsigned long)((_opts >> 4) & 0xF));
    if (!rdna4_radv_open(err)) return nil;
    rdna4dc_caps caps = rdna4_device_caps();
    if (!RDNA4R.hostExt || !rdna4_vkGetMHPP) NCFAIL(112, @"VK_EXT_external_memory_host is not available on this RADV");
    uint64_t base, span, delta;
    if (rdna4nc_import_range((uint64_t)(uintptr_t)p, len, RDNA4R.hostAlign, &base, &span, &delta)) NCFAIL(113, @"client range overflows or host import alignment is invalid");
    if (!rdna4dc_buffer_ok(&caps, span)) NCFAIL(114, @"import span exceeds the Vulkan storage/allocation limit");
    if (!ownedSpan && span > 64u << 20) NCFAIL(114, @"import span %llu > 64 MiB (the NoCopy single-import limit)", (unsigned long long)span);
    if (ownedSpan && (base != (uint64_t)(uintptr_t)p || span > ownedSpan)) NCFAIL(113, @"owned CPU range does not cover the aligned import");
    void *hostBase = (void *)(uintptr_t)base;
    VkExternalMemoryBufferCreateInfo external = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo bc = rdna4_heap_buffer_create_info(span, &external);
    VkResult r = vkCreateBuffer(RDNA4R.dev, &bc, NULL, &_vkbuf);
    if (r != VK_SUCCESS) { _vkbuf = VK_NULL_HANDLE; NCFAIL(115, @"vkCreateBuffer = %d", r); }
    VkMemoryHostPointerPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    uint64_t timp_ = rdna4_now();
    r = rdna4_vkGetMHPP(RDNA4R.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, hostBase, &hp);
    if (r != VK_SUCCESS || !hp.memoryTypeBits) NCFAIL(116, @"vkGetMemoryHostPointerPropertiesEXT = %d bits 0x%x", r, hp.memoryTypeBits);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(RDNA4R.dev, _vkbuf, &mr);
    if (mr.size > span) NCFAIL(117, @"Vulkan buffer memory requirement exceeds the aligned host span");
    int mt = rdna4_find_mem(hp.memoryTypeBits & mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) NCFAIL(117, @"no memory type common to the import (0x%x) and the buffer (0x%x)", hp.memoryTypeBits, mr.memoryTypeBits);
    VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = hostBase };
    VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, .pNext = &imp };
    VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = RDNA4R.bufferAddress ? (void *)&flags : (void *)&imp,
        .allocationSize = span, .memoryTypeIndex = (uint32_t)mt };
    r = vkAllocateMemory(RDNA4R.dev, &ma, NULL, &_vkmem); rdna4t_add(&T1.imp, rdna4_now() - timp_);
    if (r != VK_SUCCESS) {
        _vkmem = VK_NULL_HANDLE;
        char pages[256]; rdna4nc_region_summary(hostBase, (NSUInteger)span, pages, sizeof pages);
        NSString *message = [NSString stringWithFormat:@"vkAllocateMemory(import %lu B at %p) = %d; host_alignment %llu; vk_alignment %llu; regions %s",
                             (unsigned long)span, hostBase, r, (unsigned long long)RDNA4R.hostAlign, (unsigned long long)mr.alignment, pages];
        static _Atomic uint64_t refusedLogCount;
        uint64_t logIndex = atomic_fetch_add(&refusedLogCount, 1);
        if (logIndex < 8 || (logIndex & 1023u) == 0) RDNA4LOG("newBufferWithBytesNoCopy: refused: %s", message.UTF8String);
        if (err) *err = rdna4_err(118, message);
        return nil;
    }
    _imported = YES; _impSpan = span;
    @synchronized ([GarnetDevice class]) { RDNA4R.impTotal += span; }
    if (ownedSpan) { _asz = (NSUInteger)span; atomic_fetch_add(&rdna4_alloc_total, (uint64_t)_asz); }
    r = vkBindBufferMemory(RDNA4R.dev, _vkbuf, _vkmem, 0);
    if (r != VK_SUCCESS) NCFAIL(119, @"vkBindBufferMemory = %d", r);
    if (RDNA4R.bufferAddress) {
        VkBufferDeviceAddressInfo address = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = _vkbuf };
        _gpuAddress = rdna4_vkGetBufferAddress(RDNA4R.dev, &address);
        if (!_gpuAddress) NCFAIL(120, @"Vulkan returned a zero GPU address for the imported buffer");
        if (delta > UINT64_MAX - _gpuAddress) NCFAIL(120, @"imported GPU address overflows");
        _gpuAddress += delta;
    }
    _vkoff = delta;
    _ptr = p; _deallocator = [d copy];
    [self rdna4CheckHostReadOnly:imp.pHostPointer length:(NSUInteger)ma.allocationSize];
    RDNA4LOG("RDNA4Buffer %p: NO-COPY %lu bytes at %p imported as [%p, +%llu) offset %llu", (__bridge void *)self, (unsigned long)len, p, hostBase, (unsigned long long)span, (unsigned long long)delta);
    #undef NCFAIL
    return self;
}
- (BOOL)rdna4Imported { return _imported; }
- (BOOL)rdna4HostReadOnly { return _hostReadOnly; }
- (void)rdna4CheckHostReadOnly:(const void *)p length:(NSUInteger)len {
    // A mapping that cannot be inspected is conservatively treated as non-writable.
    int protection = rdna4nc_host_readonly(p, len);
    _hostReadOnly = protection != 0;
    if (protection < 0) RDNA4LOG("RDNA4Buffer %p: NO-COPY protection unknown, treating as read-only", (__bridge void *)self);
    if (_hostReadOnly) RDNA4LOG("RDNA4Buffer %p: NO-COPY %lu bytes at %p imported READ-ONLY (GPU writes will be dropped)",
                               (__bridge void *)self, (unsigned long)len, p);
}
- (VkDeviceMemory)rdna4NativeMemory:(uint64_t *)offset {
    if (_hpHeap) { *offset = _hpBindOff; return [_hpHeap rdna4BackingMemory:_hpBlk]; }
    *offset = _pooled ? _pm.off : 0;
    return _imported || _asz > RDNA4_BUFFER_LEGACY_MAX ? VK_NULL_HANDLE : (_pooled ? _pm.mem : _vkmem);
}
- (void)rdna4AdoptHeap:(RDNA4Heap *)h offset:(NSUInteger)o size:(NSUInteger)sz bid:(uint64_t)bid {
    _hpHeap = h; _hpOff = o; _hpSize = sz; _hpId = bid;
    if (h) _hpPhysSize = sz;
    if (!_noacct && (!_imported || _ownedHostSpan)) { atomic_fetch_sub(&rdna4_alloc_total, (uint64_t)_asz); _noacct = YES; }
}
- (void)dealloc {
    const BOOL heapBacked = _hpHeap && _hpBlk;
    RDNA4_HEAP_DEALLOC;
    if (RDNA4R.ok && !heapBacked) {
        if (_vkbuf) vkDestroyBuffer(RDNA4R.dev, _vkbuf, NULL);
        if (_pooled) { rdna4_mem_free(&_pm); if (!_noacct) atomic_fetch_sub(&rdna4_alloc_total, (uint64_t)_asz); }   // P1
        else if (_vkmem) {
            vkFreeMemory(RDNA4R.dev, _vkmem, NULL);
            if (_imported) { @synchronized ([GarnetDevice class]) { RDNA4R.impTotal -= _impSpan; } }
            if (!_noacct && (!_imported || _ownedHostSpan)) atomic_fetch_sub(&rdna4_alloc_total, (uint64_t)_asz);
        }
    }
    // Free the CPU VA after Vulkan retires the imports; quarantined descriptors retain their pinned physical pages.
    if (_ownedHostSpan) (void)munmap(_ptr, (size_t)_ownedHostSpan);
    if (_deallocator) { void (^d)(void *, NSUInteger) = _deallocator; _deallocator = nil; d(_ptr, _len); }
}
- (void)doesNotRecognizeSelector:(SEL)sel {
    RDNA4LOG("UNRECOGNIZED selector %s on RDNA4Buffer", sel_getName(sel));
    [super doesNotRecognizeSelector:sel];
}
- (VkBuffer)vkBuffer { return _vkbuf; }
- (VkDeviceSize)rdna4VkOffset { return _vkoff; }
- (RDNA4Heap *)rdna4ResourceHeap { return _hpHeap; }
- (BOOL)rdna4ResourceAliasable { return _hpAl; }
- (void)rdna4ResourceMakeAliasable { RDNA4_HEAP_RETIRE; }
- (BOOL)rdna4ResourceAliases:(id)other {
    return _hpHeap && _hpBlk && [_hpHeap rdna4Block:_hpBlk offset:_hpBindOff size:_hpPhysSize aliases:other];
}
- (uint64_t)rdna4BackingBlock { return _hpBlk; }
- (uint64_t)rdna4BackingOffset { return _hpBindOff; }
- (uint64_t)rdna4BackingSize { return _hpPhysSize; }
- (void)rdna4AbortHeapConstruction {
    RDNA4Heap *heap = _hpHeap;
    uint64_t bid = _hpId, block = _hpBlk, lease = _hpLease;
    const BOOL bound = _hpBacking != nil;
    [self rdna4DestroyRetiredBacking];
    _hpBacking = nil;
    if (heap && block && bound) [heap rdna4CloseBackingHandle:block];
    if (heap && lease) [heap rdna4AbandonBacking:lease];
    if (heap && bid) [heap rdna4Free:bid];
    _hpHeap = nil;
    _hpId = _hpBlk = _hpLease = _hpBindOff = _hpPhysSize = 0;
    _hpDedicated = _hpAl = NO; _hpOff = _hpSize = 0; _noacct = NO;
}
- (void)rdna4DestroyRetiredBacking {
    if (_hpBacking) [_hpBacking destroyHandles];
    else if (RDNA4R.ok && _vkbuf) vkDestroyBuffer(RDNA4R.dev, _vkbuf, NULL);
    _vkbuf = VK_NULL_HANDLE; _ptr = NULL; _gpuAddress = 0;
}
- (void *)contents { return _ptr; }
- (NSUInteger)length { return _len; }
- (uint64_t)gpuAddress { return _gpuAddress; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)resourceOptions { return _opts; }
- (MTLStorageMode)storageMode { return (MTLStorageMode)((_opts >> 4) & 0xF); }
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)(_opts & 0xF); }
- (MTLHazardTrackingMode)hazardTrackingMode { return (MTLHazardTrackingMode)((_opts >> 8) & 0x3); }
- (void)didModifyRange:(NSRange)r { (void)r; }   // Shared memory is coherent
// ---- gap census: MTLResource / MTLBuffer surface sent by QuartzCore, SkyLight and Metal ----
RDNA4_RESOURCE_SPI
- (NSUInteger)allocatedSize { return _hpHeap ? _hpSize : (_asz ? _asz : _len); }
- (void *)virtualAddress { return _ptr; }   // MTLIOAccelResource -virtualAddress is ^v (CPU pointer); QuartzCore sends it next to -contents and -didModifyRange: (SUSPECTED use)
- (void)addDebugMarker:(NSString *)m range:(NSRange)r { (void)m; (void)r; }
- (void)removeAllDebugMarkers {}
- (IOSurfaceRef)iosurface { return NULL; }
@end


