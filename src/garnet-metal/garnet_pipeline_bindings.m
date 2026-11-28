// garnet_pipeline_bindings.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_arguments_internal.h"
#include "garnet_pipelines_internal.h"

// ---------------------------------------------------------------------------------------------------------------
// Bound-resource state shared by the render and compute encoders, descriptor-set filling, dummy resources (11e).
// ---------------------------------------------------------------------------------------------------------------

GARNET_INTERNAL VkSampler rdna4_sampler_object_handle(id object, BOOL comparison,
                                             BOOL normalized, NSError **err) {
    if ([object isKindOfClass:[RDNA4SamplerBinding class]])
        return [(RDNA4SamplerBinding *)object rdna4SamplerComparison:comparison
            normalizedCoordinates:normalized error:err];
    if ([object isKindOfClass:[RDNA4SamplerState class]])
        return normalized
            ? [(RDNA4SamplerState *)object rdna4SamplerComparison:comparison
                normalizedCoordinates:YES error:err]
            : [(RDNA4SamplerState *)object rdna4SamplerComparison:comparison error:err];
    if (object && err) *err = rdna4_err(71, @"bound sampler has an invalid class");
    return VK_NULL_HANDLE;
}

GARNET_INTERNAL BOOL rdna4_sampler_object_pixel(id object) {
    if ([object isKindOfClass:[RDNA4SamplerBinding class]])
        return [(RDNA4SamplerBinding *)object rdna4PixelCoordinates];
    if ([object isKindOfClass:[RDNA4SamplerState class]])
        return [(RDNA4SamplerState *)object rdna4PixelCoordinates];
    return NO;
}



GARNET_INTERNAL void rdna4_argument_scan_prepared_destroy(RDNA4ArgumentScanPrepared *prepared) {
    if (!prepared) return;
    for (size_t i = 0; i < prepared->count; ++i)
        free(prepared->stages[i].works);
    memset(prepared, 0, sizeof(*prepared));
}

GARNET_INTERNAL uint8_t rdna4_argument_scan_storage(MTLStorageMode mode) {
    switch (mode) {
    case MTLStorageModeShared: return RDNA4_AS_STORAGE_SHARED;
    case MTLStorageModeManaged: return RDNA4_AS_STORAGE_MANAGED;
    case MTLStorageModePrivate: return RDNA4_AS_STORAGE_PRIVATE;
    case MTLStorageModeMemoryless: return RDNA4_AS_STORAGE_MEMORYLESS;
    default: return RDNA4_AS_STORAGE_UNKNOWN;
    }
}

GARNET_INTERNAL BOOL rdna4_argument_scan_writable(NSSet *writable, RDNA4Buffer *buffer) {
    if (!writable || !buffer) return NO;
    if ([writable containsObject:buffer]) return YES;
    id heap = buffer.heap;
    return heap && [writable containsObject:heap];
}

static void rdna4_argument_scan_bounds(rdna4_as_bound bounds[RDNA4_AS_ROOTS],
                                       const RDNA4StageState *state,
                                       NSSet *writable, id device) {
    memset(bounds, 0, RDNA4_AS_ROOTS * sizeof(*bounds));
    for (uint32_t root = 0; root < RDNA4_AS_ROOTS; ++root) {
        RDNA4Buffer *buffer = state->buf[root].b;
        if (!buffer) continue;
        rdna4_as_bound *bound = &bounds[root];
        bound->present = true;
        bound->length = (uint64_t)buffer.length;
        bound->binding_offset = (uint64_t)state->buf[root].off;
        bound->writable = rdna4_argument_scan_writable(writable, buffer);
        bound->storage = rdna4_argument_scan_storage(buffer.storageMode);
        bound->mapped = buffer.contents != NULL && buffer.device == device &&
            ![buffer isAliasable];
        if ([buffer isAliasable]) {
            bound->mapped = false;
            bound->storage = RDNA4_AS_STORAGE_UNKNOWN;
        }
    }
}

