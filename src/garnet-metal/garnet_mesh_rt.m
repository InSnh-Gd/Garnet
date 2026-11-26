// garnet_mesh_rt.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_fragment_output.h"

// ---------------------------------------------------------------------------------------------------------------
// Metal 3 Hardware Ray Tracing Runtime Binding & MTLIntersectionFunctionTable (Task 7.3).
// Implements RDNA4IntersectionFunctionTable (MTLIntersectionFunctionTable + MTLResource) backed by a
// host-coherent Shared RDNA4Buffer matching 0077 (`word[0]=slot_count, word[1+slot]=handle_id`),
// compute/vertex/fragment AS & IFT encoder helpers (`VkWriteDescriptorSetAccelerationStructureKHR` at
// `BUFFER_BINDING_BASE + idx`), argument-buffer 64-bit BDA (`u64_device_address`) encoders + GPU
// retirement retention, pipeline IFT/handle helpers, and honest ray tracing capability helpers.
//
// Wave 3E integration checklist:
//   1. GarnetDevice.m:
//      a) Place right after `#include "rdna4_dynlib_vft_impl.m"` (before render/compute pipelines):
//           #include "rdna4_rt_runtime_impl.m"
//      b) Register protocols in +[GarnetDevice load] (belt-and-suspenders with +initialize):
//           { const char *n[] = { "MTLIntersectionFunctionTable", "MTLResource" };
//             rdna4_add_protocols([RDNA4IntersectionFunctionTable class], n, 2, "RDNA4IntersectionFunctionTable"); }
//      c) Wire honest MTLDevice ray tracing capability queries in GarnetDevice's implementation
//         (never open RADV inside capability queries):
//           - (BOOL)supportsRaytracing { return rdna4_rt_runtime_supports_raytracing(); }
//           - (BOOL)supportsRaytracingFromRender { return rdna4_rt_runtime_supports_raytracing_from_render(); }
//           - (BOOL)supportsPrimitiveMotionBlur { return rdna4_rt_runtime_supports_primitive_motion_blur(); }
//   2. garnet_spvrefl.h:
//      a) In rdna4s_parse(): recognize `case 5341: /* OpTypeAccelerationStructureKHR */ { RDNA4sId *t = ID(a[0]); t->op = op; break; }`
//         (and optionally track OpCapability 4472 RayQueryKHR).
//      b) In rdna4s_reflect(): map `else if (x->op == 5341) dt = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;`.
//   3. rdna4_shader_runtime_impl.inc:
//      a) When MTLLinkedFunctions contains `MTLFunctionTypeIntersection` functions, resolve
//         `garnetx_translate_stage3` via `rdna4x_ensure_stage3_xlib(...)`, classify each function via
//         `rdna4rtr_infer_ift_kind_from_ir(...)`, and pass `rdna4x_cift_fn[]` alongside deps/dynlibs/visFns.
//   4. rdna4_stage_bindings_impl.inc & rdna4_encoder_bindings.inc:
//      a) Add `RDNA4RTStageSlots rt;` to `RDNA4StageState`.
//      b) Add `RDNA4_RESOURCE_ACCEL = 7` to `rdna4_encoder_bindings.inc`.
//   5. rdna4_command_buffer_impl.inc:
//      a) Descriptor pool creation: size `VkDescriptorPoolSize sz[10]`, populate the 9 existing types, and
//         append `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` via `rdna4_rt_descriptor_pool_append(...)`.
//      b) In `rdna4FillSet:`: add stack arrays `VkWriteDescriptorSetAccelerationStructureKHR asw[MAXW];`
//         and `VkAccelerationStructureKHR ash[MAXI]; uint32_t nash = 0;` and handle
//         `case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:` via `rdna4_rt_fill_accel_descriptor(...)`.
//   6. rdna4_compute_pipeline_impl.inc & rdna4_render_pipeline_impl.inc:
//      a) Add ivars: `uint64_t _iftOwnerToken;` and `rdna4rtr_reflection_t _rt;` (or `_rtV, _rtF` on render).
//      b) Allocate `_iftOwnerToken = rdna4_vft_next_owner_token();` (distinct from `_vftOwnerToken` so
//         1-based VFT and IFT handle IDs can never collide!) and parse `_rt` via `rdna4_rt_parse_meta(...)`.
//      c) On render pipelines, pass `hasRenderRT = rdna4rtr_refl_uses_raytracing(&_rtV) || rdna4rtr_refl_uses_raytracing(&_rtF)`
//         to `rdna4_vft_validate_render_footnote10(...)` (AMD 6000-series Footnote 10 parity).
//      d) In `dealloc`: `rdna4rtr_refl_destroy(...)`.
//      e) Route `MTLFunctionTypeIntersection` in `functionHandleWithFunction:` / `:stage:` to
//         `rdna4_rt_pipeline_function_handle(...)`, and wire `newIntersectionFunctionTableWithDescriptor:` /
//         `newIntersectionFunctionTableWithDescriptor:stage:` to `rdna4_rt_pipeline_new_ift(...)`.
//   7. rdna4_compute_encoder_impl.inc & rdna4_render_encoder_impl.inc:
//      a) Wire `setAccelerationStructure:atBufferIndex:`, `setIntersectionFunctionTable:atBufferIndex:`,
//         `setIntersectionFunctionTables:withBufferRange:` (plus vertex/fragment variants on render) via
//         `rdna4_rt_encoder_bind_accel` / `rdna4_rt_encoder_bind_ift` / `rdna4_rt_encoder_bind_ifts`.
//      b) Clear `_st.rt.as[i] = nil` when `setBuffer:` binds a regular buffer at slot `i`.
//      c) In `rdna4DcStage:` (`rdna4_render_encoder_impl.inc`): handle `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`
//         via `rdna4_rt_dc_push_accel_sig(...)`.
//      d) On dispatch/draw, call `rdna4_rt_retain_argbuf_resources(buf, _cb)` for bound argument buffers.
//   8. rdna4_argument.inc:
//      a) Add `MTLDataTypeInstanceAccelerationStructure`, `MTLDataTypePrimitiveAccelerationStructure`,
//         and `MTLDataTypeIntersectionFunctionTable` to `rdna4_argument_scalar`, `constantOffsetAtIndex:`,
//         and `rdna4Destination:type:` (`rdna4_rt_arg_is_rt_data_type`).
//      b) Wire `setAccelerationStructure:atIndex:`, `setIntersectionFunctionTable:atIndex:`, and
//         `setIntersectionFunctionTables:withRange:` using `rdna4_rt_arg_resolve_as_address` /
//         `rdna4_rt_arg_resolve_ift_address` + `rdna4_argument_sampler_store(...)` + `rdna4_rt_arg_note(...)`.
// ---------------------------------------------------------------------------------------------------------------
#ifndef RDNA4_RT_RUNTIME_IMPL_INC
#define RDNA4_RT_RUNTIME_IMPL_INC


// Pin macOS 11/12 Metal enum bits against garnet_rt_runtime.h pure-C mirrors.
_Static_assert((uint64_t)MTLIntersectionFunctionSignatureNone == RDNA4RTR_SIG_NONE,
               "MTLIntersectionFunctionSignatureNone mismatch");
_Static_assert((uint64_t)MTLIntersectionFunctionSignatureInstancing == RDNA4RTR_SIG_INSTANCING,
               "MTLIntersectionFunctionSignatureInstancing mismatch");
_Static_assert((uint64_t)MTLIntersectionFunctionSignatureTriangleData == RDNA4RTR_SIG_TRIANGLE_DATA,
               "MTLIntersectionFunctionSignatureTriangleData mismatch");
_Static_assert((uint64_t)MTLIntersectionFunctionSignatureWorldSpaceData == RDNA4RTR_SIG_WORLD_SPACE_DATA,
               "MTLIntersectionFunctionSignatureWorldSpaceData mismatch");
_Static_assert((uint64_t)MTLIntersectionFunctionSignatureInstanceMotion == RDNA4RTR_SIG_INSTANCE_MOTION,
               "MTLIntersectionFunctionSignatureInstanceMotion mismatch");
_Static_assert((uint64_t)MTLIntersectionFunctionSignaturePrimitiveMotion == RDNA4RTR_SIG_PRIMITIVE_MOTION,
               "MTLIntersectionFunctionSignaturePrimitiveMotion mismatch");
_Static_assert((uint64_t)MTLIntersectionFunctionSignatureExtendedLimits == RDNA4RTR_SIG_EXTENDED_LIMITS,
               "MTLIntersectionFunctionSignatureExtendedLimits mismatch");
_Static_assert((int)RDNA4RTR_AS_KIND_UNKNOWN == (int)RDNA4RT_KIND_UNKNOWN,
               "AS kind UNKNOWN mismatch with garnet_rt_accel.h");
_Static_assert((int)RDNA4RTR_AS_KIND_PRIMITIVE == (int)RDNA4RT_KIND_BLAS,
               "AS kind PRIMITIVE mismatch with RDNA4RT_KIND_BLAS");
_Static_assert((int)RDNA4RTR_AS_KIND_INSTANCE == (int)RDNA4RT_KIND_TLAS,
               "AS kind INSTANCE mismatch with RDNA4RT_KIND_TLAS");


// Exact C ABI match for third-party/metal2vulkan-rdna4/garnetxlate/garnetxlate.h Stage 3 export.

__attribute__((unused)) static BOOL rdna4_rt_runtime_translator_stage3_ready(void) {
    if (!rdna4x_active()) return NO;
    rdna4x_translate_stage3_fn fn = NULL;
    return rdna4x_ensure_stage3_xlib(NULL, &fn, NULL) && fn != NULL;
}

// Builds honest ray tracing capabilities without opening RADV (rdna4_metal3_caps returns zeroed
// fallback until RADV opens, and probed GFX12 capabilities thereafter).
__attribute__((unused)) static rdna4rtr_hw_caps_t rdna4_rt_runtime_caps(void) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    const uint8_t vkRtOk = rdna4dc_metal3_raytracing_supported(&m3) ? 1u : 0u;
    const uint8_t stage3Ok = rdna4_rt_runtime_translator_stage3_ready() ? 1u : 0u;
    return (rdna4rtr_hw_caps_t){
        .accel_struct_supported    = vkRtOk,
        .ray_query_supported       = vkRtOk,
        .buffer_device_address     = vkRtOk,
        .deferred_host_ops         = vkRtOk,
        .translator_stage3_ready   = stage3Ok,
        .motion_blur_ext_supported = 0u, // RADV GFX12 lacks VK_NV_ray_tracing_motion_blur
        .curve_geometry_supported  = 0u, // Metal 3.1 curve RT is outside Metal 3.0 / RADV GFX12
    };
}

__attribute__((unused)) GARNET_INTERNAL BOOL rdna4_rt_runtime_supports_raytracing(void) {
    const rdna4rtr_hw_caps_t c = rdna4_rt_runtime_caps();
    return rdna4rtr_supports_raytracing(&c) ? YES : NO;
}

__attribute__((unused)) GARNET_INTERNAL BOOL rdna4_rt_runtime_supports_raytracing_from_render(void) {
    const rdna4rtr_hw_caps_t c = rdna4_rt_runtime_caps();
    return rdna4rtr_supports_raytracing_from_render(&c) ? YES : NO;
}

__attribute__((unused)) GARNET_INTERNAL BOOL rdna4_rt_runtime_supports_primitive_motion_blur(void) {
    const rdna4rtr_hw_caps_t c = rdna4_rt_runtime_caps();
    return rdna4rtr_supports_primitive_motion_blur(&c) ? YES : NO;
}

// Helper for retaining objects on an RDNA4CommandBuffer without requiring its full @interface ahead of include order.
static inline void rdna4_rt_cb_retain(id cb, id obj) {
    if (!cb || !obj) return;
    SEL sel = sel_registerName("rdna4RetainEncoded:");
    if ([cb respondsToSelector:sel])
        ((void (*)(id, SEL, id))objc_msgSend)(cb, sel, obj);
}

static inline void rdna4_rt_cb_fail(id cb, NSString *msg) {
    if (!cb) return;
    SEL sel = sel_registerName("rdna4Fail:");
    if ([cb respondsToSelector:sel])
        ((void (*)(id, SEL, id))objc_msgSend)(cb, sel, msg ?: @"ray tracing runtime error");
}

// =================================================================================================
// RDNA4IntersectionFunctionTable (MTLIntersectionFunctionTable + MTLResource)
// =================================================================================================

@implementation RDNA4IntersectionFunctionTable

+ (void)initialize {
    if (self == [RDNA4IntersectionFunctionTable class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLIntersectionFunctionTable", "MTLResource" };
            rdna4_add_protocols(self, n, 2, "RDNA4IntersectionFunctionTable");
        });
    }
}

RDNA4_DNR(RDNA4IntersectionFunctionTable)

- (BOOL)rdna4SyncBackingLocked {
    void *ptr = _backing ? [_backing contents] : NULL;
    if (!ptr) return NO;
    size_t capWords = [_backing length] / sizeof(uint32_t);
    size_t written = 0;
    return rdna4rtr_ift_encode(&_table, (uint32_t *)ptr, capWords, &written) == RDNA4RTR_OK;
}

- (instancetype)initWithDevice:(id)dev
                    descriptor:(MTLIntersectionFunctionTableDescriptor *)descriptor
                    ownerToken:(uint64_t)token
                         stage:(NSUInteger)stage
                          refl:(const rdna4rtr_ift_reflection_t *)refl
                         error:(NSError **)error {
    if (error) *error = nil;
    if (!dev || !descriptor) {
        if (error) *error = rdna4_err(145, @"intersection function table requires non-nil device and descriptor");
        return nil;
    }
    NSUInteger count = descriptor.functionCount;
    if (count > RDNA4RTR_MAX_IFT_SLOTS) {
        if (error) *error = rdna4_err(145, @"intersection function table functionCount exceeds limit");
        return nil;
    }
    uint32_t maxHid = (refl && refl->present) ? refl->max_handle_id : 0u;
    if (maxHid > RDNA4RTR_MAX_IFT_HANDLES) {
        if (error) *error = rdna4_err(145, @"intersection function table max handle ID exceeds limit");
        return nil;
    }
    self = [super init];
    if (!self) return nil;
    _dev = dev;
    _ownerToken = token;
    _stage = stage;
    _lock = [NSLock new];

    rdna4rtr_status_t st = rdna4rtr_ift_init(&_table, (uint32_t)count, maxHid);
    if (st != RDNA4RTR_OK) {
        if (error) *error = rdna4_err(145, [NSString stringWithFormat:@"intersection function table init failed: %s",
                                            rdna4rtr_status_str(st)]);
        return nil;
    }
    if (maxHid > 0u) {
        _handleKinds = (uint8_t *)malloc((size_t)maxHid + 1u);
        if (!_handleKinds) {
            rdna4rtr_ift_destroy(&_table);
            if (error) *error = rdna4_err(145, @"out of memory allocating IFT handle-kind table");
            return nil;
        }
        memset(_handleKinds, 0xFF, (size_t)maxHid + 1u);
        if (refl && refl->present) {
            for (uint32_t i = 0; i < refl->handle_count; i++) {
                uint32_t hid = refl->handles[i].handle_id;
                if (hid >= 1u && hid <= maxHid)
                    _handleKinds[hid] = (uint8_t)refl->handles[i].kind;
            }
        }
    }

    // Backing Shared RDNA4Buffer holds [slot_count, handle_id_0..handle_id_{N-1}] matching 0077 (>= 16 B aligned).
    size_t allocBytes = rdna4rtr_ift_aligned_buffer_bytes((uint32_t)count);
    _backing = [[RDNA4Buffer alloc] initWithDevice:dev length:allocBytes options:MTLResourceStorageModeShared error:error];
    void *ptr = _backing ? [_backing contents] : NULL;
    if (!ptr) {
        free(_handleKinds); _handleKinds = NULL;
        rdna4rtr_ift_destroy(&_table);
        return nil;
    }
    memset(ptr, 0, allocBytes);
    if (![self rdna4SyncBackingLocked]) {
        free(_handleKinds); _handleKinds = NULL;
        rdna4rtr_ift_destroy(&_table);
        if (error) *error = rdna4_err(145, @"failed to initialize IFT backing buffer");
        return nil;
    }
    RDNA4LOGR("RDNA4IntersectionFunctionTable %p: slots=%lu maxHandle=%u buf=%lu B",
              (__bridge void *)self, (unsigned long)count, maxHid, (unsigned long)allocBytes);
    return self;
}

- (instancetype)initWithDevice:(id)dev
                    descriptor:(MTLIntersectionFunctionTableDescriptor *)descriptor
                    ownerToken:(uint64_t)token
                          refl:(const rdna4rtr_ift_reflection_t *)refl {
    return [self initWithDevice:dev descriptor:descriptor ownerToken:token stage:0u refl:refl error:nil];
}

- (void)dealloc {
    free(_handleKinds);
    _handleKinds = NULL;
    rdna4rtr_ift_destroy(&_table);
}

- (BOOL)rdna4ValidateHandleLocked:(id)function handleIdOut:(uint32_t *)hidOut kindOut:(uint32_t *)kindOut {
    *hidOut = RDNA4RTR_IFT_HANDLE_NULL;
    *kindOut = RDNA4RTR_IFT_KIND_BOUNDING_BOX;
    if (!function) return YES;
    if (![function isKindOfClass:[RDNA4FunctionHandle class]]) {
        RDNA4LOG("RDNA4IntersectionFunctionTable: rejected non-RDNA4FunctionHandle object");
        return NO;
    }
    RDNA4FunctionHandle *fh = (RDNA4FunctionHandle *)function;
    if ([fh functionType] != MTLFunctionTypeIntersection) {
        RDNA4LOG("RDNA4IntersectionFunctionTable: rejected handle '%s' with non-intersection functionType %lu",
                 [fh name].UTF8String ?: "?", (unsigned long)[fh functionType]);
        return NO;
    }
    if ([fh device] != _dev || [fh rdna4OwnerToken] != _ownerToken || [fh rdna4Stage] != _stage) {
        RDNA4LOG("RDNA4IntersectionFunctionTable: rejected handle '%s' from mismatched pipeline/stage/device",
                 [fh name].UTF8String ?: "?");
        return NO;
    }
    uint32_t hid = [fh rdna4HandleId];
    if (hid == RDNA4RTR_IFT_HANDLE_NULL || hid > _table.max_handle_id ||
        !_handleKinds || _handleKinds[hid] == 0xFFu) {
        RDNA4LOG("RDNA4IntersectionFunctionTable: rejected invalid handle id %u (max %u)", hid, _table.max_handle_id);
        return NO;
    }
    *hidOut = hid;
    *kindOut = (uint32_t)_handleKinds[hid];
    return YES;
}

- (void)setFunction:(id<MTLFunctionHandle>)function atIndex:(NSUInteger)index {
    [_lock lock];
    if (index >= _table.slot_count) {
        RDNA4LOG("RDNA4IntersectionFunctionTable setFunction:atIndex:%lu out of bounds (count=%u)",
                 (unsigned long)index, _table.slot_count);
        [_lock unlock];
        return;
    }
    uint32_t hid = RDNA4RTR_IFT_HANDLE_NULL;
    uint32_t kind = RDNA4RTR_IFT_KIND_BOUNDING_BOX;
    if ([self rdna4ValidateHandleLocked:function handleIdOut:&hid kindOut:&kind]) {
        if (rdna4rtr_ift_set_function(&_table, (uint32_t)index, hid, kind) == RDNA4RTR_OK)
            (void)[self rdna4SyncBackingLocked];
    }
    [_lock unlock];
}

- (void)setFunctions:(const id<MTLFunctionHandle> __unsafe_unretained *)functions withRange:(NSRange)range {
    if (range.length == 0) return;
    if (!functions) {
        RDNA4LOG("RDNA4IntersectionFunctionTable setFunctions:withRange: NULL array");
        return;
    }
    [_lock lock];
    if (range.location > _table.slot_count || range.length > _table.slot_count - range.location) {
        RDNA4LOG("RDNA4IntersectionFunctionTable setFunctions:withRange:[%lu,+%lu) out of bounds (count=%u)",
                 (unsigned long)range.location, (unsigned long)range.length, _table.slot_count);
        [_lock unlock];
        return;
    }
    uint32_t *ids = (uint32_t *)malloc(range.length * sizeof(uint32_t));
    uint32_t *kinds = (uint32_t *)malloc(range.length * sizeof(uint32_t));
    if (!ids || !kinds) {
        free(ids); free(kinds);
        [_lock unlock];
        return;
    }
    BOOL valid = YES;
    for (NSUInteger i = 0; i < range.length; i++) {
        if (![self rdna4ValidateHandleLocked:functions[i] handleIdOut:&ids[i] kindOut:&kinds[i]]) {
            valid = NO;
            break;
        }
    }
    if (valid &&
        rdna4rtr_ift_set_functions_range(&_table, (uint32_t)range.location, (uint32_t)range.length, ids, kinds) == RDNA4RTR_OK)
        (void)[self rdna4SyncBackingLocked];
    free(ids);
    free(kinds);
    [_lock unlock];
}

- (void)setOpaqueTriangleIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)signature
                                                   atIndex:(NSUInteger)index {
    [_lock lock];
    rdna4rtr_status_t st = (index <= UINT32_MAX)
        ? rdna4rtr_ift_set_opaque_triangle(&_table, (uint32_t)index, (uint64_t)signature)
        : RDNA4RTR_ERR_BOUNDS;
    if (st == RDNA4RTR_OK) {
        (void)[self rdna4SyncBackingLocked];
    } else {
        RDNA4LOG("RDNA4IntersectionFunctionTable setOpaqueTriangleIntersectionFunctionWithSignature:0x%lx atIndex:%lu rejected: %s",
                 (unsigned long)signature, (unsigned long)index, rdna4rtr_status_str(st));
    }
    [_lock unlock];
}

- (void)setOpaqueTriangleIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)signature
                                                 withRange:(NSRange)range {
    if (range.length == 0) return;
    [_lock lock];
    rdna4rtr_status_t st = (range.location <= UINT32_MAX && range.length <= UINT32_MAX)
        ? rdna4rtr_ift_set_opaque_triangle_range(&_table, (uint32_t)range.location, (uint32_t)range.length, (uint64_t)signature)
        : RDNA4RTR_ERR_BOUNDS;
    if (st == RDNA4RTR_OK) {
        (void)[self rdna4SyncBackingLocked];
    } else {
        RDNA4LOG("RDNA4IntersectionFunctionTable setOpaqueTriangleIntersectionFunctionWithSignature:0x%lx withRange:[%lu,+%lu) rejected: %s",
                 (unsigned long)signature, (unsigned long)range.location, (unsigned long)range.length, rdna4rtr_status_str(st));
    }
    [_lock unlock];
}

- (void)setOpaqueCurveIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)signature
                                                atIndex:(NSUInteger)index {
    [self setOpaqueCurveIntersectionFunctionWithSignature:signature withRange:NSMakeRange(index, 1u)];
}

- (void)setOpaqueCurveIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)signature
                                              withRange:(NSRange)range {
    if (range.length == 0) return;
    [_lock lock];
    rdna4rtr_status_t st = (range.location <= UINT32_MAX && range.length <= UINT32_MAX)
        ? rdna4rtr_ift_set_opaque_curve_range(&_table, (uint32_t)range.location, (uint32_t)range.length, (uint64_t)signature)
        : RDNA4RTR_ERR_BOUNDS;
    if (st == RDNA4RTR_ERR_UNSUPPORTED) {
        // Safe handle_id = 0 has been encoded; sync so GPU never reads stale handles.
        (void)[self rdna4SyncBackingLocked];
        RDNA4_ONCE("RDNA4IntersectionFunctionTable: curve intersection functions are unsupported on RADV GFX12 (encoded safe null handle)");
    } else if (st != RDNA4RTR_OK) {
        RDNA4LOG("RDNA4IntersectionFunctionTable setOpaqueCurveIntersectionFunctionWithSignature:0x%lx withRange:[%lu,+%lu) rejected: %s",
                 (unsigned long)signature, (unsigned long)range.location, (unsigned long)range.length, rdna4rtr_status_str(st));
    }
    [_lock unlock];
}

- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index >= RDNA4RTR_MAX_ATTACH_BUFFERS) {
        RDNA4LOG("RDNA4IntersectionFunctionTable setBuffer:atIndex:%lu out of bounds", (unsigned long)index);
        return;
    }
    if (buffer && (![buffer isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)buffer device] != _dev)) {
        RDNA4LOG("RDNA4IntersectionFunctionTable setBuffer: invalid buffer class or device");
        return;
    }
    [_lock lock];
    _attachedBuffers[index] = (RDNA4Buffer *)buffer;
    _attachedBufferOffsets[index] = buffer ? offset : 0u;
    [_lock unlock];
}

- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained *)buffers
           offsets:(const NSUInteger *)offsets
         withRange:(NSRange)range {
    if (range.length == 0) return;
    if (!buffers || !offsets ||
        range.location > RDNA4RTR_MAX_ATTACH_BUFFERS ||
        range.length > RDNA4RTR_MAX_ATTACH_BUFFERS - range.location) {
        RDNA4LOG("RDNA4IntersectionFunctionTable setBuffers:offsets:withRange: invalid range or NULL array");
        return;
    }
    for (NSUInteger i = 0; i < range.length; i++)
        [self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}

- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)functionTable
                  atBufferIndex:(NSUInteger)bufferIndex {
    if (bufferIndex >= RDNA4RTR_MAX_NESTED_VFTS) {
        RDNA4LOG("RDNA4IntersectionFunctionTable setVisibleFunctionTable:atBufferIndex:%lu out of bounds",
                 (unsigned long)bufferIndex);
        return;
    }
    if (functionTable &&
        (![functionTable isKindOfClass:[RDNA4VisibleFunctionTable class]] ||
         [(RDNA4VisibleFunctionTable *)functionTable device] != _dev)) {
        RDNA4LOG("RDNA4IntersectionFunctionTable: rejected invalid nested VFT class or device");
        return;
    }
    [_lock lock];
    _nestedVFTs[bufferIndex] = functionTable;
    [_lock unlock];
}

- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)functionTables
                 withBufferRange:(NSRange)bufferRange {
    if (bufferRange.length == 0) return;
    if (!functionTables ||
        bufferRange.location > RDNA4RTR_MAX_NESTED_VFTS ||
        bufferRange.length > RDNA4RTR_MAX_NESTED_VFTS - bufferRange.location) {
        RDNA4LOG("RDNA4IntersectionFunctionTable setVisibleFunctionTables:withBufferRange: invalid range or NULL array");
        return;
    }
    for (NSUInteger i = 0; i < bufferRange.length; i++)
        [self setVisibleFunctionTable:functionTables[i] atBufferIndex:bufferRange.location + i];
}

- (RDNA4Buffer *)rdna4BackingBuffer { return _backing; }
- (uint64_t)rdna4BackingGPUAddress { return _backing ? [_backing gpuAddress] : 0u; }
- (NSUInteger)rdna4FunctionCount { return (NSUInteger)_table.slot_count; }
- (uint64_t)rdna4OwnerToken { return _ownerToken; }
- (NSUInteger)rdna4Stage { return _stage; }

- (uint32_t)rdna4HandleIdAtIndex:(NSUInteger)index {
    [_lock lock];
    uint32_t hid = (index <= UINT32_MAX) ? rdna4rtr_ift_get_handle(&_table, (uint32_t)index) : RDNA4RTR_IFT_HANDLE_NULL;
    [_lock unlock];
    return hid;
}

- (rdna4rtr_ift_slot_mode_t)rdna4SlotModeAtIndex:(NSUInteger)index {
    [_lock lock];
    rdna4rtr_ift_slot_mode_t mode = (index <= UINT32_MAX)
        ? rdna4rtr_ift_get_mode(&_table, (uint32_t)index) : RDNA4RTR_IFT_SLOT_EMPTY;
    [_lock unlock];
    return mode;
}

- (RDNA4Buffer *)rdna4AttachedBufferAtIndex:(NSUInteger)index offset:(NSUInteger *)offsetOut {
    if (offsetOut) *offsetOut = 0;
    if (index >= RDNA4RTR_MAX_ATTACH_BUFFERS) return nil;
    [_lock lock];
    RDNA4Buffer *b = _attachedBuffers[index];
    if (offsetOut) *offsetOut = _attachedBufferOffsets[index];
    [_lock unlock];
    return b;
}

- (id)rdna4NestedVFTAtBufferIndex:(NSUInteger)index {
    if (index >= RDNA4RTR_MAX_NESTED_VFTS) return nil;
    [_lock lock];
    id v = _nestedVFTs[index];
    [_lock unlock];
    return v;
}

- (void)rdna4RetainIntoCommandBuffer:(id)cb {
    if (!cb) return;
    rdna4_rt_cb_retain(cb, self);
    if (_backing) rdna4_rt_cb_retain(cb, _backing);
    NSMutableArray *nestedVfts = nil;
    [_lock lock];
    for (uint32_t i = 0; i < RDNA4RTR_MAX_ATTACH_BUFFERS; i++) {
        if (_attachedBuffers[i]) rdna4_rt_cb_retain(cb, _attachedBuffers[i]);
    }
    for (uint32_t i = 0; i < RDNA4RTR_MAX_NESTED_VFTS; i++) {
        if (_nestedVFTs[i]) {
            if (!nestedVfts) nestedVfts = [NSMutableArray array];
            [nestedVfts addObject:_nestedVFTs[i]];
        }
    }
    [_lock unlock];
    for (RDNA4VisibleFunctionTable *vft in nestedVfts)
        [vft rdna4RetainIntoCommandBuffer:cb];
}

// ---- MTLResource surface (RDNA4_RESOURCE_SPI provides gpuResourceID + uniqueIdentifier) ----
- (id)device { return _dev; }
- (NSString *)label { [_lock lock]; NSString *l = _lbl; [_lock unlock]; return l; }
- (void)setLabel:(NSString *)label { [_lock lock]; _lbl = [label copy]; [_lock unlock]; }
- (MTLStorageMode)storageMode { return MTLStorageModeShared; }
- (MTLCPUCacheMode)cpuCacheMode { return MTLCPUCacheModeDefaultCache; }
- (MTLHazardTrackingMode)hazardTrackingMode { return MTLHazardTrackingModeDefault; }
- (MTLResourceOptions)resourceOptions { return MTLResourceStorageModeShared; }
- (NSUInteger)allocatedSize { return _backing ? [_backing allocatedSize] : 0u; }
- (RDNA4Heap *)rdna4ResourceHeap { return nil; }
- (NSUInteger)rdna4ResourceHeapOffset { return 0; }
- (BOOL)rdna4ResourceAliasable { return NO; }
- (void)rdna4ResourceMakeAliasable {}
- (BOOL)rdna4ResourceAliases:(id)other { (void)other; return NO; }
RDNA4_RESOURCE_SPI

@end

// =================================================================================================
// Stage Binding State, Descriptor Pool, Descriptor Set Fill, and Descriptor Cache Helpers
// =================================================================================================

// Parallel to RDNA4StageState.buf[31]: holds __unsafe_unretained GarnetAccelerationStructure*
// (strong lifetime owned via RDNA4ResourceBindings + command-buffer retirement retention).

// Appends VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR to a descriptor pool size array ONLY when
// VK_KHR_acceleration_structure is active on the Vulkan device.
__attribute__((unused)) GARNET_INTERNAL uint32_t rdna4_rt_descriptor_pool_append(
    VkDescriptorPoolSize *sizes,
    uint32_t count,
    uint32_t capacity,
    uint32_t divisor)
{
    if (!sizes || count >= capacity || divisor == 0u) return count;
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    if (!rdna4dc_metal3_raytracing_supported(&m3)) return count;
    uint32_t asDescs = 64u / divisor;
    if (asDescs < 4u) asDescs = 4u;
    sizes[count] = (VkDescriptorPoolSize){
        .type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        .descriptorCount = asDescs,
    };
    return count + 1u;
}

static id<GarnetAccelerationStructureRuntimeInternal> rdna4_rt_validate_accel(
    id accel, id expectedDevice, NSString **whyOut)
{
    if (!accel) return nil;
    Class cls = objc_getClass("GarnetAccelerationStructure");
    if (!cls || ![accel isKindOfClass:cls]) {
        if (whyOut) *whyOut = @"acceleration structure is not an GarnetAccelerationStructure";
        return nil;
    }
    id<GarnetAccelerationStructureRuntimeInternal> as = (id<GarnetAccelerationStructureRuntimeInternal>)accel;
    if (expectedDevice && [as device] != expectedDevice) {
        if (whyOut) *whyOut = @"acceleration structure belongs to another MTLDevice";
        return nil;
    }
    return as;
}

// Compute, vertex, and fragment encoder helper for set[Vertex|Fragment]AccelerationStructure:atBufferIndex:.
// On success returns YES; caller should also clear its regular buffer slot at `bufferIndex` and record
// `accel` in `_resourceBindings` under RDNA4_RESOURCE_ACCEL.
__attribute__((unused)) GARNET_INTERNAL BOOL rdna4_rt_encoder_bind_accel(
    id cb,
    id dev,
    const char *stageName,
    RDNA4RTStageSlots *rtSlots,
    id accel,
    NSUInteger bufferIndex)
{
    if (!cb || !rtSlots) return NO;
    if (bufferIndex >= RDNA4RTR_MAX_BUFFER_SLOTS) {
        rdna4_rt_cb_fail(cb, [NSString stringWithFormat:@"%s setAccelerationStructure: buffer index %lu >= %u",
                              stageName ?: "rt", (unsigned long)bufferIndex, RDNA4RTR_MAX_BUFFER_SLOTS]);
        return NO;
    }
    if (!accel) {
        rtSlots->as[bufferIndex] = nil;
        return YES;
    }
    NSString *why = nil;
    id<GarnetAccelerationStructureRuntimeInternal> as = rdna4_rt_validate_accel(accel, dev, &why);
    if (!as) {
        rdna4_rt_cb_fail(cb, [NSString stringWithFormat:@"%s setAccelerationStructure: %@",
                              stageName ?: "rt", why ?: @"invalid object"]);
        return NO;
    }
    rtSlots->as[bufferIndex] = accel;
    rdna4_rt_cb_retain(cb, accel);
    id handle = [as rdna4CurrentHandle];
    if (handle) rdna4_rt_cb_retain(cb, handle);
    return YES;
}

// Compute, vertex, and fragment encoder helper for set[Vertex|Fragment]IntersectionFunctionTable:atBufferIndex:.
// Validates IFT, clears any AS previously bound at `bufferIndex`, retains the IFT & dependencies into `cb`,
// and writes the backing Shared RDNA4Buffer (or nil when `ift == nil`) to `*backingBufOut`.
__attribute__((unused)) GARNET_INTERNAL BOOL rdna4_rt_encoder_bind_ift(
    id cb,
    id dev,
    const char *stageName,
    RDNA4RTStageSlots *rtSlots,
    id ift,
    NSUInteger bufferIndex,
    RDNA4Buffer * __autoreleasing *backingBufOut)
{
    if (backingBufOut) *backingBufOut = nil;
    if (!cb || !rtSlots) return NO;
    if (bufferIndex >= RDNA4RTR_MAX_BUFFER_SLOTS) {
        rdna4_rt_cb_fail(cb, [NSString stringWithFormat:@"%s setIntersectionFunctionTable: buffer index %lu >= %u",
                              stageName ?: "rt", (unsigned long)bufferIndex, RDNA4RTR_MAX_BUFFER_SLOTS]);
        return NO;
    }
    rtSlots->as[bufferIndex] = nil;
    if (!ift) return YES;
    if (![ift isKindOfClass:[RDNA4IntersectionFunctionTable class]] ||
        [(RDNA4IntersectionFunctionTable *)ift device] != dev) {
        rdna4_rt_cb_fail(cb, [NSString stringWithFormat:@"%s setIntersectionFunctionTable: not an RDNA4IntersectionFunctionTable for this device",
                              stageName ?: "rt"]);
        return NO;
    }
    RDNA4IntersectionFunctionTable *tbl = (RDNA4IntersectionFunctionTable *)ift;
    [tbl rdna4RetainIntoCommandBuffer:cb];
    if (backingBufOut) *backingBufOut = [tbl rdna4BackingBuffer];
    return YES;
}

// Range helper for set[Vertex|Fragment]IntersectionFunctionTables:withBufferRange:.
__attribute__((unused)) GARNET_INTERNAL BOOL rdna4_rt_encoder_bind_ifts(
    id cb,
    id dev,
    const char *stageName,
    RDNA4RTStageSlots *rtSlots,
    const id<MTLIntersectionFunctionTable> __unsafe_unretained *tables,
    NSRange range,
    void (^bindBacking)(RDNA4Buffer *buf, NSUInteger bufferIndex))
{
    if (range.length == 0) return YES;
    if (!tables || range.location > RDNA4RTR_MAX_BUFFER_SLOTS ||
        range.length > RDNA4RTR_MAX_BUFFER_SLOTS - range.location) {
        rdna4_rt_cb_fail(cb, [NSString stringWithFormat:@"%s setIntersectionFunctionTables: invalid range or NULL array",
                              stageName ?: "rt"]);
        return NO;
    }
    for (NSUInteger k = 0; k < range.length; k++) {
        RDNA4Buffer *backing = nil;
        if (!rdna4_rt_encoder_bind_ift(cb, dev, stageName, rtSlots, tables[k], range.location + k, &backing))
            return NO;
        if (bindBacking) bindBacking(backing, range.location + k);
    }
    return YES;
}

// Fills one VkWriteDescriptorSet for VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR at
// binding = BUFFER_BINDING_BASE + metal_index. Caller owns `asWriteSlot` and `asHandlesOut`
// until vkUpdateDescriptorSets completes.
__attribute__((unused)) GARNET_INTERNAL BOOL rdna4_rt_fill_accel_descriptor(
    id cb,
    id dev,
    const char *stageName,
    rdna4rtr_stage_t stage,
    uint32_t binding,
    uint32_t count,
    const RDNA4RTStageSlots *rtSlots,
    const rdna4rtr_as_reflection_t *asRefl,
    VkWriteDescriptorSet *dstWrite,
    VkWriteDescriptorSetAccelerationStructureKHR *asWriteSlot,
    VkAccelerationStructureKHR *asHandlesOut,
    uint32_t *asHandleCursor,
    uint32_t asHandleCap)
{
    if (!cb || !dstWrite || !asWriteSlot || !asHandlesOut || !asHandleCursor || count == 0u) return NO;
    uint32_t firstMetalIdx = 0u;
    if (!rdna4rtr_binding_to_metal_index(binding, count, &firstMetalIdx)) {
        rdna4_rt_cb_fail(cb, [NSString stringWithFormat:@"%s binding %u count %u: acceleration structure outside buffer band",
                              stageName ?: "rt", binding, count]);
        return NO;
    }
    if (count > asHandleCap - *asHandleCursor) {
        rdna4_rt_cb_fail(cb, [NSString stringWithFormat:@"%s binding %u: acceleration structure descriptor array overflow",
                              stageName ?: "rt", binding]);
        return NO;
    }
    const uint32_t baseIdx = *asHandleCursor;
    for (uint32_t k = 0; k < count; k++) {
        const uint32_t metalIdx = firstMetalIdx + k;
        id raw = rtSlots ? rtSlots->as[metalIdx] : nil;
        if (!raw) {
            rdna4_rt_cb_fail(cb, [NSString stringWithFormat:@"%s accelerationStructure(%u) is used by the shader but was never bound",
                                  stageName ?: "rt", metalIdx]);
            return NO;
        }
        NSString *why = nil;
        id<GarnetAccelerationStructureRuntimeInternal> as = rdna4_rt_validate_accel(raw, dev, &why);
        if (!as) {
            rdna4_rt_cb_fail(cb, [NSString stringWithFormat:@"%s accelerationStructure(%u): %@",
                                  stageName ?: "rt", metalIdx, why ?: @"invalid"]);
            return NO;
        }
        const VkAccelerationStructureKHR vkAS = [as rdna4VkAccelerationStructure];
        const rdna4rtr_as_kind_t asKind = (rdna4rtr_as_kind_t)[as rdna4AccelKind];
        const BOOL isBuilt = [as rdna4IsBuilt];
        const rdna4rtr_as_binding_t *refEnt = asRefl ? rdna4rtr_as_find_descriptor(asRefl, metalIdx) : NULL;
        const rdna4rtr_as_kind_t wantKind = refEnt ? refEnt->kind : RDNA4RTR_AS_KIND_UNKNOWN;

        rdna4rtr_as_desc_plan_t plan;
        rdna4rtr_status_t st = rdna4rtr_plan_as_descriptor_bind(
            metalIdx, stage, (uint64_t)(uintptr_t)vkAS, asKind, isBuilt ? 1 : 0, wantKind, &plan);
        if (st != RDNA4RTR_OK) {
            rdna4_rt_cb_fail(cb, [NSString stringWithFormat:@"%s accelerationStructure(%u): %s",
                                  stageName ?: "rt", metalIdx, rdna4rtr_status_str(st)]);
            return NO;
        }
        rdna4_rt_cb_retain(cb, raw);
        id handle = [as rdna4CurrentHandle];
        if (handle) rdna4_rt_cb_retain(cb, handle);
        asHandlesOut[baseIdx + k] = vkAS;
    }

    *asWriteSlot = (VkWriteDescriptorSetAccelerationStructureKHR){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .pNext = NULL,
        .accelerationStructureCount = count,
        .pAccelerationStructures = &asHandlesOut[baseIdx],
    };
    dstWrite->pNext = asWriteSlot;
    dstWrite->descriptorCount = count;
    dstWrite->descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    *asHandleCursor = baseIdx + count;
    return YES;
}

// Descriptor-cache signature helper for rdna4DcStage: (rdna4_render_encoder_impl.inc).
__attribute__((unused)) GARNET_INTERNAL void rdna4_rt_dc_push_accel_sig(
    rdna4dc_sig *sig,
    uint32_t binding,
    uint32_t count,
    const RDNA4RTStageSlots *rtSlots)
{
    if (!sig) return;
    uint32_t firstMetalIdx = 0u;
    const BOOL validBand = rdna4rtr_binding_to_metal_index(binding, count, &firstMetalIdx) != 0;
    for (uint32_t k = 0; k < count; k++) {
        id raw = (validBand && rtSlots) ? rtSlots->as[firstMetalIdx + k] : nil;
        VkAccelerationStructureKHR vkAS = VK_NULL_HANDLE;
        VkDeviceAddress bda = 0u;
        if (raw) {
            id<GarnetAccelerationStructureRuntimeInternal> as = rdna4_rt_validate_accel(raw, nil, NULL);
            if (as) {
                vkAS = [as rdna4VkAccelerationStructure];
                bda = [as rdna4DeviceAddress];
            }
        }
        rdna4dc_sig_push(sig, (uint64_t)(uintptr_t)vkAS);
        rdna4dc_sig_push(sig, (uint64_t)bda);
    }
}

// =================================================================================================
// Argument-Buffer 64-bit BDA Helpers (u64_device_address) & GPU Retirement Retention
// =================================================================================================


@implementation RDNA4RTArgOwners
- (instancetype)init {
    if ((self = [super init])) {
        _accelStructs = [NSMutableSet set];
        _iftTables = [NSMutableSet set];
        _nestedBuffers = [NSMutableSet set];
    }
    return self;
}
@end

GARNET_INTERNAL char rdna4_rt_arg_owners_key;
static _Atomic uint32_t rdna4_rt_arg_owners_seen;

GARNET_INTERNAL BOOL rdna4_rt_any_arg_owners(void) {
    return atomic_load_explicit(&rdna4_rt_arg_owners_seen, memory_order_acquire) != 0u;
}

__attribute__((unused)) GARNET_INTERNAL void rdna4_rt_arg_note(
    RDNA4Buffer *dst, id accelStruct, RDNA4IntersectionFunctionTable *ift, RDNA4Buffer *nestedBuf)
{
    if (!dst || (!accelStruct && !ift && !nestedBuf)) return;
    atomic_store_explicit(&rdna4_rt_arg_owners_seen, 1u, memory_order_release);
    @synchronized (dst) {
        RDNA4RTArgOwners *owners = objc_getAssociatedObject(dst, &rdna4_rt_arg_owners_key);
        if (!owners) {
            owners = [RDNA4RTArgOwners new];
            objc_setAssociatedObject(dst, &rdna4_rt_arg_owners_key, owners, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        if (accelStruct) [owners.accelStructs addObject:accelStruct];
        if (ift) [owners.iftTables addObject:ift];
        if (nestedBuf && nestedBuf != dst) [owners.nestedBuffers addObject:nestedBuf];
    }
}

static void rdna4_rt_retain_argbuf_walk(RDNA4Buffer *buf, id cb, NSMutableSet *visited) {
    if (!buf || !cb || [visited containsObject:buf]) return;
    [visited addObject:buf];
    NSSet *localAS = nil, *localIFTs = nil, *localNested = nil;
    @synchronized (buf) {
        RDNA4RTArgOwners *owners = objc_getAssociatedObject(buf, &rdna4_rt_arg_owners_key);
        if (owners) {
            localAS = [owners.accelStructs copy];
            localIFTs = [owners.iftTables copy];
            localNested = [owners.nestedBuffers copy];
        }
    }
    for (id asObj in localAS) {
        rdna4_rt_cb_retain(cb, asObj);
        id<GarnetAccelerationStructureRuntimeInternal> as = rdna4_rt_validate_accel(asObj, nil, NULL);
        id h = as ? [as rdna4CurrentHandle] : nil;
        if (h) rdna4_rt_cb_retain(cb, h);
    }
    for (RDNA4IntersectionFunctionTable *ift in localIFTs)
        [ift rdna4RetainIntoCommandBuffer:cb];
    for (RDNA4Buffer *nb in localNested)
        rdna4_rt_retain_argbuf_walk(nb, cb, visited);
}

__attribute__((unused)) GARNET_INTERNAL void rdna4_rt_retain_argbuf_resources(RDNA4Buffer *buf, id cb) {
    if (!buf || !cb || !rdna4_rt_any_arg_owners()) return;
    NSMutableSet *visited = [NSMutableSet set];
    rdna4_rt_retain_argbuf_walk(buf, cb, visited);
}

__attribute__((unused)) static rdna4rtr_arg_kind_t rdna4_rt_arg_kind_from_mtl(MTLDataType dt) {
    if (dt == MTLDataTypePrimitiveAccelerationStructure) return RDNA4RTR_ARG_KIND_PRIMITIVE_AS;
    if (dt == MTLDataTypeInstanceAccelerationStructure) return RDNA4RTR_ARG_KIND_INSTANCE_AS;
    if (dt == MTLDataTypeIntersectionFunctionTable) return RDNA4RTR_ARG_KIND_IFT;
    return RDNA4RTR_ARG_KIND_NONE;
}

__attribute__((unused)) GARNET_INTERNAL BOOL rdna4_rt_arg_is_rt_data_type(MTLDataType dt) {
    return rdna4rtr_arg_kind_is_rt(rdna4_rt_arg_kind_from_mtl(dt)) ? YES : NO;
}

// Resolves & validates the 64-bit BDA (`u64_device_address`) for MTLArgumentEncoder
// `setAccelerationStructure:atIndex:`. Raises NSInvalidArgumentException on invalid inputs
// (matching RDNA4ArgumentEncoder semantics). If an AS created via `newAccelerationStructureWithSize:`
// has not yet materialized a VkAccelerationStructureKHR handle, materializes it for the field kind.
__attribute__((unused)) GARNET_INTERNAL uint64_t rdna4_rt_arg_resolve_as_address(
    id device,
    id accel,
    MTLDataType fieldType,
    NSUInteger fieldByteOffset,
    NSUInteger fieldSize,
    NSUInteger bufLength)
{
    const rdna4rtr_arg_kind_t fk = rdna4_rt_arg_kind_from_mtl(fieldType);
    if (fk != RDNA4RTR_ARG_KIND_PRIMITIVE_AS && fk != RDNA4RTR_ARG_KIND_INSTANCE_AS)
        [NSException raise:NSInvalidArgumentException
                    format:@"Argument field type %lu is not an acceleration structure", (unsigned long)fieldType];

    uint64_t addr = 0u;
    rdna4rtr_as_kind_t asKind = RDNA4RTR_AS_KIND_UNKNOWN;
    if (accel) {
        NSString *why = nil;
        id<GarnetAccelerationStructureRuntimeInternal> as = rdna4_rt_validate_accel(accel, device, &why);
        if (!as)
            [NSException raise:NSInvalidArgumentException
                        format:@"Argument acceleration structure invalid: %@", why ?: @"bad class/device"];
        asKind = (rdna4rtr_as_kind_t)[as rdna4AccelKind];
        addr = (uint64_t)[as rdna4DeviceAddress];
        if (addr == 0u && asKind == RDNA4RTR_AS_KIND_UNKNOWN) {
            rdna4rt_kind_t want = (fk == RDNA4RTR_ARG_KIND_INSTANCE_AS) ? RDNA4RT_KIND_TLAS : RDNA4RT_KIND_BLAS;
            NSError *ensureErr = nil;
            if (![as rdna4EnsureHandleForKind:want error:&ensureErr])
                [NSException raise:NSInvalidArgumentException
                            format:@"Argument acceleration structure handle creation failed: %@",
                                   ensureErr.localizedDescription ?: @"unknown"];
            addr = (uint64_t)[as rdna4DeviceAddress];
            asKind = (rdna4rtr_as_kind_t)want;
        }
    }

    rdna4rtr_arg_bda_plan_t plan;
    rdna4rtr_status_t st = rdna4rtr_plan_as_bda_encode(
        fk, (uint64_t)fieldByteOffset, (uint64_t)fieldSize, (uint64_t)bufLength,
        accel ? 0 : 1, addr, asKind, &plan);
    if (st != RDNA4RTR_OK)
        [NSException raise:NSInvalidArgumentException
                    format:@"Argument acceleration structure encoding rejected: %s", rdna4rtr_status_str(st)];
    return plan.encoded_u64;
}

// Resolves & validates the 64-bit BDA of an RDNA4IntersectionFunctionTable's backing Shared RDNA4Buffer
// for MTLArgumentEncoder `setIntersectionFunctionTable:atIndex:`.
__attribute__((unused)) GARNET_INTERNAL uint64_t rdna4_rt_arg_resolve_ift_address(
    id device,
    id ift,
    MTLDataType fieldType,
    NSUInteger fieldByteOffset,
    NSUInteger fieldSize,
    NSUInteger bufLength)
{
    const rdna4rtr_arg_kind_t fk = rdna4_rt_arg_kind_from_mtl(fieldType);
    if (fk != RDNA4RTR_ARG_KIND_IFT)
        [NSException raise:NSInvalidArgumentException
                    format:@"Argument field type %lu is not an intersection function table", (unsigned long)fieldType];

    uint64_t addr = 0u;
    if (ift) {
        if (![ift isKindOfClass:[RDNA4IntersectionFunctionTable class]] ||
            [(RDNA4IntersectionFunctionTable *)ift device] != device)
            [NSException raise:NSInvalidArgumentException
                        format:@"Argument intersection function table has an invalid class or device"];
        addr = [(RDNA4IntersectionFunctionTable *)ift rdna4BackingGPUAddress];
    }

    rdna4rtr_arg_bda_plan_t plan;
    rdna4rtr_status_t st = rdna4rtr_plan_ift_bda_encode(
        fk, (uint64_t)fieldByteOffset, (uint64_t)fieldSize, (uint64_t)bufLength,
        ift ? 0 : 1, addr, &plan);
    if (st != RDNA4RTR_OK)
        [NSException raise:NSInvalidArgumentException
                    format:@"Argument intersection function table encoding rejected: %s", rdna4rtr_status_str(st)];
    return plan.encoded_u64;
}

// Standalone direct writers (for call sites not routing through rdna4_argument_sampler_store).
__attribute__((unused)) static void rdna4_rt_arg_encode_as(
    RDNA4Buffer *argBuf,
    NSUInteger byteOffset,
    MTLDataType fieldType,
    NSUInteger fieldSize,
    id device,
    id accel)
{
    if (!argBuf || ![argBuf contents])
        [NSException raise:NSInvalidArgumentException format:@"Argument buffer destination is unmapped"];
    uint64_t addr = rdna4_rt_arg_resolve_as_address(device, accel, fieldType, byteOffset, fieldSize, [argBuf length]);
    rdna4rtr_pack_u64_le((uint8_t *)[argBuf contents] + byteOffset, addr);
    if (accel) rdna4_rt_arg_note(argBuf, accel, nil, nil);
}

__attribute__((unused)) static void rdna4_rt_arg_encode_ift(
    RDNA4Buffer *argBuf,
    NSUInteger byteOffset,
    MTLDataType fieldType,
    NSUInteger fieldSize,
    id device,
    id ift)
{
    if (!argBuf || ![argBuf contents])
        [NSException raise:NSInvalidArgumentException format:@"Argument buffer destination is unmapped"];
    uint64_t addr = rdna4_rt_arg_resolve_ift_address(device, ift, fieldType, byteOffset, fieldSize, [argBuf length]);
    rdna4rtr_pack_u64_le((uint8_t *)[argBuf contents] + byteOffset, addr);
    if (ift) rdna4_rt_arg_note(argBuf, nil, (RDNA4IntersectionFunctionTable *)ift, nil);
}

// =================================================================================================
// Pipeline Reflection, Function Handle, and IFT Factory Helpers
// =================================================================================================

// Accepts either an already-parsed NSDictionary* (from RDNA4ComputePipelineState /
// RDNA4RenderPipelineState metadata) or raw NSData* reflection JSON. `*out` must be
// zero-initialized (e.g. ObjC ivars) or previously populated by rdna4_rt_parse_meta.
__attribute__((unused)) GARNET_INTERNAL BOOL rdna4_rt_parse_meta(
    id meta, rdna4rtr_reflection_t *out, NSError **err)
{
    if (!out) return NO;
    rdna4rtr_refl_destroy(out);
    if (!meta || meta == [NSNull null]) return YES;

    NSData *raw = nil;
    if ([meta isKindOfClass:[NSData class]]) {
        raw = (NSData *)meta;
        if (!raw.length) return YES;
    } else if ([meta isKindOfClass:[NSDictionary class]]) {
        NSDictionary *dict = (NSDictionary *)meta;
        id asObj = dict[@"acceleration_structures"];
        id iftObj = dict[@"intersection_function_tables"];
        BOOL hasAS = (asObj != nil && asObj != [NSNull null]);
        BOOL hasIFT = (iftObj != nil && iftObj != [NSNull null]);
        if (!hasAS && !hasIFT) return YES;
        NSMutableDictionary *wrapper = [NSMutableDictionary dictionaryWithCapacity:2];
        if (hasAS) wrapper[@"acceleration_structures"] = asObj;
        if (hasIFT) wrapper[@"intersection_function_tables"] = iftObj;
        if (![NSJSONSerialization isValidJSONObject:wrapper]) {
            if (err) *err = rdna4_err(145, @"ray tracing reflection metadata is not JSON-serializable");
            return NO;
        }
        NSError *je = nil;
        raw = [NSJSONSerialization dataWithJSONObject:wrapper options:0 error:&je];
        if (!raw.length) {
            if (err) *err = rdna4_err(145, je.localizedDescription ?: @"failed to serialize ray tracing reflection metadata");
            return NO;
        }
    } else {
        if (err) *err = rdna4_err(145, @"ray tracing reflection metadata must be NSDictionary or NSData");
        return NO;
    }

    rdna4rtr_status_t st = rdna4rtr_parse_reflection_json((const char *)raw.bytes, raw.length, out);
    if (st != RDNA4RTR_OK) {
        rdna4rtr_refl_destroy(out);
        if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"ray tracing reflection parse failed: %s",
                                        rdna4rtr_status_str(st)]);
        return NO;
    }
    return YES;
}

