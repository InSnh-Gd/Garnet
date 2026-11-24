// ---------------------------------------------------------------------------------------------------------------
// Hardware Ray Tracing Acceleration Structures (Task 7.1):
//   RDNA4AccelerationStructure (MTLAccelerationStructure + MTLResource)
//   RDNA4AccelerationStructureCommandEncoder (MTLAccelerationStructureCommandEncoder + MTLCommandEncoder)
//   Backed by VK_KHR_acceleration_structure + VK_KHR_deferred_host_operations.
//
// Wave 3 integration checklist (GarnetDevice.m, rdna4_command_buffer_impl.inc, rdna4_buffer_impl.inc):
//   1. GarnetDevice.m include order: place after `#include "rdna4_gl_drawable_impl.inc"`
//      (and after `#include "rdna4_counters_impl.inc"` so pass sampleBufferAttachments work):
//        #include "rdna4_rt_accel_impl.inc"
//   2. GarnetDevice.m +load protocol registration & census (also auto-registered in +initialize):
//        { const char *n[] = { "MTLAccelerationStructure", "MTLResource" };
//          rdna4_add_protocols([RDNA4AccelerationStructure class], n, 2, "RDNA4AccelerationStructure"); }
//        { const char *n[] = { "MTLAccelerationStructureCommandEncoder", "MTLCommandEncoder" };
//          rdna4_add_protocols([RDNA4AccelerationStructureCommandEncoder class], n, 2, "RDNA4AccelerationStructureCommandEncoder"); }
//   3. GarnetDevice.m @implementation RDNA4CommandBuffer (Encoders):
//        - (id)accelerationStructureCommandEncoder {
//            (void)[self vk];
//            RDNA4LOGR("cb accelerationStructureCommandEncoder");
//            return [[RDNA4AccelerationStructureCommandEncoder alloc] initWithCommandBuffer:self descriptor:nil];
//        }
//        - (id)accelerationStructureCommandEncoderWithDescriptor:(id)d {
//            (void)[self vk];
//            RDNA4LOGR("cb accelerationStructureCommandEncoderWithDescriptor:");
//            return [[RDNA4AccelerationStructureCommandEncoder alloc] initWithCommandBuffer:self descriptor:d];
//        }
//   4. GarnetDevice.m @implementation GarnetDevice selectors:
//        - (MTLAccelerationStructureSizes)accelerationStructureSizesWithDescriptor:(MTLAccelerationStructureDescriptor *)d {
//            return rdna4_rt_device_sizes(self, d);
//        }
//        - (id<MTLAccelerationStructure>)newAccelerationStructureWithSize:(NSUInteger)size {
//            return rdna4_rt_device_new_with_size(self, size);
//        }
//        - (id<MTLAccelerationStructure>)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)d {
//            return rdna4_rt_device_new_with_descriptor(self, d);
//        }
//      Keep supportsRaytracing / supportsRaytracingFromRender / supportsPrimitiveMotionBlur returning NO
//      until Task 7.3/8.2 verifies full compute/render ray-query execution (helpers:
//      rdna4_rt_device_supports_raytracing(), rdna4_rt_device_supports_primitive_motion_blur()).
//   5. rdna4_buffer_impl.inc (`rdna4_heap_buffer_create_info`):
//      OR `(RDNA4R.accelStruct ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0)`
//      into `.usage` so user vertex/index/AABB/transform buffers carry Vulkan AS build-input usage bits.
//   6. rdna4_command_buffer_impl.inc (`rdna4UseResource:usage:`):
//      Accept `RDNA4AccelerationStructure` alongside `RDNA4IndirectCommandBuffer` and retain both
//      the acceleration structure and `[as rdna4CurrentHandle]` via `rdna4RetainEncoded:`.
// ---------------------------------------------------------------------------------------------------------------
#ifndef RDNA4_RT_ACCEL_IMPL_INC
#define RDNA4_RT_ACCEL_IMPL_INC

#include "rdna4_rt_accel.h"

_Static_assert(sizeof(MTLPackedFloat4x3) == sizeof(rdna4rt_packed_float4x3_t), "MTLPackedFloat4x3 size");
_Static_assert(sizeof(MTLAxisAlignedBoundingBox) == sizeof(rdna4rt_vk_aabb_t), "MTLAxisAlignedBoundingBox size");
_Static_assert(sizeof(MTLAccelerationStructureInstanceDescriptor) == sizeof(rdna4rt_mtl_instance_default_t), "MTLAccelerationStructureInstanceDescriptor size");
_Static_assert(sizeof(MTLAccelerationStructureUserIDInstanceDescriptor) == sizeof(rdna4rt_mtl_instance_userid_t), "MTLAccelerationStructureUserIDInstanceDescriptor size");
_Static_assert((uint32_t)MTLAccelerationStructureUsageNone == RDNA4RT_USAGE_NONE, "UsageNone");
_Static_assert((uint32_t)MTLAccelerationStructureUsageRefit == RDNA4RT_USAGE_REFIT, "UsageRefit");
_Static_assert((uint32_t)MTLAccelerationStructureUsagePreferFastBuild == RDNA4RT_USAGE_PREFER_FAST_BUILD, "UsagePreferFastBuild");
_Static_assert((uint32_t)MTLAccelerationStructureUsageExtendedLimits == RDNA4RT_USAGE_EXTENDED_LIMITS, "UsageExtendedLimits");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceOptionNone == RDNA4RT_INST_OPT_NONE, "InstOptNone");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceOptionDisableTriangleCulling == RDNA4RT_INST_OPT_DISABLE_CULLING, "InstOptDisableCulling");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceOptionTriangleFrontFacingWindingCounterClockwise == RDNA4RT_INST_OPT_CCW, "InstOptCCW");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceOptionOpaque == RDNA4RT_INST_OPT_OPAQUE, "InstOptOpaque");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceOptionNonOpaque == RDNA4RT_INST_OPT_NON_OPAQUE, "InstOptNonOpaque");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceDescriptorTypeDefault == RDNA4RT_INST_DESC_DEFAULT, "InstDescDefault");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceDescriptorTypeUserID == RDNA4RT_INST_DESC_USER_ID, "InstDescUserID");
_Static_assert((uint32_t)MTLAccelerationStructureInstanceDescriptorTypeMotion == RDNA4RT_INST_DESC_MOTION, "InstDescMotion");
_Static_assert((uint32_t)MTLMotionBorderModeClamp == RDNA4RT_MOTION_BORDER_CLAMP, "MotionBorderClamp");
_Static_assert((uint32_t)MTLMotionBorderModeVanish == RDNA4RT_MOTION_BORDER_VANISH, "MotionBorderVanish");

/* Lazily resolved VK_KHR_acceleration_structure + core query-copy entry points. */
typedef struct {
    BOOL ready;
    PFN_vkGetAccelerationStructureBuildSizesKHR getBuildSizes;
    PFN_vkCreateAccelerationStructureKHR createAS;
    PFN_vkDestroyAccelerationStructureKHR destroyAS;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getASAddress;
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuildAS;
    PFN_vkCmdCopyAccelerationStructureKHR cmdCopyAS;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR cmdWriteASProps;
    PFN_vkCmdCopyQueryPoolResults cmdCopyQueryPoolResults;
} RDNA4RTFnTable;

static RDNA4RTFnTable RDNA4RTFn;

static BOOL rdna4_rt_resolve_funcs(void) {
    if (!RDNA4R.ok || !RDNA4R.dev || !RDNA4R.accelStruct || !RDNA4R.bufferAddress) return NO;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (!vkGetDeviceProcAddr) return;
        RDNA4RTFn.getBuildSizes = (PFN_vkGetAccelerationStructureBuildSizesKHR)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkGetAccelerationStructureBuildSizesKHR");
        RDNA4RTFn.createAS = (PFN_vkCreateAccelerationStructureKHR)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkCreateAccelerationStructureKHR");
        RDNA4RTFn.destroyAS = (PFN_vkDestroyAccelerationStructureKHR)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkDestroyAccelerationStructureKHR");
        RDNA4RTFn.getASAddress = (PFN_vkGetAccelerationStructureDeviceAddressKHR)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkGetAccelerationStructureDeviceAddressKHR");
        RDNA4RTFn.cmdBuildAS = (PFN_vkCmdBuildAccelerationStructuresKHR)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdBuildAccelerationStructuresKHR");
        RDNA4RTFn.cmdCopyAS = (PFN_vkCmdCopyAccelerationStructureKHR)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdCopyAccelerationStructureKHR");
        RDNA4RTFn.cmdWriteASProps = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdWriteAccelerationStructuresPropertiesKHR");
        RDNA4RTFn.cmdCopyQueryPoolResults = (PFN_vkCmdCopyQueryPoolResults)
            vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdCopyQueryPoolResults");
        if (!RDNA4RTFn.cmdCopyQueryPoolResults && rdna4_gipa && RDNA4R.inst) {
            RDNA4RTFn.cmdCopyQueryPoolResults = (PFN_vkCmdCopyQueryPoolResults)
                rdna4_gipa(RDNA4R.inst, "vkCmdCopyQueryPoolResults");
        }
        RDNA4RTFn.ready = RDNA4RTFn.getBuildSizes && RDNA4RTFn.createAS && RDNA4RTFn.destroyAS &&
                          RDNA4RTFn.getASAddress && RDNA4RTFn.cmdBuildAS && RDNA4RTFn.cmdCopyAS &&
                          RDNA4RTFn.cmdWriteASProps && RDNA4RTFn.cmdCopyQueryPoolResults &&
                          vkCreateQueryPool && vkDestroyQueryPool && vkCmdResetQueryPool &&
                          rdna4_vkGetBufferAddress;
        if (!RDNA4RTFn.ready)
            RDNA4LOG("rt-accel: one or more VK_KHR_acceleration_structure entry points are missing");
    });
    return RDNA4RTFn.ready;
}

static void rdna4_rt_barrier(VkCommandBuffer cmd) {
    rdna4_full_barrier(cmd);
}

static void rdna4_rt_transfer_to_host_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT
    };
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

static BOOL rdna4_rt_vfmt_from_mtl(NSUInteger mtlFmt, rdna4rt_vfmt_t *outPure, VkFormat *outVk) {
    if (!outPure || !outVk) return NO;
    switch (mtlFmt) {
    case MTLAttributeFormatInvalid:
    case MTLAttributeFormatFloat3:
        *outPure = RDNA4RT_VFMT_FLOAT3;       *outVk = VK_FORMAT_R32G32B32_SFLOAT; return YES;
    case MTLAttributeFormatFloat2:
        *outPure = RDNA4RT_VFMT_FLOAT2;       *outVk = VK_FORMAT_R32G32_SFLOAT; return YES;
    case MTLAttributeFormatHalf2:
        *outPure = RDNA4RT_VFMT_HALF2;        *outVk = VK_FORMAT_R16G16_SFLOAT; return YES;
    case MTLAttributeFormatHalf4:
        *outPure = RDNA4RT_VFMT_HALF4;        *outVk = VK_FORMAT_R16G16B16A16_SFLOAT; return YES;
    case MTLAttributeFormatShort2Normalized:
        *outPure = RDNA4RT_VFMT_SHORT2_NORM;  *outVk = VK_FORMAT_R16G16_SNORM; return YES;
    case MTLAttributeFormatShort4Normalized:
        *outPure = RDNA4RT_VFMT_SHORT4_NORM;  *outVk = VK_FORMAT_R16G16B16A16_SNORM; return YES;
    case MTLAttributeFormatUShort2Normalized:
        *outPure = RDNA4RT_VFMT_USHORT2_NORM; *outVk = VK_FORMAT_R16G16_UNORM; return YES;
    case MTLAttributeFormatUShort4Normalized:
        *outPure = RDNA4RT_VFMT_USHORT4_NORM; *outVk = VK_FORMAT_R16G16B16A16_UNORM; return YES;
    default:
        return NO;
    }
}