static BOOL rdna4_argument_scan_prepare_stage(RDNA4ArgumentScanStage *prepared,
                                              const RDNA4StageState *state,
                                              NSSet *writable, id device,
                                              NSString **why) {
    if (!prepared || !state || !state->scanPlan ||
        !state->scanPlan->plan.count) return YES;
    size_t capacity = 0;
    rdna4_as_status status = rdna4_as_work_capacity(&state->scanPlan->plan, 0,
                                                     &capacity);
    if (status != RDNA4_AS_OK || !capacity) {
        if (why) *why = [NSString stringWithFormat:
            @"argument scan work capacity: %s",
            rdna4_argument_scan_status_name(status)];
        return NO;
    }
    if (capacity > SIZE_MAX / sizeof(rdna4_as_work)) {
        if (why) *why = @"argument scan work capacity overflows host size";
        return NO;
    }
    rdna4_as_work *works = calloc(capacity, sizeof *works);
    if (!works) {
        if (why) *why = @"argument scan work allocation failed";
        return NO;
    }
    rdna4_as_bound bounds[RDNA4_AS_ROOTS];
    rdna4_argument_scan_bounds(bounds, state, writable, device);
    rdna4_as_work_stats stats;
    size_t count = 0;
    status = rdna4_argument_scan_reflection_plan_materialize(
        state->scanPlan, bounds, NULL, 0, works, capacity, &count, &stats);
    if (status != RDNA4_AS_OK) {
        free(works);
        if (why) *why = [NSString stringWithFormat:
            @"argument scan materialization: %s",
            rdna4_argument_scan_status_name(status)];
        return NO;
    }
    prepared->state = state;
    if (!count) {
        free(works);
        return YES;
    }
    prepared->works = works;
    prepared->count = count;
    prepared->stats = stats;
    RDNA4LOGR("argscan work: %llu work, %llu CPU/%llu GPU/%llu opaque, %llu element, %llu touched B over %llu B, fallback %llu, unknown %llu, missing-root %llu, invalid-span %llu, capacity %llu%s",
        (unsigned long long)stats.works,
        (unsigned long long)stats.cpu_works,
        (unsigned long long)stats.gpu_works,
        (unsigned long long)stats.opaque_works,
        (unsigned long long)stats.elements,
        (unsigned long long)stats.touched_bytes,
        (unsigned long long)stats.extent_bytes,
        (unsigned long long)stats.fallback_works,
        (unsigned long long)stats.unknown_works,
        (unsigned long long)stats.missing_roots,
        (unsigned long long)stats.invalid_spans,
        (unsigned long long)stats.capacity_rejections,
        stats.saturated ? " (saturated)" : "");
    return YES;
}

GARNET_INTERNAL BOOL rdna4_argument_scan_prepare(RDNA4ArgumentScanPrepared *out,
                                        const RDNA4StageState *const stages[],
                                        size_t stageCount, NSSet *writable,
                                        id device, NSString **why) {
    if (!out || (!stages && stageCount) || stageCount > 2) return NO;
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < stageCount; ++i) {
        RDNA4ArgumentScanStage stage = {0};
        if (!rdna4_argument_scan_prepare_stage(&stage, stages[i], writable,
                                               device, why)) {
            rdna4_argument_scan_prepared_destroy(out);
            return NO;
        }
        if (stage.state) out->stages[out->count++] = stage;
    }
    return YES;
}



static BOOL rdna4_embedded_token(const RDNA4PB *p, uint32_t element, const RDNA4StageState *st, id device, uint64_t *token) {
    *token = 0;
    if (!p->embedded || element >= p->count || p->embeddedBuffer >= 31) return NO;
    RDNA4BufSlot slot = st->buf[p->embeddedBuffer];
    RDNA4Buffer *buffer = slot.b;
    if (!buffer || buffer.device != device || ![buffer contents] || slot.off > buffer.length || p->embeddedOffset > NSUIntegerMax - slot.off) return NO;
    NSUInteger start = slot.off + p->embeddedOffset;
    if ((start & (sizeof(uint64_t) - 1)) != 0 || element > (NSUIntegerMax - start) / sizeof(uint64_t)) return NO;
    NSUInteger at = start + (NSUInteger)element * sizeof(uint64_t);
    if (at > buffer.length || sizeof(uint64_t) > buffer.length - at) return NO;
    memcpy(token, (const uint8_t *)[buffer contents] + at, sizeof *token);
    return YES;
}

GARNET_INTERNAL id rdna4_embedded_resource(const RDNA4PB *p, uint32_t element, const RDNA4StageState *st, BOOL texture, id device) {
    uint64_t token = 0;
    if (!rdna4_embedded_token(p, element, st, device, &token) || !token) return nil;
    return rdna4_argument_resource_for_token(token, texture, device);
}

GARNET_INTERNAL BOOL rdna4nc_drop_write(id resource, RDNA4NCOp op, const char *operation, const char *encoder) {
    if (![resource rdna4HostReadOnly]) return NO;
    static _Atomic uint64_t dropped[RDNA4NC_OP_COUNT];
    uint64_t n = atomic_fetch_add(&dropped[op], 1);
    if (n < 8 || (n & 1023u) == 0) {
        NSString *label = [resource label];
        RDNA4LOG_WARN("-[MTLDebug%s ] validation: %s writes read-only no-copy buffer %p (%s) — command dropped",
                      encoder, operation, (__bridge void *)resource, label ? label.UTF8String : "unlabeled");
    }
    return YES;
}