__attribute__((unused)) GARNET_INTERNAL id<MTLFunctionHandle> rdna4_rt_pipeline_function_handle(
    id pso, id dev, uint64_t iftOwnerToken, const rdna4rtr_ift_reflection_t *refl,
    id<MTLFunction> function, NSUInteger stage)
{
    if (!pso || !dev || !iftOwnerToken || !refl || !refl->present || !function) return nil;
    if ([function respondsToSelector:@selector(functionType)] &&
        [function functionType] != MTLFunctionTypeIntersection)
        return nil;
    NSString *nm = [function respondsToSelector:@selector(name)] ? [function name] : nil;
    if (!nm.length) return nil;
    uint32_t hid = rdna4rtr_ift_refl_find_handle(refl, nm.UTF8String, NULL);
    if (hid == RDNA4RTR_IFT_HANDLE_NULL) return nil;
    return (id<MTLFunctionHandle>)[[RDNA4FunctionHandle alloc]
        initWithDevice:dev ownerPipeline:pso ownerToken:iftOwnerToken stage:stage
          functionType:MTLFunctionTypeIntersection name:nm handleId:hid];
}

__attribute__((unused)) GARNET_INTERNAL id<MTLIntersectionFunctionTable> rdna4_rt_pipeline_new_ift(
    id pso, id dev, uint64_t iftOwnerToken, const rdna4rtr_ift_reflection_t *refl,
    MTLIntersectionFunctionTableDescriptor *descriptor, NSUInteger stage)
{
    if (!pso || !dev || !iftOwnerToken || !descriptor) return nil;
    NSError *e = nil;
    RDNA4IntersectionFunctionTable *tbl = [[RDNA4IntersectionFunctionTable alloc]
        initWithDevice:dev descriptor:descriptor ownerToken:iftOwnerToken stage:stage refl:refl error:&e];
    if (!tbl && e)
        RDNA4LOG("newIntersectionFunctionTableWithDescriptor failed: %s", e.localizedDescription.UTF8String);
    return (id<MTLIntersectionFunctionTable>)tbl;
}

#endif // RDNA4_RT_RUNTIME_IMPL_INC
// ---------------------------------------------------------------------------------------------------------------
// Hardware Ray Tracing Acceleration Structures (Task 7.1):
//   GarnetAccelerationStructure (MTLAccelerationStructure + MTLResource)
//   GarnetAccelerationStructureCommandEncoder (MTLAccelerationStructureCommandEncoder + MTLCommandEncoder)
//   Backed by VK_KHR_acceleration_structure + VK_KHR_deferred_host_operations.
//
// Wave 3 integration checklist (GarnetDevice.m, rdna4_command_buffer_impl.inc, rdna4_buffer_impl.inc):
//   1. GarnetDevice.m include order: place after `#include "rdna4_gl_drawable_impl.m"`
//      (and after `#include "rdna4_counters_impl.m"` so pass sampleBufferAttachments work):
//        #include "rdna4_rt_accel_impl.m"
//   2. GarnetDevice.m +load protocol registration & census (also auto-registered in +initialize):
//        { const char *n[] = { "MTLAccelerationStructure", "MTLResource" };
//          rdna4_add_protocols([GarnetAccelerationStructure class], n, 2, "GarnetAccelerationStructure"); }
//        { const char *n[] = { "MTLAccelerationStructureCommandEncoder", "MTLCommandEncoder" };
//          rdna4_add_protocols([GarnetAccelerationStructureCommandEncoder class], n, 2, "GarnetAccelerationStructureCommandEncoder"); }
//   3. GarnetDevice.m @implementation RDNA4CommandBuffer (Encoders):
//        - (id)accelerationStructureCommandEncoder {
//            (void)[self vk];
//            RDNA4LOGR("cb accelerationStructureCommandEncoder");
//            return [[GarnetAccelerationStructureCommandEncoder alloc] initWithCommandBuffer:self descriptor:nil];
//        }
//        - (id)accelerationStructureCommandEncoderWithDescriptor:(id)d {
//            (void)[self vk];
//            RDNA4LOGR("cb accelerationStructureCommandEncoderWithDescriptor:");
//            return [[GarnetAccelerationStructureCommandEncoder alloc] initWithCommandBuffer:self descriptor:d];
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
//      Accept `GarnetAccelerationStructure` alongside `RDNA4IndirectCommandBuffer` and retain both
//      the acceleration structure and `[as rdna4CurrentHandle]` via `rdna4RetainEncoded:`.
// ---------------------------------------------------------------------------------------------------------------
#ifndef RDNA4_RT_ACCEL_IMPL_INC
#define RDNA4_RT_ACCEL_IMPL_INC


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

// Backing Private VkBuffer + VkDeviceMemory. Held strongly by GarnetASHandle so Vulkan
// always destroys VkAccelerationStructureKHR before its storage VkBuffer and VkDeviceMemory.

@implementation GarnetASStorage
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


@implementation GarnetASHandle
- (instancetype)initWithStorage:(GarnetASStorage *)storage kind:(rdna4rt_kind_t)kind error:(NSError **)err {
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
// GarnetAccelerationStructure (MTLAccelerationStructure + MTLResource)
// ---------------------------------------------------------------------------------------------------------------

@implementation GarnetAccelerationStructure
+ (void)initialize {
    if (self == [GarnetAccelerationStructure class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLAccelerationStructure", "MTLResource" };
            rdna4_add_protocols(self, n, 2, "GarnetAccelerationStructure");
        });
    }
}
RDNA4_DNR(GarnetAccelerationStructure)

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
    GarnetASStorage *storage = [[GarnetASStorage alloc] initWithBackingSize:backingSize error:err];
    if (!storage) return nil;
    GarnetASHandle *handle = nil;
    if (kind == RDNA4RT_KIND_BLAS || kind == RDNA4RT_KIND_TLAS) {
        handle = [[GarnetASHandle alloc] initWithStorage:storage kind:kind error:err];
        if (!handle) return nil;
    }
    pthread_mutex_init(&_mu, NULL);
    _muInit = YES;
    _dev = dev;
    _size = size;
    _storage = storage;
    _handle = handle;
    rdna4rt_meta_init(&_meta, (uint64_t)size, kind);
    RDNA4LOGR("GarnetAccelerationStructure %p: size %lu (backing %llu, alloc %llu, kind %u)",
              (__bridge void *)self, (unsigned long)size,
              (unsigned long long)backingSize, (unsigned long long)storage->_allocatedSize, (unsigned)kind);
    return self;
}

- (void)dealloc {
    _handle = nil;
    _storage = nil;
    if (_muInit) pthread_mutex_destroy(&_mu);
}

- (GarnetASHandle *)rdna4EnsureHandleForKind:(rdna4rt_kind_t)kind error:(NSError **)err {
    if (kind != RDNA4RT_KIND_BLAS && kind != RDNA4RT_KIND_TLAS) {
        if (err) *err = rdna4_err(144, @"invalid acceleration structure kind");
        return nil;
    }
    pthread_mutex_lock(&_mu);
    if (_handle && _handle->_kind == kind) {
        GarnetASHandle *existing = _handle;
        pthread_mutex_unlock(&_mu);
        return existing;
    }
    GarnetASStorage *storage = _storage;
    pthread_mutex_unlock(&_mu);
    GarnetASHandle *created = [[GarnetASHandle alloc] initWithStorage:storage kind:kind error:err];
    if (!created) return nil;
    pthread_mutex_lock(&_mu);
    if (!_handle || _handle->_kind != kind) _handle = created;
    GarnetASHandle *result = _handle;
    pthread_mutex_unlock(&_mu);
    return result;
}

- (GarnetASHandle *)rdna4CurrentHandle {
    pthread_mutex_lock(&_mu);
    GarnetASHandle *h = _handle;
    pthread_mutex_unlock(&_mu);
    return h;
}

- (VkAccelerationStructureKHR)rdna4VkAccelerationStructure {
    GarnetASHandle *h = [self rdna4CurrentHandle];
    return h ? h->_vkAS : VK_NULL_HANDLE;
}

- (VkDeviceAddress)rdna4DeviceAddress {
    GarnetASHandle *h = [self rdna4CurrentHandle];
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
        int hasTriangles = 0;

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
                hasTriangles = 1;
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
        plan->vkBuildFlags = rdna4rt_blas_build_flags_with_caps(plan->vkBuildFlags, hasTriangles, &m3);
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
                if (![obj isKindOfClass:[GarnetAccelerationStructure class]] || [(GarnetAccelerationStructure *)obj device] != dev) {
                    free(blasAddrs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"instancedAccelerationStructures[%lu] is not an GarnetAccelerationStructure on this device", (unsigned long)bi]);
                    return NO;
                }
                GarnetAccelerationStructure *blas = (GarnetAccelerationStructure *)obj;
                rdna4rt_accel_meta_t bmeta = [blas rdna4MetaSnapshot];
                GarnetASHandle *bhandle = [blas rdna4CurrentHandle];
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
// GarnetAccelerationStructureCommandEncoder (MTLAccelerationStructureCommandEncoder + MTLCommandEncoder)
// ---------------------------------------------------------------------------------------------------------------

@implementation GarnetAccelerationStructureCommandEncoder
+ (void)initialize {
    if (self == [GarnetAccelerationStructureCommandEncoder class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLAccelerationStructureCommandEncoder", "MTLCommandEncoder" };
            rdna4_add_protocols(self, n, 2, "GarnetAccelerationStructureCommandEncoder");
        });
    }
}
RDNA4_DNR(GarnetAccelerationStructureCommandEncoder)

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
    RDNA4LOGR("GarnetAccelerationStructureCommandEncoder %p", (__bridge void *)self);
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
    if (![accelerationStructure isKindOfClass:[GarnetAccelerationStructure class]] ||
        [(GarnetAccelerationStructure *)accelerationStructure device] != dev) {
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

    GarnetAccelerationStructure *dstAS = (GarnetAccelerationStructure *)accelerationStructure;
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

    GarnetASHandle *dstHandle = [dstAS rdna4EnsureHandleForKind:plan.kind error:&err];
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
    if (![sourceAccelerationStructure isKindOfClass:[GarnetAccelerationStructure class]] ||
        [(GarnetAccelerationStructure *)sourceAccelerationStructure device] != dev ||
        ![effectiveDst isKindOfClass:[GarnetAccelerationStructure class]] ||
        [(GarnetAccelerationStructure *)effectiveDst device] != dev) {
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

    GarnetAccelerationStructure *srcAS = (GarnetAccelerationStructure *)sourceAccelerationStructure;
    GarnetAccelerationStructure *dstAS = (GarnetAccelerationStructure *)effectiveDst;
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

    GarnetASHandle *srcHandle = [srcAS rdna4CurrentHandle];
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

    GarnetASHandle *dstHandle = (dstAS == srcAS) ? srcHandle : [dstAS rdna4EnsureHandleForKind:plan.kind error:&err];
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
    if (![sourceAccelerationStructure isKindOfClass:[GarnetAccelerationStructure class]] ||
        [(GarnetAccelerationStructure *)sourceAccelerationStructure device] != dev ||
        ![destinationAccelerationStructure isKindOfClass:[GarnetAccelerationStructure class]] ||
        [(GarnetAccelerationStructure *)destinationAccelerationStructure device] != dev) {
        [_cb rdna4Fail:@"copyAccelerationStructure: invalid source or destination acceleration structure"];
        return;
    }
    GarnetAccelerationStructure *srcAS = (GarnetAccelerationStructure *)sourceAccelerationStructure;
    GarnetAccelerationStructure *dstAS = (GarnetAccelerationStructure *)destinationAccelerationStructure;
    rdna4rt_accel_meta_t srcMeta = [srcAS rdna4MetaSnapshot];
    rdna4rt_accel_meta_t dstMeta = [dstAS rdna4MetaSnapshot];
    rdna4rt_err_t ce = rdna4rt_state_can_copy(&srcMeta, &dstMeta, srcAS == dstAS ? 1 : 0);
    if (ce != RDNA4RT_OK) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"copyAccelerationStructure: %s", rdna4rt_err_str(ce)]];
        return;
    }
    GarnetASHandle *srcHandle = [srcAS rdna4CurrentHandle];
    NSError *err = nil;
    GarnetASHandle *dstHandle = [dstAS rdna4EnsureHandleForKind:srcMeta.kind error:&err];
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
    if (![accelerationStructure isKindOfClass:[GarnetAccelerationStructure class]] ||
        [(GarnetAccelerationStructure *)accelerationStructure device] != dev) {
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
    GarnetAccelerationStructure *srcAS = (GarnetAccelerationStructure *)accelerationStructure;
    rdna4rt_accel_meta_t srcMeta = [srcAS rdna4MetaSnapshot];
    rdna4rt_err_t se = rdna4rt_state_can_write_compacted_size(&srcMeta);
    if (se != RDNA4RT_OK) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"writeCompactedAccelerationStructureSize: %s", rdna4rt_err_str(se)]];
        return;
    }
    GarnetASHandle *srcHandle = [srcAS rdna4CurrentHandle];
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
    if (![sourceAccelerationStructure isKindOfClass:[GarnetAccelerationStructure class]] ||
        [(GarnetAccelerationStructure *)sourceAccelerationStructure device] != dev ||
        ![destinationAccelerationStructure isKindOfClass:[GarnetAccelerationStructure class]] ||
        [(GarnetAccelerationStructure *)destinationAccelerationStructure device] != dev) {
        [_cb rdna4Fail:@"copyAndCompactAccelerationStructure: invalid source or destination acceleration structure"];
        return;
    }
    GarnetAccelerationStructure *srcAS = (GarnetAccelerationStructure *)sourceAccelerationStructure;
    GarnetAccelerationStructure *dstAS = (GarnetAccelerationStructure *)destinationAccelerationStructure;
    rdna4rt_accel_meta_t srcMeta = [srcAS rdna4MetaSnapshot];
    rdna4rt_accel_meta_t dstMeta = [dstAS rdna4MetaSnapshot];
    rdna4rt_err_t ce = rdna4rt_state_can_compact(&srcMeta, &dstMeta, srcAS == dstAS ? 1 : 0, 0u);
    if (ce != RDNA4RT_OK) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"copyAndCompactAccelerationStructure: %s", rdna4rt_err_str(ce)]];
        return;
    }
    GarnetASHandle *srcHandle = [srcAS rdna4CurrentHandle];
    NSError *err = nil;
    GarnetASHandle *dstHandle = [dstAS rdna4EnsureHandleForKind:srcMeta.kind error:&err];
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
    if ([resource isKindOfClass:[GarnetAccelerationStructure class]]) {
        if ([(GarnetAccelerationStructure *)resource device] != [_cb device] ||
            !usage || ((NSUInteger)usage & ~(NSUInteger)7)) {
            [_cb rdna4Fail:@"accel useResource: invalid acceleration structure device or usage"];
            return;
        }
        [_cb rdna4RetainEncoded:resource];
        GarnetASHandle *h = [(GarnetAccelerationStructure *)resource rdna4CurrentHandle];
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
    RDNA4LOGR("GarnetAccelerationStructureCommandEncoder endEncoding");
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

GARNET_INTERNAL __attribute__((unused)) MTLAccelerationStructureSizes rdna4_rt_device_sizes(
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

GARNET_INTERNAL __attribute__((unused)) id<MTLAccelerationStructure> rdna4_rt_device_new_with_size(
    id dev, NSUInteger size) {
    NSError *err = nil;
    GarnetAccelerationStructure *as = [[GarnetAccelerationStructure alloc]
        initWithDevice:dev size:size initialKind:RDNA4RT_KIND_UNKNOWN error:&err];
    if (!as) RDNA4LOG("newAccelerationStructureWithSize:%lu failed: %s",
                      (unsigned long)size, err.localizedDescription.UTF8String);
    return (id<MTLAccelerationStructure>)as;
}

GARNET_INTERNAL __attribute__((unused)) id<MTLAccelerationStructure> rdna4_rt_device_new_with_descriptor(
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
    GarnetAccelerationStructure *as = [[GarnetAccelerationStructure alloc]
        initWithDevice:dev size:reqSize initialKind:kind error:&err];
    if (!as) RDNA4LOG("newAccelerationStructureWithDescriptor: alloc %lu failed: %s",
                      (unsigned long)reqSize, err.localizedDescription.UTF8String);
    return (id<MTLAccelerationStructure>)as;
}

#endif /* RDNA4_RT_ACCEL_IMPL_INC */
// ---------------------------------------------------------------------------------------------------------------
// Metal 3 Mesh & Object Shader Runtime (Task 6.2):
//   RDNA4MeshRenderPipelineState (subclass of RDNA4RenderPipelineState conforming to MTLRenderPipelineState)
//   RDNA4RenderEncoder (RDNA4Mesh) (object/mesh bindings + direct/threads/indirect mesh draws)
//   Supports:
//     1. Direct hardware Mesh-only (`VK_SHADER_STAGE_MESH_BIT_EXT`, GFX ring 0 with mesa-patches/0029)
//     2. Native hardware Object+Mesh (`VK_SHADER_STAGE_TASK_BIT_EXT + VK_SHADER_STAGE_MESH_BIT_EXT`)
//     3. GFX-ring compute-emulated Object (`GLCompute` with GARNETX_CAP_MESH_EMULATED_PAYLOAD) +
//        `COMPUTE -> DRAW_INDIRECT | MESH_SHADER` barrier + `vkCmdDrawMeshTasksIndirectEXT` (0078 ABI)
//
// Wave 3E integration checklist (GarnetDevice.m, rdna4_backend_impl.inc, rdna4_render_encoder_impl.inc, rdna4_shader_runtime_impl.inc):
//   1. rdna4_backend_impl.inc (REQUIRED for multi-group / indirect emulated Object+Mesh `drawCount > 1`
//      under VUID-vkCmdDrawMeshTasksIndirectEXT-drawCount-02718):
//        Enable `ef.multiDrawIndirect = pf.multiDrawIndirect;` in `VkPhysicalDeviceFeatures`,
//        store `BOOL multiDrawIndirect;` in `RDNA4R` (`RDNA4R.multiDrawIndirect = ef.multiDrawIndirect != VK_FALSE;`),
//        and add `#define RDNA4_HAS_MULTI_DRAW_INDIRECT 1` right after `RDNA4R`.
//   2. GarnetDevice.m include order: place after `#include "rdna4_rt_accel_impl.m"` (after
//      RDNA4RenderPipelineState, RDNA4RenderEncoder, RDNA4CommandBuffer, and rdna4_shader_runtime_impl.inc):
//        #include "rdna4_mesh_impl.m"
//   3. GarnetDevice.m +load protocol registration (also auto-registered in +initialize):
//        { const char *n[] = { "MTLRenderPipelineState" };
//          rdna4_add_protocols([RDNA4MeshRenderPipelineState class], n, 1, "RDNA4MeshRenderPipelineState"); }
//   4. GarnetDevice.m @implementation GarnetDevice selectors:
//        - (id)newRenderPipelineStateWithMeshDescriptor:(id)d options:(NSUInteger)opts
//                                            reflection:(MTLRenderPipelineReflection **)refl
//                                                 error:(NSError **)error {
//            return rdna4_mesh_device_new_pipeline(self, d, opts, refl, error);
//        }
//        - (void)newRenderPipelineStateWithMeshDescriptor:(id)d options:(NSUInteger)opts
//                                       completionHandler:(void (^)(id, id, NSError *))h {
//            rdna4_mesh_device_new_pipeline_async(self, d, opts, h);
//        }
//   5. rdna4_render_encoder_impl.inc (`rdna4PrepareDraw:`):
//        Add guard right after `_pso` hot-swap:
//          if ([_pso rdna4IsMeshPipeline]) {
//              [_cb rdna4Fail:@"drawPrimitives/drawPatches called with a mesh render pipeline"];
//              return NO;
//          }
//        All object/mesh binding and draw selectors live in `RDNA4RenderEncoder (RDNA4Mesh)` below.
//   6. rdna4_shader_runtime_impl.inc / rdna4_argument_tables.inc (optional):
//        If Object/Mesh stages ever use Tier 2 GPU argument tables, OR
//        `VK_SHADER_STAGE_MESH_BIT_EXT` / `VK_SHADER_STAGE_TASK_BIT_EXT` into
//        `rdna4_argument_stage_flags()` when `RDNA4R.meshShader` / `RDNA4R.objectShader` is YES.
// ---------------------------------------------------------------------------------------------------------------
#ifndef RDNA4_MESH_IMPL_INC
#define RDNA4_MESH_IMPL_INC


// Distinct encoder resource-binding slot kinds (1..6 live in rdna4_encoder_bindings.inc;
// reserve 32..35 for Object/Mesh so Wave 3E integration cannot collide with RT/other bands).
enum {
    RDNA4_RESOURCE_OBJECT_BUFFER  = 32,
    RDNA4_RESOURCE_OBJECT_TEXTURE = 33,
    RDNA4_RESOURCE_MESH_BUFFER    = 34,
    RDNA4_RESOURCE_MESH_TEXTURE   = 35,
};

// =================================================================================================
// 1. Lazy VK_EXT_mesh_shader proc loader & garnetxlate Stage 3 FFI resolver
// =================================================================================================

typedef struct {
    BOOL ready;
    PFN_vkCmdDrawMeshTasksEXT cmdDrawMeshTasksEXT;
    PFN_vkCmdDrawMeshTasksIndirectEXT cmdDrawMeshTasksIndirectEXT;
} RDNA4MeshFnTable;

static RDNA4MeshFnTable RDNA4MeshFn;

static BOOL rdna4_mesh_resolve_vk_funcs(void) {
    if (!RDNA4R.ok || !RDNA4R.dev || !RDNA4R.meshShader) return NO;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (vkGetDeviceProcAddr) {
            RDNA4MeshFn.cmdDrawMeshTasksEXT = (PFN_vkCmdDrawMeshTasksEXT)
                vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdDrawMeshTasksEXT");
            RDNA4MeshFn.cmdDrawMeshTasksIndirectEXT = (PFN_vkCmdDrawMeshTasksIndirectEXT)
                vkGetDeviceProcAddr(RDNA4R.dev, "vkCmdDrawMeshTasksIndirectEXT");
        }
        if (!RDNA4MeshFn.cmdDrawMeshTasksEXT && rdna4_gipa && RDNA4R.inst) {
            RDNA4MeshFn.cmdDrawMeshTasksEXT = (PFN_vkCmdDrawMeshTasksEXT)
                rdna4_gipa(RDNA4R.inst, "vkCmdDrawMeshTasksEXT");
        }
        if (!RDNA4MeshFn.cmdDrawMeshTasksIndirectEXT && rdna4_gipa && RDNA4R.inst) {
            RDNA4MeshFn.cmdDrawMeshTasksIndirectEXT = (PFN_vkCmdDrawMeshTasksIndirectEXT)
                rdna4_gipa(RDNA4R.inst, "vkCmdDrawMeshTasksIndirectEXT");
        }
        RDNA4MeshFn.ready = (RDNA4MeshFn.cmdDrawMeshTasksEXT != NULL &&
                             RDNA4MeshFn.cmdDrawMeshTasksIndirectEXT != NULL);
        if (!RDNA4MeshFn.ready) {
            RDNA4LOG("mesh: VK_EXT_mesh_shader command entry points are missing");
        }
    });
    return RDNA4MeshFn.ready;
}