static BOOL rdna4_rt_vk_vfmt_supported(VkFormat vkFmt) {
    if (!RDNA4R.ok || !RDNA4R.pd || !vkGetPhysicalDeviceFormatProperties) return NO;
    VkFormatProperties fp;
    memset(&fp, 0, sizeof fp);
    vkGetPhysicalDeviceFormatProperties(RDNA4R.pd, vkFmt, &fp);
    return (fp.bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR) != 0;
}

static uint32_t rdna4_rt_compacted_size_bytes(MTLDataType dt) {
    if (dt == MTLDataTypeUInt) return RDNA4RT_SIZE_TYPE_UINT32;
    if (dt == MTLDataTypeULong) return RDNA4RT_SIZE_TYPE_UINT64;
    return 0u;
}

// ---------------------------------------------------------------------------------------------------------------
// Retirement-owned Vulkan helpers: retained on RDNA4CommandBuffer segments until GPU fence completion.
// ---------------------------------------------------------------------------------------------------------------
@interface RDNA4RTStagingBuffer : NSObject {
@public
    VkBuffer _buf;
    VkDeviceMemory _mem;
    void *_map;
    VkDeviceAddress _addr;
    uint64_t _size;
    uint64_t _allocSize;
}
- (instancetype)initWithSize:(uint64_t)size error:(NSError **)err;
@end

@implementation RDNA4RTStagingBuffer
- (instancetype)initWithSize:(uint64_t)size error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!size) size = 64u;
    uint64_t aligned = 0;
    if (!rdna4rt_align_up_u64(size, RDNA4RT_VK_INSTANCE_ALIGN, &aligned)) {
        if (err) *err = rdna4_err(140, @"RT staging buffer size overflow");
        return nil;
    }
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = aligned,
        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT
    };
    VkResult r = vkCreateBuffer(RDNA4R.dev, &bci, NULL, &_buf);
    if (r != VK_SUCCESS || !_buf) {
        if (err) *err = rdna4_err(140, [NSString stringWithFormat:@"rt staging vkCreateBuffer = %d", r]);
        return nil;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(RDNA4R.dev, _buf, &mr);
    int mt = rdna4_find_mem(mr.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) {
        if (err) *err = rdna4_err(140, @"rt staging: no host-visible coherent memory type");
        return nil;
    }
    VkMemoryAllocateFlagsInfo afi = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &afi,
        .allocationSize = mr.size,
        .memoryTypeIndex = (uint32_t)mt
    };
    r = vkAllocateMemory(RDNA4R.dev, &mai, NULL, &_mem);
    if (r != VK_SUCCESS || !_mem) {
        if (err) *err = rdna4_err(140, [NSString stringWithFormat:@"rt staging vkAllocateMemory = %d", r]);
        return nil;
    }
    _allocSize = mr.size;
    atomic_fetch_add(&rdna4_alloc_total, _allocSize);
    r = vkBindBufferMemory(RDNA4R.dev, _buf, _mem, 0);
    if (r != VK_SUCCESS) {
        if (err) *err = rdna4_err(140, [NSString stringWithFormat:@"rt staging vkBindBufferMemory = %d", r]);
        return nil;
    }
    r = vkMapMemory(RDNA4R.dev, _mem, 0, VK_WHOLE_SIZE, 0, &_map);
    if (r != VK_SUCCESS || !_map) {
        if (err) *err = rdna4_err(140, [NSString stringWithFormat:@"rt staging vkMapMemory = %d", r]);
        return nil;
    }
    VkBufferDeviceAddressInfo bda = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = _buf
    };
    _addr = rdna4_vkGetBufferAddress(RDNA4R.dev, &bda);
    if (!_addr || (_addr % RDNA4RT_VK_INSTANCE_ALIGN) != 0u) {
        if (err) *err = rdna4_err(140, @"rt staging: invalid or misaligned buffer device address");
        return nil;
    }
    _size = aligned;
    return self;
}

- (void)dealloc {
    if (RDNA4R.ok) {
        if (_buf) vkDestroyBuffer(RDNA4R.dev, _buf, NULL);
        if (_mem) {
            vkFreeMemory(RDNA4R.dev, _mem, NULL);
            atomic_fetch_sub(&rdna4_alloc_total, _allocSize);
        }
    }
    _buf = VK_NULL_HANDLE;
    _mem = VK_NULL_HANDLE;
    _map = NULL;
    _addr = 0;
}
@end

@interface RDNA4RTQueryPool : NSObject {
@public
    VkQueryPool _qp;
}
- (instancetype)initCompactedSizePoolWithError:(NSError **)err;
@end

@implementation RDNA4RTQueryPool
- (instancetype)initCompactedSizePoolWithError:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    VkQueryPoolCreateInfo qi = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
        .queryCount = 1u
    };
    VkResult r = vkCreateQueryPool(RDNA4R.dev, &qi, NULL, &_qp);
    if (r != VK_SUCCESS || !_qp) {
        if (err) *err = rdna4_err(141, [NSString stringWithFormat:@"rt query pool vkCreateQueryPool = %d", r]);
        return nil;
    }
    return self;
}

- (void)dealloc {
    if (RDNA4R.ok && _qp && vkDestroyQueryPool) vkDestroyQueryPool(RDNA4R.dev, _qp, NULL);
    _qp = VK_NULL_HANDLE;
}
@end

// Backing Private VkBuffer + VkDeviceMemory. Held strongly by RDNA4AccelHandle so Vulkan
// always destroys VkAccelerationStructureKHR before its storage VkBuffer and VkDeviceMemory.
@interface RDNA4AccelStorage : NSObject {
@public
    VkBuffer _buf;
    VkDeviceMemory _mem;
    uint64_t _backingSize;
    uint64_t _allocatedSize;
}
- (instancetype)initWithBackingSize:(uint64_t)backingSize error:(NSError **)err;
@end

@implementation RDNA4AccelStorage
- (instancetype)initWithBackingSize:(uint64_t)backingSize error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = backingSize,
        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
    };
    VkResult r = vkCreateBuffer(RDNA4R.dev, &bci, NULL, &_buf);
    if (r != VK_SUCCESS || !_buf) {
        if (err) *err = rdna4_err(142, [NSString stringWithFormat:@"accel storage vkCreateBuffer = %d", r]);
        return nil;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(RDNA4R.dev, _buf, &mr);
    int mt = rdna4_find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) {
        if (err) *err = rdna4_err(142, @"accel storage: no device-local memory type");
        return nil;
    }
    VkMemoryAllocateFlagsInfo afi = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT
    };
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &afi,
        .allocationSize = mr.size,
        .memoryTypeIndex = (uint32_t)mt
    };
    r = vkAllocateMemory(RDNA4R.dev, &mai, NULL, &_mem);
    if (r != VK_SUCCESS || !_mem) {
        if (err) *err = rdna4_err(142, [NSString stringWithFormat:@"accel storage vkAllocateMemory = %d", r]);
        return nil;
    }
    _allocatedSize = mr.size;
    atomic_fetch_add(&rdna4_alloc_total, _allocatedSize);
    r = vkBindBufferMemory(RDNA4R.dev, _buf, _mem, 0);
    if (r != VK_SUCCESS) {
        if (err) *err = rdna4_err(142, [NSString stringWithFormat:@"accel storage vkBindBufferMemory = %d", r]);
        return nil;
    }
    _backingSize = backingSize;
    return self;
}

- (void)dealloc {
    if (RDNA4R.ok) {
        if (_buf) vkDestroyBuffer(RDNA4R.dev, _buf, NULL);
        if (_mem) {
            vkFreeMemory(RDNA4R.dev, _mem, NULL);
            atomic_fetch_sub(&rdna4_alloc_total, _allocatedSize);
        }
    }
    _buf = VK_NULL_HANDLE;
    _mem = VK_NULL_HANDLE;
}
@end

@interface RDNA4AccelHandle : NSObject {
@public
    RDNA4AccelStorage *_storage;
    VkAccelerationStructureKHR _vkAS;
    VkDeviceAddress _deviceAddress;
    rdna4rt_kind_t _kind;
}
- (instancetype)initWithStorage:(RDNA4AccelStorage *)storage kind:(rdna4rt_kind_t)kind error:(NSError **)err;
@end

@implementation RDNA4AccelHandle
- (instancetype)initWithStorage:(RDNA4AccelStorage *)storage kind:(rdna4rt_kind_t)kind error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!storage || !storage->_buf || (kind != RDNA4RT_KIND_BLAS && kind != RDNA4RT_KIND_TLAS)) {
        if (err) *err = rdna4_err(143, @"accel handle: invalid storage or AS kind");
        return nil;
    }
    if (!rdna4_rt_resolve_funcs()) {
        if (err) *err = rdna4_err(143, @"accel handle: VK_KHR_acceleration_structure unavailable");
        return nil;
    }
    _storage = storage;
    _kind = kind;
    VkAccelerationStructureCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .buffer = storage->_buf,
        .offset = 0,
        .size = storage->_backingSize,
        .type = (kind == RDNA4RT_KIND_TLAS)
            ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
            : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR
    };
    VkResult r = RDNA4RTFn.createAS(RDNA4R.dev, &ci, NULL, &_vkAS);
    if (r != VK_SUCCESS || _vkAS == VK_NULL_HANDLE) {
        if (err) *err = rdna4_err(143, [NSString stringWithFormat:@"vkCreateAccelerationStructureKHR = %d", r]);
        return nil;
    }
    VkAccelerationStructureDeviceAddressInfoKHR ai = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .accelerationStructure = _vkAS
    };
    _deviceAddress = RDNA4RTFn.getASAddress(RDNA4R.dev, &ai);
    if (!_deviceAddress) {
        if (err) *err = rdna4_err(143, @"vkGetAccelerationStructureDeviceAddressKHR returned 0");
        return nil;
    }
    return self;
}

- (void)dealloc {
    if (RDNA4R.ok && _vkAS != VK_NULL_HANDLE && RDNA4RTFn.destroyAS)
        RDNA4RTFn.destroyAS(RDNA4R.dev, _vkAS, NULL);
    _vkAS = VK_NULL_HANDLE;
    _deviceAddress = 0;
    _storage = nil;
}
@end

// ---------------------------------------------------------------------------------------------------------------
// RDNA4AccelerationStructure (MTLAccelerationStructure + MTLResource)
// ---------------------------------------------------------------------------------------------------------------
@interface RDNA4AccelerationStructure : _MTLResource {
    id _dev;
    NSString *_lbl;
    NSUInteger _size;
    RDNA4AccelStorage *_storage;
    RDNA4AccelHandle *_handle;
    rdna4rt_accel_meta_t _meta;
    pthread_mutex_t _mu;
    BOOL _muInit;
    RDNA4_RES_IVARS
}
- (instancetype)initWithDevice:(id)dev size:(NSUInteger)size initialKind:(rdna4rt_kind_t)kind error:(NSError **)err;
- (RDNA4AccelHandle *)rdna4EnsureHandleForKind:(rdna4rt_kind_t)kind error:(NSError **)err;
- (RDNA4AccelHandle *)rdna4CurrentHandle;
- (VkAccelerationStructureKHR)rdna4VkAccelerationStructure;
- (VkDeviceAddress)rdna4DeviceAddress;
- (rdna4rt_accel_meta_t)rdna4MetaSnapshot;
- (void)rdna4SetMeta:(rdna4rt_accel_meta_t)meta;
- (uint32_t)rdna4AccelKind;
- (BOOL)rdna4IsBuilt;
- (BOOL)rdna4AllowsRefit;
- (BOOL)rdna4AllowsCompaction;
- (RDNA4Heap *)rdna4ResourceHeap;
- (NSUInteger)rdna4ResourceHeapOffset;
- (BOOL)rdna4ResourceAliasable;
- (void)rdna4ResourceMakeAliasable;
- (BOOL)rdna4ResourceAliases:(id)other;
@end

@implementation RDNA4AccelerationStructure
+ (void)initialize {
    if (self == [RDNA4AccelerationStructure class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLAccelerationStructure", "MTLResource" };
            rdna4_add_protocols(self, n, 2, "RDNA4AccelerationStructure");
        });
    }
}
RDNA4_DNR(RDNA4AccelerationStructure)