// Use existing optional Metal argument reflection. Descriptor types alone do not prove write access.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
GARNET_INTERNAL NSArray *rdna4nc_stage_arguments(id reflection, RDNA4NCOp stage) {
    if (stage == RDNA4NC_COMPUTE) return [(MTLComputePipelineReflection *)reflection arguments];
    if (stage == RDNA4NC_VERTEX) return [(MTLRenderPipelineReflection *)reflection vertexArguments];
    return [(MTLRenderPipelineReflection *)reflection fragmentArguments];
}
GARNET_INTERNAL BOOL rdna4nc_stage_writes(const RDNA4StageState *state, NSArray *arguments, RDNA4NCOp op,
                                 const char *stage, uint32_t *warned) {
    uint32_t mask = state->roBufMask;
    if (!mask || !rdna4nc_any_readonly_buffer()) return NO;
    BOOL drop = NO;
    while (mask) {
        unsigned i = (unsigned)__builtin_ctz(mask);
        mask &= mask - 1u;
        if (i >= 31) break;
        RDNA4Buffer *buffer = state->buf[i].b;
        if (!buffer || ![buffer rdna4HostReadOnly]) continue;
        BOOL known = NO;
        for (MTLArgument *argument in arguments) {
            if (argument.type != MTLArgumentTypeBuffer || argument.index != i) continue;
            known = YES;
            if (argument.access == MTLArgumentAccessWriteOnly || argument.access == MTLArgumentAccessReadWrite)
                drop |= rdna4nc_drop_write(buffer, op, stage, op == RDNA4NC_COMPUTE ? "ComputeCommandEncoder" : "RenderCommandEncoder");
            break;
        }
        if (!known && !(*warned & (1u << i))) {
            *warned |= 1u << i;
            RDNA4LOG_WARN("%s: cannot tell if binding %u writes a read-only buffer %p", stage, i, (__bridge void *)buffer);
        }
    }
    return drop;
}

GARNET_INTERNAL void rdna4_argument_stage_resources(NSMutableSet *resources, NSMutableSet *writable,
        const RDNA4PB *bindings, uint32_t count, const RDNA4StageState *state, NSArray *arguments) {
    uint32_t known = 0;
    for (MTLArgument *argument in arguments) {
        if (argument.type != MTLArgumentTypeBuffer || !argument.active || argument.index >= 31) continue;
        uint32_t index = (uint32_t)argument.index;
        known |= 1u << index;
        RDNA4Buffer *buffer = state->buf[index].b;
        if (!buffer) continue;
        [resources addObject:buffer];
        if (argument.access != MTLArgumentAccessReadOnly) [writable addObject:buffer];
    }
    for (uint32_t n = 0; n < count; ++n) {
        const RDNA4PB *binding = &bindings[n];
        uint32_t textureBase = binding->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? 32 : 480;
        if (!binding->embedded && (binding->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || binding->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) &&
            binding->binding >= textureBase && binding->binding < textureBase + 128) {
            uint32_t first = binding->binding - textureBase;
            for (uint32_t k = 0; k < binding->count && k < 128 - first; ++k)
                if (state->tex[first + k]) [resources addObject:state->tex[first + k]];
        }
        if (binding->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && binding->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) continue;
        uint32_t first = binding->argumentRoot ? binding->embeddedBuffer : binding->binding;
        for (uint32_t k = 0; first < 31 && k < binding->count && k < 31 - first; ++k) {
            uint32_t index = first + k;
            RDNA4Buffer *buffer = state->buf[index].b;
            if (!buffer) continue;
            [resources addObject:buffer];
            if (!(known & (1u << index))) [writable addObject:buffer];
        }
    }
    uint32_t iftMask = state->rt.iftMask;
    while (iftMask) {
        uint32_t idx = (uint32_t)__builtin_ctz(iftMask);
        iftMask &= (iftMask - 1u);
        if (idx < RDNA4RTR_MAX_BUFFER_SLOTS && state->rt.ift[idx])
            [resources addObject:state->rt.ift[idx]];
    }
}

#pragma clang diagnostic pop