static uint32_t rdna4_mesh_max_draw_indirect_count(void) {
#ifdef RDNA4_HAS_MULTI_DRAW_INDIRECT
    if (RDNA4R.multiDrawIndirect && RDNA4R.lim.maxDrawIndirectCount > 1u)
        return RDNA4R.lim.maxDrawIndirectCount;
#endif
    return 1u;
}

typedef struct {
    const uint8_t *symbol;
    size_t symbol_len;
    const uint8_t *ll;
    size_t ll_len;
    uint32_t kind;
} rdna4_mesh_cift_fn;

typedef int (*rdna4_mesh_detect_stage_fn)(
    const uint8_t *ll, size_t ll_len,
    uint32_t *out_stage_kind,
    char *err, size_t err_len);

typedef int (*rdna4_mesh_translate_stage3_fn)(
    const uint8_t *ll, size_t ll_len,
    const uint32_t *local_xyz,
    const rdna4x_cdep *deps, size_t ndeps,
    const rdna4x_cdynlib *dynlibs, size_t ndynlibs,
    const rdna4x_cdep *visible_fns, size_t nvisible_fns,
    const rdna4_mesh_cift_fn *intersection_fns, size_t nintersection_fns,
    const rdna4x_backend_capabilities *capabilities,
    uint8_t **spv, size_t *spv_len,
    uint8_t **meta, size_t *meta_len,
    char *err, size_t err_len);

static struct {
    pthread_mutex_t mu;
    BOOL resolved;
    rdna4_mesh_detect_stage_fn detectStage;
    rdna4_mesh_translate_stage3_fn translateStage3;
} RDNA4MeshXL = { .mu = PTHREAD_MUTEX_INITIALIZER };

static BOOL rdna4_mesh_ensure_stage3_xlib(const rdna4x_xlib **xlibOut,
                                          rdna4_mesh_detect_stage_fn *detectOut,
                                          rdna4_mesh_translate_stage3_fn *translateOut,
                                          NSError **err) {
    const rdna4x_xlib *xl = rdna4x_ensure_xlib_raw(err);
    if (!xl || !xl->h) return NO;
    pthread_mutex_lock(&RDNA4MeshXL.mu);
    if (!RDNA4MeshXL.resolved) {
        *(void **)&RDNA4MeshXL.detectStage = dlsym(xl->h, "garnetx_detect_stage");
        *(void **)&RDNA4MeshXL.translateStage3 = dlsym(xl->h, "garnetx_translate_stage3");
        RDNA4MeshXL.resolved = YES;
    }
    rdna4_mesh_detect_stage_fn detFn = RDNA4MeshXL.detectStage;
    rdna4_mesh_translate_stage3_fn xlatFn = RDNA4MeshXL.translateStage3;
    pthread_mutex_unlock(&RDNA4MeshXL.mu);
    if ((detectOut && !detFn) || (translateOut && !xlatFn)) {
        if (err) *err = rdna4_err(42, @"libgarnetxlate.dylib lacks Metal 3 Stage 3 mesh/object entry points");
        return NO;
    }
    if (xlibOut) *xlibOut = xl;
    if (detectOut) *detectOut = detFn;
    if (translateOut) *translateOut = xlatFn;
    return YES;
}

// =================================================================================================
// 2. Availability-safe descriptor accessors & Footnote 10 classifier
// =================================================================================================

static NSUInteger rdna4_mesh_uint_prop(id obj, const char *selName, NSUInteger fallback) {
    if (!obj || !selName) return fallback;
    SEL s = sel_registerName(selName);
    if (![obj respondsToSelector:s]) return fallback;
    return ((NSUInteger (*)(id, SEL))objc_msgSend)(obj, s);
}

static uint32_t rdna4_mesh_linked_fn10_flags(id linked) {
    if (!linked) return 0u;
    uint32_t flags = 0u;
    if (rdna4x_array_nonempty(linked, "functions"))        flags |= RDNA4MESH_FN10_PUBLIC_FUNCTIONS;
    if (rdna4x_array_nonempty(linked, "binaryFunctions"))  flags |= RDNA4MESH_FN10_BINARY_FUNCTIONS;
    if (rdna4x_array_nonempty(linked, "privateFunctions")) flags |= RDNA4MESH_FN10_PRIVATE_FUNCTIONS;
    id gr = rdna4x_prop(linked, "groups");
    if ([gr isKindOfClass:[NSDictionary class]] && [(NSDictionary *)gr count] > 0)
        flags |= RDNA4MESH_FN10_GROUPS;
    return flags;
}

static uint32_t rdna4_mesh_desc_fn10_flags(id desc) {
    uint32_t flags = 0u;
    flags |= rdna4_mesh_linked_fn10_flags(rdna4x_prop(desc, "objectLinkedFunctions"));
    flags |= rdna4_mesh_linked_fn10_flags(rdna4x_prop(desc, "meshLinkedFunctions"));
    flags |= rdna4_mesh_linked_fn10_flags(rdna4x_prop(desc, "fragmentLinkedFunctions"));
    if (rdna4x_bool_prop(desc, "supportAddingObjectBinaryFunctions") ||
        rdna4x_bool_prop(desc, "supportAddingMeshBinaryFunctions") ||
        rdna4x_bool_prop(desc, "supportAddingFragmentBinaryFunctions"))
        flags |= RDNA4MESH_FN10_SUPPORT_ADD_BIN_FNS;
    return flags;
}

static BOOL rdna4_mesh_parse_meta_dict(NSDictionary *meta, rdna4mesh_refl_t *out, NSError **err) {
    memset(out, 0, sizeof *out);
    if (!meta) return YES;
    if (![NSJSONSerialization isValidJSONObject:meta]) {
        if (err) *err = rdna4_err(49, @"mesh/object reflection metadata is not JSON-serializable");
        return NO;
    }
    NSError *je = nil;
    NSData *raw = [NSJSONSerialization dataWithJSONObject:meta options:0 error:&je];
    if (!raw.length) {
        if (err) *err = rdna4_err(49, je.localizedDescription ?: @"failed to serialize mesh reflection metadata");
        return NO;
    }
    rdna4mesh_status_t st = rdna4mesh_parse_reflection_json((const char *)raw.bytes, raw.length, out);
    if (st != RDNA4MESH_OK) {
        if (err) *err = rdna4_err(49, [NSString stringWithFormat:@"mesh reflection metadata invalid: %s",
                                       rdna4mesh_status_str(st)]);
        return NO;
    }
    return YES;
}

// =================================================================================================
// 3. Stage 3 mesh/object translation + cache key helper (64 MiB worker stack)
// =================================================================================================

static NSString *rdna4_mesh_stage_cache_key(const char *role, NSString *entrySha,
                                            BOOL emulatedPayload, const uint32_t local[3],
                                            NSArray<NSDictionary *> *deps,
                                            NSArray<NSDictionary *> *dynlibs) {
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    char hdr[160];
    int n = snprintf(hdr, sizeof hdr, "rdna4-mesh-s3-v1\n%s\n%s\nemul=%u\nlocal=%u,%u,%u\n",
                     role ?: "mesh", entrySha.UTF8String ?: "",
                     emulatedPayload ? 1u : 0u, local[0], local[1], local[2]);
    if (n > 0) CC_SHA256_Update(&ctx, hdr, (CC_LONG)n);
    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    for (NSDictionary *d in deps) {
        [parts addObject:[NSString stringWithFormat:@"D:%@:%@", d[@"name"] ?: @"", d[@"sha"] ?: @""]];
    }
    for (NSDictionary *d in dynlibs) {
        [parts addObject:[NSString stringWithFormat:@"L:%@:%@", d[@"name"] ?: @"", d[@"sha"] ?: @""]];
    }
    [parts sortUsingSelector:@selector(compare:)];
    NSString *prev = nil;
    for (NSString *p in parts) {
        if (prev && [prev isEqualToString:p]) continue;
        prev = p;
        const char *u = p.UTF8String;
        if (u) CC_SHA256_Update(&ctx, u, (CC_LONG)strlen(u));
        CC_SHA256_Update(&ctx, "\n", 1);
    }
    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);
    char hex[65];
    rdna4x_hex(hex, digest, CC_SHA256_DIGEST_LENGTH);
    return [NSString stringWithUTF8String:hex];
}

typedef struct {
    const rdna4x_xlib *xlib;
    rdna4_mesh_translate_stage3_fn translateFn;
    const char *dir;
    const char *sha;
    const uint8_t *entryAir;
    size_t entryAirLen;
    uint32_t local[3];
    const rdna4x_stage2_in_t *deps;
    size_t ndeps;
    const rdna4x_stage2_in_t *dynlibs;
    size_t ndynlibs;
    rdna4x_backend_capabilities caps;
    uint8_t *outSpv;
    size_t outSpvLen;
    uint8_t *outMeta;
    size_t outMetaLen;
    int rc;
    char err[512];
} rdna4_mesh_s3_work_t;

static void *rdna4_mesh_s3_worker(void *opaque) {
    rdna4_mesh_s3_work_t *w = (rdna4_mesh_s3_work_t *)opaque;
    if (!w->entryAir || !rdna4x_air_size_ok(w->entryAirLen)) {
        snprintf(w->err, sizeof w->err, "mesh/object entry bitcode size outside caps");
        w->rc = RDNA4X_E_CAP;
        return NULL;
    }
    if (w->ndeps > RDNA4X_MAX_DEPS || w->ndynlibs > RDNA4X_MAX_DEPS) {
        snprintf(w->err, sizeof w->err, "mesh/object linkage count exceeds cap (%u)", RDNA4X_MAX_DEPS);
        w->rc = RDNA4X_E_CAP;
        return NULL;
    }

    char *entryLl = NULL; size_t entryLlLen = 0;
    char **depTexts = NULL, **libTexts = NULL;
    rdna4x_cdep *cdeps = NULL;
    rdna4x_cdynlib *clibs = NULL;
    uint8_t *spv = NULL, *meta = NULL;
    size_t spvLen = 0, metaLen = 0;
    int rc = RDNA4X_OK;
    size_t totalLl = 0;

    rc = rdna4x_module_to_llvm_text(w->entryAir, w->entryAirLen, RDNA4DL_PAYLOAD_AIR_BITCODE,
                                    &entryLl, &entryLlLen, w->err, sizeof w->err);
    if (rc != RDNA4X_OK) goto done;
    totalLl = entryLlLen;

    if (w->ndeps) {
        depTexts = (char **)calloc(w->ndeps, sizeof *depTexts);
        cdeps = (rdna4x_cdep *)calloc(w->ndeps, sizeof *cdeps);
        if (!depTexts || !cdeps) { snprintf(w->err, sizeof w->err, "out of memory"); rc = RDNA4X_E_IO; goto done; }
        for (size_t i = 0; i < w->ndeps; i++) {
            size_t tlen = 0;
            rc = rdna4x_module_to_llvm_text(w->deps[i].bytes, w->deps[i].len, w->deps[i].kind,
                                            &depTexts[i], &tlen, w->err, sizeof w->err);
            if (rc != RDNA4X_OK) goto done;
            totalLl += tlen;
            if (totalLl > RDNA4X_MAX_LL) {
                snprintf(w->err, sizeof w->err, "mesh/object linked LLVM IR exceeds cap");
                rc = RDNA4X_E_CAP;
                goto done;
            }
            cdeps[i].symbol = (const uint8_t *)w->deps[i].name;
            cdeps[i].symbol_len = strlen(w->deps[i].name);
            cdeps[i].ll = (const uint8_t *)depTexts[i];
            cdeps[i].ll_len = tlen;
        }
    }
    if (w->ndynlibs) {
        libTexts = (char **)calloc(w->ndynlibs, sizeof *libTexts);
        clibs = (rdna4x_cdynlib *)calloc(w->ndynlibs, sizeof *clibs);
        if (!libTexts || !clibs) { snprintf(w->err, sizeof w->err, "out of memory"); rc = RDNA4X_E_IO; goto done; }
        for (size_t i = 0; i < w->ndynlibs; i++) {
            size_t tlen = 0;
            rc = rdna4x_module_to_llvm_text(w->dynlibs[i].bytes, w->dynlibs[i].len, w->dynlibs[i].kind,
                                            &libTexts[i], &tlen, w->err, sizeof w->err);
            if (rc != RDNA4X_OK) goto done;
            totalLl += tlen;
            if (totalLl > RDNA4X_MAX_LL) {
                snprintf(w->err, sizeof w->err, "mesh/object dynamic library LLVM IR exceeds cap");
                rc = RDNA4X_E_CAP;
                goto done;
            }
            clibs[i].name = (const uint8_t *)w->dynlibs[i].name;
            clibs[i].name_len = strlen(w->dynlibs[i].name);
            clibs[i].ll = (const uint8_t *)libTexts[i];
            clibs[i].ll_len = tlen;
        }
    }

    {
        int tr = w->translateFn((const uint8_t *)entryLl, entryLlLen, w->local,
                                cdeps, w->ndeps, clibs, w->ndynlibs,
                                NULL, 0u, NULL, 0u,
                                &w->caps, &spv, &spvLen, &meta, &metaLen,
                                w->err, sizeof w->err);
        if (tr != 0) {
            if (!w->err[0]) snprintf(w->err, sizeof w->err, "garnetx_translate_stage3 returned %d", tr);
            rc = RDNA4X_E_REFUSED;
            goto done;
        }
    }
    if (!spv || !meta || !rdna4x_spv_size_ok(spvLen) || !rdna4x_meta_size_ok(metaLen)) {
        snprintf(w->err, sizeof w->err, "mesh/object SPIR-V (%zu B) / meta (%zu B) outside caps", spvLen, metaLen);
        rc = RDNA4X_E_CAP;
        goto done;
    }

    if (w->dir && w->dir[0] && w->sha && rdna4x_sha_ok(w->sha)) {
        char path[RDNA4X_PATH_MAX];
        if (rdna4x_file_path(path, sizeof path, w->dir, w->sha, RDNA4X_EXT_META))
            (void)rdna4x_write_atomic(path, meta, metaLen);
        if (rdna4x_file_path(path, sizeof path, w->dir, w->sha, RDNA4X_EXT_SPV))
            (void)rdna4x_write_atomic(path, spv, spvLen);
        (void)rdna4x_trim_dir(w->dir, RDNA4X_CACHE_CAP);
    }

    w->outSpv = (uint8_t *)malloc(spvLen);
    w->outMeta = (uint8_t *)malloc(metaLen);
    if (w->outSpv && w->outMeta) {
        memcpy(w->outSpv, spv, spvLen); w->outSpvLen = spvLen;
        memcpy(w->outMeta, meta, metaLen); w->outMetaLen = metaLen;
    } else {
        free(w->outSpv); w->outSpv = NULL; w->outSpvLen = 0;
        free(w->outMeta); w->outMeta = NULL; w->outMetaLen = 0;
        snprintf(w->err, sizeof w->err, "out of memory copying mesh/object outputs");
        rc = RDNA4X_E_IO;
    }

done:
    free(entryLl);
    if (depTexts) { for (size_t i = 0; i < w->ndeps; i++) free(depTexts[i]); free(depTexts); }
    if (libTexts) { for (size_t i = 0; i < w->ndynlibs; i++) free(libTexts[i]); free(libTexts); }
    free(cdeps); free(clibs);
    if (spv) w->xlib->freep(spv, spvLen);
    if (meta) w->xlib->freep(meta, metaLen);
    w->rc = rc;
    return NULL;
}

static BOOL rdna4_mesh_translate_stage(id fn, const char *role, BOOL emulatedPayload,
                                       const uint32_t local[3],
                                       NSArray<NSDictionary *> *deps,
                                       NSArray<NSDictionary *> *dynlibs,
                                       NSData **outSpv, NSDictionary **outMeta,
                                       rdna4mesh_refl_t *outRefl, NSError **err) {
    if (outSpv) *outSpv = nil;
    if (outMeta) *outMeta = nil;
    if (outRefl) memset(outRefl, 0, sizeof *outRefl);
    if (!fn) {
        if (err) *err = rdna4_err(40, [NSString stringWithFormat:@"%s function is nil", role]);
        return NO;
    }
    SEL bcSel = NSSelectorFromString(@"bitcodeData");
    NSData *bc = [fn respondsToSelector:bcSel] ? ((NSData *(*)(id, SEL))objc_msgSend)(fn, bcSel) : nil;
    NSString *fname = [fn respondsToSelector:@selector(name)] ? [fn name] : @"?";
    if (!bc.length) {
        if (err) *err = rdna4_err(40, [NSString stringWithFormat:@"%s function '%@' has no bitcodeData", role, fname]);
        return NO;
    }
    NSString *entrySha = rdna4_sha256hex(bc);
    NSString *cacheKey = rdna4_mesh_stage_cache_key(role, entrySha, emulatedPayload, local, deps, dynlibs);
    NSString *userDir = rdna4x_user_dir();

    // 1. Check disk cache (composite key first; plain entrySha only for non-emulated default-local precompiled spvcache).
    NSMutableArray<NSString *> *tryKeys = [NSMutableArray arrayWithObject:cacheKey];
    if (!emulatedPayload && !deps.count && !dynlibs.count &&
        local[0] == 64u && local[1] == 1u && local[2] == 1u) {
        [tryKeys addObject:entrySha];
    }
    for (NSString *k in tryKeys) {
        for (NSString *dir in rdna4_spv_dirs()) {
            NSString *spvPath = [dir stringByAppendingPathComponent:[k stringByAppendingString:@".spv"]];
            NSData *cachedSpv = [NSData dataWithContentsOfFile:spvPath];
            if (cachedSpv.length < 20 || (cachedSpv.length & 3)) continue;
            NSDictionary *cachedMeta = rdna4_meta_for_spv(spvPath);
            if (!cachedMeta) continue;
            rdna4mesh_refl_t parsed = {0};
            if (rdna4_mesh_parse_meta_dict(cachedMeta, &parsed, NULL)) {
                if (outSpv) *outSpv = cachedSpv;
                if (outMeta) *outMeta = cachedMeta;
                if (outRefl) *outRefl = parsed;
                return YES;
            }
        }
    }

    // 2. Translate in-process via garnetx_translate_stage3 on a 64 MiB worker stack.
    if (!rdna4x_active()) {
        if (err) *err = rdna4_err(41, [NSString stringWithFormat:@"spvcache miss for %s '%@' and in-process translation is disabled", role, fname]);
        return NO;
    }
    const rdna4x_xlib *xl = NULL;
    rdna4_mesh_translate_stage3_fn xlatFn = NULL;
    if (!rdna4_mesh_ensure_stage3_xlib(&xl, NULL, &xlatFn, err)) return NO;

    NSUInteger nd = deps.count, nl = dynlibs.count;
    rdna4x_stage2_in_t *inDeps = nd ? (rdna4x_stage2_in_t *)calloc(nd, sizeof *inDeps) : NULL;
    rdna4x_stage2_in_t *inLibs = nl ? (rdna4x_stage2_in_t *)calloc(nl, sizeof *inLibs) : NULL;
    if ((nd && !inDeps) || (nl && !inLibs)) {
        free(inDeps); free(inLibs);
        if (err) *err = rdna4_err(42, @"out of memory preparing mesh/object linkage inputs");
        return NO;
    }
    for (NSUInteger i = 0; i < nd; i++) {
        NSDictionary *d = deps[i]; NSData *dbc = d[@"bc"];
        inDeps[i] = (rdna4x_stage2_in_t){ [d[@"name"] UTF8String], (const uint8_t *)dbc.bytes, dbc.length, RDNA4DL_PAYLOAD_AIR_BITCODE };
    }
    for (NSUInteger i = 0; i < nl; i++) {
        NSDictionary *d = dynlibs[i]; NSData *mod = d[@"data"];
        inLibs[i] = (rdna4x_stage2_in_t){ [d[@"name"] UTF8String], (const uint8_t *)mod.bytes, mod.length, (uint32_t)[d[@"kind"] unsignedIntValue] };
    }

    rdna4_mesh_s3_work_t work;
    memset(&work, 0, sizeof work);
    work.xlib = xl;
    work.translateFn = xlatFn;
    work.dir = userDir.fileSystemRepresentation;
    work.sha = cacheKey.UTF8String;
    work.entryAir = (const uint8_t *)bc.bytes;
    work.entryAirLen = bc.length;
    work.local[0] = local[0]; work.local[1] = local[1]; work.local[2] = local[2];
    work.deps = inDeps; work.ndeps = nd;
    work.dynlibs = inLibs; work.ndynlibs = nl;
    work.caps = rdna4_query_caps();
    if (emulatedPayload) work.caps.flags |= GARNETX_CAP_MESH_EMULATED_PAYLOAD;

    int joinRc = rdna4x_run_on_worker_stack(rdna4_mesh_s3_worker, &work);
    free(inDeps); free(inLibs);
    if (joinRc != 0 || work.rc != RDNA4X_OK || !work.outSpv || work.outSpvLen < 20 || (work.outSpvLen & 3) ||
        !work.outMeta || !work.outMetaLen) {
        free(work.outSpv); free(work.outMeta);
        if (err) {
            *err = rdna4_err(42, [NSString stringWithFormat:@"%s '%@' translation failed: %s",
                                  role, fname, work.err[0] ? work.err : "stage 3 worker failed"]);
        }
        return NO;
    }

    NSData *spvObj = [NSData dataWithBytesNoCopy:work.outSpv length:work.outSpvLen freeWhenDone:YES];
    NSData *metaData = [NSData dataWithBytesNoCopy:work.outMeta length:work.outMetaLen freeWhenDone:YES];
    id obj = [NSJSONSerialization JSONObjectWithData:metaData options:0 error:NULL];
    if (![obj isKindOfClass:[NSDictionary class]]) {
        if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"%s '%@' produced non-object reflection JSON", role, fname]);
        return NO;
    }
    rdna4mesh_refl_t parsed = {0};
    rdna4mesh_status_t pst = rdna4mesh_parse_reflection_json((const char *)metaData.bytes, metaData.length, &parsed);
    if (pst != RDNA4MESH_OK) {
        if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"%s '%@' reflection JSON invalid: %s",
                                       role, fname, rdna4mesh_status_str(pst)]);
        return NO;
    }
    if (outSpv) *outSpv = spvObj;
    if (outMeta) *outMeta = (NSDictionary *)obj;
    if (outRefl) *outRefl = parsed;
    return YES;
}