- (instancetype)initWithDevice:(id)dev size:(NSUInteger)size initialKind:(rdna4rt_kind_t)kind error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!dev || size == 0) {
        if (err) *err = rdna4_err(144, @"acceleration structure requires non-nil device and size > 0");
        return nil;
    }
    if (!rdna4_radv_open(err)) return nil;
    if (!rdna4_rt_resolve_funcs()) {
        if (err) *err = rdna4_err(144, @"VK_KHR_acceleration_structure is unavailable on this backend");
        return nil;
    }
    uint64_t backingSize = 0;
    if (!rdna4rt_align_up_u64((uint64_t)size, RDNA4RT_AS_SIZE_ALIGN, &backingSize)) {
        if (err) *err = rdna4_err(144, @"acceleration structure size overflows 256-byte alignment");
        return nil;
    }
    rdna4dc_caps caps = rdna4_device_caps();
    if (!rdna4dc_buffer_ok(&caps, backingSize)) {
        if (err) *err = rdna4_err(144, @"acceleration structure size exceeds device buffer limit");
        return nil;
    }
    RDNA4AccelStorage *storage = [[RDNA4AccelStorage alloc] initWithBackingSize:backingSize error:err];
    if (!storage) return nil;
    RDNA4AccelHandle *handle = nil;
    if (kind == RDNA4RT_KIND_BLAS || kind == RDNA4RT_KIND_TLAS) {
        handle = [[RDNA4AccelHandle alloc] initWithStorage:storage kind:kind error:err];
        if (!handle) return nil;
    }
    pthread_mutex_init(&_mu, NULL);
    _muInit = YES;
    _dev = dev;
    _size = size;
    _storage = storage;
    _handle = handle;
    rdna4rt_meta_init(&_meta, (uint64_t)size, kind);
    RDNA4LOGR("RDNA4AccelerationStructure %p: size %lu (backing %llu, alloc %llu, kind %u)",
              (__bridge void *)self, (unsigned long)size,
              (unsigned long long)backingSize, (unsigned long long)storage->_allocatedSize, (unsigned)kind);
    return self;
}

- (void)dealloc {
    _handle = nil;
    _storage = nil;
    if (_muInit) pthread_mutex_destroy(&_mu);
}

- (RDNA4AccelHandle *)rdna4EnsureHandleForKind:(rdna4rt_kind_t)kind error:(NSError **)err {
    if (kind != RDNA4RT_KIND_BLAS && kind != RDNA4RT_KIND_TLAS) {
        if (err) *err = rdna4_err(144, @"invalid acceleration structure kind");
        return nil;
    }
    pthread_mutex_lock(&_mu);
    if (_handle && _handle->_kind == kind) {
        RDNA4AccelHandle *existing = _handle;
        pthread_mutex_unlock(&_mu);
        return existing;
    }
    RDNA4AccelStorage *storage = _storage;
    pthread_mutex_unlock(&_mu);
    RDNA4AccelHandle *created = [[RDNA4AccelHandle alloc] initWithStorage:storage kind:kind error:err];
    if (!created) return nil;
    pthread_mutex_lock(&_mu);
    if (!_handle || _handle->_kind != kind) _handle = created;
    RDNA4AccelHandle *result = _handle;
    pthread_mutex_unlock(&_mu);
    return result;
}

- (RDNA4AccelHandle *)rdna4CurrentHandle {
    pthread_mutex_lock(&_mu);
    RDNA4AccelHandle *h = _handle;
    pthread_mutex_unlock(&_mu);
    return h;
}

- (VkAccelerationStructureKHR)rdna4VkAccelerationStructure {
    RDNA4AccelHandle *h = [self rdna4CurrentHandle];
    return h ? h->_vkAS : VK_NULL_HANDLE;
}

- (VkDeviceAddress)rdna4DeviceAddress {
    RDNA4AccelHandle *h = [self rdna4CurrentHandle];
    return h ? h->_deviceAddress : 0u;
}

- (rdna4rt_accel_meta_t)rdna4MetaSnapshot {
    pthread_mutex_lock(&_mu);
    rdna4rt_accel_meta_t snap = _meta;
    pthread_mutex_unlock(&_mu);
    return snap;
}

- (void)rdna4SetMeta:(rdna4rt_accel_meta_t)meta {
    pthread_mutex_lock(&_mu);
    _meta = meta;
    pthread_mutex_unlock(&_mu);
}

- (uint32_t)rdna4AccelKind { return (uint32_t)[self rdna4MetaSnapshot].kind; }
- (BOOL)rdna4IsBuilt { return [self rdna4MetaSnapshot].state != RDNA4RT_STATE_UNBUILT; }
- (BOOL)rdna4AllowsRefit { return [self rdna4MetaSnapshot].allow_refit != 0u; }
- (BOOL)rdna4AllowsCompaction { return [self rdna4MetaSnapshot].allow_compaction != 0u; }

- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)size { return _size; }
- (NSUInteger)allocatedSize { return _storage ? (NSUInteger)_storage->_allocatedSize : _size; }
- (MTLStorageMode)storageMode { return MTLStorageModePrivate; }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeTracked; }
- (NSUInteger)resourceOptions { return MTLResourceStorageModePrivate | MTLResourceHazardTrackingModeTracked; }
- (RDNA4Heap *)rdna4ResourceHeap { return _hpHeap; }
- (NSUInteger)rdna4ResourceHeapOffset { return 0; }
- (BOOL)rdna4ResourceAliasable { return NO; }
- (void)rdna4ResourceMakeAliasable {}
- (BOOL)rdna4ResourceAliases:(id)other { (void)other; return NO; }
RDNA4_RESOURCE_SPI
@end

// ---------------------------------------------------------------------------------------------------------------
// Descriptor -> Vulkan build plan translator (shared by size queries, build, and refit).
// ---------------------------------------------------------------------------------------------------------------
typedef struct {
    rdna4rt_kind_t kind;
    uint64_t usage;
    uint32_t vkBuildFlags;
    uint32_t geomCount;
    uint64_t topologyHash;
    VkAccelerationStructureGeometryKHR *vkGeoms;
    VkAccelerationStructureBuildRangeInfoKHR *vkRanges;
    uint32_t *maxPrimCounts;
    VkAccelerationStructureBuildSizesInfoKHR vkSizes;
    rdna4rt_sizes_t metalSizes;
} RDNA4RTBuildPlan;

static void rdna4_rt_plan_free(RDNA4RTBuildPlan *p) {
    if (!p) return;
    free(p->vkGeoms);
    free(p->vkRanges);
    free(p->maxPrimCounts);
    p->vkGeoms = NULL;
    p->vkRanges = NULL;
    p->maxPrimCounts = NULL;
}

static BOOL rdna4_rt_check_buffer(id obj, id dev, BOOL writable, RDNA4Buffer **outBuf, NSString **why) {
    if (outBuf) *outBuf = nil;
    if (!obj) return YES;
    if (![obj isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)obj device] != dev || [obj isAliasable]) {
        if (why) *why = @"buffer is not a live RDNA4Buffer on this device";
        return NO;
    }
    if (writable && [(RDNA4Buffer *)obj rdna4HostReadOnly]) {
        if (why) *why = @"destination buffer is read-only NoCopy host memory";
        return NO;
    }
    if (outBuf) *outBuf = (RDNA4Buffer *)obj;
    return YES;
}

static BOOL rdna4_rt_descriptor_has_primitive_data(id geomDesc) {
    SEL sel = sel_registerName("primitiveDataBuffer");
    if (![geomDesc respondsToSelector:sel]) return NO;
    id buf = ((id (*)(id, SEL))objc_msgSend)(geomDesc, sel);
    return buf != nil;
}