// Fallbacks for slots a shader reads but the client never bound. Metal reads zeros / a black texel there; a garbage descriptor could
// fault the GPU, so a zero-filled buffer, a 1x1 texel and a default sampler stand in (each use is logged as a WARN).
GARNET_INTERNAL RDNA4Buffer *rdna4_dummy_buffer(id dev) {
    static RDNA4Buffer *b;
    @synchronized ([GarnetDevice class]) {
        if (!b) { NSError *e = nil; b = [[RDNA4Buffer alloc] initWithDevice:dev length:65536 options:MTLResourceStorageModeShared error:&e];
                  if (b) memset([b contents], 0, 65536); else RDNA4LOG_ERR("dummy buffer failed: %s", e.localizedDescription.UTF8String); }
    }
    return b;
}
GARNET_INTERNAL RDNA4Texture *rdna4_dummy_texture(id dev) {
    static RDNA4Texture *t;
    static id owner;
    @synchronized ([GarnetDevice class]) {
        if (!t || owner != dev) {
            owner = dev;
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
            NSError *e = nil; t = [[RDNA4Texture alloc] initWithDevice:dev descriptor:td error:&e];
            if (t) { uint32_t z = 0; [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&z bytesPerRow:4]; }
            else RDNA4LOG_ERR("dummy texture failed: %s", e.localizedDescription.UTF8String);
        }
    }
    return t;
}
static const RDNA4Fmt *rdna4_dummy_storage_format(void) {
    static const MTLPixelFormat formats[] = {
        MTLPixelFormatR32Uint, MTLPixelFormatR32Sint, MTLPixelFormatR32Float, MTLPixelFormatRGBA8Unorm
    };
    for (size_t i = 0; i < sizeof formats / sizeof *formats; ++i) {
        const RDNA4Fmt *f = rdna4_fmt(formats[i]);
        if (f && (rdna4_fmt_feats(f) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return f;
    }
    return NULL;
}
GARNET_INTERNAL RDNA4Texture *rdna4_dummy_texture_write(id dev) {
    static RDNA4Texture *t;
    static id owner;
    @synchronized ([GarnetDevice class]) {
        if (!t || owner != dev) {
            owner = dev;
            const RDNA4Fmt *f = rdna4_dummy_storage_format();
            if (!f) {
                RDNA4LOG_WARN("dummy shader-write texture unavailable: no storage-image format is supported");
                t = nil;
                return t;
            }
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f->mtl width:1 height:1 mipmapped:NO];
            td.usage = MTLTextureUsageShaderWrite; td.storageMode = MTLStorageModePrivate;
            NSError *e = nil; t = [[RDNA4Texture alloc] initWithDevice:dev descriptor:td error:&e];
            if (!t) RDNA4LOG_ERR("dummy shader-write texture failed: %s", e.localizedDescription.UTF8String);
        }
    }
    return t;
}

typedef struct {
    MTLTextureType textureType;
    MTLPixelFormat pixelFormat;
    MTLTextureUsage usage;
    MTLStorageMode storageMode;
    NSUInteger sampleCount;
    BOOL depth;
    BOOL storage;
} RDNA4DummyImagePlan;

// The 48 ABI-5 shape descriptors describe float/uint/int, dimension/array, and depth.
// Sample count is a separate axis because the ABI-5 shape word has no MS bit.
static BOOL rdna4_dummy_image_plan_make(uint32_t shape, BOOL storage, uint32_t samples,
                                        RDNA4DummyImagePlan *p, const char **why) {
    if (!p) return NO;
    memset(p, 0, sizeof *p);
    if (why) *why = NULL;
    BOOL depth = NO;
    if (!rdna4_argument_shape(shape, NULL, &depth)) {
        if (why) *why = "not a legal ABI-5 image shape";
        return NO;
    }
    if (!samples) samples = 1;
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) {
        if (why) *why = "sample count must be 1, 2, 4, or 8";
        return NO;
    }
    uint32_t component = shape / 16, dim = (shape & 7u) >> 1, arrayed = shape & 1u;
    if (depth && dim != 1 && dim != 3) {
        if (why) *why = "depth formats are 2D or cube, including arrays";
        return NO;
    }
    if (storage && depth) {
        if (why) *why = "depth images cannot be storage images";
        return NO;
    }
    if (samples > 1 && dim != 1) {
        if (why) *why = "multisample dummies are 2D, including arrays";
        return NO;
    }
    if (storage && samples > 1) {
        if (why) *why = "storage images cannot be multisampled";
        return NO;
    }
    MTLTextureType type = samples > 1 ? (arrayed ? (MTLTextureType)RDNA4TD_MTL_2DMSARRAY : MTLTextureType2DMultisample) :
        dim == 0 ? (arrayed ? MTLTextureType1DArray : MTLTextureType1D) :
        dim == 1 ? (arrayed ? MTLTextureType2DArray : MTLTextureType2D) :
        dim == 2 ? MTLTextureType3D : (arrayed ? MTLTextureTypeCubeArray : MTLTextureTypeCube);
    MTLPixelFormat pixelFormat = depth ? MTLPixelFormatDepth32Float :
        component == 1 ? MTLPixelFormatR32Uint : component == 2 ? MTLPixelFormatR32Sint : MTLPixelFormatR32Float;
    if (storage) {
        const RDNA4Fmt *f = rdna4_fmt(pixelFormat);
        if (!f || !(rdna4_fmt_feats(f) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
            if (why) *why = "the selected format has no storage-image feature";
            return NO;
        }
    }
    p->textureType = type;
    p->pixelFormat = pixelFormat;
    p->usage = storage ? MTLTextureUsageShaderWrite : MTLTextureUsageShaderRead;
    p->storageMode = (depth || storage || samples > 1) ? MTLStorageModePrivate : MTLStorageModeShared;
    p->sampleCount = samples;
    p->depth = depth;
    p->storage = storage;
    return YES;
}

static void rdna4_dummy_zero_texture(RDNA4Texture *t) {
    if (!t || [t storageMode] != MTLStorageModeShared) return;
    uint32_t z = 0;
    MTLTextureType type = [t textureType];
    if (type == MTLTextureType1D)
        [t replaceRegion:MTLRegionMake1D(0, 1) mipmapLevel:0 withBytes:&z bytesPerRow:4];
    else if (type == MTLTextureType3D)
        [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:&z bytesPerRow:4 bytesPerImage:4];
    else if (type == MTLTextureType1DArray || type == MTLTextureType2DArray ||
             type == MTLTextureTypeCube || type == MTLTextureTypeCubeArray) {
        MTLRegion region = type == MTLTextureType1DArray ? MTLRegionMake1D(0, 1) : MTLRegionMake2D(0, 0, 1, 1);
        for (uint32_t slice = 0; slice < [t rdna4Layers]; ++slice)
            [t replaceRegion:region mipmapLevel:0 slice:slice withBytes:&z bytesPerRow:4 bytesPerImage:4];
    }
    else
        [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&z bytesPerRow:4];
}

typedef struct {
    RDNA4Texture *texture;
    uintptr_t owner;
} RDNA4DummyImageSlot;

#define RDNA4_DUMMY_SAMPLE_VARIANTS 4u
static RDNA4DummyImageSlot rdna4_dummy_null_read[RDNA4_ARGUMENT_NULL_IMAGES][RDNA4_DUMMY_SAMPLE_VARIANTS];
static RDNA4DummyImageSlot rdna4_dummy_null_write[RDNA4_ARGUMENT_NULL_IMAGES];

// Shape-specific read and write caches are separate. A null store therefore
// cannot overwrite a shared read dummy. Each cache is a fixed ABI-sized array.
// The shaped descriptor must use [texture vkView]; rdna4AttViewLevel:0 is a
// single-layer view for array storage and does not preserve the array shape.
static RDNA4Texture *rdna4_dummy_null_image_legacy(id dev, uint32_t shape, BOOL writable, uint32_t samples) {
    if (!dev || shape >= RDNA4_ARGUMENT_NULL_IMAGES) return nil;
    if (!samples) samples = 1;
    if (writable && samples != 1) return nil;
    unsigned sampleSlot = samples == 1 ? 0 : samples == 2 ? 1 : samples == 4 ? 2 : samples == 8 ? 3 : UINT32_MAX;
    if (sampleSlot == UINT32_MAX) return nil;
    RDNA4DummyImagePlan plan;
    const char *why = NULL;
    if (!rdna4_dummy_image_plan_make(shape, writable, samples, &plan, &why)) {
        RDNA4LOG_WARN("dummy %s texture shape %u samples %u rejected: %s", writable ? "storage" : "sampled",
                      shape, samples, why ?: "invalid descriptor");
        return nil;
    }
    RDNA4DummyImageSlot *slot = writable ? &rdna4_dummy_null_write[shape] : &rdna4_dummy_null_read[shape][sampleSlot];
    uintptr_t owner = (uintptr_t)(__bridge void *)dev;
    @synchronized ([GarnetDevice class]) {
        if (slot->owner != owner) { slot->owner = owner; slot->texture = nil; }
        if (slot->texture) return slot->texture;
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.textureType = plan.textureType;
        td.pixelFormat = plan.pixelFormat;
        td.width = 1; td.height = 1; td.depth = 1; td.arrayLength = 1;
        td.mipmapLevelCount = 1; td.sampleCount = plan.sampleCount;
        td.usage = plan.usage; td.storageMode = plan.storageMode;
        NSError *e = nil;
        slot->texture = [[RDNA4Texture alloc] initWithDevice:dev descriptor:td error:&e];
        if (!slot->texture) {
            RDNA4LOG_ERR("dummy %s texture shape %u samples %u failed: %s", writable ? "storage" : "sampled",
                         shape, samples, e.localizedDescription.UTF8String);
        } else if (!plan.storage && !plan.depth && plan.sampleCount == 1) {
            rdna4_dummy_zero_texture(slot->texture);
        }
    }
    return slot->texture;
}

// Legacy entry point: all 48 ABI-5 values retain their previous acceptance or
// rejection, and every legal single-sample shape follows the old read-only path.
GARNET_INTERNAL RDNA4Texture *rdna4_dummy_null_texture(id dev, uint32_t shape) {
    return rdna4_dummy_null_image_legacy(dev, shape, NO, 1);
}

GARNET_INTERNAL RDNA4Texture *rdna4_dummy_null_image(id dev, const rdna4ns_shape *shape) {
    uint32_t legacy = 0;
    if (rdna4ns_shape_image_legacy(shape, &legacy) != RDNA4NS_OK) return nil;
    if (!shape->multisampled)
        return rdna4_dummy_null_image_legacy(dev, legacy,
            shape->access == RDNA4NS_ACCESS_STORAGE, 1);
    VkSampleCountFlags counts = shape->depth
        ? RDNA4R.lim.sampledImageDepthSampleCounts
        : shape->component == RDNA4NS_COMP_FLOAT
            ? RDNA4R.lim.sampledImageColorSampleCounts
            : RDNA4R.lim.sampledImageIntegerSampleCounts;
    for (uint32_t samples = 2; samples <= 8; samples *= 2) {
        if (!(counts & samples)) continue;
        RDNA4Texture *texture = rdna4_dummy_null_image_legacy(dev, legacy, NO, samples);
        if (texture) return texture;
    }
    return nil;
}

// Texel dummies use real VkBufferView descriptors, never image views.
typedef struct {
    VkBufferCreateInfo buffer;
    VkBufferViewCreateInfo view;
    VkDeviceSize offset;
    VkDeviceSize range;
} RDNA4DummyTexelPlan;

static inline uint32_t rdna4_dummy_texel_bytes(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT: return 4;
    default: return 0;
    }
}