// Build a stage descriptor set layout reusing rdna4_stage_dsl for user bindings while cleanly
// isolating set-0 emulated Object+Mesh SSBO bindings 673/674/675 (0078 ABI).
static BOOL rdna4_mesh_build_stage_dsl(const RDNA4sRefl *r, int set, VkShaderStageFlags stage,
                                       NSDictionary *meta, const rdna4mesh_refl_t *mrefl,
                                       BOOL allowEmulated, uint32_t *emulMaskOut,
                                       VkDescriptorSetLayout *dslOut, RDNA4PB **pbOut,
                                       uint32_t *npbOut, NSMutableArray *ownedSamplers,
                                       NSError **err) {
    if (emulMaskOut) *emulMaskOut = 0u;
    *dslOut = VK_NULL_HANDLE;
    *pbOut = NULL;
    *npbOut = 0u;

    uint32_t emulMask = 0u;
    RDNA4sRefl *userR = NULL;
    const RDNA4sRefl *passR = r;
    if (r && allowEmulated && set == 0) {
        userR = (RDNA4sRefl *)malloc(sizeof *userR);
        if (!userR) {
            if (err) *err = rdna4_err(49, @"out of memory copying mesh reflection for emulated DSL");
            return NO;
        }
        *userR = *r;
        const struct { uint32_t binding; uint32_t bit; } slots[3] = {
            { GARNETX_MESH_GRID_PARAMS_BINDING, RDNA4MESH_EMUL_BIT_GRID_PARAMS },
            { GARNETX_MESH_PAYLOAD_BINDING,     RDNA4MESH_EMUL_BIT_PAYLOAD },
            { GARNETX_MESH_INDIRECT_BINDING,    RDNA4MESH_EMUL_BIT_INDIRECT },
        };
        for (size_t i = 0; i < 3u; i++) {
            const RDNA4sBind *bd = &r->bind[0][slots[i].binding];
            if (!bd->used) continue;
            if (bd->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || bd->count != 1u || bd->runtime) {
                free(userR);
                if (err) *err = rdna4_err(49, [NSString stringWithFormat:@"emulated mesh binding %u must be a single StorageBuffer", slots[i].binding]);
                return NO;
            }
            if (slots[i].binding == GARNETX_MESH_PAYLOAD_BINDING && mrefl && mrefl->has_sampler_coord_flags) {
                free(userR);
                if (err) *err = rdna4_err(49, @"emulated mesh payload binding 674 conflicts with SamplerCoordinateFlags");
                return NO;
            }
            if (slots[i].binding == GARNETX_MESH_GRID_PARAMS_BINDING && mrefl && mrefl->has_argument_tables) {
                free(userR);
                if (err) *err = rdna4_err(49, @"emulated mesh grid-params binding 673 conflicts with GPU argument tables");
                return NO;
            }
            emulMask |= slots[i].bit;
            userR->bind[0][slots[i].binding].used = 0;
        }
        passR = userR;
    }

    VkDescriptorSetLayout userDsl = VK_NULL_HANDLE;
    BOOL okDsl = rdna4_stage_dsl(passR, set, stage, meta, &userDsl, pbOut, npbOut, ownedSamplers, err);
    free(userR);
    if (!okDsl)
        return NO;

    if (!emulMask) {
        *dslOut = userDsl;
        if (emulMaskOut) *emulMaskOut = 0u;
        return YES;
    }

    vkDestroyDescriptorSetLayout(RDNA4R.dev, userDsl, NULL);
    uint32_t nb = *npbOut;
    VkDescriptorSetLayoutBinding *bnd = calloc((size_t)nb + 3u, sizeof *bnd);
    if (!bnd) {
        free(*pbOut); *pbOut = NULL; *npbOut = 0u;
        if (err) *err = rdna4_err(49, @"out of memory building emulated mesh descriptor set layout");
        return NO;
    }
    uint32_t pos = 0u;
    for (uint32_t i = 0; i < nb; i++) {
        bnd[pos++] = (VkDescriptorSetLayoutBinding){ (*pbOut)[i].binding, (*pbOut)[i].type, (*pbOut)[i].count, stage, NULL };
    }
    if (emulMask & RDNA4MESH_EMUL_BIT_GRID_PARAMS)
        bnd[pos++] = (VkDescriptorSetLayoutBinding){ GARNETX_MESH_GRID_PARAMS_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, stage, NULL };
    if (emulMask & RDNA4MESH_EMUL_BIT_PAYLOAD)
        bnd[pos++] = (VkDescriptorSetLayoutBinding){ GARNETX_MESH_PAYLOAD_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, stage, NULL };
    if (emulMask & RDNA4MESH_EMUL_BIT_INDIRECT)
        bnd[pos++] = (VkDescriptorSetLayoutBinding){ GARNETX_MESH_INDIRECT_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u, stage, NULL };

    VkDescriptorSetLayoutCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = pos,
        .pBindings = bnd,
    };
    VkResult vr = vkCreateDescriptorSetLayout(RDNA4R.dev, &ci, NULL, dslOut);
    free(bnd);
    if (vr != VK_SUCCESS) {
        free(*pbOut); *pbOut = NULL; *npbOut = 0u;
        if (err) *err = rdna4_err(50, [NSString stringWithFormat:@"vkCreateDescriptorSetLayout(mesh emul) = %d", vr]);
        return NO;
    }
    if (emulMaskOut) *emulMaskOut = emulMask;
    return YES;
}

// =================================================================================================
// 4. RDNA4MeshRenderPipelineState (subclass of RDNA4RenderPipelineState)
// =================================================================================================

#ifndef RDNA4_HAS_MESH_PIPELINE_QUERY

@implementation RDNA4RenderPipelineState (RDNA4MeshQuery)
- (BOOL)rdna4IsMeshPipeline { return NO; }
@end
#endif


@implementation RDNA4MeshRenderPipelineState

+ (void)initialize {
    if (self == [RDNA4MeshRenderPipelineState class]) {
        const char *n[] = { "MTLRenderPipelineState" };
        rdna4_add_protocols(self, n, 1, "RDNA4MeshRenderPipelineState");
    }
}

- (BOOL)rdna4IsMeshPipeline { return YES; }
- (rdna4mesh_exec_mode_t)rdna4MeshExecMode { return _meshExecMode; }
- (BOOL)rdna4HasObjectStage { return _hasObject; }
- (const rdna4mesh_linked_info_t *)rdna4MeshLinkedInfo { return &_linkedInfo; }
- (const rdna4mesh_refl_t *)rdna4ObjRefl { return &_objRefl; }
- (const rdna4mesh_refl_t *)rdna4MeshRefl { return &_meshRefl; }
- (uint32_t)rdna4ObjectSet { return _objSet; }
- (VkDescriptorSetLayout)rdna4ObjectDsl { return _dslObj; }
- (VkDescriptorSetLayout)rdna4ObjectComputeDsl { return _objCompDsl; }
- (VkPipelineLayout)rdna4ObjectComputeLayout { return _objCompPl; }
- (const RDNA4PB *)rdna4PbO { return _pbO; }
- (uint32_t)rdna4NpbO { return _npbO; }
- (const RDNA4PB *)rdna4PbM { return _pbM; }
- (uint32_t)rdna4NpbM { return _npbM; }
- (uint32_t)rdna4EmulObjMask { return _emulObjMask; }
- (uint32_t)rdna4EmulMeshMask { return _emulMeshMask; }
- (BOOL)rdna4ObjMultExecWidth { return _objMultExecW; }
- (BOOL)rdna4MeshMultExecWidth { return _meshMultExecW; }
- (NSUInteger)maxTotalThreadsPerObjectThreadgroup { return (NSUInteger)_maxObjThreads; }
- (NSUInteger)maxTotalThreadsPerMeshThreadgroup { return (NSUInteger)_maxMeshThreads; }
- (NSUInteger)objectThreadExecutionWidth { return (NSUInteger)_objExecWidth; }
- (NSUInteger)meshThreadExecutionWidth { return (NSUInteger)_meshExecWidth; }
- (VkPipeline)vkPipeline {
    return [self rdna4MeshPipelineForCull:VK_CULL_MODE_NONE front:VK_FRONT_FACE_CLOCKWISE
                                       ds:[self rdna4DSKey:NULL] rflags:0
                                 objLocal:_initObjLocal meshLocal:_initMeshLocal error:NULL];
}

// Footnote 10: mesh render pipelines never expose function handles / VFT / IFT.
- (id<MTLFunctionHandle>)functionHandleWithFunction:(id<MTLFunction>)function stage:(MTLRenderStages)stage {
    (void)function; (void)stage;
    return nil;
}
- (id<MTLVisibleFunctionTable>)newVisibleFunctionTableWithDescriptor:(MTLVisibleFunctionTableDescriptor *)descriptor stage:(MTLRenderStages)stage {
    (void)descriptor; (void)stage;
    return nil;
}
- (id<MTLIntersectionFunctionTable>)newIntersectionFunctionTableWithDescriptor:(MTLIntersectionFunctionTableDescriptor *)descriptor stage:(MTLRenderStages)stage {
    (void)descriptor; (void)stage;
    return nil;
}

- (VkShaderModule)rdna4ShaderModuleForObjectLocalLocked:(const uint32_t[3])local error:(NSError **)err {
    if (local[0] == _initObjLocal[0] && local[1] == _initObjLocal[1] && local[2] == _initObjLocal[2])
        return _objMod;
    NSString *k = [NSString stringWithFormat:@"%u,%u,%u", local[0], local[1], local[2]];
    NSValue *cached = _objModCache[k];
    if (cached) return (VkShaderModule)cached.pointerValue;

    BOOL emul = (_meshExecMode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH);
    NSData *osd = nil; NSDictionary *mo = nil; rdna4mesh_refl_t r = {0};
    if (!rdna4_mesh_translate_stage(_objFn, "object", emul, local, _objDeps, _objDynlibs,
                                    &osd, &mo, &r, err))
        return VK_NULL_HANDLE;
    rdna4mesh_spv_info_t info = {0};
    if (rdna4mesh_inspect_spv((const uint32_t *)osd.bytes, osd.length / 4u, &info) != RDNA4MESH_OK ||
        info.ep_model != (emul ? RDNA4MESH_SPV_EM_GLCOMPUTE : RDNA4MESH_SPV_EM_TASK_EXT)) {
        if (err) *err = rdna4_err(51, @"specialized object SPIR-V has unexpected execution model");
        return VK_NULL_HANDLE;
    }
    NSData *useSpv = emul ? osd : rdna4_spv_set(osd, _objSet);
    VkShaderModule m = rdna4_spv_module(useSpv.bytes, useSpv.length, "object-spec", err);
    if (!m) return VK_NULL_HANDLE;
    _objModCache[k] = [NSValue valueWithPointer:(void *)m];
    return m;
}

- (VkShaderModule)rdna4ShaderModuleForMeshLocalLocked:(const uint32_t[3])local error:(NSError **)err {
    if (local[0] == _initMeshLocal[0] && local[1] == _initMeshLocal[1] && local[2] == _initMeshLocal[2])
        return _meshMod;
    NSString *k = [NSString stringWithFormat:@"%u,%u,%u", local[0], local[1], local[2]];
    NSValue *cached = _meshModCache[k];
    if (cached) return (VkShaderModule)cached.pointerValue;

    BOOL emul = (_meshExecMode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH);
    NSData *msd = nil; NSDictionary *mm = nil; rdna4mesh_refl_t r = {0};
    if (!rdna4_mesh_translate_stage(_meshFn, "mesh", emul, local, _meshDeps, _meshDynlibs,
                                    &msd, &mm, &r, err))
        return VK_NULL_HANDLE;
    rdna4mesh_spv_info_t info = {0};
    if (rdna4mesh_inspect_spv((const uint32_t *)msd.bytes, msd.length / 4u, &info) != RDNA4MESH_OK ||
        info.ep_model != RDNA4MESH_SPV_EM_MESH_EXT) {
        if (err) *err = rdna4_err(51, @"specialized mesh SPIR-V is not MeshEXT");
        return VK_NULL_HANDLE;
    }
    VkShaderModule m = rdna4_spv_module(msd.bytes, msd.length, "mesh-spec", err);
    if (!m) return VK_NULL_HANDLE;
    _meshModCache[k] = [NSValue valueWithPointer:(void *)m];
    return m;
}

- (VkPipeline)rdna4BuildObjectComputeLocked:(const uint32_t[3])objLocal error:(NSError **)err {
    VkShaderModule mod = [self rdna4ShaderModuleForObjectLocalLocked:objLocal error:err];
    if (!mod) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo cp = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = mod,
            .pName = _oep[0] ? _oep : "main",
        },
        .layout = _objCompPl,
    };
    VkPipelineCache pc = RDNA4R.pc;
#ifdef RDNA4_BINARY_ARCHIVE_IMPL_INC
    pc = rdna4_ba_vkcache();
#endif
    VkPipeline p = VK_NULL_HANDLE;
    uint64_t t0 = rdna4_now();
    VkResult vr = vkCreateComputePipelines(RDNA4R.dev, pc, 1, &cp, NULL, &p);
    rdna4t_add(&T1.cmp, rdna4_now() - t0);
    atomic_fetch_add(&rdna4_pipes_new, 1);
    if (vr != VK_SUCCESS) {
        if (err) *err = rdna4_err(47, [NSString stringWithFormat:@"vkCreateComputePipelines(object-emul) = %d", vr]);
        return VK_NULL_HANDLE;
    }
    return p;
}

- (VkPipeline)rdna4ObjectComputePipelineForLocal:(const uint32_t[3])objLocal error:(NSError **)err {
    if (_meshExecMode != RDNA4MESH_MODE_EMULATED_OBJECT_MESH || !_objCompPl) {
        if (err) *err = rdna4_err(47, @"object compute pipeline requested on non-emulated mesh pipeline");
        return VK_NULL_HANDLE;
    }
    NSString *k = [NSString stringWithFormat:@"%u,%u,%u", objLocal[0], objLocal[1], objLocal[2]];
    [_lock lock];
    NSValue *v = _objCompVariants[k];
    if (!v) {
        VkPipeline p = [self rdna4BuildObjectComputeLocked:objLocal error:err];
        if (p) {
            v = [NSValue valueWithPointer:(void *)p];
            _objCompVariants[k] = v;
        }
    }
    [_lock unlock];
    return v ? (VkPipeline)v.pointerValue : VK_NULL_HANDLE;
}

- (VkPipeline)rdna4BuildMeshGfxLocked:(VkCullModeFlags)cull front:(VkFrontFace)front
                                   ds:(uint32_t)dk rflags:(uint32_t)rflags
                             objLocal:(const uint32_t[3])objLocal
                            meshLocal:(const uint32_t[3])meshLocal
                                error:(NSError **)err {
    BOOL fillLine = (rflags & 1u) != 0;
    BOOL depthClamp = (rflags & 2u) != 0;
    BOOL sampleLoc = (rflags & 4u) != 0;
    uint32_t vpCount = ((rflags >> 3) & 0xFu) + 1u;
    if (fillLine && !RDNA4R.fillModeNonSolid) {
        if (err) *err = rdna4_err(47, @"MTLTriangleFillModeLines requires fillModeNonSolid");
        return VK_NULL_HANDLE;
    }
    if (vpCount > (RDNA4R.maxViewports ?: 1u)) {
        if (err) *err = rdna4_err(47, [NSString stringWithFormat:@"viewportCount %u exceeds RADV max %u", vpCount, RDNA4R.maxViewports]);
        return VK_NULL_HANDLE;
    }
    if (sampleLoc && (!RDNA4R.sampleLocationsExt || !(RDNA4R.sampleLocationCounts & _pc.samples))) {
        if (err) *err = rdna4_err(47, @"programmable sample positions unsupported for sample count");
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo st[3];
    uint32_t stCount = 0u;
    if (_meshExecMode == RDNA4MESH_MODE_NATIVE_TASK_MESH) {
        VkShaderModule oMod = [self rdna4ShaderModuleForObjectLocalLocked:objLocal error:err];
        if (!oMod) return VK_NULL_HANDLE;
        st[stCount++] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_TASK_BIT_EXT,
            .module = oMod,
            .pName = _oep[0] ? _oep : "main",
        };
    }
    VkShaderModule mMod = [self rdna4ShaderModuleForMeshLocalLocked:meshLocal error:err];
    if (!mMod) return VK_NULL_HANDLE;
    st[stCount++] = (VkPipelineShaderStageCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_MESH_BIT_EXT,
        .module = mMod,
        .pName = _mep[0] ? _mep : "main",
    };
    if (_hasFrag && _fs) {
        st[stCount++] = (VkPipelineShaderStageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = _fs,
            .pName = _fep[0] ? _fep : "main",
        };
    }

    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = vpCount,
        .scissorCount = vpCount,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = (depthClamp && RDNA4R.depthClamp) ? VK_TRUE : VK_FALSE,
        .polygonMode = fillLine ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
        .rasterizerDiscardEnable = _rastOff ? VK_TRUE : VK_FALSE,
        .cullMode = cull,
        .frontFace = front,
        .lineWidth = 1.0f,
        .depthBiasEnable = _pc.depthBias ? VK_TRUE : VK_FALSE,
    };
    VkSampleLocationEXT defSL[8];
    for (uint32_t s = 0; s < _pc.samples && s < 8; s++) defSL[s] = (VkSampleLocationEXT){ 0.5f, 0.5f };
    VkPipelineSampleLocationsStateCreateInfoEXT psli = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT,
        .sampleLocationsEnable = VK_TRUE,
        .sampleLocationsInfo = {
            .sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,
            .sampleLocationsPerPixel = (VkSampleCountFlagBits)_pc.samples,
            .sampleLocationGridSize = { 1, 1 },
            .sampleLocationsCount = _pc.samples,
            .pSampleLocations = defSL,
        },
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = sampleLoc ? &psli : NULL,
        .rasterizationSamples = (VkSampleCountFlagBits)_pc.samples,
        .alphaToCoverageEnable = _pc.alphaToCoverage ? VK_TRUE : VK_FALSE,
        .alphaToOneEnable = _alphaToOne ? VK_TRUE : VK_FALSE,
    };
    rdna4ds_vk_t dv; rdna4ds_decode(dk, &dv);
    VkPipelineDepthStencilStateCreateInfo dss = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = dv.depthTest ? VK_TRUE : VK_FALSE,
        .depthWriteEnable = dv.depthWrite ? VK_TRUE : VK_FALSE,
        .depthCompareOp = (VkCompareOp)dv.depthCompareOp,
        .stencilTestEnable = dv.stencilTest ? VK_TRUE : VK_FALSE,
        .front = { (VkStencilOp)dv.front.failOp, (VkStencilOp)dv.front.passOp, (VkStencilOp)dv.front.depthFailOp, (VkCompareOp)dv.front.compareOp, 0, 0, 0 },
        .back  = { (VkStencilOp)dv.back.failOp,  (VkStencilOp)dv.back.passOp,  (VkStencilOp)dv.back.depthFailOp,  (VkCompareOp)dv.back.compareOp,  0, 0, 0 },
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .flags = _fetch && RDNA4R.orderedColorAccess
            ? VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT : 0,
        .attachmentCount = _na,
        .pAttachments = _cba,
    };
    VkDynamicState dyn[9]; uint32_t ndyn = 0u;
    for (unsigned i = 0; i < _pc.ndyn && i < 8; i++) dyn[ndyn++] = (VkDynamicState)_pc.dyn[i];
    if (sampleLoc) dyn[ndyn++] = VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT;
    VkPipelineDynamicStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = ndyn,
        .pDynamicStates = dyn,
    };
    // VK_EXT_mesh_shader VUIDs: pVertexInputState, pInputAssemblyState, and pTessellationState MUST be NULL.
    VkGraphicsPipelineCreateInfo gpc = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = stCount,
        .pStages = st,
        .pVertexInputState = NULL,
        .pInputAssemblyState = NULL,
        .pTessellationState = NULL,
        .pViewportState = &vps,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = _pc.hasDS ? &dss : NULL,
        .pColorBlendState = &cb,
        .pDynamicState = &ds,
        .layout = _pl,
        .renderPass = _rp,
    };
    VkPipelineCache pc = RDNA4R.pc;
#ifdef RDNA4_BINARY_ARCHIVE_IMPL_INC
    pc = rdna4_ba_vkcache();
#endif
    VkPipeline p = VK_NULL_HANDLE;
    uint64_t t0 = rdna4_now();
    VkResult vr = vkCreateGraphicsPipelines(RDNA4R.dev, pc, 1, &gpc, NULL, &p);
    rdna4t_add(&T1.gfx, rdna4_now() - t0);
    atomic_fetch_add(&rdna4_pipes_new, 1);
    if (vr != VK_SUCCESS) {
        if (err) *err = rdna4_err(47, [NSString stringWithFormat:@"vkCreateGraphicsPipelines(mesh) = %d", vr]);
        return VK_NULL_HANDLE;
    }
    return p;
}

- (VkPipeline)rdna4MeshPipelineForCull:(VkCullModeFlags)cull front:(VkFrontFace)front
                                    ds:(uint32_t)dk rflags:(uint32_t)rflags
                              objLocal:(const uint32_t[3])objLocal
                             meshLocal:(const uint32_t[3])meshLocal
                                 error:(NSError **)err {
    uint32_t ol[3] = { 0u, 0u, 0u };
    if (_meshExecMode == RDNA4MESH_MODE_NATIVE_TASK_MESH && objLocal) {
        ol[0] = objLocal[0]; ol[1] = objLocal[1]; ol[2] = objLocal[2];
    }
    NSString *k = [NSString stringWithFormat:@"%u:%u:%u:%u:%u,%u,%u:%u,%u,%u",
                   (unsigned)cull, (unsigned)front, dk, rflags,
                   ol[0], ol[1], ol[2], meshLocal[0], meshLocal[1], meshLocal[2]];
    [_lock lock];
    NSValue *v = _meshVariants[k];
    if (!v) {
        VkPipeline p = [self rdna4BuildMeshGfxLocked:cull front:front ds:dk rflags:rflags
                                            objLocal:ol meshLocal:meshLocal error:err];
        if (p) {
            v = [NSValue valueWithPointer:(void *)p];
            _meshVariants[k] = v;
        }
    }
    [_lock unlock];
    return v ? (VkPipeline)v.pointerValue : VK_NULL_HANDLE;
}