static BOOL rdna4_rt_build_plan_make(
    id dev, MTLAccelerationStructureDescriptor *desc, BOOL requireBuffers,
    BOOL useOverrideUsage, uint64_t overrideUsage,
    NSMutableArray *retainedObjects,
    RDNA4RTBuildPlan *plan, NSError **err) {
    memset(plan, 0, sizeof(*plan));
    if (!dev || !desc) {
        if (err) *err = rdna4_err(145, @"acceleration structure descriptor is nil");
        return NO;
    }
    if (!rdna4_rt_resolve_funcs()) {
        if (err) *err = rdna4_err(145, @"VK_KHR_acceleration_structure unavailable");
        return NO;
    }
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    const uint32_t scratchAlign = rdna4rt_scratch_align(&m3);
    if (!scratchAlign) {
        if (err) *err = rdna4_err(145, @"acceleration structure scratch alignment unavailable");
        return NO;
    }
    const int allowMotion = rdna4rt_motion_blur_supported(&m3);

    Class indirectInstCls = objc_getClass("MTLIndirectInstanceAccelerationStructureDescriptor");
    if (indirectInstCls && [desc isKindOfClass:indirectInstCls]) {
        // shortcut: indirect TLAS descriptors require GPU-side ID-to-BDA translation; reject cleanly until needed
        if (err) *err = rdna4_err(145, @"MTLIndirectInstanceAccelerationStructureDescriptor is not supported");
        return NO;
    }

    /* Validate caller descriptor usage first; refit inherits source build flags for Vulkan VUID compliance. */
    uint32_t callerFlags = 0;
    rdna4rt_err_t cerr = rdna4rt_usage_to_vk_build_flags((uint64_t)desc.usage, &callerFlags);
    if (cerr != RDNA4RT_OK) {
        if (err) *err = rdna4_err(145, [NSString stringWithUTF8String:rdna4rt_err_str(cerr)]);
        return NO;
    }
    plan->usage = useOverrideUsage ? overrideUsage : (uint64_t)desc.usage;
    rdna4rt_err_t uerr = rdna4rt_usage_to_vk_build_flags(plan->usage, &plan->vkBuildFlags);
    if (uerr != RDNA4RT_OK) {
        if (err) *err = rdna4_err(145, [NSString stringWithUTF8String:rdna4rt_err_str(uerr)]);
        return NO;
    }

    if ([desc isKindOfClass:[MTLPrimitiveAccelerationStructureDescriptor class]]) {
        MTLPrimitiveAccelerationStructureDescriptor *pdesc = (MTLPrimitiveAccelerationStructureDescriptor *)desc;
        plan->kind = RDNA4RT_KIND_BLAS;

        /* Honest motion-blur validation and gating. */
        NSUInteger motionKeyframes = 1;
        uint32_t startBorder = RDNA4RT_MOTION_BORDER_CLAMP, endBorder = RDNA4RT_MOTION_BORDER_CLAMP;
        float startTime = 0.0f, endTime = 1.0f;
        if ([pdesc respondsToSelector:@selector(motionKeyframeCount)]) {
            motionKeyframes = pdesc.motionKeyframeCount;
            startBorder = (uint32_t)pdesc.motionStartBorderMode;
            endBorder = (uint32_t)pdesc.motionEndBorderMode;
            startTime = pdesc.motionStartTime;
            endTime = pdesc.motionEndTime;
        }
        if (motionKeyframes > 1) {
            rdna4rt_motion_params_t mp = {
                .is_motion = 1,
                .keyframe_count = (uint32_t)MIN(motionKeyframes, (NSUInteger)UINT32_MAX),
                .keyframe_buffer_count = 0,
                .start_border_mode = startBorder,
                .end_border_mode = endBorder,
                .start_time = startTime,
                .end_time = endTime
            };
            rdna4rt_err_t me = rdna4rt_validate_motion_params(&mp, allowMotion);
            if (me != RDNA4RT_OK) {
                if (err) *err = rdna4_err(145, [NSString stringWithUTF8String:rdna4rt_err_str(me)]);
                return NO;
            }
        }

        NSArray<MTLAccelerationStructureGeometryDescriptor *> *geoms = pdesc.geometryDescriptors ?: @[];
        NSUInteger gcount = geoms.count;
        uint64_t totalPrims = 0;
        rdna4rt_err_t totErr = rdna4rt_validate_blas_totals((uint64_t)gcount, 0u, plan->usage, &m3);
        if (totErr != RDNA4RT_OK) {
            if (err) *err = rdna4_err(145, [NSString stringWithUTF8String:rdna4rt_err_str(totErr)]);
            return NO;
        }

        plan->geomCount = (uint32_t)gcount;
        uint32_t *geomSigs = NULL;
        if (gcount > 0) {
            plan->vkGeoms = (VkAccelerationStructureGeometryKHR *)calloc(gcount, sizeof(*plan->vkGeoms));
            plan->vkRanges = (VkAccelerationStructureBuildRangeInfoKHR *)calloc(gcount, sizeof(*plan->vkRanges));
            plan->maxPrimCounts = (uint32_t *)calloc(gcount, sizeof(*plan->maxPrimCounts));
            geomSigs = (uint32_t *)calloc(gcount, sizeof(*geomSigs));
            if (!plan->vkGeoms || !plan->vkRanges || !plan->maxPrimCounts || !geomSigs) {
                free(geomSigs);
                rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, @"out of host memory building BLAS plan");
                return NO;
            }
        }

        Class motionTriCls = objc_getClass("MTLAccelerationStructureMotionTriangleGeometryDescriptor");
        Class motionBoxCls = objc_getClass("MTLAccelerationStructureMotionBoundingBoxGeometryDescriptor");

        for (NSUInteger i = 0; i < gcount; i++) {
            MTLAccelerationStructureGeometryDescriptor *gd = geoms[i];
            if (!gd) {
                free(geomSigs); rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, @"nil geometry descriptor in BLAS");
                return NO;
            }
            if ((motionTriCls && [gd isKindOfClass:motionTriCls]) ||
                (motionBoxCls && [gd isKindOfClass:motionBoxCls])) {
                rdna4rt_motion_params_t mp = {
                    .is_motion = 1,
                    .keyframe_count = (uint32_t)MAX(motionKeyframes, (NSUInteger)2),
                    .keyframe_buffer_count = 0,
                    .start_border_mode = startBorder,
                    .end_border_mode = endBorder,
                    .start_time = startTime,
                    .end_time = endTime
                };
                rdna4rt_err_t me = rdna4rt_validate_motion_params(&mp, allowMotion);
                free(geomSigs); rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, [NSString stringWithUTF8String:rdna4rt_err_str(me != RDNA4RT_OK ? me : RDNA4RT_ERR_MOTION_UNSUPPORTED)]);
                return NO;
            }
            if (rdna4_rt_descriptor_has_primitive_data(gd)) {
                free(geomSigs); rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, @"per-primitive data buffers are not supported in acceleration structure builds");
                return NO;
            }

            if ([gd isKindOfClass:[MTLAccelerationStructureTriangleGeometryDescriptor class]]) {
                MTLAccelerationStructureTriangleGeometryDescriptor *td =
                    (MTLAccelerationStructureTriangleGeometryDescriptor *)gd;
                RDNA4Buffer *vbuf = nil, *ibuf = nil, *tbuf = nil;
                NSString *bwhy = nil;
                if (!rdna4_rt_check_buffer(td.vertexBuffer, dev, NO, &vbuf, &bwhy) ||
                    !rdna4_rt_check_buffer(td.indexBuffer, dev, NO, &ibuf, &bwhy)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, bwhy ?: @"invalid triangle geometry buffer");
                    return NO;
                }
                id rawTbuf = nil;
                NSUInteger tbufOff = 0;
                NSUInteger tlayout = RDNA4RT_MATRIX_COLUMN_MAJOR;
                SEL tbSel = sel_registerName("transformationMatrixBuffer");
                if ([td respondsToSelector:tbSel])
                    rawTbuf = ((id (*)(id, SEL))objc_msgSend)(td, tbSel);
                SEL toSel = sel_registerName("transformationMatrixBufferOffset");
                if ([td respondsToSelector:toSel])
                    tbufOff = ((NSUInteger (*)(id, SEL))objc_msgSend)(td, toSel);
                SEL tlSel = sel_registerName("transformationMatrixLayout");
                if ([td respondsToSelector:tlSel])
                    tlayout = ((NSUInteger (*)(id, SEL))objc_msgSend)(td, tlSel);
                if (!rdna4_rt_check_buffer(rawTbuf, dev, NO, &tbuf, &bwhy)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, bwhy ?: @"invalid triangle transform buffer");
                    return NO;
                }

                NSUInteger mtlVfmt = MTLAttributeFormatFloat3;
                SEL vfSel = sel_registerName("vertexFormat");
                if ([td respondsToSelector:vfSel])
                    mtlVfmt = ((NSUInteger (*)(id, SEL))objc_msgSend)(td, vfSel);
                rdna4rt_vfmt_t pureVfmt = RDNA4RT_VFMT_FLOAT3;
                VkFormat vkVfmt = VK_FORMAT_R32G32B32_SFLOAT;
                if (!rdna4_rt_vfmt_from_mtl(mtlVfmt, &pureVfmt, &vkVfmt) ||
                    !rdna4_rt_vk_vfmt_supported(vkVfmt)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"unsupported triangle vertexFormat %lu", (unsigned long)mtlVfmt]);
                    return NO;
                }

                rdna4rt_index_type_t idxType = RDNA4RT_INDEX_NONE;
                if (ibuf) {
                    if (td.indexType == MTLIndexTypeUInt16) idxType = RDNA4RT_INDEX_UINT16;
                    else if (td.indexType == MTLIndexTypeUInt32) idxType = RDNA4RT_INDEX_UINT32;
                    else {
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        if (err) *err = rdna4_err(145, @"unsupported triangle indexType");
                        return NO;
                    }
                }

                rdna4rt_tri_desc_t tdesc = {
                    .vertex_format = pureVfmt,
                    .has_vertex_buf = vbuf != nil ? 1 : 0,
                    .vertex_buf_gpu_addr = vbuf ? [vbuf gpuAddress] : 0u,
                    .vertex_buf_len = vbuf ? (uint64_t)[vbuf length] : 0u,
                    .vertex_buf_offset = (uint64_t)td.vertexBufferOffset,
                    .vertex_stride = (uint64_t)td.vertexStride,
                    .has_index_buf = ibuf != nil ? 1 : 0,
                    .index_type = idxType,
                    .index_buf_gpu_addr = ibuf ? [ibuf gpuAddress] : 0u,
                    .index_buf_len = ibuf ? (uint64_t)[ibuf length] : 0u,
                    .index_buf_offset = (uint64_t)td.indexBufferOffset,
                    .triangle_count = (uint64_t)td.triangleCount,
                    .has_transform_buf = tbuf != nil ? 1 : 0,
                    .transform_buf_gpu_addr = tbuf ? [tbuf gpuAddress] : 0u,
                    .transform_buf_len = tbuf ? (uint64_t)[tbuf length] : 0u,
                    .transform_buf_offset = (uint64_t)tbufOff,
                    .transform_layout = (rdna4rt_matrix_layout_t)tlayout,
                    .opaque = td.opaque ? 1 : 0,
                    .allow_duplicate_any_hit = td.allowDuplicateIntersectionFunctionInvocation ? 1 : 0
                };
                rdna4rt_tri_plan_t tplan;
                rdna4rt_err_t te = rdna4rt_validate_triangle_geom(&tdesc, requireBuffers ? 1 : 0, &m3, &tplan);
                if (te != RDNA4RT_OK) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"triangle geometry[%lu]: %s", (unsigned long)i, rdna4rt_err_str(te)]);
                    return NO;
                }

                VkDeviceAddress transformAddr = tplan.transform_addr;
                if (requireBuffers && tbuf) {
                    const uint8_t *tmap = (const uint8_t *)[tbuf contents];
                    if (tplan.needs_transform_staging) {
                        if (!tmap) {
                            // shortcut: Private column-major/unaligned triangle transform requires a GPU transpose pass; add if needed
                            free(geomSigs); rdna4_rt_plan_free(plan);
                            if (err) *err = rdna4_err(145, @"column-major or unaligned transformationMatrixBuffer must be Shared/Managed");
                            return NO;
                        }
                        rdna4rt_vk_transform_t vkXf;
                        rdna4rt_err_t xerr = RDNA4RT_OK;
                        if (tdesc.transform_layout == RDNA4RT_MATRIX_COLUMN_MAJOR) {
                            rdna4rt_packed_float4x3_t colXf;
                            memcpy(&colXf, tmap + tbufOff, sizeof(colXf));
                            xerr = rdna4rt_transpose_4x3_to_3x4(&colXf, &vkXf);
                        } else {
                            rdna4rt_vk_transform_t rowXf;
                            memcpy(&rowXf, tmap + tbufOff, sizeof(rowXf));
                            xerr = rdna4rt_validate_transform_3x4(&rowXf, &vkXf);
                        }
                        if (xerr != RDNA4RT_OK) {
                            free(geomSigs); rdna4_rt_plan_free(plan);
                            if (err) *err = rdna4_err(145, [NSString stringWithUTF8String:rdna4rt_err_str(xerr)]);
                            return NO;
                        }
                        RDNA4RTStagingBuffer *stg = [[RDNA4RTStagingBuffer alloc] initWithSize:sizeof(vkXf) error:err];
                        if (!stg) { free(geomSigs); rdna4_rt_plan_free(plan); return NO; }
                        memcpy(stg->_map, &vkXf, sizeof(vkXf));
                        transformAddr = stg->_addr;
                        [retainedObjects addObject:stg];
                    } else if (tmap) {
                        rdna4rt_vk_transform_t rowXf, checkXf;
                        memcpy(&rowXf, tmap + tbufOff, sizeof(rowXf));
                        rdna4rt_err_t xerr = rdna4rt_validate_transform_3x4(&rowXf, &checkXf);
                        if (xerr != RDNA4RT_OK) {
                            free(geomSigs); rdna4_rt_plan_free(plan);
                            if (err) *err = rdna4_err(145, [NSString stringWithUTF8String:rdna4rt_err_str(xerr)]);
                            return NO;
                        }
                    }
                }

                if (requireBuffers) {
                    if (vbuf) [retainedObjects addObject:vbuf];
                    if (ibuf) [retainedObjects addObject:ibuf];
                    if (tbuf) [retainedObjects addObject:tbuf];
                }

                plan->vkGeoms[i] = (VkAccelerationStructureGeometryKHR){
                    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                    .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                    .flags = tplan.vk_geom_flags,
                    .geometry.triangles = {
                        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                        .vertexFormat = vkVfmt,
                        .vertexData.deviceAddress = requireBuffers ? tplan.vertex_addr : 0u,
                        .vertexStride = tplan.vertex_stride,
                        .maxVertex = tplan.max_vertex,
                        .indexType = (tplan.effective_index_type == RDNA4RT_INDEX_UINT16) ? VK_INDEX_TYPE_UINT16 :
                                     (tplan.effective_index_type == RDNA4RT_INDEX_UINT32) ? VK_INDEX_TYPE_UINT32 :
                                     VK_INDEX_TYPE_NONE_KHR,
                        .indexData.deviceAddress = requireBuffers ? tplan.index_addr : 0u,
                        .transformData.deviceAddress = requireBuffers ? transformAddr : (tbuf ? RDNA4RT_TRANSFORM_ALIGN : 0u)
                    }
                };
                plan->vkRanges[i] = (VkAccelerationStructureBuildRangeInfoKHR){
                    .primitiveCount = tplan.primitive_count,
                    .primitiveOffset = 0u,
                    .firstVertex = 0u,
                    .transformOffset = 0u
                };
                plan->maxPrimCounts[i] = tplan.primitive_count;
                geomSigs[i] = rdna4rt_geom_sig(RDNA4RT_GEOM_TRIANGLES, tplan.effective_index_type, pureVfmt, tplan.vk_geom_flags);
                if (!rdna4rt_add_u64(totalPrims, (uint64_t)tplan.primitive_count, &totalPrims)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, @"total BLAS primitive count overflow");
                    return NO;
                }
            } else if ([gd isKindOfClass:[MTLAccelerationStructureBoundingBoxGeometryDescriptor class]]) {
                MTLAccelerationStructureBoundingBoxGeometryDescriptor *bd =
                    (MTLAccelerationStructureBoundingBoxGeometryDescriptor *)gd;
                RDNA4Buffer *bbuf = nil;
                NSString *bwhy = nil;
                if (!rdna4_rt_check_buffer(bd.boundingBoxBuffer, dev, NO, &bbuf, &bwhy)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, bwhy ?: @"invalid bounding box buffer");
                    return NO;
                }
                rdna4rt_aabb_desc_t adesc = {
                    .has_buf = bbuf != nil ? 1 : 0,
                    .buf_gpu_addr = bbuf ? [bbuf gpuAddress] : 0u,
                    .buf_len = bbuf ? (uint64_t)[bbuf length] : 0u,
                    .buf_offset = (uint64_t)bd.boundingBoxBufferOffset,
                    .stride = (uint64_t)bd.boundingBoxStride,
                    .box_count = (uint64_t)bd.boundingBoxCount,
                    .opaque = bd.opaque ? 1 : 0,
                    .allow_duplicate_any_hit = bd.allowDuplicateIntersectionFunctionInvocation ? 1 : 0
                };
                rdna4rt_aabb_plan_t aplan;
                rdna4rt_err_t ae = rdna4rt_validate_aabb_geom(&adesc, requireBuffers ? 1 : 0, &m3, &aplan);
                if (ae != RDNA4RT_OK) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"bounding-box geometry[%lu]: %s", (unsigned long)i, rdna4rt_err_str(ae)]);
                    return NO;
                }
                if (requireBuffers && bbuf) [retainedObjects addObject:bbuf];
                plan->vkGeoms[i] = (VkAccelerationStructureGeometryKHR){
                    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                    .geometryType = VK_GEOMETRY_TYPE_AABBS_KHR,
                    .flags = aplan.vk_geom_flags,
                    .geometry.aabbs = {
                        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
                        .data.deviceAddress = requireBuffers ? aplan.data_addr : 0u,
                        .stride = aplan.stride
                    }
                };
                plan->vkRanges[i] = (VkAccelerationStructureBuildRangeInfoKHR){
                    .primitiveCount = aplan.primitive_count,
                    .primitiveOffset = 0u,
                    .firstVertex = 0u,
                    .transformOffset = 0u
                };
                plan->maxPrimCounts[i] = aplan.primitive_count;
                geomSigs[i] = rdna4rt_geom_sig(RDNA4RT_GEOM_AABBS, RDNA4RT_INDEX_NONE, RDNA4RT_VFMT_FLOAT3, aplan.vk_geom_flags);
                if (!rdna4rt_add_u64(totalPrims, (uint64_t)aplan.primitive_count, &totalPrims)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, @"total BLAS primitive count overflow");
                    return NO;
                }
            } else {
                free(geomSigs); rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"unsupported geometry descriptor class %@", NSStringFromClass([gd class])]);
                return NO;
            }
        }

        totErr = rdna4rt_validate_blas_totals((uint64_t)gcount, totalPrims, plan->usage, &m3);
        if (totErr != RDNA4RT_OK) {
            free(geomSigs); rdna4_rt_plan_free(plan);
            if (err) *err = rdna4_err(145, [NSString stringWithUTF8String:rdna4rt_err_str(totErr)]);
            return NO;
        }
        plan->topologyHash = rdna4rt_topology_hash(geomSigs, plan->maxPrimCounts, plan->geomCount);
        free(geomSigs);
    } else if ([desc isKindOfClass:[MTLInstanceAccelerationStructureDescriptor class]]) {
        MTLInstanceAccelerationStructureDescriptor *idesc = (MTLInstanceAccelerationStructureDescriptor *)desc;
        plan->kind = RDNA4RT_KIND_TLAS;

        RDNA4Buffer *instBuf = nil, *motionBuf = nil;
        NSString *bwhy = nil;
        if (!rdna4_rt_check_buffer(idesc.instanceDescriptorBuffer, dev, NO, &instBuf, &bwhy)) {
            if (err) *err = rdna4_err(145, bwhy ?: @"invalid TLAS instanceDescriptorBuffer");
            return NO;
        }
        NSUInteger descType = RDNA4RT_INST_DESC_DEFAULT;
        NSUInteger motionCount = 0, motionOff = 0;
        if ([idesc respondsToSelector:@selector(instanceDescriptorType)]) {
            descType = (NSUInteger)idesc.instanceDescriptorType;
            if (!rdna4_rt_check_buffer(idesc.motionTransformBuffer, dev, NO, &motionBuf, &bwhy)) {
                if (err) *err = rdna4_err(145, bwhy ?: @"invalid TLAS motionTransformBuffer");
                return NO;
            }
            motionCount = idesc.motionTransformCount;
            motionOff = idesc.motionTransformBufferOffset;
        }

        NSArray<id<MTLAccelerationStructure>> *blasList = idesc.instancedAccelerationStructures ?: @[];
        rdna4rt_tlas_desc_t tdesc = {
            .descriptor_type = (rdna4rt_inst_desc_type_t)descType,
            .usage = plan->usage,
            .instance_count = (uint64_t)idesc.instanceCount,
            .has_inst_buf = instBuf != nil ? 1 : 0,
            .inst_buf_len = instBuf ? (uint64_t)[instBuf length] : 0u,
            .inst_buf_offset = (uint64_t)idesc.instanceDescriptorBufferOffset,
            .inst_stride = (uint64_t)idesc.instanceDescriptorStride,
            .blas_count = (uint64_t)blasList.count,
            .has_motion_transform_buf = motionBuf != nil ? 1 : 0,
            .motion_transform_buf_len = motionBuf ? (uint64_t)[motionBuf length] : 0u,
            .motion_transform_buf_offset = (uint64_t)motionOff,
            .motion_transform_count = (uint64_t)motionCount
        };
        rdna4rt_tlas_plan_t tplan;
        rdna4rt_err_t te = rdna4rt_validate_tlas_desc(&tdesc, requireBuffers ? 1 : 0, allowMotion, &m3, &tplan);
        if (te != RDNA4RT_OK) {
            if (err) *err = rdna4_err(145, [NSString stringWithUTF8String:rdna4rt_err_str(te)]);
            return NO;
        }

        /* Vulkan requires geometryCount == 1 for TOP_LEVEL even when instanceCount == 0. */
        plan->geomCount = 1u;
        plan->vkGeoms = (VkAccelerationStructureGeometryKHR *)calloc(1, sizeof(*plan->vkGeoms));
        plan->vkRanges = (VkAccelerationStructureBuildRangeInfoKHR *)calloc(1, sizeof(*plan->vkRanges));
        plan->maxPrimCounts = (uint32_t *)calloc(1, sizeof(*plan->maxPrimCounts));
        if (!plan->vkGeoms || !plan->vkRanges || !plan->maxPrimCounts) {
            rdna4_rt_plan_free(plan);
            if (err) *err = rdna4_err(145, @"out of host memory building TLAS plan");
            return NO;
        }

        VkDeviceAddress vkInstAddr = 0;
        if (requireBuffers && tplan.instance_count > 0u) {
            if (![instBuf contents]) {
                // shortcut: Private TLAS instanceDescriptorBuffer requires a GPU compute translation pass; add when needed
                rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, @"TLAS instanceDescriptorBuffer must be Shared/Managed for Vulkan instance translation");
                return NO;
            }
            NSUInteger blasCount = blasList.count;
            uint64_t *blasAddrs = blasCount ? (uint64_t *)calloc(blasCount, sizeof(*blasAddrs)) : NULL;
            if (blasCount && !blasAddrs) {
                rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, @"out of host memory collecting BLAS addresses");
                return NO;
            }
            for (NSUInteger bi = 0; bi < blasCount; bi++) {
                id obj = blasList[bi];
                if (![obj isKindOfClass:[RDNA4AccelerationStructure class]] || [(RDNA4AccelerationStructure *)obj device] != dev) {
                    free(blasAddrs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"instancedAccelerationStructures[%lu] is not an RDNA4AccelerationStructure on this device", (unsigned long)bi]);
                    return NO;
                }
                RDNA4AccelerationStructure *blas = (RDNA4AccelerationStructure *)obj;
                rdna4rt_accel_meta_t bmeta = [blas rdna4MetaSnapshot];
                RDNA4AccelHandle *bhandle = [blas rdna4CurrentHandle];
                if (bmeta.kind != RDNA4RT_KIND_BLAS || bmeta.state == RDNA4RT_STATE_UNBUILT ||
                    !bhandle || !bhandle->_deviceAddress) {
                    free(blasAddrs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"instancedAccelerationStructures[%lu] is not a built BLAS", (unsigned long)bi]);
                    return NO;
                }
                blasAddrs[bi] = bhandle->_deviceAddress;
                [retainedObjects addObject:blas];
                [retainedObjects addObject:bhandle];
            }

            uint64_t stageBytes = 0;
            if (!rdna4rt_mul_u64((uint64_t)tplan.instance_count, RDNA4RT_VK_INSTANCE_SIZE, &stageBytes)) {
                free(blasAddrs); rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, @"TLAS instance staging size overflow");
                return NO;
            }
            RDNA4RTStagingBuffer *stg = [[RDNA4RTStagingBuffer alloc] initWithSize:stageBytes error:err];
            if (!stg) { free(blasAddrs); rdna4_rt_plan_free(plan); return NO; }

            rdna4rt_err_t ce = rdna4rt_convert_instances(
                tdesc.descriptor_type, (const uint8_t *)[instBuf contents], (size_t)[instBuf length],
                tdesc.inst_buf_offset, tplan.stride, tplan.instance_count,
                blasAddrs, (uint32_t)blasCount, (rdna4rt_vk_instance_t *)stg->_map);
            free(blasAddrs);
            if (ce != RDNA4RT_OK) {
                rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"TLAS instance conversion failed: %s", rdna4rt_err_str(ce)]);
                return NO;
            }
            vkInstAddr = stg->_addr;
            [retainedObjects addObject:stg];
            [retainedObjects addObject:instBuf];
        } else if (requireBuffers && instBuf) {
            [retainedObjects addObject:instBuf];
        }

        plan->vkGeoms[0] = (VkAccelerationStructureGeometryKHR){
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
            .flags = 0u,
            .geometry.instances = {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                .arrayOfPointers = VK_FALSE,
                .data.deviceAddress = vkInstAddr
            }
        };
        plan->vkRanges[0] = (VkAccelerationStructureBuildRangeInfoKHR){
            .primitiveCount = tplan.instance_count,
            .primitiveOffset = 0u,
            .firstVertex = 0u,
            .transformOffset = 0u
        };
        plan->maxPrimCounts[0] = tplan.instance_count;
        uint32_t tsig = rdna4rt_geom_sig(RDNA4RT_GEOM_INSTANCES, RDNA4RT_INDEX_NONE, RDNA4RT_VFMT_FLOAT3, 0u);
        plan->topologyHash = rdna4rt_topology_hash(&tsig, plan->maxPrimCounts, 1u);
    } else {
        if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"unsupported acceleration structure descriptor class %@", NSStringFromClass([desc class])]);
        return NO;
    }

    VkAccelerationStructureBuildGeometryInfoKHR sizeInfoReq = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = (plan->kind == RDNA4RT_KIND_TLAS)
            ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
            : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = plan->vkBuildFlags,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = plan->geomCount,
        .pGeometries = plan->vkGeoms
    };
    plan->vkSizes = (VkAccelerationStructureBuildSizesInfoKHR){
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
    };
    uint32_t dummyPrim = 0;
    RDNA4RTFn.getBuildSizes(RDNA4R.dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                            &sizeInfoReq, plan->geomCount ? plan->maxPrimCounts : &dummyPrim,
                            &plan->vkSizes);

    rdna4rt_err_t se = rdna4rt_compute_sizes(
        plan->vkSizes.accelerationStructureSize,
        plan->vkSizes.buildScratchSize,
        plan->vkSizes.updateScratchSize,
        plan->usage, scratchAlign, &plan->metalSizes);
    if (se != RDNA4RT_OK) {
        rdna4_rt_plan_free(plan);
        if (err) *err = rdna4_err(145, [NSString stringWithUTF8String:rdna4rt_err_str(se)]);
        return NO;
    }
    return YES;
}