static inline BOOL rdna4_dummy_texel_plan(BOOL storage, VkFormat format,
                                          VkFormatFeatureFlags bufferFeatures, BOOL featureEnabled,
                                          VkDeviceSize offset, VkDeviceSize range,
                                          VkDeviceSize offsetAlignment, uint64_t maxElements,
                                          RDNA4DummyTexelPlan *plan, const char **why) {
    if (why) *why = NULL;
    uint32_t texel = rdna4_dummy_texel_bytes(format);
    if (!texel || !plan) {
        if (why) *why = "texel dummies support R32 UINT/SINT/SFLOAT";
        return NO;
    }
    if (!featureEnabled) {
        if (why) *why = storage ? "storageTexelBuffer is not enabled" : "uniformTexelBuffer is not enabled";
        return NO;
    }
    VkFormatFeatureFlagBits required = storage ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT
                                               : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
    if (!(bufferFeatures & required)) {
        if (why) *why = storage ? "format lacks VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT"
                                : "format lacks VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT";
        return NO;
    }
    if (!offsetAlignment || (offsetAlignment & (offsetAlignment - 1))) {
        if (why) *why = "minTexelBufferOffsetAlignment is not a power of two";
        return NO;
    }
    if (offset % offsetAlignment) {
        if (why) *why = "buffer view offset violates minTexelBufferOffsetAlignment";
        return NO;
    }
    if (!maxElements) {
        if (why) *why = "maxTexelBufferElements is zero";
        return NO;
    }
    if (range == VK_WHOLE_SIZE) range = texel;
    if (!range || (range % texel)) {
        if (why) *why = "texel buffer view range must be a nonzero whole number of texels";
        return NO;
    }
    if (range / texel > maxElements) {
        if (why) *why = "texel buffer view exceeds maxTexelBufferElements";
        return NO;
    }
    if (offset > UINT64_MAX - range) {
        if (why) *why = "texel buffer size overflows VkDeviceSize";
        return NO;
    }
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        (storage ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT : VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
    *plan = (RDNA4DummyTexelPlan){
        .buffer = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = offset + range, .usage = usage },
        .view = { .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                  .buffer = VK_NULL_HANDLE, .format = format, .offset = offset, .range = range },
        .offset = offset, .range = range
    };
    return YES;
}

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkBufferView view;
} RDNA4DummyTexelSlot;