- (instancetype)initWithDevice:(id)dev meshDescriptor:(id)d options:(NSUInteger)options error:(NSError **)err {
    (void)options;
    self = [super init];
    if (!self) return nil;
    _dev = dev;
    _lock = [NSLock new];
    _variants = [NSMutableDictionary dictionary];
    _icbVariants = [NSMutableDictionary dictionary];
    _owned = [NSMutableArray array];
    _meshVariants = [NSMutableDictionary dictionary];
    _objCompVariants = [NSMutableDictionary dictionary];
    _objModCache = [NSMutableDictionary dictionary];
    _meshModCache = [NSMutableDictionary dictionary];
    if (!d) {
        if (err) *err = rdna4_err(40, @"mesh pipeline descriptor is nil");
        return nil;
    }

    NSString *lbl = rdna4x_prop(d, "label");
    if ([lbl isKindOfClass:[NSString class]]) _lbl = [lbl copy];
    id objFn = rdna4x_prop(d, "objectFunction");
    id meshFn = rdna4x_prop(d, "meshFunction");
    id fragFn = rdna4x_prop(d, "fragmentFunction");
    _hasObject = (objFn != nil);
    _hasFrag = (fragFn != nil);
    _rastOff = [d respondsToSelector:sel_registerName("isRasterizationEnabled")]
        ? !rdna4x_bool_prop(d, "isRasterizationEnabled") : NO;
    _alphaToOne = rdna4x_bool_prop(d, "isAlphaToOneEnabled");
    _icb = rdna4x_bool_prop(d, "supportIndirectCommandBuffers");
    _objFn = objFn;
    _meshFn = meshFn;

    if (!rdna4_radv_open(err)) return nil;
    if (!rdna4_mesh_resolve_vk_funcs()) {
        if (err) *err = rdna4_err(49, @"backend does not expose VK_EXT_mesh_shader command entry points");
        return nil;
    }

    const rdna4dc_caps caps = rdna4_device_caps();
    rdna4mesh_desc_t mdesc;
    memset(&mdesc, 0, sizeof mdesc);
    mdesc.has_object_function = _hasObject ? 1 : 0;
    mdesc.has_mesh_function = meshFn ? 1 : 0;
    mdesc.has_fragment_function = _hasFrag ? 1 : 0;
    mdesc.payload_memory_length = (uint64_t)rdna4_mesh_uint_prop(d, "payloadMemoryLength", 0u);
    mdesc.max_total_threadgroups_per_mesh_grid = (uint64_t)rdna4_mesh_uint_prop(d, "maxTotalThreadgroupsPerMeshGrid", 0u);
    mdesc.max_total_threads_per_object_threadgroup = (uint64_t)rdna4_mesh_uint_prop(d, "maxTotalThreadsPerObjectThreadgroup", 0u);
    mdesc.max_total_threads_per_mesh_threadgroup = (uint64_t)rdna4_mesh_uint_prop(d, "maxTotalThreadsPerMeshThreadgroup", 0u);
    mdesc.object_tg_size_mult_exec_width = rdna4x_bool_prop(d, "objectThreadgroupSizeIsMultipleOfThreadExecutionWidth") ? 1 : 0;
    mdesc.mesh_tg_size_mult_exec_width = rdna4x_bool_prop(d, "meshThreadgroupSizeIsMultipleOfThreadExecutionWidth") ? 1 : 0;
    mdesc.max_vertex_amplification_count = (uint64_t)rdna4_mesh_uint_prop(d, "maxVertexAmplificationCount", 1u);
    mdesc.footnote10_flags = rdna4_mesh_desc_fn10_flags(d);

    const char *why = NULL;
    rdna4mesh_exec_mode_t execMode = RDNA4MESH_MODE_UNSUPPORTED;
    rdna4mesh_status_t dst = rdna4mesh_validate_desc(&mdesc, &caps, 1u, &execMode, &why);
    if (dst != RDNA4MESH_OK) {
        if (err) *err = rdna4_err(49, [NSString stringWithUTF8String:why ? why : rdna4mesh_status_str(dst)]);
        return nil;
    }
    _meshExecMode = execMode;
    _maxAmp = (uint32_t)(mdesc.max_vertex_amplification_count ? mdesc.max_vertex_amplification_count : 1u);
    _objExecWidth = rdna4dc_thread_execution_width(&caps);
    _meshExecWidth = rdna4dc_thread_execution_width(&caps);
    _objMultExecW = mdesc.object_tg_size_mult_exec_width ? YES : NO;
    _meshMultExecW = mdesc.mesh_tg_size_mult_exec_width ? YES : NO;

    uint32_t hwObjInv = rdna4mesh_effective_obj_max_invocations(&caps, execMode);
    uint32_t hwMeshInv = rdna4mesh_effective_mesh_max_invocations(&caps);
    _maxObjThreads = _hasObject
        ? (mdesc.max_total_threads_per_object_threadgroup ? (uint32_t)mdesc.max_total_threads_per_object_threadgroup : hwObjInv)
        : 0u;
    _maxMeshThreads = mdesc.max_total_threads_per_mesh_threadgroup
        ? (uint32_t)mdesc.max_total_threads_per_mesh_threadgroup : hwMeshInv;

    // Collect allowed MTLLinkedFunctions.privateFunctions + preloadedLibraries for each stage.
    NSArray<NSDictionary *> *objDeps = @[], *objLibs = @[], *objVis = @[];
    NSArray<NSDictionary *> *meshDeps = @[], *meshLibs = @[], *meshVis = @[];
    NSArray<NSDictionary *> *fragDeps = @[], *fragLibs = @[], *fragVis = @[];
    NSString *linkWhy = nil;
    if (_hasObject && !rdna4x_stage2_link_collect(rdna4x_prop(d, "objectLinkedFunctions"),
                                                  rdna4x_prop(d, "objectPreloadedLibraries"),
                                                  &objDeps, &objVis, &objLibs, &linkWhy)) {
        if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"object linkage invalid: %@", linkWhy ?: @"unknown"]);
        return nil;
    }
    if (!rdna4x_stage2_link_collect(rdna4x_prop(d, "meshLinkedFunctions"),
                                    rdna4x_prop(d, "meshPreloadedLibraries"),
                                    &meshDeps, &meshVis, &meshLibs, &linkWhy)) {
        if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"mesh linkage invalid: %@", linkWhy ?: @"unknown"]);
        return nil;
    }
    if (_hasFrag && !rdna4x_stage2_link_collect(rdna4x_prop(d, "fragmentLinkedFunctions"),
                                                rdna4x_prop(d, "fragmentPreloadedLibraries"),
                                                &fragDeps, &fragVis, &fragLibs, &linkWhy)) {
        if (err) *err = rdna4_err(42, [NSString stringWithFormat:@"fragment linkage invalid: %@", linkWhy ?: @"unknown"]);
        return nil;
    }
    (void)objVis; (void)meshVis; (void)fragVis;
    _objDeps = objDeps; _objDynlibs = objLibs;
    _meshDeps = meshDeps; _meshDynlibs = meshLibs;

    // Pick legal initial LocalSize for compilation; retry with (1,1,1) if shader declares a tighter air.max_work_group_size.
    rdna4mesh_initial_local_size(_maxObjThreads, hwObjInv, caps.workgroup_size[0], _objExecWidth, _objMultExecW ? 1 : 0, _initObjLocal);
    rdna4mesh_initial_local_size(_maxMeshThreads, hwMeshInv, caps.workgroup_size[0], _meshExecWidth, _meshMultExecW ? 1 : 0, _initMeshLocal);

    const BOOL emul = (execMode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH);
    NSData *osd = nil, *msd = nil, *fsd = nil;
    NSDictionary *mo_ = nil, *mm_ = nil, *mf_ = nil, *preF = nil;
    NSError *te = nil;

    if (_hasObject) {
        if (!rdna4_mesh_translate_stage(objFn, "object", emul, _initObjLocal, objDeps, objLibs,
                                        &osd, &mo_, &_objRefl, &te)) {
            if (te && [te.localizedDescription containsString:@"air.max_work_group_size"] &&
                (_initObjLocal[0] != 1u || _initObjLocal[1] != 1u || _initObjLocal[2] != 1u)) {
                _initObjLocal[0] = _initObjLocal[1] = _initObjLocal[2] = 1u;
                te = nil;
                if (!rdna4_mesh_translate_stage(objFn, "object", emul, _initObjLocal, objDeps, objLibs,
                                                &osd, &mo_, &_objRefl, &te)) {
                    if (err) *err = te;
                    return nil;
                }
            } else {
                if (err) *err = te;
                return nil;
            }
        }
    }
    if (!rdna4_mesh_translate_stage(meshFn, "mesh", emul, _initMeshLocal, meshDeps, meshLibs,
                                    &msd, &mm_, &_meshRefl, &te)) {
        if (te && [te.localizedDescription containsString:@"air.max_work_group_size"] &&
            (_initMeshLocal[0] != 1u || _initMeshLocal[1] != 1u || _initMeshLocal[2] != 1u)) {
            _initMeshLocal[0] = _initMeshLocal[1] = _initMeshLocal[2] = 1u;
            te = nil;
            if (!rdna4_mesh_translate_stage(meshFn, "mesh", emul, _initMeshLocal, meshDeps, meshLibs,
                                            &msd, &mm_, &_meshRefl, &te)) {
                if (err) *err = te;
                return nil;
            }
        } else {
            if (err) *err = te;
            return nil;
        }
    }
    if (_hasFrag) {
        const uint32_t fragLocal[3] = { 1u, 1u, 1u };
        if (!rdna4_mesh_translate_stage(fragFn, "fragment", NO, fragLocal, fragDeps, fragLibs,
                                        &fsd, &mf_, &_fragRefl, &te)) {
            if (err) *err = te;
            return nil;
        }
        preF = mf_;
        fsd = rdna4_gpu_argument_spv(fsd, &mf_, rdna4_now() + 3000000000ull, err);
        if (!fsd) return nil;
        if (mf_ != preF && !rdna4_mesh_parse_meta_dict(mf_, &_fragRefl, err)) return nil;
    }

    // Inspect SPIR-V execution models, mesh modes, and Footnote 10 ray-tracing capabilities.
    rdna4mesh_spv_info_t objSpvInfo = {0}, meshSpvInfo = {0}, fragSpvInfo = {0};
    if (_hasObject && rdna4mesh_inspect_spv((const uint32_t *)osd.bytes, osd.length / 4u, &objSpvInfo) != RDNA4MESH_OK) {
        if (err) *err = rdna4_err(51, @"object SPIR-V header/instruction inspection failed");
        return nil;
    }
    if (rdna4mesh_inspect_spv((const uint32_t *)msd.bytes, msd.length / 4u, &meshSpvInfo) != RDNA4MESH_OK) {
        if (err) *err = rdna4_err(51, @"mesh SPIR-V header/instruction inspection failed");
        return nil;
    }
    if (_hasFrag && rdna4mesh_inspect_spv((const uint32_t *)fsd.bytes, fsd.length / 4u, &fragSpvInfo) != RDNA4MESH_OK) {
        if (err) *err = rdna4_err(52, @"fragment SPIR-V header/instruction inspection failed");
        return nil;
    }

    rdna4mesh_status_t lst = rdna4mesh_validate_stage_linkage(
        execMode, &caps, &mdesc,
        _hasObject ? &_objRefl : NULL, _hasObject ? &objSpvInfo : NULL,
        &_meshRefl, &meshSpvInfo,
        _hasFrag ? &_fragRefl : NULL, _hasFrag ? &fragSpvInfo : NULL,
        &_linkedInfo, &why);
    if (lst != RDNA4MESH_OK) {
        if (err) *err = rdna4_err(49, [NSString stringWithUTF8String:why ? why : rdna4mesh_status_str(lst)]);
        return nil;
    }

    // Reflect SPIR-V descriptors for Object, Mesh, and Fragment stages.
    NSData *fsp = _hasFrag ? rdna4_spv_set(fsd, 1u) : nil;
    if (_hasFrag) {
        uint64_t perPrimMask = (_meshRefl.has_mesh ? _meshRefl.mesh.primitive_out_mask : 0u) |
                               meshSpvInfo.per_prim_out_mask;
        if (perPrimMask != 0u) {
            uint32_t *patched = NULL;
            size_t patchedWords = 0u;
            rdna4mesh_status_t pst = rdna4_mesh_patch_frag_per_primitive(
                (const uint32_t *)fsp.bytes, fsp.length / 4u, perPrimMask,
                &patched, &patchedWords);
            if (pst != RDNA4MESH_OK) {
                if (err) *err = rdna4_err(52, [NSString stringWithFormat:@"fragment SPIR-V PerPrimitiveEXT pairing patch failed: %s",
                                               rdna4mesh_status_str(pst)]);
                return nil;
            }
            if (patched) {
                fsp = [NSData dataWithBytesNoCopy:patched length:patchedWords * 4u freeWhenDone:YES];
            }
        }
    }
    RDNA4sRefl *ro = _hasObject ? calloc(1, sizeof *ro) : NULL;
    RDNA4sRefl *rm = calloc(1, sizeof *rm);
    RDNA4sRefl *rf = _hasFrag ? calloc(1, sizeof *rf) : NULL;
    RDNA4sMod moMod = {0}, mmMod = {0}, mfMod = {0};
    char swhy[256] = "";
    BOOL bad = (!rm || (_hasObject && !ro) || (_hasFrag && !rf));
    if (bad && err) *err = rdna4_err(49, @"out of memory allocating mesh SPIR-V reflection state");

    if (!bad && _hasObject) {
        if (rdna4s_parse((const uint32_t *)osd.bytes, osd.length / 4u, &moMod, swhy, sizeof swhy) ||
            rdna4s_reflect(&moMod, 2, ro)) {
            if (err) *err = rdna4_err(51, [NSString stringWithFormat:@"object SPIR-V reflection: %s", swhy[0] ? swhy : ro->why]);
            bad = YES;
        } else {
            strlcpy(_oep, moMod.ep_name[0] ? moMod.ep_name : "main", sizeof _oep);
        }
    }
    if (!bad) {
        if (rdna4s_parse((const uint32_t *)msd.bytes, msd.length / 4u, &mmMod, swhy, sizeof swhy) ||
            rdna4s_reflect(&mmMod, 2, rm)) {
            if (err) *err = rdna4_err(51, [NSString stringWithFormat:@"mesh SPIR-V reflection: %s", swhy[0] ? swhy : rm->why]);
            bad = YES;
        } else {
            strlcpy(_mep, mmMod.ep_name[0] ? mmMod.ep_name : "main", sizeof _mep);
        }
    }
    if (!bad && _hasFrag) {
        if (rdna4s_parse((const uint32_t *)fsp.bytes, fsp.length / 4u, &mfMod, swhy, sizeof swhy) ||
            !mfMod.has_ep || mfMod.ep_model != 4 || rdna4s_reflect(&mfMod, 1, rf)) {
            if (err) *err = rdna4_err(52, [NSString stringWithFormat:@"fragment SPIR-V reflection: %s", swhy[0] ? swhy : rf->why]);
            bad = YES;
        } else {
            strlcpy(_fep, mfMod.ep_name[0] ? mfMod.ep_name : "main", sizeof _fep);
            size_t count = fsp.length / 4u;
            uint32_t *normalized = NULL;
            if (rdna4s_widen_fragment_output((const uint32_t *)fsp.bytes, &count, &mfMod, &normalized)) {
                if (err) *err = rdna4_err(52, @"fragment SPIR-V: insufficient memory to widen color outputs");
                bad = YES;
            } else if (normalized) {
                fsp = [NSData dataWithBytesNoCopy:normalized length:count * 4u freeWhenDone:YES];
            }
        }
    }
    rdna4s_free(&moMod); rdna4s_free(&mmMod); rdna4s_free(&mfMod);
    if (bad) { free(ro); free(rm); free(rf); return nil; }

    // Set up colour & depth/stencil attachments + render pass (mirrors RDNA4RenderPipelineState).
    MTLPixelFormat dpf = (MTLPixelFormat)rdna4_mesh_uint_prop(d, "depthAttachmentPixelFormat", (NSUInteger)MTLPixelFormatInvalid);
    MTLPixelFormat spf = (MTLPixelFormat)rdna4_mesh_uint_prop(d, "stencilAttachmentPixelFormat", (NSUInteger)MTLPixelFormatInvalid);
    NSUInteger sampleCount = rdna4_mesh_uint_prop(d, "rasterSampleCount", 1u);
    BOOL a2c = rdna4x_bool_prop(d, "isAlphaToCoverageEnabled");
    const char *pcwhy = NULL;
    if (!rdna4pc_config(dpf != MTLPixelFormatInvalid, spf != MTLPixelFormatInvalid,
                        (unsigned long)sampleCount, a2c ? 1 : 0, rdna4_ms_mask(), &_pc, &pcwhy)) {
        if (err) *err = rdna4_err(44, [NSString stringWithFormat:@"rasterSampleCount %lu: %s", (unsigned long)sampleCount, pcwhy ?: "unsupported"]);
        free(ro); free(rm); free(rf); return nil;
    }

    VkAttachmentDescription ads[9];
    VkAttachmentReference refs[8];
    uint32_t na = 0u, dualSrcUsed = 0u;
    id colorAtts = rdna4x_prop(d, "colorAttachments");
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *ca = colorAtts ? colorAtts[i] : nil;
        if (!ca || ca.pixelFormat == MTLPixelFormatInvalid) continue;
        const RDNA4Fmt *f = rdna4_fmt(ca.pixelFormat);
        if (!f || (f->flags & (RDNA4F_A8 | RDNA4F_BC))) {
            if (err) *err = rdna4_err(43, [NSString stringWithFormat:@"unsupported colour attachment format %lu", (unsigned long)ca.pixelFormat]);
            free(ro); free(rm); free(rf); return nil;
        }
        VkFormatFeatureFlags ff = rdna4_fmt_feats(f);
        VkFormatFeatureFlags needF = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            (ca.isBlendingEnabled ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT : 0);
        if ((ff & needF) != needF) {
            if (err) *err = rdna4_err(43, [NSString stringWithFormat:@"unsupported colour attachment format %lu (RADV lacks attachment/blend support)", (unsigned long)ca.pixelFormat]);
            free(ro); free(rm); free(rf); return nil;
        }
        ads[na] = (VkAttachmentDescription){
            .format = f->vk, .samples = (VkSampleCountFlagBits)_pc.samples,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        refs[na] = (VkAttachmentReference){ na, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkColorComponentFlags wm = 0;
        MTLColorWriteMask m = ca.writeMask;
        if (m & MTLColorWriteMaskRed)   wm |= VK_COLOR_COMPONENT_R_BIT;
        if (m & MTLColorWriteMaskGreen) wm |= VK_COLOR_COMPONENT_G_BIT;
        if (m & MTLColorWriteMaskBlue)  wm |= VK_COLOR_COMPONENT_B_BIT;
        if (m & MTLColorWriteMaskAlpha) wm |= VK_COLOR_COMPONENT_A_BIT;
        if (!_hasFrag) wm = 0;
        else if (rf && !(rf->outmask & (1u << na))) wm = 0;
        _cba[na] = (VkPipelineColorBlendAttachmentState){ .colorWriteMask = wm };
        if (ca.isBlendingEnabled) {
            BOOL blendBad = NO;
            BOOL (^isDual)(MTLBlendFactor) = ^(MTLBlendFactor bf) {
                return (BOOL)(bf == MTLBlendFactorSource1Color || bf == MTLBlendFactorOneMinusSource1Color ||
                              bf == MTLBlendFactorSource1Alpha || bf == MTLBlendFactorOneMinusSource1Alpha);
            };
            if (isDual(ca.sourceRGBBlendFactor) || isDual(ca.destinationRGBBlendFactor) ||
                isDual(ca.sourceAlphaBlendFactor) || isDual(ca.destinationAlphaBlendFactor)) {
                dualSrcUsed++;
                if (!RDNA4R.dualSrcBlend || na >= RDNA4R.lim.maxFragmentDualSrcAttachments ||
                    dualSrcUsed > RDNA4R.lim.maxFragmentDualSrcAttachments)
                    blendBad = YES;
            }
            _cba[na].blendEnable = VK_TRUE;
            _cba[na].srcColorBlendFactor = rdna4_bf(ca.sourceRGBBlendFactor, &blendBad);
            _cba[na].dstColorBlendFactor = rdna4_bf(ca.destinationRGBBlendFactor, &blendBad);
            _cba[na].srcAlphaBlendFactor = rdna4_bf(ca.sourceAlphaBlendFactor, &blendBad);
            _cba[na].dstAlphaBlendFactor = rdna4_bf(ca.destinationAlphaBlendFactor, &blendBad);
            _cba[na].colorBlendOp = rdna4_bo(ca.rgbBlendOperation, &blendBad);
            _cba[na].alphaBlendOp = rdna4_bo(ca.alphaBlendOperation, &blendBad);
            if (blendBad) {
                if (err) *err = rdna4_err(56, @"blend factor/operation or dual-source attachment limit not supported");
                free(ro); free(rm); free(rf); return nil;
            }
        }
        na++;
    }
    _na = na;
    _argumentTables.fragmentColorAttachments = na;
    rdna4_argument_descriptor_usage descriptorUsage;
    if (!rdna4_argument_descriptor_scan(NULL, rf, na, &descriptorUsage) ||
        !rdna4_argument_descriptor_validate(&descriptorUsage, NO, err)) {
        free(ro); free(rm); free(rf); return nil;
    }

    uint32_t nd = 0u;
    VkAttachmentReference dref = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    _dpf = dpf; _spf = spf; _dsVk = VK_FORMAT_UNDEFINED; _dsAsp = 0u;
    if (_dpf != MTLPixelFormatInvalid || _spf != MTLPixelFormatInvalid) {
        MTLPixelFormat dsp = _dpf != MTLPixelFormatInvalid ? _dpf : _spf;
        rdna4dp_fmt_t df; const char *dwhy = NULL;
        if (_dpf != MTLPixelFormatInvalid && _spf != MTLPixelFormatInvalid && _dpf != _spf)
            dwhy = "separate depth and stencil attachment formats are not supported";
        else if (!rdna4dp_fmt((unsigned long)dsp, &df)) dwhy = df.why;
        else if (_dpf != MTLPixelFormatInvalid && !(df.aspects & RDNA4DP_ASP_DEPTH)) dwhy = "depthAttachmentPixelFormat is stencil-only";
        else if (_spf != MTLPixelFormatInvalid && !(df.aspects & RDNA4DP_ASP_STENCIL)) dwhy = "stencilAttachmentPixelFormat lacks stencil";
        const RDNA4Fmt *dsf = dwhy ? NULL : rdna4_fmt_ds(dsp);
        if (!dwhy && !dsf) dwhy = "no Vulkan depth/stencil mapping";
        if (!dwhy && !(rdna4_fmt_feats(dsf) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
            dwhy = "backend lacks depth/stencil attachment support";
        if (dwhy) {
            if (err) *err = rdna4_err(44, [NSString stringWithFormat:@"unsupported depth/stencil attachment: %s", dwhy]);
            free(ro); free(rm); free(rf); return nil;
        }
        _dsVk = dsf->vk; _dsAsp = df.aspects; nd = 1u; dref.attachment = na;
        ads[na] = (VkAttachmentDescription){
            .format = dsf->vk, .samples = (VkSampleCountFlagBits)_pc.samples,
            .loadOp = (df.aspects & RDNA4DP_ASP_DEPTH) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = (df.aspects & RDNA4DP_ASP_DEPTH) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = (df.aspects & RDNA4DP_ASP_STENCIL) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = (df.aspects & RDNA4DP_ASP_STENCIL) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
    }

    for (int b = 192; rf && b < 200; b++) {
        if (rf->bind[1][b].used && rf->bind[1][b].type == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
            _fetch = YES; _fetchMax = (uint32_t)(b - 192);
        }
    }
    if (_fetch && (_fetchMax >= na || _pc.samples > 1)) {
        if (err) *err = rdna4_err(59, @"mesh pipeline framebuffer fetch attachment index out of range or multisampled");
        free(ro); free(rm); free(rf); return nil;
    }
    if (_fetch) {
        for (uint32_t i = 0; i < na; i++) ads[i].initialLayout = ads[i].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        _rp = rdna4_mk_rp(ads, na, nd, YES);
        if (!_rp) { if (err) *err = rdna4_err(45, @"vkCreateRenderPass(mesh fetch)"); free(ro); free(rm); free(rf); return nil; }
    } else {
        VkSubpassDescription sp = {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = na, .pColorAttachments = na ? refs : NULL,
            .pDepthStencilAttachment = nd ? &dref : NULL,
        };
        VkRenderPassCreateInfo rpc = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = na + nd, .pAttachments = (na + nd) ? ads : NULL,
            .subpassCount = 1, .pSubpasses = &sp,
        };
        VkResult rpr = vkCreateRenderPass(RDNA4R.dev, &rpc, NULL, &_rp);
        if (rpr != VK_SUCCESS) {
            if (err) *err = rdna4_err(45, [NSString stringWithFormat:@"vkCreateRenderPass(mesh) = %d", rpr]);
            free(ro); free(rm); free(rf); return nil;
        }
    }

    // Build stage descriptor set layouts + pipeline layout(s).
    NSError *le = nil;
    if (!rdna4_mesh_build_stage_dsl(rm, 0, VK_SHADER_STAGE_MESH_BIT_EXT, mm_, &_meshRefl, emul,
                                    &_emulMeshMask, &_dsl[0], &_pbM, &_npbM, _owned, &le) ||
        !rdna4_stage_dsl(rf, 1, VK_SHADER_STAGE_FRAGMENT_BIT, mf_, &_dsl[1], &_pbF, &_npbF, _owned, &le)) {
        if (err) *err = le; free(ro); free(rm); free(rf); return nil;
    }
    if (_hasObject) {
        VkShaderStageFlags objStage = emul ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_TASK_BIT_EXT;
        VkDescriptorSetLayout *objDslPtr = emul ? &_objCompDsl : &_dslObj;
        if (!rdna4_mesh_build_stage_dsl(ro, 0, objStage, mo_, &_objRefl, emul,
                                        &_emulObjMask, objDslPtr, &_pbO, &_npbO, _owned, &le)) {
            if (err) *err = le; free(ro); free(rm); free(rf); return nil;
        }
        if (emul && !(_emulObjMask & RDNA4MESH_EMUL_BIT_INDIRECT)) {
            if (err) *err = rdna4_err(49, @"emulated object shader is missing set-0 binding 675 (indirect command SSBO)");
            free(ro); free(rm); free(rf); return nil;
        }
        if (emul && _objRefl.has_object && _objRefl.obj.has_payload && !(_emulObjMask & RDNA4MESH_EMUL_BIT_PAYLOAD)) {
            if (err) *err = rdna4_err(49, @"emulated object shader with payload is missing set-0 binding 674 (payload SSBO)");
            free(ro); free(rm); free(rf); return nil;
        }
        if (emul && _meshRefl.has_mesh && _meshRefl.mesh.has_payload && !(_emulMeshMask & RDNA4MESH_EMUL_BIT_PAYLOAD)) {
            if (err) *err = rdna4_err(49, @"emulated mesh shader with payload is missing set-0 binding 674 (payload SSBO)");
            free(ro); free(rm); free(rf); return nil;
        }
    }

    if (_hasFrag) {
        _argumentTables.comparisonSamplers = [mf_[@"argument_resource_tables"][@"comparison_sampler"] boolValue];
        _argumentTables.samplerCoordinates = [mf_[@"normalized_sampler_coordinates"] boolValue];
        if (!rdna4_argument_table_metadata_apply(&_argumentTables, rf, 1, mf_, &le) ||
            !rdna4_argument_table_layouts(&_argumentTables, NULL, rf, NO, &le) ||
            !rdna4_argument_stage_remap(&_argumentTables, _pbF, _npbF, &le) ||
            !rdna4_argument_scan_plan_build(&_scanF, preF, mf_, "mesh-fragment", &le)) {
            if (err) *err = le; free(ro); free(rm); free(rf); return nil;
        }
    }

    // Push constants across graphics stages (+ separate compute layout for emulated Object).
    uint32_t gfxPcs = 0u; VkShaderStageFlags gfxPcf = 0;
    if (rm && rm->pc_size) { gfxPcs = rm->pc_size; gfxPcf |= VK_SHADER_STAGE_MESH_BIT_EXT; }
    if (rf && rf->pc_size) { if (rf->pc_size > gfxPcs) gfxPcs = rf->pc_size; gfxPcf |= VK_SHADER_STAGE_FRAGMENT_BIT; }
    if (_hasObject && !emul && ro && ro->pc_size) {
        if (ro->pc_size > gfxPcs) gfxPcs = ro->pc_size;
        gfxPcf |= VK_SHADER_STAGE_TASK_BIT_EXT;
    }
    if (gfxPcs > 256u) {
        if (err) *err = rdna4_err(48, [NSString stringWithFormat:@"mesh pipeline push constant block %u B > 256", gfxPcs]);
        free(ro); free(rm); free(rf); return nil;
    }
    _pcs = gfxPcs; _pcf = gfxPcf;

    _objSet = _argumentTables.active ? 5u : 2u;
    VkDescriptorSetLayout layouts[6] = {
        _dsl[0], _dsl[1],
        _argumentTables.active ? _argumentTables.dsl[0] : _dslObj,
        _argumentTables.dsl[1], _argumentTables.dsl[2], _dslObj,
    };
    BOOL nativeObj = (_meshExecMode == RDNA4MESH_MODE_NATIVE_TASK_MESH);
    uint32_t layoutCount = _argumentTables.active ? (nativeObj ? 6u : 5u) : (nativeObj ? 3u : 2u);
    VkPushConstantRange pcr = { gfxPcf, 0, gfxPcs };
    VkPipelineLayoutCreateInfo plc = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = layoutCount,
        .pSetLayouts = layouts,
        .pushConstantRangeCount = gfxPcs ? 1u : 0u,
        .pPushConstantRanges = &pcr,
    };
    VkResult plr = vkCreatePipelineLayout(RDNA4R.dev, &plc, NULL, &_pl);
    if (plr != VK_SUCCESS) {
        if (err) *err = rdna4_err(46, [NSString stringWithFormat:@"vkCreatePipelineLayout(mesh) = %d", plr]);
        free(ro); free(rm); free(rf); return nil;
    }

    if (emul) {
        uint32_t objPcs = (ro && ro->pc_size) ? ro->pc_size : 0u;
        VkPushConstantRange opcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, objPcs };
        VkPipelineLayoutCreateInfo oplc = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1u,
            .pSetLayouts = &_objCompDsl,
            .pushConstantRangeCount = objPcs ? 1u : 0u,
            .pPushConstantRanges = &opcr,
        };
        VkResult oplr = vkCreatePipelineLayout(RDNA4R.dev, &oplc, NULL, &_objCompPl);
        if (oplr != VK_SUCCESS) {
            if (err) *err = rdna4_err(46, [NSString stringWithFormat:@"vkCreatePipelineLayout(object-emul) = %d", oplr]);
            free(ro); free(rm); free(rf); return nil;
        }
    }
    free(ro); free(rm); free(rf);

    // Create initial VkShaderModules (relocating native TaskEXT to _objSet).
    if (_hasObject) {
        NSData *objUse = emul ? osd : rdna4_spv_set(osd, _objSet);
        _objMod = rdna4_spv_module(objUse.bytes, objUse.length, emul ? "object-emul" : "object-task", err);
        if (!_objMod) return nil;
    }
    _meshMod = rdna4_spv_module(msd.bytes, msd.length, "mesh", err);
    if (!_meshMod) return nil;
    if (_hasFrag) {
        _fs = rdna4_spv_module(fsp.bytes, fsp.length, "mesh-fragment", err);
        if (!_fs) return nil;
    }

    // Build and verify initial pipeline(s) so bad shader/state fails synchronously at creation time.
    if (emul) {
        VkPipeline op0 = [self rdna4ObjectComputePipelineForLocal:_initObjLocal error:err];
        if (!op0) return nil;
    }
    uint32_t dk0 = [self rdna4DSKey:NULL];
    VkPipeline gp0 = [self rdna4MeshPipelineForCull:VK_CULL_MODE_NONE front:VK_FRONT_FACE_CLOCKWISE
                                                 ds:dk0 rflags:0
                                           objLocal:_initObjLocal meshLocal:_initMeshLocal error:err];
    if (!gp0) return nil;

    RDNA4LOG("RDNA4MeshRenderPipelineState %p: mode=%d payload=%u B (stride %u B) maxV=%u maxP=%u topo=%d colorAtt=%u",
             (__bridge void *)self, (int)_meshExecMode, _linkedInfo.effective_payload_bytes,
             _linkedInfo.effective_payload_stride, _linkedInfo.max_vertices, _linkedInfo.max_primitives,
             (int)_linkedInfo.topology, _na);
    return self;
}