// ---------------------------------------------------------------------------------------------------------------
// RDNA4AccelerationStructureCommandEncoder (MTLAccelerationStructureCommandEncoder + MTLCommandEncoder)
// ---------------------------------------------------------------------------------------------------------------
@interface RDNA4AccelerationStructureCommandEncoder : _MTLCommandEncoder {
    RDNA4CommandBuffer *_cb;
    BOOL _ended;
    NSString *_lbl;
    id _csbEnd[4];
    NSUInteger _csbEndIdx[4];
    uint32_t _nCsbEnd;
}
- (instancetype)initWithCommandBuffer:(id)cb descriptor:(id)d;
@end

@implementation RDNA4AccelerationStructureCommandEncoder
+ (void)initialize {
    if (self == [RDNA4AccelerationStructureCommandEncoder class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLAccelerationStructureCommandEncoder", "MTLCommandEncoder" };
            rdna4_add_protocols(self, n, 2, "RDNA4AccelerationStructureCommandEncoder");
        });
    }
}
RDNA4_DNR(RDNA4AccelerationStructureCommandEncoder)

- (instancetype)initWithCommandBuffer:(id)cb descriptor:(id)d {
    self = [super initWithCommandBuffer:cb];
    if (!self) return nil;
    _cb = cb;
    if (d && [d respondsToSelector:@selector(sampleBufferAttachments)]) {
        id sbas = ((id (*)(id, SEL))objc_msgSend)(d, @selector(sampleBufferAttachments));
        SEL sampleSel = sel_registerName("rdna4SampleOnCommandBuffer:vkCommandBuffer:sampleIndex:barrier:stageFlags:who:");
        for (NSUInteger k = 0; sbas && k < 4u; k++) {
            id att = [sbas objectAtIndexedSubscript:k];
            if (!att) continue;
            id sb = [att respondsToSelector:@selector(sampleBuffer)]
                ? ((id (*)(id, SEL))objc_msgSend)(att, @selector(sampleBuffer)) : nil;
            if (!sb) continue;
            NSUInteger startIdx = ((NSUInteger (*)(id, SEL))objc_msgSend)(att, @selector(startOfEncoderSampleIndex));
            NSUInteger endIdx = ((NSUInteger (*)(id, SEL))objc_msgSend)(att, @selector(endOfEncoderSampleIndex));
            if (![sb respondsToSelector:sampleSel] || ![sb respondsToSelector:@selector(device)] ||
                [sb device] != [_cb device] || ![sb respondsToSelector:@selector(sampleCount)]) {
                RDNA4LOG("accel pass: sampleBufferAttachments[%lu] invalid; refused", (unsigned long)k);
                _ended = YES;
                [super endEncoding];
                return nil;
            }
            NSUInteger count = ((NSUInteger (*)(id, SEL))objc_msgSend)(sb, @selector(sampleCount));
            if ((startIdx != MTLCounterDontSample && startIdx >= count) ||
                (endIdx != MTLCounterDontSample && endIdx >= count)) {
                RDNA4LOG("accel pass: sampleBufferAttachments[%lu] sample index out of range; refused", (unsigned long)k);
                _ended = YES;
                [super endEncoding];
                return nil;
            }
        }
        for (NSUInteger k = 0; sbas && k < 4u; k++) {
            id att = [sbas objectAtIndexedSubscript:k];
            if (!att) continue;
            id sb = [att respondsToSelector:@selector(sampleBuffer)]
                ? ((id (*)(id, SEL))objc_msgSend)(att, @selector(sampleBuffer)) : nil;
            if (!sb) continue;
            NSUInteger startIdx = ((NSUInteger (*)(id, SEL))objc_msgSend)(att, @selector(startOfEncoderSampleIndex));
            NSUInteger endIdx = ((NSUInteger (*)(id, SEL))objc_msgSend)(att, @selector(endOfEncoderSampleIndex));
            if (startIdx != MTLCounterDontSample) {
                ((BOOL (*)(id, SEL, id, VkCommandBuffer, NSUInteger, BOOL, VkPipelineStageFlagBits, const char *))objc_msgSend)(
                    sb, sampleSel, _cb, [_cb vk], startIdx, NO, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, "accel-pass-start");
            }
            if (endIdx != MTLCounterDontSample && _nCsbEnd < 4u) {
                _csbEnd[_nCsbEnd] = sb;
                _csbEndIdx[_nCsbEnd] = endIdx;
                _nCsbEnd++;
            }
        }
    }
    RDNA4LOGR("RDNA4AccelerationStructureCommandEncoder %p", (__bridge void *)self);
    return self;
}