// One device owns RADV for the process; bounded caches have process lifetime,
// like the image dummies. Failed construction never publishes a partial slot.
static RDNA4DummyTexelSlot rdna4_dummy_texels[2][RDNA4NS_TEXEL_SHAPES];

GARNET_INTERNAL VkBufferView rdna4_dummy_null_texel(id dev, const rdna4ns_shape *shape,
                                          const char **why) {
    if (why) *why = NULL;
    if (!dev || !shape || !rdna4ns_shape_valid(shape) ||
        !rdna4ns_access_is_texel(shape->access)) {
        if (why) *why = "invalid null texel shape";
        return VK_NULL_HANDLE;
    }
    BOOL storage = shape->access == RDNA4NS_ACCESS_STORAGE_TEXEL;
    const VkFormat formats[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT};
    RDNA4DummyTexelSlot *slot = &rdna4_dummy_texels[storage][shape->component];
    @synchronized ([GarnetDevice class]) {
        if (slot->view) return slot->view;
        VkFormatProperties properties = {0};
        vkGetPhysicalDeviceFormatProperties(RDNA4R.pd, formats[shape->component], &properties);
        RDNA4DummyTexelPlan plan;
        if (!rdna4_dummy_texel_plan(storage, formats[shape->component], properties.bufferFeatures,
                storage ? RDNA4R.storageTexelBuffers : RDNA4R.uniformTexelBuffers,
                0, 4, RDNA4R.lim.minTexelBufferOffsetAlignment,
                RDNA4R.lim.maxTexelBufferElements, &plan, why)) return VK_NULL_HANDLE;
        RDNA4DummyTexelSlot built = {0};
        VkResult result = vkCreateBuffer(RDNA4R.dev, &plan.buffer, NULL, &built.buffer);
        if (result == VK_SUCCESS) {
            VkMemoryRequirements requirements = {0};
            vkGetBufferMemoryRequirements(RDNA4R.dev, built.buffer, &requirements);
            uint32_t memoryType = UINT32_MAX;
            VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            for (uint32_t i = 0; i < RDNA4R.mp.memoryTypeCount; ++i)
                if ((requirements.memoryTypeBits & (1u << i)) &&
                    (RDNA4R.mp.memoryTypes[i].propertyFlags & flags) == flags) {
                    memoryType = i;
                    break;
                }
            if (memoryType == UINT32_MAX) result = VK_ERROR_FEATURE_NOT_PRESENT;
            else {
                VkMemoryAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                    .allocationSize = requirements.size, .memoryTypeIndex = memoryType};
                result = vkAllocateMemory(RDNA4R.dev, &info, NULL, &built.memory);
            }
        }
        if (result == VK_SUCCESS)
            result = vkBindBufferMemory(RDNA4R.dev, built.buffer, built.memory, 0);
        if (result == VK_SUCCESS) {
            void *bytes = NULL;
            result = vkMapMemory(RDNA4R.dev, built.memory, 0, VK_WHOLE_SIZE, 0, &bytes);
            if (result == VK_SUCCESS) {
                memset(bytes, 0, 4);
                vkUnmapMemory(RDNA4R.dev, built.memory);
            }
        }
        if (result == VK_SUCCESS) {
            plan.view.buffer = built.buffer;
            result = vkCreateBufferView(RDNA4R.dev, &plan.view, NULL, &built.view);
        }
        if (result != VK_SUCCESS) {
            if (built.buffer) vkDestroyBuffer(RDNA4R.dev, built.buffer, NULL);
            if (built.memory) vkFreeMemory(RDNA4R.dev, built.memory, NULL);
            if (why) *why = "null texel buffer allocation, zeroing or view creation failed";
            return VK_NULL_HANDLE;
        }
        *slot = built;
        return slot->view;
    }
}