- (void)dealloc {
    if (RDNA4R.ok) {
        for (NSValue *v in _meshVariants.allValues) vkDestroyPipeline(RDNA4R.dev, (VkPipeline)v.pointerValue, NULL);
        for (NSValue *v in _objCompVariants.allValues) vkDestroyPipeline(RDNA4R.dev, (VkPipeline)v.pointerValue, NULL);
        for (NSValue *v in _objModCache.allValues) vkDestroyShaderModule(RDNA4R.dev, (VkShaderModule)v.pointerValue, NULL);
        for (NSValue *v in _meshModCache.allValues) vkDestroyShaderModule(RDNA4R.dev, (VkShaderModule)v.pointerValue, NULL);
        if (_objCompPl) vkDestroyPipelineLayout(RDNA4R.dev, _objCompPl, NULL);
        if (_objCompDsl) vkDestroyDescriptorSetLayout(RDNA4R.dev, _objCompDsl, NULL);
        if (_dslObj) vkDestroyDescriptorSetLayout(RDNA4R.dev, _dslObj, NULL);
        if (_objMod) vkDestroyShaderModule(RDNA4R.dev, _objMod, NULL);
        if (_meshMod) vkDestroyShaderModule(RDNA4R.dev, _meshMod, NULL);
    }
    free(_pbO); _pbO = NULL; _npbO = 0u;
    free(_pbM); _pbM = NULL; _npbM = 0u;
}

@end

// =================================================================================================
// 5. Device factory helpers for GarnetDevice.m (Wave 3E)
// =================================================================================================

__attribute__((unused)) GARNET_INTERNAL id rdna4_mesh_device_new_pipeline(
    id dev, id desc, NSUInteger options, MTLRenderPipelineReflection **refl, NSError **error)
{
    if (refl) *refl = nil;
    NSError *e = nil;
    RDNA4MeshRenderPipelineState *p = [[RDNA4MeshRenderPipelineState alloc]
        initWithDevice:dev meshDescriptor:desc options:options error:&e];
    if (!p) {
        if (e) RDNA4LOG("mesh pipeline creation failed: %s", e.localizedDescription.UTF8String);
        if (error) *error = [e copy];
        return nil;
    }
    return p;
}

__attribute__((unused)) GARNET_INTERNAL void rdna4_mesh_device_new_pipeline_async(
    id dev, id desc, NSUInteger options, void (^handler)(id, id, NSError *))
{
    if (!handler) return;
    id saved = [desc respondsToSelector:@selector(copy)] ? [desc copy] : desc;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
        NSError *e = nil;
        MTLRenderPipelineReflection *r = nil;
        id p = rdna4_mesh_device_new_pipeline(dev, saved, options, &r, &e);
        handler(p, r, e);
    }});
}

// =================================================================================================
// 6. RDNA4RenderEncoder (RDNA4Mesh): Object & Mesh binding tables + direct/threads/indirect draws
// =================================================================================================

@implementation RDNA4MeshEncoderState @end

static char rdna4_mesh_enc_state_key;

static BOOL rdna4_mesh_update_emul_descriptors(VkDescriptorSet ds, uint32_t emulMask,
                                               RDNA4Buffer *gpBuf, uint64_t gpVkOff,
                                               RDNA4Buffer *scratchBuf,
                                               const rdna4mesh_emul_plan_t *plan) {
    VkWriteDescriptorSet writes[3];
    VkDescriptorBufferInfo infos[3];
    uint32_t nw = 0u;
    uint64_t sBase = [scratchBuf rdna4VkOffset];
    VkBuffer sVk = [scratchBuf vkBuffer];

    if (emulMask & RDNA4MESH_EMUL_BIT_GRID_PARAMS) {
        infos[nw] = (VkDescriptorBufferInfo){ [gpBuf vkBuffer], gpVkOff, RDNA4MESH_GRID_PARAMS_BYTES };
        writes[nw] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ds, .dstBinding = GARNETX_MESH_GRID_PARAMS_BINDING,
            .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &infos[nw],
        };
        nw++;
    }
    if (emulMask & RDNA4MESH_EMUL_BIT_PAYLOAD) {
        infos[nw] = (VkDescriptorBufferInfo){ sVk, sBase + plan->payload_offset, plan->payload_bytes };
        writes[nw] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ds, .dstBinding = GARNETX_MESH_PAYLOAD_BINDING,
            .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &infos[nw],
        };
        nw++;
    }
    if (emulMask & RDNA4MESH_EMUL_BIT_INDIRECT) {
        infos[nw] = (VkDescriptorBufferInfo){ sVk, sBase + plan->indirect_offset, plan->indirect_bytes };
        writes[nw] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ds, .dstBinding = GARNETX_MESH_INDIRECT_BINDING,
            .descriptorCount = 1u, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &infos[nw],
        };
        nw++;
    }
    if (nw > 0u) vkUpdateDescriptorSets(RDNA4R.dev, nw, writes, 0u, NULL);
    return YES;
}

@implementation RDNA4RenderEncoder (RDNA4Mesh)

- (RDNA4MeshEncoderState *)rdna4MeshState {
    RDNA4MeshEncoderState *st = objc_getAssociatedObject(self, &rdna4_mesh_enc_state_key);
    if (!st) {
        st = [RDNA4MeshEncoderState new];
        objc_setAssociatedObject(self, &rdna4_mesh_enc_state_key, st, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return st;
}

// ---- Object & Mesh stage binding helpers ----
- (void)rdna4SetMeshStageBuffer:(id)buf offset:(NSUInteger)off index:(NSUInteger)i isMesh:(BOOL)isMesh {
    if ([_cb rdna4Error]) return;
    if (i >= RDNA4MESH_MAX_BUFFERS) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s buffer index %lu >= 31", isMesh ? "mesh" : "object", (unsigned long)i]];
        return;
    }
    if (buf && ![buf isKindOfClass:[RDNA4Buffer class]]) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s setBuffer: not an RDNA4Buffer", isMesh ? "mesh" : "object"]];
        return;
    }
    RDNA4MeshEncoderState *ms = [self rdna4MeshState];
    RDNA4StageState *s = isMesh ? &ms->meshState : &ms->objState;
    rdna4_stage_set_buf(s, i, buf, off);
    [self rdna4OwnResource:buf kind:(isMesh ? RDNA4_RESOURCE_MESH_BUFFER : RDNA4_RESOURCE_OBJECT_BUFFER)
                  fragment:NO index:i];
}

- (void)rdna4SetMeshStageBytes:(const void *)p length:(NSUInteger)n index:(NSUInteger)i isMesh:(BOOL)isMesh {
    if ([_cb rdna4Error]) return;
    if (i >= RDNA4MESH_MAX_BUFFERS || (n && !p)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s setBytes: index %lu / bytes %p", isMesh ? "mesh" : "object", (unsigned long)i, p]];
        return;
    }
    RDNA4Buffer *b = nil; NSUInteger off = 0;
    if ([_cb rdna4Bytes:p length:n buffer:&b offset:&off])
        [self rdna4SetMeshStageBuffer:b offset:off index:i isMesh:isMesh];
}

- (void)rdna4SetMeshStageTexture:(id)t index:(NSUInteger)i isMesh:(BOOL)isMesh {
    if ([_cb rdna4Error]) return;
    if (i >= RDNA4MESH_MAX_TEXTURES) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s texture index %lu >= 128", isMesh ? "mesh" : "object", (unsigned long)i]];
        return;
    }
    if (t && ![t isKindOfClass:[RDNA4Texture class]]) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s setTexture: not an RDNA4Texture", isMesh ? "mesh" : "object"]];
        return;
    }
    RDNA4MeshEncoderState *ms = [self rdna4MeshState];
    RDNA4StageState *s = isMesh ? &ms->meshState : &ms->objState;
    s->tex[i] = t;
    [self rdna4OwnResource:t kind:(isMesh ? RDNA4_RESOURCE_MESH_TEXTURE : RDNA4_RESOURCE_OBJECT_TEXTURE)
                  fragment:NO index:i];
}

- (void)rdna4SetMeshStageSampler:(id)s lodMinClamp:(float)a lodMaxClamp:(float)b
                        override:(BOOL)override index:(NSUInteger)i isMesh:(BOOL)isMesh {
    if ([_cb rdna4Error]) return;
    if (i >= RDNA4MESH_MAX_SAMPLERS) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s sampler index %lu >= 16", isMesh ? "mesh" : "object", (unsigned long)i]];
        return;
    }
    if (s && ![s isKindOfClass:[RDNA4SamplerState class]]) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"%s setSamplerState: not an RDNA4SamplerState", isMesh ? "mesh" : "object"]];
        return;
    }
    RDNA4SamplerBinding *binding = nil;
    if (s) {
        NSError *error = nil;
        RDNA4SamplerState *sampler = (RDNA4SamplerState *)s;
        binding = override
            ? [sampler rdna4BindingWithLODMinClamp:a lodMaxClamp:b error:&error]
            : [sampler rdna4Binding:&error];
        if (!binding) {
            [_cb rdna4Fail:error.localizedDescription ?: @"mesh/object sampler binding unavailable"];
            return;
        }
        [_cb rdna4RetainArgument:binding];
        if ([_cb rdna4Error]) return;
    }
    RDNA4MeshEncoderState *ms = [self rdna4MeshState];
    RDNA4StageState *st = isMesh ? &ms->meshState : &ms->objState;
    RDNA4SamplerBinding *old = st->smp[i];
    st->smp[i] = binding;
    if (binding) {
        if (!ms->samplers) ms->samplers = [NSMutableSet set];
        [ms->samplers addObject:binding];
    }
    if (old && old != binding) {
        BOOL used = NO;
        for (NSUInteger k = 0; k < RDNA4MESH_MAX_SAMPLERS && !used; k++)
            used = (ms->objState.smp[k] == old || ms->meshState.smp[k] == old);
        if (!used) [ms->samplers removeObject:old];
    }
}

// ---- Object stage bindings ----
- (void)setObjectBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self rdna4SetMeshStageBuffer:b offset:o index:i isMesh:NO]; }
- (void)setObjectBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {
    if (i < RDNA4MESH_MAX_BUFFERS) [self rdna4MeshState]->objState.buf[i].off = o;
}
- (void)setObjectBuffers:(const id<MTLBuffer> __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r {
    if (r.length && (!b || !o || r.location > RDNA4MESH_MAX_BUFFERS || r.length > RDNA4MESH_MAX_BUFFERS - r.location)) {
        [_cb rdna4Fail:@"setObjectBuffers: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageBuffer:b[k] offset:o[k] index:r.location + k isMesh:NO];
}
- (void)setObjectBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i { [self rdna4SetMeshStageBytes:p length:n index:i isMesh:NO]; }
- (void)setObjectTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i { [self rdna4SetMeshStageTexture:t index:i isMesh:NO]; }
- (void)setObjectTextures:(const id<MTLTexture> __unsafe_unretained *)t withRange:(NSRange)r {
    if (r.length && (!t || r.location > RDNA4MESH_MAX_TEXTURES || r.length > RDNA4MESH_MAX_TEXTURES - r.location)) {
        [_cb rdna4Fail:@"setObjectTextures: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageTexture:t[k] index:r.location + k isMesh:NO];
}
- (void)setObjectSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {
    [self rdna4SetMeshStageSampler:s lodMinClamp:0 lodMaxClamp:0 override:NO index:i isMesh:NO];
}
- (void)setObjectSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i {
    [self rdna4SetMeshStageSampler:s lodMinClamp:lo lodMaxClamp:hi override:YES index:i isMesh:NO];
}
- (void)setObjectSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)s withRange:(NSRange)r {
    if (r.length && (!s || r.location > RDNA4MESH_MAX_SAMPLERS || r.length > RDNA4MESH_MAX_SAMPLERS - r.location)) {
        [_cb rdna4Fail:@"setObjectSamplerStates: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageSampler:s[k] lodMinClamp:0 lodMaxClamp:0 override:NO index:r.location + k isMesh:NO];
}
- (void)setObjectSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)s
                  lodMinClamps:(const float *)lo lodMaxClamps:(const float *)hi withRange:(NSRange)r {
    if (r.length && (!s || !lo || !hi || r.location > RDNA4MESH_MAX_SAMPLERS || r.length > RDNA4MESH_MAX_SAMPLERS - r.location)) {
        [_cb rdna4Fail:@"setObjectSamplerStates:lodMinClamps: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageSampler:s[k] lodMinClamp:lo[k] lodMaxClamp:hi[k] override:YES index:r.location + k isMesh:NO];
}
- (void)setObjectThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index {
    if ([_cb rdna4Error]) return;
    RDNA4MeshEncoderState *ms = [self rdna4MeshState];
    const char *why = NULL;
    uint64_t newTotal = 0u;
    rdna4mesh_status_t st = rdna4mesh_validate_tg_memory(
        ms->objTgMem, index <= UINT32_MAX ? (uint32_t)index : UINT32_MAX,
        (uint64_t)length, (uint64_t)rdna4_device_caps().shared_memory, &newTotal, &why);
    if (st != RDNA4MESH_OK) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"setObjectThreadgroupMemoryLength:%lu atIndex:%lu: %s",
                        (unsigned long)length, (unsigned long)index, why ? why : rdna4mesh_status_str(st)]];
        return;
    }
    ms->objTgMem[index] = (uint64_t)length;
}

// ---- Mesh stage bindings ----
- (void)setMeshBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self rdna4SetMeshStageBuffer:b offset:o index:i isMesh:YES]; }
- (void)setMeshBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {
    if (i < RDNA4MESH_MAX_BUFFERS) [self rdna4MeshState]->meshState.buf[i].off = o;
}
- (void)setMeshBuffers:(const id<MTLBuffer> __unsafe_unretained *)b offsets:(const NSUInteger *)o withRange:(NSRange)r {
    if (r.length && (!b || !o || r.location > RDNA4MESH_MAX_BUFFERS || r.length > RDNA4MESH_MAX_BUFFERS - r.location)) {
        [_cb rdna4Fail:@"setMeshBuffers: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageBuffer:b[k] offset:o[k] index:r.location + k isMesh:YES];
}
- (void)setMeshBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i { [self rdna4SetMeshStageBytes:p length:n index:i isMesh:YES]; }
- (void)setMeshTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i { [self rdna4SetMeshStageTexture:t index:i isMesh:YES]; }
- (void)setMeshTextures:(const id<MTLTexture> __unsafe_unretained *)t withRange:(NSRange)r {
    if (r.length && (!t || r.location > RDNA4MESH_MAX_TEXTURES || r.length > RDNA4MESH_MAX_TEXTURES - r.location)) {
        [_cb rdna4Fail:@"setMeshTextures: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageTexture:t[k] index:r.location + k isMesh:YES];
}
- (void)setMeshSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {
    [self rdna4SetMeshStageSampler:s lodMinClamp:0 lodMaxClamp:0 override:NO index:i isMesh:YES];
}
- (void)setMeshSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i {
    [self rdna4SetMeshStageSampler:s lodMinClamp:lo lodMaxClamp:hi override:YES index:i isMesh:YES];
}
- (void)setMeshSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)s withRange:(NSRange)r {
    if (r.length && (!s || r.location > RDNA4MESH_MAX_SAMPLERS || r.length > RDNA4MESH_MAX_SAMPLERS - r.location)) {
        [_cb rdna4Fail:@"setMeshSamplerStates: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageSampler:s[k] lodMinClamp:0 lodMaxClamp:0 override:NO index:r.location + k isMesh:YES];
}
- (void)setMeshSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)s
                lodMinClamps:(const float *)lo lodMaxClamps:(const float *)hi withRange:(NSRange)r {
    if (r.length && (!s || !lo || !hi || r.location > RDNA4MESH_MAX_SAMPLERS || r.length > RDNA4MESH_MAX_SAMPLERS - r.location)) {
        [_cb rdna4Fail:@"setMeshSamplerStates:lodMinClamps: invalid range or NULL array"]; return;
    }
    for (NSUInteger k = 0; k < r.length; k++) [self rdna4SetMeshStageSampler:s[k] lodMinClamp:lo[k] lodMaxClamp:hi[k] override:YES index:r.location + k isMesh:YES];
}

// Footnote 10 encoder guards: VFT / IFT / AS cannot be bound to Object or Mesh stages.
- (void)setObjectVisibleFunctionTable:(id)t atBufferIndex:(NSUInteger)i {
    (void)i; if (t) [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_FN_PTR))];
}
- (void)setObjectVisibleFunctionTables:(const id __unsafe_unretained *)t withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; t && k < r.length; k++) if (t[k]) { [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_FN_PTR))]; return; }
}
- (void)setMeshVisibleFunctionTable:(id)t atBufferIndex:(NSUInteger)i {
    (void)i; if (t) [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_FN_PTR))];
}
- (void)setMeshVisibleFunctionTables:(const id __unsafe_unretained *)t withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; t && k < r.length; k++) if (t[k]) { [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_FN_PTR))]; return; }
}
- (void)setObjectIntersectionFunctionTable:(id)t atBufferIndex:(NSUInteger)i {
    (void)i; if (t) [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_RT))];
}
- (void)setObjectIntersectionFunctionTables:(const id __unsafe_unretained *)t withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; t && k < r.length; k++) if (t[k]) { [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_RT))]; return; }
}
- (void)setMeshIntersectionFunctionTable:(id)t atBufferIndex:(NSUInteger)i {
    (void)i; if (t) [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_RT))];
}
- (void)setMeshIntersectionFunctionTables:(const id __unsafe_unretained *)t withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; t && k < r.length; k++) if (t[k]) { [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_RT))]; return; }
}
- (void)setObjectAccelerationStructure:(id)as atBufferIndex:(NSUInteger)i {
    (void)i; if (as) [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_RT))];
}
- (void)setMeshAccelerationStructure:(id)as atBufferIndex:(NSUInteger)i {
    (void)i; if (as) [_cb rdna4Fail:@(rdna4mesh_status_str(RDNA4MESH_ERR_FOOTNOTE10_RT))];
}