- (id)device { return [_cb device]; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)getType { return 5; }

- (BOOL)rdna4GuardReady:(const char *)who {
    if (_ended) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s called after endEncoding", who]];
        return NO;
    }
    if ([_cb rdna4Error]) return NO;
    if (!rdna4_rt_resolve_funcs()) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s: VK_KHR_acceleration_structure unavailable", who]];
        return NO;
    }
    return YES;
}

- (void)buildAccelerationStructure:(id<MTLAccelerationStructure>)accelerationStructure
                        descriptor:(MTLAccelerationStructureDescriptor *)descriptor
                     scratchBuffer:(id<MTLBuffer>)scratchBuffer
               scratchBufferOffset:(NSUInteger)scratchBufferOffset {
    if (![self rdna4GuardReady:"buildAccelerationStructure"]) return;
    id dev = [_cb device];
    if (![accelerationStructure isKindOfClass:[RDNA4AccelerationStructure class]] ||
        [(RDNA4AccelerationStructure *)accelerationStructure device] != dev) {
        [_cb rdna4Fail:@"buildAccelerationStructure: invalid destination acceleration structure"];
        return;
    }
    RDNA4Buffer *scratch = nil;
    NSString *swhy = nil;
    if (!rdna4_rt_check_buffer(scratchBuffer, dev, YES, &scratch, &swhy)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"buildAccelerationStructure scratchBuffer: %@", swhy ?: @"invalid"]];
        return;
    }

    NSMutableArray *retained = [NSMutableArray array];
    RDNA4RTBuildPlan plan;
    NSError *err = nil;
    if (!rdna4_rt_build_plan_make(dev, descriptor, YES, NO, 0u, retained, &plan, &err)) {
        [_cb rdna4Fail:err.localizedDescription ?: @"buildAccelerationStructure: invalid descriptor"];
        return;
    }

    RDNA4AccelerationStructure *dstAS = (RDNA4AccelerationStructure *)accelerationStructure;
    rdna4rt_accel_meta_t dstMeta = [dstAS rdna4MetaSnapshot];
    rdna4rt_err_t ce = rdna4rt_state_can_build(&dstMeta, plan.metalSizes.acceleration_structure_size);
    if (ce != RDNA4RT_OK) {
        rdna4_rt_plan_free(&plan);
        [_cb rdna4Fail:[NSString stringWithFormat:@"buildAccelerationStructure destination: %s (capacity %llu < required %llu)",
                        rdna4rt_err_str(ce), (unsigned long long)dstMeta.capacity_bytes,
                        (unsigned long long)plan.metalSizes.acceleration_structure_size]];
        return;
    }

    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    uint64_t scratchAddr = 0;
    rdna4rt_err_t se = rdna4rt_resolve_scratch_addr(
        scratch ? [scratch gpuAddress] : 0u,
        scratch ? (uint64_t)[scratch length] : 0u,
        (uint64_t)scratchBufferOffset,
        plan.vkSizes.buildScratchSize,
        rdna4rt_scratch_align(&m3),
        &scratchAddr);
    if (se != RDNA4RT_OK) {
        rdna4_rt_plan_free(&plan);
        [_cb rdna4Fail:[NSString stringWithFormat:@"buildAccelerationStructure scratchBuffer: %s", rdna4rt_err_str(se)]];
        return;
    }

    RDNA4AccelHandle *dstHandle = [dstAS rdna4EnsureHandleForKind:plan.kind error:&err];
    if (!dstHandle) {
        rdna4_rt_plan_free(&plan);
        [_cb rdna4Fail:err.localizedDescription ?: @"buildAccelerationStructure: failed to create Vulkan AS handle"];
        return;
    }

    [_cb rdna4RetainEncoded:dstAS];
    [_cb rdna4RetainEncoded:dstHandle];
    if (scratch) [_cb rdna4RetainEncoded:scratch];
    for (id obj in retained) [_cb rdna4RetainEncoded:obj];

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = (plan.kind == RDNA4RT_KIND_TLAS)
            ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
            : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = plan.vkBuildFlags,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .srcAccelerationStructure = VK_NULL_HANDLE,
        .dstAccelerationStructure = dstHandle->_vkAS,
        .geometryCount = plan.geomCount,
        .pGeometries = plan.vkGeoms,
        .scratchData.deviceAddress = scratchAddr
    };
    VkAccelerationStructureBuildRangeInfoKHR dummyRange = { 0u, 0u, 0u, 0u };
    const VkAccelerationStructureBuildRangeInfoKHR *pRanges = plan.geomCount ? plan.vkRanges : &dummyRange;

    VkCommandBuffer cmd = [_cb vk];
    rdna4_rt_barrier(cmd);
    RDNA4RTFn.cmdBuildAS(cmd, 1u, &buildInfo, &pRanges);
    rdna4_rt_barrier(cmd);

    (void)rdna4rt_state_apply_build(&dstMeta, plan.kind, plan.metalSizes.acceleration_structure_size,
                                    plan.usage, plan.geomCount, plan.topologyHash);
    [dstAS rdna4SetMeta:dstMeta];
    rdna4_rt_plan_free(&plan);
}

- (void)refitAccelerationStructure:(id<MTLAccelerationStructure>)sourceAccelerationStructure
                        descriptor:(MTLAccelerationStructureDescriptor *)descriptor
                       destination:(id<MTLAccelerationStructure>)destinationAccelerationStructure
                     scratchBuffer:(id<MTLBuffer>)scratchBuffer
               scratchBufferOffset:(NSUInteger)scratchBufferOffset {
    [self refitAccelerationStructure:sourceAccelerationStructure
                          descriptor:descriptor
                         destination:destinationAccelerationStructure
                       scratchBuffer:scratchBuffer
                 scratchBufferOffset:scratchBufferOffset
                             options:(RDNA4RT_REFIT_OPT_VERTEX_DATA | RDNA4RT_REFIT_OPT_PER_PRIMITIVE_DATA)];
}