GARNET_INTERNAL RDNA4SamplerState *rdna4_dummy_sampler(id dev) {
    static RDNA4SamplerState *s;
    @synchronized ([GarnetDevice class]) {
        if (!s) { NSError *e = nil; s = [[RDNA4SamplerState alloc] initWithDevice:dev descriptor:[MTLSamplerDescriptor new] error:&e];
                  if (!s) RDNA4LOG_ERR("dummy sampler failed: %s", e.localizedDescription.UTF8String); }
    }
    return s;
}

// Visits every image a stage's bindings will read or write, with the layout the descriptor needs (SHADER_READ_ONLY for sampled images,
// GENERAL for storage images), so the encoder can transition them BEFORE the draw/dispatch (never inside a render pass).
GARNET_INTERNAL void rdna4_each_tex(const RDNA4PB *pb, uint32_t n, const RDNA4StageState *st, id dev, void (^fn)(RDNA4Texture *t, VkImageLayout l, BOOL writable)) {
    for (uint32_t i = 0; i < n; i++) {
        if ((pb[i].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || pb[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) && pb[i].binding >= 32 && pb[i].binding < 160) {
            for (uint32_t k = 0; k < pb[i].count; k++) { uint32_t ix = pb[i].binding - 32 + k;
                RDNA4Texture *t = pb[i].embedded ? (RDNA4Texture *)rdna4_embedded_resource(&pb[i], k, st, YES, dev) : (ix < 128 ? st->tex[ix] : nil);
                if (!t && !pb[i].embedded) t = rdna4_dummy_texture(dev); if (t) fn(t, pb[i].gpuArguments ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, NO); }
        } else if ((pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) && pb[i].binding >= 480 && pb[i].binding < 608) {
            for (uint32_t k = 0; k < pb[i].count; k++) {
                uint32_t ix = pb[i].binding - 480 + k;
                RDNA4Texture *t = pb[i].embedded ? (RDNA4Texture *)rdna4_embedded_resource(&pb[i], k, st, YES, dev) : (ix < 128 ? st->tex[ix] : nil);
                if (t) fn(t, VK_IMAGE_LAYOUT_GENERAL, YES);
            }
        }
    }
}

// KernelDispatchPlan::new (metal2vulkan src/reflect/mod.rs): a dispatchThreads grid becomes <= 8 rectangular regions (full groups + boundary tails).
GARNET_INTERNAL int rdna4_plan(const uint32_t threads[3], const uint32_t nom[3], RDNA4Region *out) {
    int n = 0;
    for (int mask = 0; mask < 8; mask++) {
        RDNA4Region r = { { nom[0], nom[1], nom[2] }, { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } }; BOOL nonempty = YES;
        for (int d = 0; d < 3; d++) {
            uint32_t full = threads[d] / nom[d], tail = threads[d] % nom[d];
            if (!(mask & (1 << d))) r.groups[d] = full;
            else if (tail == 0) nonempty = NO;
            else { r.local[d] = tail; r.groups[d] = 1; r.tbase[d] = full * nom[d]; r.gbase[d] = full; }
            nonempty = nonempty && r.groups[d] != 0;
        }
        if (nonempty) out[n++] = r;
    }
    return n;
}

#ifndef RDNA4_ENCODER_BINDINGS_INC
#define RDNA4_ENCODER_BINDINGS_INC


GARNET_INTERNAL uint64_t rdna4_resource_slot_key(uint32_t kind, BOOL fragment,
                                     NSUInteger index) {
    return ((uint64_t)kind << 56) |
        ((uint64_t)(fragment ? 1u : 0u) << 48) | (uint64_t)index;
}

GARNET_INTERNAL BOOL rdna4_encoder_take_segment_change(uint64_t *tracked,
                                              uint64_t current) {
    if (!tracked || *tracked == current) return NO;
    *tracked = current;
    return YES;
}

/*
 * Current encoder state is bounded by the descriptor bands. The flat unboxed
 * table owns only objects still bound to a slot; it is not a command-buffer
 * history. Every draw/dispatch incrementally snapshots dirty entries into the
 * current segment, and snapshots all entries when the command buffer or
 * segment serial changes.
 */

@implementation RDNA4ResourceBindings
- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    return self;
}
- (void)dealloc {
    for (uint32_t i = 0; i < _count; i++) {
        if (_entries[i].obj) CFRelease(_entries[i].obj);
    }
    if (_entries && _entries != _inline) free(_entries);
}
- (void)setObject:(id)object key:(uint64_t)key {
    if (!_entries) { _entries = _inline; _cap = 8u; }
    for (uint32_t i = 0; i < _count; i++) {
        if (_entries[i].key != key) continue;
        if (!object) {
            CFRelease(_entries[i].obj);
            _count--;
            if (i < _count) _entries[i] = _entries[_count];
            return;
        }
        if (_entries[i].obj == (__bridge CFTypeRef)object) return;
        CFRelease(_entries[i].obj);
        _entries[i].obj = (__bridge_retained CFTypeRef)object;
        _entries[i].snapshotted = 0u;
        _hasUnsnapshotted = YES;
        return;
    }
    if (!object) return;
    if (_count == _cap) {
        uint32_t nextCap = _cap ? _cap * 2u : 16u;
        RDNA4ResBindEntry *grown;
        if (_entries == _inline) {
            grown = malloc((size_t)nextCap * sizeof(*grown));
            if (!grown) return;
            if (_count) memcpy(grown, _inline, (size_t)_count * sizeof(*grown));
        } else {
            grown = realloc(_entries, (size_t)nextCap * sizeof(*grown));
            if (!grown) return;
        }
        _entries = grown;
        _cap = nextCap;
    }
    _entries[_count++] = (RDNA4ResBindEntry){
        .key = key,
        .obj = (__bridge_retained CFTypeRef)object,
        .snapshotted = 0u,
    };
    _hasUnsnapshotted = YES;
}
- (void)setObject:(id)object slot:(NSNumber *)slot {
    if (!slot) return;
    [self setObject:object key:slot.unsignedLongLongValue];
}
- (void)clear {
    for (uint32_t i = 0; i < _count; i++) {
        if (_entries[i].obj) CFRelease(_entries[i].obj);
    }
    _count = 0u;
    _lastCBId = 0u;
    _lastSegmentSerial = 0u;
    _hasUnsnapshotted = NO;
}
- (NSUInteger)count {
    return (NSUInteger)_count;
}
- (void)snapshotToCommandBuffer:(RDNA4CommandBuffer *)commandBuffer {
    if (!commandBuffer || !_count) return;
    const uint64_t cbid = [commandBuffer rdna4CBId];
    const uint64_t segSer = [commandBuffer rdna4SegmentSerial];
    const BOOL canCache = (cbid != 0u && segSer != 0u);
    if (canCache && cbid == _lastCBId && segSer == _lastSegmentSerial) {
        if (!_hasUnsnapshotted) return;
        for (uint32_t i = 0; i < _count; i++) {
            if (!_entries[i].snapshotted) {
                [commandBuffer rdna4RetainEncoded:(__bridge id)_entries[i].obj];
                _entries[i].snapshotted = 1u;
            }
        }
        _hasUnsnapshotted = NO;
        return;
    }
    for (uint32_t i = 0; i < _count; i++) {
        [commandBuffer rdna4RetainEncoded:(__bridge id)_entries[i].obj];
        _entries[i].snapshotted = 1u;
    }
    if (canCache) {
        _lastCBId = cbid;
        _lastSegmentSerial = segSer;
        _hasUnsnapshotted = NO;
    } else {
        _lastCBId = 0u;
        _lastSegmentSerial = 0u;
        _hasUnsnapshotted = YES;
    }
}
@end

#endif