// ---- Core mesh draw implementation (direct, threads, and indirect) ----
- (void)rdna4DrawMeshGroups:(MTLSize)grid
                  objLocal:(MTLSize)objTg
                 meshLocal:(MTLSize)meshTg
                  indirect:(BOOL)isIndirect
            indirectBuffer:(id)indirectBuf
            indirectOffset:(NSUInteger)indirectOff {
    if (_hostWriteDropped) return;
    if (_ended) { [_cb rdna4Fail:@"mesh draw after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (!RDNA4MeshFn.ready && !rdna4_mesh_resolve_vk_funcs()) {
        [_cb rdna4Fail:@"VK_EXT_mesh_shader command entry points are unavailable"];
        return;
    }
    if (!_pso || ![_pso rdna4IsMeshPipeline]) {
        [_cb rdna4Fail:@"drawMesh* requires a mesh render pipeline state"];
        return;
    }

    RDNA4MeshRenderPipelineState *mpso = (RDNA4MeshRenderPipelineState *)_pso;
    const rdna4mesh_exec_mode_t mode = [mpso rdna4MeshExecMode];
    const BOOL hasObj = [mpso rdna4HasObjectStage];
    const BOOL hasFrag = [mpso rdna4HasFragment];
    const rdna4dc_caps caps = rdna4_device_caps();
    const char *why = NULL;

    // 1. Validate threadgroup dimensions for Mesh (always) and Object (when present).
    uint32_t meshLocal3[3] = { 0u, 0u, 0u }, objLocal3[3] = { 1u, 1u, 1u };
    {
        const rdna4mesh_refl_t *mr = [mpso rdna4MeshRefl];
        rdna4mesh_status_t mst = rdna4mesh_validate_tg_size(
            (uint64_t)meshTg.width, (uint64_t)meshTg.height, (uint64_t)meshTg.depth,
            caps.workgroup_size, (uint32_t)[mpso maxTotalThreadsPerMeshThreadgroup],
            mr->has_max_work_group_size ? mr->max_work_group_size : 0u,
            (uint32_t)[mpso meshThreadExecutionWidth], [mpso rdna4MeshMultExecWidth] ? 1 : 0, &why);
        if (mst != RDNA4MESH_OK) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"threadsPerMeshThreadgroup (%lu,%lu,%lu): %s",
                            (unsigned long)meshTg.width, (unsigned long)meshTg.height, (unsigned long)meshTg.depth,
                            why ? why : rdna4mesh_status_str(mst)]];
            return;
        }
        meshLocal3[0] = (uint32_t)meshTg.width;
        meshLocal3[1] = (uint32_t)meshTg.height;
        meshLocal3[2] = (uint32_t)meshTg.depth;
    }
    if (hasObj) {
        const rdna4mesh_refl_t *or_ = [mpso rdna4ObjRefl];
        rdna4mesh_status_t ost = rdna4mesh_validate_tg_size(
            (uint64_t)objTg.width, (uint64_t)objTg.height, (uint64_t)objTg.depth,
            caps.workgroup_size, (uint32_t)[mpso maxTotalThreadsPerObjectThreadgroup],
            or_->has_max_work_group_size ? or_->max_work_group_size : 0u,
            (uint32_t)[mpso objectThreadExecutionWidth], [mpso rdna4ObjMultExecWidth] ? 1 : 0, &why);
        if (ost != RDNA4MESH_OK) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"threadsPerObjectThreadgroup (%lu,%lu,%lu): %s",
                            (unsigned long)objTg.width, (unsigned long)objTg.height, (unsigned long)objTg.depth,
                            why ? why : rdna4mesh_status_str(ost)]];
            return;
        }
        objLocal3[0] = (uint32_t)objTg.width;
        objLocal3[1] = (uint32_t)objTg.height;
        objLocal3[2] = (uint32_t)objTg.depth;
    }

    // 2. Validate grid (direct) or indirect buffer bounds & alignment (indirect).
    uint64_t userIndVkOff = 0u;
    if (isIndirect) {
        if (![indirectBuf isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)indirectBuf device] != [_cb device]) {
            [_cb rdna4Fail:@"drawMeshThreadgroupsWithIndirectBuffer: buffer is nil, wrong class, or wrong device"];
            return;
        }
        rdna4mesh_status_t ist = rdna4mesh_validate_indirect_buffer(
            (uint64_t)[(RDNA4Buffer *)indirectBuf length], (uint64_t)indirectOff,
            [(RDNA4Buffer *)indirectBuf rdna4VkOffset], &userIndVkOff, &why);
        if (ist != RDNA4MESH_OK) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"drawMeshThreadgroupsWithIndirectBuffer: %s",
                            why ? why : rdna4mesh_status_str(ist)]];
            return;
        }
    } else {
        int isNoop = 0;
        uint64_t totalGroups = 0u;
        const uint32_t *axisLim = (mode == RDNA4MESH_MODE_NATIVE_TASK_MESH)
            ? caps.metal3.object_max_workgroup_count
            : (mode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH)
                ? RDNA4R.lim.maxComputeWorkGroupCount
                : caps.metal3.mesh_max_workgroup_count;
        uint32_t totalLim = (mode == RDNA4MESH_MODE_NATIVE_TASK_MESH)
            ? caps.metal3.object_max_workgroup_total_count
            : (mode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH)
                ? rdna4_mesh_max_draw_indirect_count()
                : [mpso rdna4MeshLinkedInfo]->effective_max_mesh_grid;
        rdna4mesh_status_t gst = rdna4mesh_validate_grid(
            (uint64_t)grid.width, (uint64_t)grid.height, (uint64_t)grid.depth,
            axisLim, totalLim, &isNoop, &totalGroups, &why);
        if (gst != RDNA4MESH_OK) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"drawMeshThreadgroups (%lu,%lu,%lu): %s",
                            (unsigned long)grid.width, (unsigned long)grid.height, (unsigned long)grid.depth,
                            why ? why : rdna4mesh_status_str(gst)]];
            return;
        }
        if (isNoop) return;
    }

    // 3. Validate pass compatibility & build/fetch Vulkan pipeline(s).
    if ([mpso rdna4DSVk] != _dsVkFmt || [mpso rdna4SampleCount] != _encSamples) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"mesh pipeline depth/stencil (vk %d) or sample count (%u) differs from pass (vk %d, %u)",
                        (int)[mpso rdna4DSVk], [mpso rdna4SampleCount], (int)_dsVkFmt, _encSamples]];
        return;
    }
    if ([_cb rdna4HasArgumentWrites]) {
        if (_inPass) [self rdna4EndPass];
        [_cb rdna4ArgumentCheckpoint];
        if ([_cb rdna4Error]) return;
    }
    [self rdna4SyncResourceSegment];
    [self rdna4SnapshotResources];

    VkCullModeFlags cull = _cull == MTLCullModeFront ? VK_CULL_MODE_FRONT_BIT
        : _cull == MTLCullModeBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    VkFrontFace front = _wind == MTLWindingCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    uint32_t dk = [mpso rdna4DSKey:&_dsi];
    uint32_t vpOff = _ampHasMappings ? _ampMappings[0].viewport_offset : 0u;
    uint32_t vpCount = rdna4amp_viewport_count(_nvp, _nsr, vpOff, RDNA4R.maxViewports ?: 1u);
    uint32_t rflags = ((_fillMode == MTLTriangleFillModeLines) ? 1u : 0u) |
                      ((_depthClipMode == MTLDepthClipModeClamp) ? 2u : 0u) |
                      (_hasSamplePos ? 4u : 0u) | ((vpCount - 1u) << 3);

    NSError *pe = nil;
    VkPipeline objCompPipe = VK_NULL_HANDLE;
    if (mode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH) {
        objCompPipe = [mpso rdna4ObjectComputePipelineForLocal:objLocal3 error:&pe];
        if (!objCompPipe) {
            [_cb rdna4Fail:pe.localizedDescription ?: @"failed to build emulated object compute pipeline"];
            return;
        }
    }
    VkPipeline gfxPipe = [mpso rdna4MeshPipelineForCull:cull front:front ds:dk rflags:rflags
                                               objLocal:objLocal3 meshLocal:meshLocal3 error:&pe];
    if (!gfxPipe) {
        [_cb rdna4Fail:pe.localizedDescription ?: @"failed to build mesh graphics pipeline variant"];
        return;
    }

    // 4. Plan emulated Object+Mesh scratch buffer (if emulated) and execute Object compute pass outside render pass.
    RDNA4MeshEncoderState *ms = [self rdna4MeshState];
    id dev = [_cb device];
    VkCommandBuffer cmd = [_cb vk];
    RDNA4Buffer *emulScratch = nil;
    RDNA4Buffer *emulGpBuf = nil;
    uint64_t emulGpVkOff = 0u;
    rdna4mesh_emul_plan_t emulPlan;
    memset(&emulPlan, 0, sizeof emulPlan);

    void (^needTex)(RDNA4Texture *, VkImageLayout, BOOL) = ^(RDNA4Texture *t, VkImageLayout l, BOOL writable) {
        if ([t rdna4IsIOS]) (void)rdna4_ios_touch(self->_cb, t, YES, writable, ^{
            if (self->_inPass) [self rdna4EndPass];
        });
        [self->_cb rdna4CBRead:t];
        if ([t rdna4DispImgOnly]) (void)[self->_cb rdna4IOFirstTouch:t];
        if (([t rdna4IsIOS] || [t rdna4DispImgOnly]) && writable) {
            [self->_cb rdna4IOWritten:t];
            [self->_cb rdna4DispNote:t bits:RDNA4DF_W_COMPUTE pso:nil];
        }
        [self rdna4Need:t layout:l];
    };

    if (mode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH) {
        const uint32_t payloadStride = [mpso rdna4MeshLinkedInfo]->effective_payload_stride;
        const uint32_t maxDrawInd = rdna4_mesh_max_draw_indirect_count();
        rdna4mesh_status_t pst;
        if (isIndirect) {
            if (maxDrawInd <= 1u) {
                [_cb rdna4Fail:@"indirect emulated object+mesh requires VkPhysicalDeviceFeatures.multiDrawIndirect"];
                return;
            }
            // shortcut: emulated indirect Object+Mesh caps GPU-written object groups to
            // RDNA4MESH_EMUL_INDIRECT_MAX_GROUPS (1024) until kext MEC gang submit enables taskShader=true.
            pst = rdna4mesh_plan_emulated_indirect_draw(
                payloadStride, caps.buffer_length, RDNA4R.lim.maxStorageBufferRange,
                maxDrawInd, RDNA4R.lim.minStorageBufferOffsetAlignment,
                RDNA4MESH_EMUL_INDIRECT_MAX_GROUPS, &emulPlan, &why);
        } else {
            pst = rdna4mesh_plan_emulated_draw(
                (uint64_t)grid.width, (uint64_t)grid.height, (uint64_t)grid.depth,
                payloadStride, caps.buffer_length, RDNA4R.lim.maxStorageBufferRange,
                maxDrawInd, RDNA4R.lim.minStorageBufferOffsetAlignment,
                &emulPlan, &why);
        }
        if (pst != RDNA4MESH_OK) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"emulated object+mesh scratch planning failed: %s",
                            why ? why : rdna4mesh_status_str(pst)]];
            return;
        }
        if (emulPlan.is_noop) return;

        NSError *be = nil;
        emulScratch = [[RDNA4Buffer alloc] initWithDevice:dev length:(NSUInteger)emulPlan.total_scratch_bytes
                                                  options:MTLResourceStorageModePrivate error:&be];
        if (!emulScratch) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"emulated object+mesh scratch buffer allocation failed: %@",
                            be.localizedDescription ?: @"OOM"]];
            return;
        }
        [_cb rdna4RetainEncoded:emulScratch];

        // Object compute must execute outside any Vulkan render pass; continuation pass reloads attachments (LOAD_OP_LOAD).
        if (_inPass) [self rdna4EndPass];
        if ([mpso rdna4NpbO]) rdna4_each_tex([mpso rdna4PbO], [mpso rdna4NpbO], &ms->objState, dev, needTex);

        VkBuffer scratchVk = [emulScratch vkBuffer];
        uint64_t scratchBase = [emulScratch rdna4VkOffset];
        if (isIndirect) {
            [_cb rdna4RetainEncoded:indirectBuf];
            rdna4_full_barrier(cmd);
            VkBufferCopy copyReg = { .srcOffset = userIndVkOff, .dstOffset = scratchBase + emulPlan.grid_params_offset, .size = RDNA4MESH_INDIRECT_CMD_BYTES };
            vkCmdCopyBuffer(cmd, [(RDNA4Buffer *)indirectBuf vkBuffer], scratchVk, 1u, &copyReg);
            vkCmdFillBuffer(cmd, scratchVk, scratchBase + emulPlan.grid_params_offset + 12u, 4u, emulPlan.grid_params.payload_stride_bytes);
            emulGpBuf = emulScratch;
            emulGpVkOff = scratchBase + emulPlan.grid_params_offset;
        } else {
            NSUInteger gpRingOff = 0;
            if (![_cb rdna4Bytes:&emulPlan.grid_params length:sizeof(emulPlan.grid_params) buffer:&emulGpBuf offset:&gpRingOff])
                return;
            emulGpVkOff = [emulGpBuf rdna4VkOffset] + gpRingOff;
        }
        vkCmdFillBuffer(cmd, scratchVk, scratchBase + emulPlan.indirect_offset, emulPlan.indirect_fill_bytes, 0u);

        // Barrier: transfer fills/copies -> Object compute + indirect dispatch read.
        VkMemoryBarrier t2c = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                             0, 1u, &t2c, 0u, NULL, 0u, NULL);

        VkDescriptorSet objDs = [_cb rdna4AllocSet:[mpso rdna4ObjectComputeDsl]];
        if (!objDs) return;
        if ([mpso rdna4NpbO] &&
            ![_cb rdna4FillSet:objDs bindings:[mpso rdna4PbO] count:[mpso rdna4NpbO] state:&ms->objState stage:"object"])
            return;
        rdna4_mesh_update_emul_descriptors(objDs, [mpso rdna4EmulObjMask], emulGpBuf, emulGpVkOff, emulScratch, &emulPlan);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, objCompPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, [mpso rdna4ObjectComputeLayout], 0u, 1u, &objDs, 0u, NULL);
        if (isIndirect) {
            vkCmdDispatchIndirect(cmd, [(RDNA4Buffer *)indirectBuf vkBuffer], userIndVkOff);
        } else {
            vkCmdDispatch(cmd, (uint32_t)grid.width, (uint32_t)grid.height, (uint32_t)grid.depth);
        }

        // Barrier: Object compute writes (payload + indirect commands + user buffers) -> Mesh indirect draw & shader reads.
        VkMemoryBarrier c2m = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1u, &c2m, 0u, NULL, 0u, NULL);
    } else if (isIndirect) {
        if (_inPass) [self rdna4EndPass];
        rdna4_full_barrier(cmd);
    }

    // 5. Transition textures for native Object, Mesh, and Fragment stages outside the render pass.
    RDNA4RenderScratch *encSc = [self rdna4EnsureScratch];
    if (mode == RDNA4MESH_MODE_NATIVE_TASK_MESH && [mpso rdna4NpbO])
        rdna4_each_tex([mpso rdna4PbO], [mpso rdna4NpbO], &ms->objState, dev, needTex);
    if ([mpso rdna4NpbM])
        rdna4_each_tex([mpso rdna4PbM], [mpso rdna4NpbM], &ms->meshState, dev, needTex);
    if (hasFrag && [mpso npbF])
        rdna4_each_tex([mpso pbF], [mpso npbF], &encSc->sf, dev, needTex);

    if (hasFrag && [mpso argumentTables]->active) {
        NSMutableSet *argRes = [_argumentResources mutableCopy] ?: [NSMutableSet set];
        NSMutableSet *argW = [_argumentWritable mutableCopy] ?: [NSMutableSet set];
        rdna4_argument_stage_resources(argRes, argW, [mpso pbF], [mpso npbF], &encSc->sf, nil);
        RDNA4ArgumentScanPrepared scan = {0};
        const RDNA4StageState *scanStages[1] = { &encSc->sf };
        NSString *scanErr = nil;
        if (!rdna4_argument_scan_prepare(&scan, scanStages, 1u, argW, dev, &scanErr)) {
            [_cb rdna4Fail:scanErr ?: @"mesh fragment argument scan failed"];
            return;
        }
        BOOL ready = [_cb rdna4PrepareArguments:[mpso argumentTables] resources:argRes writable:argW scan:&scan textures:needTex];
        rdna4_argument_scan_prepared_destroy(&scan);
        if (!ready) return;
    }

    if ([mpso fetch] != _fetch) {
        if ([mpso fetch] && [mpso fetchMax] >= _na) {
            [_cb rdna4Fail:[NSString stringWithFormat:@"mesh pipeline reads [[color(%u)]] past %u attachments", [mpso fetchMax], _na]];
            return;
        }
        if (_inPass) [self rdna4EndPass];
        _fetch = [mpso fetch];
    }

    // 6. Begin (or resume with LOAD_OP_LOAD) the render pass and bind mesh graphics state.
    [self rdna4BeginPass];
    if ([_cb rdna4Error]) return;
    [self rdna4OccDraw];
    if (_fetch) {
        VkMemoryBarrier mb = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_DEPENDENCY_BY_REGION_BIT, 1u, &mb, 0u, NULL, 0u, NULL);
    }

    if (_dcOn) rdna4dc_enc_reset_bound(&encSc->dc);
    _drawDirty = GARNET_DRAW_DIRTY_ALL;
    _cleanFastOk = NO;
    _vbDirtyMask = ~0u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gfxPipe);

    VkViewport vps[16]; VkRect2D scs[16];
    for (uint32_t i = 0; i < vpCount; i++) {
        uint32_t vi = i + vpOff, si = i + vpOff;
        MTLViewport mv = (_nvp <= 1u || !_extraVps) ? _vp : (vi < _nvp ? _extraVps[vi] : _extraVps[_nvp - 1u]);
        MTLScissorRect sc = si < _nsr ? ((_nsr <= 1u || !_extraSrs) ? _sr : _extraSrs[si]) : (MTLScissorRect){ 0, 0, _w, _h };
        vps[i] = (VkViewport){ (float)mv.originX, (float)(mv.originY + mv.height), (float)mv.width, -(float)mv.height, (float)mv.znear, (float)mv.zfar };
        scs[i] = (VkRect2D){ { (int32_t)sc.x, (int32_t)sc.y }, { (uint32_t)sc.width, (uint32_t)sc.height } };
    }
    vkCmdSetViewport(cmd, 0u, vpCount, vps);
    vkCmdSetScissor(cmd, 0u, vpCount, scs);
    vkCmdSetBlendConstants(cmd, _blend);
    if (_hasSamplePos && rdna4_vkCmdSetSampleLocationsEXT) {
        VkSampleLocationEXT vkSL[8];
        for (uint32_t s = 0; s < _samplePosCount && s < 8; s++) vkSL[s] = (VkSampleLocationEXT){ _samplePos[s].x, _samplePos[s].y };
        VkSampleLocationsInfoEXT slInfo = {
            .sType = VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,
            .sampleLocationsPerPixel = (VkSampleCountFlagBits)_encSamples,
            .sampleLocationGridSize = { 1, 1 },
            .sampleLocationsCount = _samplePosCount,
            .pSampleLocations = vkSL,
        };
        rdna4_vkCmdSetSampleLocationsEXT(cmd, &slInfo);
    }
    if ([mpso rdna4HasDS]) {
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, _dsi.fRead);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, _dsi.bRead);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, _dsi.fWrite);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, _dsi.bWrite);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, _sref[0]);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, _sref[1]);
        if ([mpso rdna4HasDepth]) vkCmdSetDepthBias(cmd, _bias[0], _bias[1], _bias[2]);
    }

    [_cb rdna4BindArguments:[mpso argumentTables] point:VK_PIPELINE_BIND_POINT_GRAPHICS layout:[mpso layout]];
    if ([mpso rdna4NpbM] || [mpso rdna4EmulMeshMask]) {
        VkDescriptorSet meshDs = [_cb rdna4AllocSet:[mpso dsl:0]];
        if (!meshDs) return;
        if ([mpso rdna4NpbM] &&
            ![_cb rdna4FillSet:meshDs bindings:[mpso rdna4PbM] count:[mpso rdna4NpbM] state:&ms->meshState stage:"mesh"])
            return;
        if ([mpso rdna4EmulMeshMask]) {
            rdna4_mesh_update_emul_descriptors(meshDs, [mpso rdna4EmulMeshMask], emulGpBuf, emulGpVkOff, emulScratch, &emulPlan);
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, [mpso layout], 0u, 1u, &meshDs, 0u, NULL);
    }
    if (hasFrag && [mpso npbF]) {
        VkDescriptorSet fragDs = [_cb rdna4AllocSet:[mpso dsl:1]];
        if (!fragDs || ![_cb rdna4FillSet:fragDs bindings:[mpso pbF] count:[mpso npbF] state:&encSc->sf stage:"mesh-fragment"])
            return;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, [mpso layout], 1u, 1u, &fragDs, 0u, NULL);
    }
    if (mode == RDNA4MESH_MODE_NATIVE_TASK_MESH && [mpso rdna4NpbO]) {
        VkDescriptorSet objDs = [_cb rdna4AllocSet:[mpso rdna4ObjectDsl]];
        if (!objDs || ![_cb rdna4FillSet:objDs bindings:[mpso rdna4PbO] count:[mpso rdna4NpbO] state:&ms->objState stage:"object"])
            return;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, [mpso layout], [mpso rdna4ObjectSet], 1u, &objDs, 0u, NULL);
    }

    // 7. Issue VK_EXT_mesh_shader draw.
    if (mode == RDNA4MESH_MODE_EMULATED_OBJECT_MESH) {
        RDNA4MeshFn.cmdDrawMeshTasksIndirectEXT(
            cmd, [emulScratch vkBuffer],
            [emulScratch rdna4VkOffset] + emulPlan.indirect_offset,
            emulPlan.total_object_groups,
            RDNA4MESH_INDIRECT_CMD_BYTES);
    } else if (isIndirect) {
        [_cb rdna4RetainEncoded:indirectBuf];
        RDNA4MeshFn.cmdDrawMeshTasksIndirectEXT(
            cmd, [(RDNA4Buffer *)indirectBuf vkBuffer], userIndVkOff, 1u, RDNA4MESH_INDIRECT_CMD_BYTES);
    } else {
        RDNA4MeshFn.cmdDrawMeshTasksEXT(cmd, (uint32_t)grid.width, (uint32_t)grid.height, (uint32_t)grid.depth);
    }
}

- (void)drawMeshThreadgroups:(MTLSize)threadgroupsPerGrid
 threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup
   threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup {
    [self rdna4DrawMeshGroups:threadgroupsPerGrid
                     objLocal:threadsPerObjectThreadgroup
                    meshLocal:threadsPerMeshThreadgroup
                     indirect:NO indirectBuffer:nil indirectOffset:0];
}

- (void)drawMeshThreads:(MTLSize)threadsPerGrid
threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup
threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup {
    if (_hostWriteDropped) return;
    if (_ended) { [_cb rdna4Fail:@"drawMeshThreads after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (!_pso || ![_pso rdna4IsMeshPipeline]) {
        [_cb rdna4Fail:@"drawMeshThreads: requires a mesh render pipeline state"];
        return;
    }
    RDNA4MeshRenderPipelineState *mpso = (RDNA4MeshRenderPipelineState *)_pso;
    MTLSize activeTg = [mpso rdna4HasObjectStage] ? threadsPerObjectThreadgroup : threadsPerMeshThreadgroup;
    uint64_t groups[3] = { 0u, 0u, 0u };
    int isNoop = 0;
    const char *why = NULL;
    rdna4mesh_status_t st = rdna4mesh_threads_to_threadgroups(
        (uint64_t)threadsPerGrid.width, (uint64_t)threadsPerGrid.height, (uint64_t)threadsPerGrid.depth,
        (uint64_t)activeTg.width, (uint64_t)activeTg.height, (uint64_t)activeTg.depth,
        groups, &isNoop, &why);
    if (st != RDNA4MESH_OK) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"drawMeshThreads: %s", why ? why : rdna4mesh_status_str(st)]];
        return;
    }
    if (isNoop) return;
    [self rdna4DrawMeshGroups:MTLSizeMake((NSUInteger)groups[0], (NSUInteger)groups[1], (NSUInteger)groups[2])
                     objLocal:threadsPerObjectThreadgroup
                    meshLocal:threadsPerMeshThreadgroup
                     indirect:NO indirectBuffer:nil indirectOffset:0];
}

- (void)drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)indirectBuffer
                          indirectBufferOffset:(NSUInteger)indirectBufferOffset
                   threadsPerObjectThreadgroup:(MTLSize)threadsPerObjectThreadgroup
                     threadsPerMeshThreadgroup:(MTLSize)threadsPerMeshThreadgroup {
    [self rdna4DrawMeshGroups:MTLSizeMake(0, 0, 0)
                     objLocal:threadsPerObjectThreadgroup
                    meshLocal:threadsPerMeshThreadgroup
                     indirect:YES indirectBuffer:indirectBuffer indirectOffset:indirectBufferOffset];
}

@end

#endif // RDNA4_MESH_IMPL_INC