- (void)refitAccelerationStructure:(id<MTLAccelerationStructure>)sourceAccelerationStructure
                        descriptor:(MTLAccelerationStructureDescriptor *)descriptor
                       destination:(id<MTLAccelerationStructure>)destinationAccelerationStructure
                     scratchBuffer:(id<MTLBuffer>)scratchBuffer
               scratchBufferOffset:(NSUInteger)scratchBufferOffset
                           options:(NSUInteger)options {
    if (![self rdna4GuardReady:"refitAccelerationStructure"]) return;
    id dev = [_cb device];
    id<MTLAccelerationStructure> effectiveDst = destinationAccelerationStructure ?: sourceAccelerationStructure;
    if (![sourceAccelerationStructure isKindOfClass:[RDNA4AccelerationStructure class]] ||
        [(RDNA4AccelerationStructure *)sourceAccelerationStructure device] != dev ||
        ![effectiveDst isKindOfClass:[RDNA4AccelerationStructure class]] ||
        [(RDNA4AccelerationStructure *)effectiveDst device] != dev) {
        [_cb rdna4Fail:@"refitAccelerationStructure: invalid source or destination acceleration structure"];
        return;
    }
    RDNA4Buffer *scratch = nil;
    NSString *swhy = nil;
    if (!rdna4_rt_check_buffer(scratchBuffer, dev, YES, &scratch, &swhy)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"refitAccelerationStructure scratchBuffer: %@", swhy ?: @"invalid"]];
        return;
    }

    if (((uint64_t)options & ~(uint64_t)RDNA4RT_REFIT_OPT_KNOWN_MASK) != 0u) {
        [_cb rdna4Fail:@"refitAccelerationStructure: invalid refit options"];
        return;
    }
    if (!(options & RDNA4RT_REFIT_OPT_VERTEX_DATA) && (options & RDNA4RT_REFIT_OPT_PER_PRIMITIVE_DATA)) {
        [_cb rdna4Fail:@"refitAccelerationStructure: per-primitive-only refit without vertex data is unsupported"];
        return;
    }

    RDNA4AccelerationStructure *srcAS = (RDNA4AccelerationStructure *)sourceAccelerationStructure;
    RDNA4AccelerationStructure *dstAS = (RDNA4AccelerationStructure *)effectiveDst;
    rdna4rt_accel_meta_t srcMeta = [srcAS rdna4MetaSnapshot];
    if (srcMeta.state != RDNA4RT_STATE_BUILT || !srcMeta.allow_refit) {
        [_cb rdna4Fail:@"refitAccelerationStructure: source acceleration structure was not built with MTLAccelerationStructureUsageRefit or was compacted"];
        return;
    }

    NSMutableArray *retained = [NSMutableArray array];
    RDNA4RTBuildPlan plan;
    NSError *err = nil;
    if (!rdna4_rt_build_plan_make(dev, descriptor, YES, YES, srcMeta.usage, retained, &plan, &err)) {
        [_cb rdna4Fail:err.localizedDescription ?: @"refitAccelerationStructure: invalid descriptor"];
        return;
    }

    rdna4rt_accel_meta_t dstMeta = (dstAS == srcAS) ? srcMeta : [dstAS rdna4MetaSnapshot];
    rdna4rt_err_t re = rdna4rt_state_can_refit(
        &srcMeta, &dstMeta, plan.kind, plan.metalSizes.acceleration_structure_size,
        plan.geomCount, plan.topologyHash, (uint64_t)options);
    if (re != RDNA4RT_OK) {
        rdna4_rt_plan_free(&plan);
        [_cb rdna4Fail:[NSString stringWithFormat:@"refitAccelerationStructure: %s", rdna4rt_err_str(re)]];
        return;
    }

    RDNA4AccelHandle *srcHandle = [srcAS rdna4CurrentHandle];
    if (!srcHandle || srcHandle->_vkAS == VK_NULL_HANDLE) {
        rdna4_rt_plan_free(&plan);
        [_cb rdna4Fail:@"refitAccelerationStructure: source acceleration structure has no Vulkan handle"];
        return;
    }

    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    uint64_t scratchAddr = 0;
    rdna4rt_err_t se = rdna4rt_resolve_scratch_addr(
        scratch ? [scratch gpuAddress] : 0u,
        scratch ? (uint64_t)[scratch length] : 0u,
        (uint64_t)scratchBufferOffset,
        plan.vkSizes.updateScratchSize,
        rdna4rt_scratch_align(&m3),
        &scratchAddr);
    if (se != RDNA4RT_OK) {
        rdna4_rt_plan_free(&plan);
        [_cb rdna4Fail:[NSString stringWithFormat:@"refitAccelerationStructure scratchBuffer: %s", rdna4rt_err_str(se)]];
        return;
    }

    RDNA4AccelHandle *dstHandle = (dstAS == srcAS) ? srcHandle : [dstAS rdna4EnsureHandleForKind:plan.kind error:&err];
    if (!dstHandle) {
        rdna4_rt_plan_free(&plan);
        [_cb rdna4Fail:err.localizedDescription ?: @"refitAccelerationStructure: failed to create destination Vulkan AS handle"];
        return;
    }

    [_cb rdna4RetainEncoded:srcAS];
    [_cb rdna4RetainEncoded:srcHandle];
    if (dstAS != srcAS) {
        [_cb rdna4RetainEncoded:dstAS];
        [_cb rdna4RetainEncoded:dstHandle];
    }
    if (scratch) [_cb rdna4RetainEncoded:scratch];
    for (id obj in retained) [_cb rdna4RetainEncoded:obj];

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = (plan.kind == RDNA4RT_KIND_TLAS)
            ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
            : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .flags = plan.vkBuildFlags,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR,
        .srcAccelerationStructure = srcHandle->_vkAS,
        .dstAccelerationStructure = dstHandle->_vkAS,
        .geometryCount = plan.geomCount,
        .pGeometries = plan.vkGeoms,
        .scratchData.deviceAddress = scratchAddr
    };
    VkAccelerationStructureBuildRangeInfoKHR dummyRange = { 0u, 0u, 0u, 0u };
    const VkAccelerationStructureBuildRangeInfoKHR *pRanges = plan.geomCount ? plan.vkRanges : &dummyRange;

    VkCommandBuffer cmd = [_cb vk];
    rdna4_rt_barrier(cmd);
    RDNA4RTFn.cmdBuildAS(cmd, 1u, &buildInfo, &pRanges);
    rdna4_rt_barrier(cmd);

    (void)rdna4rt_state_apply_refit(
        &srcMeta, &dstMeta, plan.kind, plan.metalSizes.acceleration_structure_size,
        plan.geomCount, plan.topologyHash, (uint64_t)options);
    [dstAS rdna4SetMeta:dstMeta];
    rdna4_rt_plan_free(&plan);
}

- (void)copyAccelerationStructure:(id<MTLAccelerationStructure>)sourceAccelerationStructure
          toAccelerationStructure:(id<MTLAccelerationStructure>)destinationAccelerationStructure {
    if (![self rdna4GuardReady:"copyAccelerationStructure"]) return;
    id dev = [_cb device];
    if (![sourceAccelerationStructure isKindOfClass:[RDNA4AccelerationStructure class]] ||
        [(RDNA4AccelerationStructure *)sourceAccelerationStructure device] != dev ||
        ![destinationAccelerationStructure isKindOfClass:[RDNA4AccelerationStructure class]] ||
        [(RDNA4AccelerationStructure *)destinationAccelerationStructure device] != dev) {
        [_cb rdna4Fail:@"copyAccelerationStructure: invalid source or destination acceleration structure"];
        return;
    }
    RDNA4AccelerationStructure *srcAS = (RDNA4AccelerationStructure *)sourceAccelerationStructure;
    RDNA4AccelerationStructure *dstAS = (RDNA4AccelerationStructure *)destinationAccelerationStructure;
    rdna4rt_accel_meta_t srcMeta = [srcAS rdna4MetaSnapshot];
    rdna4rt_accel_meta_t dstMeta = [dstAS rdna4MetaSnapshot];
    rdna4rt_err_t ce = rdna4rt_state_can_copy(&srcMeta, &dstMeta, srcAS == dstAS ? 1 : 0);
    if (ce != RDNA4RT_OK) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"copyAccelerationStructure: %s", rdna4rt_err_str(ce)]];
        return;
    }
    RDNA4AccelHandle *srcHandle = [srcAS rdna4CurrentHandle];
    NSError *err = nil;
    RDNA4AccelHandle *dstHandle = [dstAS rdna4EnsureHandleForKind:srcMeta.kind error:&err];
    if (!srcHandle || !srcHandle->_vkAS || !dstHandle) {
        [_cb rdna4Fail:err.localizedDescription ?: @"copyAccelerationStructure: missing Vulkan AS handle"];
        return;
    }

    [_cb rdna4RetainEncoded:srcAS];
    [_cb rdna4RetainEncoded:srcHandle];
    [_cb rdna4RetainEncoded:dstAS];
    [_cb rdna4RetainEncoded:dstHandle];

    VkCopyAccelerationStructureInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
        .src = srcHandle->_vkAS,
        .dst = dstHandle->_vkAS,
        .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR
    };
    VkCommandBuffer cmd = [_cb vk];
    rdna4_rt_barrier(cmd);
    RDNA4RTFn.cmdCopyAS(cmd, &ci);
    rdna4_rt_barrier(cmd);

    (void)rdna4rt_state_apply_copy(&srcMeta, &dstMeta, 0);
    [dstAS rdna4SetMeta:dstMeta];
}

- (void)writeCompactedAccelerationStructureSize:(id<MTLAccelerationStructure>)accelerationStructure
                                       toBuffer:(id<MTLBuffer>)buffer
                                         offset:(NSUInteger)offset {
    [self writeCompactedAccelerationStructureSize:accelerationStructure
                                         toBuffer:buffer
                                           offset:offset
                                     sizeDataType:MTLDataTypeUInt];
}

- (void)writeCompactedAccelerationStructureSize:(id<MTLAccelerationStructure>)accelerationStructure
                                       toBuffer:(id<MTLBuffer>)buffer
                                         offset:(NSUInteger)offset
                                   sizeDataType:(MTLDataType)sizeDataType {
    if (![self rdna4GuardReady:"writeCompactedAccelerationStructureSize"]) return;
    id dev = [_cb device];
    if (![accelerationStructure isKindOfClass:[RDNA4AccelerationStructure class]] ||
        [(RDNA4AccelerationStructure *)accelerationStructure device] != dev) {
        [_cb rdna4Fail:@"writeCompactedAccelerationStructureSize: invalid acceleration structure"];
        return;
    }
    RDNA4Buffer *dstBuf = nil;
    NSString *bwhy = nil;
    if (!buffer || !rdna4_rt_check_buffer(buffer, dev, YES, &dstBuf, &bwhy)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"writeCompactedAccelerationStructureSize buffer: %@", bwhy ?: @"nil or invalid"]];
        return;
    }
    uint32_t sizeBytes = rdna4_rt_compacted_size_bytes(sizeDataType);
    rdna4rt_err_t we = rdna4rt_validate_compacted_size_write((uint64_t)[dstBuf length], (uint64_t)offset, sizeBytes);
    if (we != RDNA4RT_OK) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"writeCompactedAccelerationStructureSize: %s", rdna4rt_err_str(we)]];
        return;
    }
    RDNA4AccelerationStructure *srcAS = (RDNA4AccelerationStructure *)accelerationStructure;
    rdna4rt_accel_meta_t srcMeta = [srcAS rdna4MetaSnapshot];
    rdna4rt_err_t se = rdna4rt_state_can_write_compacted_size(&srcMeta);
    if (se != RDNA4RT_OK) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"writeCompactedAccelerationStructureSize: %s", rdna4rt_err_str(se)]];
        return;
    }
    RDNA4AccelHandle *srcHandle = [srcAS rdna4CurrentHandle];
    if (!srcHandle || !srcHandle->_vkAS) {
        [_cb rdna4Fail:@"writeCompactedAccelerationStructureSize: acceleration structure has no Vulkan handle"];
        return;
    }

    NSError *err = nil;
    RDNA4RTQueryPool *qp = [[RDNA4RTQueryPool alloc] initCompactedSizePoolWithError:&err];
    if (!qp) {
        [_cb rdna4Fail:err.localizedDescription ?: @"writeCompactedAccelerationStructureSize: query pool failed"];
        return;
    }

    VkDeviceSize dstVkOff = 0;
    if (__builtin_add_overflow([dstBuf rdna4VkOffset], (VkDeviceSize)offset, &dstVkOff)) {
        [_cb rdna4Fail:@"writeCompactedAccelerationStructureSize: buffer offset overflow"];
        return;
    }

    /* Always extract compacted size with VK_QUERY_RESULT_64_BIT (VUID compliant); stage when writing
     * 32-bit MTLDataTypeUInt or when an imported NoCopy buffer base offset is not 8-byte aligned. */
    BOOL useStaging = (sizeBytes == RDNA4RT_SIZE_TYPE_UINT32) || ((dstVkOff % 8u) != 0u);
    RDNA4RTStagingBuffer *stg = nil;
    if (useStaging) {
        stg = [[RDNA4RTStagingBuffer alloc] initWithSize:16u error:&err];
        if (!stg) {
            [_cb rdna4Fail:err.localizedDescription ?: @"writeCompactedAccelerationStructureSize: staging failed"];
            return;
        }
    }

    [_cb rdna4RetainEncoded:srcAS];
    [_cb rdna4RetainEncoded:srcHandle];
    [_cb rdna4RetainEncoded:dstBuf];
    [_cb rdna4RetainEncoded:qp];
    if (stg) [_cb rdna4RetainEncoded:stg];

    VkCommandBuffer cmd = [_cb vk];
    rdna4_rt_barrier(cmd);
    vkCmdResetQueryPool(cmd, qp->_qp, 0u, 1u);
    RDNA4RTFn.cmdWriteASProps(cmd, 1u, &srcHandle->_vkAS,
                              VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                              qp->_qp, 0u);
    if (useStaging) {
        RDNA4RTFn.cmdCopyQueryPoolResults(cmd, qp->_qp, 0u, 1u, stg->_buf, 0u, 8u,
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        VkMemoryBarrier mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &mb, 0, NULL, 0, NULL);
        VkBufferCopy copy = { .srcOffset = 0u, .dstOffset = dstVkOff, .size = sizeBytes };
        vkCmdCopyBuffer(cmd, stg->_buf, [dstBuf vkBuffer], 1u, &copy);
    } else {
        RDNA4RTFn.cmdCopyQueryPoolResults(cmd, qp->_qp, 0u, 1u, [dstBuf vkBuffer], dstVkOff, 8u,
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    }
    rdna4_rt_transfer_to_host_barrier(cmd);

    (void)rdna4rt_state_apply_write_compacted_size(&srcMeta);
    [srcAS rdna4SetMeta:srcMeta];
}

- (void)copyAndCompactAccelerationStructure:(id<MTLAccelerationStructure>)sourceAccelerationStructure
                    toAccelerationStructure:(id<MTLAccelerationStructure>)destinationAccelerationStructure {
    if (![self rdna4GuardReady:"copyAndCompactAccelerationStructure"]) return;
    id dev = [_cb device];
    if (![sourceAccelerationStructure isKindOfClass:[RDNA4AccelerationStructure class]] ||
        [(RDNA4AccelerationStructure *)sourceAccelerationStructure device] != dev ||
        ![destinationAccelerationStructure isKindOfClass:[RDNA4AccelerationStructure class]] ||
        [(RDNA4AccelerationStructure *)destinationAccelerationStructure device] != dev) {
        [_cb rdna4Fail:@"copyAndCompactAccelerationStructure: invalid source or destination acceleration structure"];
        return;
    }
    RDNA4AccelerationStructure *srcAS = (RDNA4AccelerationStructure *)sourceAccelerationStructure;
    RDNA4AccelerationStructure *dstAS = (RDNA4AccelerationStructure *)destinationAccelerationStructure;
    rdna4rt_accel_meta_t srcMeta = [srcAS rdna4MetaSnapshot];
    rdna4rt_accel_meta_t dstMeta = [dstAS rdna4MetaSnapshot];
    rdna4rt_err_t ce = rdna4rt_state_can_compact(&srcMeta, &dstMeta, srcAS == dstAS ? 1 : 0, 0u);
    if (ce != RDNA4RT_OK) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"copyAndCompactAccelerationStructure: %s", rdna4rt_err_str(ce)]];
        return;
    }
    RDNA4AccelHandle *srcHandle = [srcAS rdna4CurrentHandle];
    NSError *err = nil;
    RDNA4AccelHandle *dstHandle = [dstAS rdna4EnsureHandleForKind:srcMeta.kind error:&err];
    if (!srcHandle || !srcHandle->_vkAS || !dstHandle) {
        [_cb rdna4Fail:err.localizedDescription ?: @"copyAndCompactAccelerationStructure: missing Vulkan AS handle"];
        return;
    }

    [_cb rdna4RetainEncoded:srcAS];
    [_cb rdna4RetainEncoded:srcHandle];
    [_cb rdna4RetainEncoded:dstAS];
    [_cb rdna4RetainEncoded:dstHandle];

    VkCopyAccelerationStructureInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
        .src = srcHandle->_vkAS,
        .dst = dstHandle->_vkAS,
        .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR
    };
    VkCommandBuffer cmd = [_cb vk];
    rdna4_rt_barrier(cmd);
    RDNA4RTFn.cmdCopyAS(cmd, &ci);
    rdna4_rt_barrier(cmd);

    (void)rdna4rt_state_apply_compact(&srcMeta, &dstMeta, 0, 0u);
    [dstAS rdna4SetMeta:dstMeta];
}

- (void)updateFence:(id)fence {
    if (_ended) { [_cb rdna4Fail:@"accel updateFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [_cb rdna4UpdateFence:fence];
}

- (void)waitForFence:(id)fence {
    if (_ended) { [_cb rdna4Fail:@"accel waitForFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [_cb rdna4WaitForFence:fence];
}

- (void)useResource:(id)resource usage:(MTLResourceUsage)usage {
    if (_ended) { [_cb rdna4Fail:@"accel useResource after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if ([resource isKindOfClass:[RDNA4AccelerationStructure class]]) {
        if ([(RDNA4AccelerationStructure *)resource device] != [_cb device] ||
            !usage || ((NSUInteger)usage & ~(NSUInteger)7)) {
            [_cb rdna4Fail:@"accel useResource: invalid acceleration structure device or usage"];
            return;
        }
        [_cb rdna4RetainEncoded:resource];
        RDNA4AccelHandle *h = [(RDNA4AccelerationStructure *)resource rdna4CurrentHandle];
        if (h) [_cb rdna4RetainEncoded:h];
        return;
    }
    [_cb rdna4UseResource:resource usage:(NSUInteger)usage];
}

- (void)useResources:(const id __unsafe_unretained *)resources count:(NSUInteger)count usage:(MTLResourceUsage)usage {
    if (_ended) { [_cb rdna4Fail:@"accel useResources after endEncoding"]; return; }
    if (count && !resources) { [_cb rdna4Fail:@"accel useResources: NULL array"]; return; }
    for (NSUInteger i = 0; i < count; i++) [self useResource:resources[i] usage:usage];
}

- (void)useHeap:(id)heap {
    if (_ended) { [_cb rdna4Fail:@"accel useHeap after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [_cb rdna4UseHeap:heap];
}

- (void)useHeaps:(const id __unsafe_unretained *)heaps count:(NSUInteger)count {
    if (_ended) { [_cb rdna4Fail:@"accel useHeaps after endEncoding"]; return; }
    if (count && !heaps) { [_cb rdna4Fail:@"accel useHeaps: NULL array"]; return; }
    for (NSUInteger i = 0; i < count; i++) [self useHeap:heaps[i]];
}

- (void)sampleCountersInBuffer:(id)sampleBuffer atSampleIndex:(NSUInteger)sampleIndex withBarrier:(BOOL)barrier {
    if (_ended) { [_cb rdna4Fail:@"accel sampleCountersInBuffer after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    SEL sampleSel = sel_registerName("rdna4SampleOnCommandBuffer:vkCommandBuffer:sampleIndex:barrier:stageFlags:who:");
    if (!sampleBuffer || ![sampleBuffer respondsToSelector:sampleSel] ||
        ![sampleBuffer respondsToSelector:@selector(device)] || [sampleBuffer device] != [_cb device]) {
        [_cb rdna4Fail:@"accel sampleCountersInBuffer: invalid counter sample buffer"];
        return;
    }
    ((BOOL (*)(id, SEL, id, VkCommandBuffer, NSUInteger, BOOL, VkPipelineStageFlagBits, const char *))objc_msgSend)(
        sampleBuffer, sampleSel, _cb, [_cb vk], sampleIndex, barrier, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, "accel");
}

- (void)insertDebugSignpost:(NSString *)s { (void)s; }
- (void)pushDebugGroup:(NSString *)s { (void)s; }
- (void)popDebugGroup {}

- (void)endEncoding {
    if (_ended) return;
    _ended = YES;
    if (_nCsbEnd) {
        VkCommandBuffer cmd = [_cb vk];
        rdna4_rt_barrier(cmd);
        BOOL cbOk = ![_cb rdna4Error];
        SEL sampleSel = sel_registerName("rdna4SampleOnCommandBuffer:vkCommandBuffer:sampleIndex:barrier:stageFlags:who:");
        for (uint32_t k = 0; k < _nCsbEnd; k++) {
            if (cbOk && _csbEnd[k] && _csbEndIdx[k] != MTLCounterDontSample && [_csbEnd[k] respondsToSelector:sampleSel]) {
                ((BOOL (*)(id, SEL, id, VkCommandBuffer, NSUInteger, BOOL, VkPipelineStageFlagBits, const char *))objc_msgSend)(
                    _csbEnd[k], sampleSel, _cb, cmd, _csbEndIdx[k], NO, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, "accel-pass-end");
            }
            _csbEnd[k] = nil;
        }
        _nCsbEnd = 0u;
    } else if (![_cb rdna4Error]) {
        rdna4_rt_barrier([_cb vk]);
    }
    RDNA4LOGR("RDNA4AccelerationStructureCommandEncoder endEncoding");
    [super endEncoding];
}
@end

// ---------------------------------------------------------------------------------------------------------------
// Device entry helpers (wired into GarnetDevice in Wave 3).
// ---------------------------------------------------------------------------------------------------------------
static __attribute__((unused)) BOOL rdna4_rt_device_supports_raytracing(void) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    return rdna4rt_raytracing_supported(&m3) ? YES : NO;
}

static __attribute__((unused)) BOOL rdna4_rt_device_supports_primitive_motion_blur(void) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    return rdna4rt_motion_blur_supported(&m3) ? YES : NO;
}

static __attribute__((unused)) MTLAccelerationStructureSizes rdna4_rt_device_sizes(
    id dev, MTLAccelerationStructureDescriptor *descriptor) {
    MTLAccelerationStructureSizes zero = { 0, 0, 0 };
    if (!dev || !descriptor) return zero;
    NSError *err = nil;
    if (!rdna4_radv_open(&err)) {
        RDNA4LOG("accelerationStructureSizesWithDescriptor: RADV open failed: %s",
                 err.localizedDescription.UTF8String);
        return zero;
    }
    RDNA4RTBuildPlan plan;
    if (!rdna4_rt_build_plan_make(dev, descriptor, NO, NO, 0u, nil, &plan, &err)) {
        RDNA4LOG("accelerationStructureSizesWithDescriptor: refused: %s",
                 err.localizedDescription.UTF8String);
        return zero;
    }
    MTLAccelerationStructureSizes out = {
        .accelerationStructureSize = (NSUInteger)plan.metalSizes.acceleration_structure_size,
        .buildScratchBufferSize = (NSUInteger)plan.metalSizes.build_scratch_size,
        .refitScratchBufferSize = (NSUInteger)plan.metalSizes.refit_scratch_size
    };
    rdna4_rt_plan_free(&plan);
    return out;
}

static __attribute__((unused)) id<MTLAccelerationStructure> rdna4_rt_device_new_with_size(
    id dev, NSUInteger size) {
    NSError *err = nil;
    RDNA4AccelerationStructure *as = [[RDNA4AccelerationStructure alloc]
        initWithDevice:dev size:size initialKind:RDNA4RT_KIND_UNKNOWN error:&err];
    if (!as) RDNA4LOG("newAccelerationStructureWithSize:%lu failed: %s",
                      (unsigned long)size, err.localizedDescription.UTF8String);
    return (id<MTLAccelerationStructure>)as;
}

static __attribute__((unused)) id<MTLAccelerationStructure> rdna4_rt_device_new_with_descriptor(
    id dev, MTLAccelerationStructureDescriptor *descriptor) {
    if (!dev || !descriptor) return nil;
    NSError *err = nil;
    if (!rdna4_radv_open(&err)) {
        RDNA4LOG("newAccelerationStructureWithDescriptor: RADV open failed: %s",
                 err.localizedDescription.UTF8String);
        return nil;
    }
    RDNA4RTBuildPlan plan;
    if (!rdna4_rt_build_plan_make(dev, descriptor, NO, NO, 0u, nil, &plan, &err)) {
        RDNA4LOG("newAccelerationStructureWithDescriptor: refused: %s",
                 err.localizedDescription.UTF8String);
        return nil;
    }
    rdna4rt_kind_t kind = plan.kind;
    NSUInteger reqSize = (NSUInteger)plan.metalSizes.acceleration_structure_size;
    rdna4_rt_plan_free(&plan);
    RDNA4AccelerationStructure *as = [[RDNA4AccelerationStructure alloc]
        initWithDevice:dev size:reqSize initialKind:kind error:&err];
    if (!as) RDNA4LOG("newAccelerationStructureWithDescriptor: alloc %lu failed: %s",
                      (unsigned long)reqSize, err.localizedDescription.UTF8String);
    return (id<MTLAccelerationStructure>)as;
}

#endif /* RDNA4_RT_ACCEL_IMPL_INC */
