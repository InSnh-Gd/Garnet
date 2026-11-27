// garnet_rt_runtime.m - GarnetMetal modular compilation unit: ray tracing runtime, IFT, and capabilities.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif

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

// Builds honest ray tracing capabilities without opening RADV: pre-open reports RDNA4-guaranteed
// RT hardware caps as 1u (matching garnet_device.m pre-open policy so apps querying
// supportsRaytracing before newCommandQueue succeed with radv_open=0), while post-open reflects
// probed GFX12 Vulkan capabilities and always keeps stage3Ok gating.
GARNET_INTERNAL BOOL rdna4_rt_curves_enabled(void) {
    if (!rdna4_allow()) return NO;
    const char *v = garnet_getenv("GARNET_RT_CURVES", NULL);
    return (v && v[0] && v[0] != '0') ? YES : NO;
}

GARNET_INTERNAL BOOL rdna4_rt_motion_enabled(void) {
    if (!rdna4_allow()) return NO;
    const char *v = garnet_getenv("GARNET_RT_MOTION", NULL);
    return (v && v[0] && v[0] != '0') ? YES : NO;
}

__attribute__((unused)) static rdna4rtr_hw_caps_t rdna4_rt_runtime_caps(void) {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    const uint8_t vkRtOk = RDNA4R.ok ? (rdna4dc_metal3_raytracing_supported(&m3) ? 1u : 0u) : 1u;
    const uint8_t stage3Ok = rdna4_rt_runtime_translator_stage3_ready() ? 1u : 0u;
    const uint8_t curvesOk = rdna4_rt_curves_enabled() ? 1u : 0u;
    const uint8_t motionOk = rdna4_rt_motion_enabled() ? 1u : 0u;
    return (rdna4rtr_hw_caps_t){
        .accel_struct_supported    = vkRtOk,
        .ray_query_supported       = vkRtOk,
        .buffer_device_address     = vkRtOk,
        .deferred_host_ops         = vkRtOk,
        .translator_stage3_ready   = stage3Ok,
        .motion_blur_ext_supported = motionOk,
        .curve_geometry_supported  = curvesOk,
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

- (void)rdna4SyncSlotsLocked:(uint32_t)first count:(uint32_t)count {
    (void)rdna4rtr_ift_encode_slots(&_table, [_backing contents], [_backing length] / sizeof(uint32_t), first, count);
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

    uint32_t abiVer = (refl && refl->present) ? refl->abi_version : RDNA4RTR_IFT_ABI_VERSION_V2;
    rdna4rtr_status_t st = rdna4rtr_ift_init_with_abi(&_table, (uint32_t)count, maxHid, abiVer);
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

    // Backing Shared RDNA4Buffer holds ABI v1 or v2 header + handle_id[0..N-1] (>= 16 B aligned).
    size_t allocBytes = rdna4rtr_ift_aligned_buffer_bytes_for_abi((uint32_t)count, abiVer);
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
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable: rejected non-RDNA4FunctionHandle object");
        return NO;
    }
    RDNA4FunctionHandle *fh = (RDNA4FunctionHandle *)function;
    if ([fh functionType] != MTLFunctionTypeIntersection) {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable: rejected handle '%s' with non-intersection functionType %lu",
                      [fh name].UTF8String ?: "?", (unsigned long)[fh functionType]);
        return NO;
    }
    if ([fh device] != _dev || [fh rdna4OwnerToken] != _ownerToken || [fh rdna4Stage] != _stage) {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable: rejected handle '%s' from mismatched pipeline/stage/device",
                      [fh name].UTF8String ?: "?");
        return NO;
    }
    uint32_t hid = [fh rdna4HandleId];
    if (hid == RDNA4RTR_IFT_HANDLE_NULL || hid > _table.max_handle_id ||
        !_handleKinds || _handleKinds[hid] == 0xFFu) {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable: rejected invalid handle id %u (max %u)", hid, _table.max_handle_id);
        return NO;
    }
    *hidOut = hid;
    *kindOut = (uint32_t)_handleKinds[hid];
    return YES;
}

- (void)setFunction:(id<MTLFunctionHandle>)function atIndex:(NSUInteger)index {
    [_lock lock];
    if (index >= _table.slot_count) {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable setFunction:atIndex:%lu out of bounds (count=%u)",
                      (unsigned long)index, _table.slot_count);
        [_lock unlock];
        return;
    }
    uint32_t hid = RDNA4RTR_IFT_HANDLE_NULL;
    uint32_t kind = RDNA4RTR_IFT_KIND_BOUNDING_BOX;
    if ([self rdna4ValidateHandleLocked:function handleIdOut:&hid kindOut:&kind]) {
        if (rdna4rtr_ift_set_function(&_table, (uint32_t)index, hid, kind) == RDNA4RTR_OK)
            [self rdna4SyncSlotsLocked:(uint32_t)index count:1u];
    }
    [_lock unlock];
}

- (void)setFunctions:(const id<MTLFunctionHandle> __unsafe_unretained *)functions withRange:(NSRange)range {
    if (range.length == 0) return;
    if (!functions) {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable setFunctions:withRange: NULL array");
        return;
    }
    [_lock lock];
    if (range.location > _table.slot_count || range.length > _table.slot_count - range.location) {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable setFunctions:withRange:[%lu,+%lu) out of bounds (count=%u)",
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
        [self rdna4SyncSlotsLocked:(uint32_t)range.location count:(uint32_t)range.length];
    free(ids);
    free(kinds);
    [_lock unlock];
}

- (void)setOpaqueTriangleIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)signature
                                                   atIndex:(NSUInteger)index {
    [_lock lock];
    const int motionOk = rdna4_rt_motion_enabled() ? 1 : 0;
    rdna4rtr_status_t st = (index <= UINT32_MAX)
        ? rdna4rtr_ift_set_opaque_triangle_range_ex(&_table, (uint32_t)index, 1u, (uint64_t)signature, motionOk)
        : RDNA4RTR_ERR_BOUNDS;
    if (st == RDNA4RTR_OK) {
        (void)[self rdna4SyncBackingLocked];
    } else {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable setOpaqueTriangleIntersectionFunctionWithSignature:0x%lx atIndex:%lu rejected: %s",
                      (unsigned long)signature, (unsigned long)index, rdna4rtr_status_str(st));
    }
    [_lock unlock];
}

- (void)setOpaqueTriangleIntersectionFunctionWithSignature:(MTLIntersectionFunctionSignature)signature
                                                 withRange:(NSRange)range {
    if (range.length == 0) return;
    [_lock lock];
    const int motionOk = rdna4_rt_motion_enabled() ? 1 : 0;
    rdna4rtr_status_t st = (range.location <= UINT32_MAX && range.length <= UINT32_MAX)
        ? rdna4rtr_ift_set_opaque_triangle_range_ex(&_table, (uint32_t)range.location, (uint32_t)range.length, (uint64_t)signature, motionOk)
        : RDNA4RTR_ERR_BOUNDS;
    if (st == RDNA4RTR_OK) {
        (void)[self rdna4SyncBackingLocked];
    } else {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable setOpaqueTriangleIntersectionFunctionWithSignature:0x%lx withRange:[%lu,+%lu) rejected: %s",
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
    const int curveSupported = rdna4_rt_curves_enabled() ? 1 : 0;
    rdna4rtr_status_t st = (range.location <= UINT32_MAX && range.length <= UINT32_MAX)
        ? rdna4rtr_ift_set_opaque_curve_range_ex(&_table, (uint32_t)range.location, (uint32_t)range.length, (uint64_t)signature, curveSupported)
        : RDNA4RTR_ERR_BOUNDS;
    if (st == RDNA4RTR_OK) {
        [self rdna4SyncBackingLocked];
    } else if (st == RDNA4RTR_ERR_UNSUPPORTED) {
        // Safe handle_id = 0 has been encoded; sync so GPU never reads stale handles.
        (void)[self rdna4SyncBackingLocked];
        RDNA4_ONCE("RDNA4IntersectionFunctionTable: curve intersection functions disabled (set GARNET_RT_CURVES=1; encoded safe null handle)");
    } else {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable setOpaqueCurveIntersectionFunctionWithSignature:0x%lx withRange:[%lu,+%lu) rejected: %s",
                      (unsigned long)signature, (unsigned long)range.location, (unsigned long)range.length, rdna4rtr_status_str(st));
    }
    [_lock unlock];
}

- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index >= RDNA4RTR_MAX_ATTACH_BUFFERS) {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable setBuffer:atIndex:%lu out of bounds", (unsigned long)index);
        return;
    }
    if (buffer && (![buffer isKindOfClass:[RDNA4Buffer class]] || [(RDNA4Buffer *)buffer device] != _dev)) {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable setBuffer: invalid buffer class or device");
        return;
    }
    if (buffer && offset > [(RDNA4Buffer *)buffer length]) {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable setBuffer: offset %lu exceeds buffer length %lu",
                      (unsigned long)offset, (unsigned long)[(RDNA4Buffer *)buffer length]);
        return;
    }
    uint64_t bda = buffer ? ([(RDNA4Buffer *)buffer gpuAddress] + (uint64_t)offset) : 0u;
    [_lock lock];
    _attachedBuffers[index] = (RDNA4Buffer *)buffer;
    _attachedBufferOffsets[index] = buffer ? offset : 0u;
    _nestedVFTs[index] = nil;
    (void)rdna4rtr_ift_set_attached_bda(&_table, (uint32_t)index, bda);
    (void)[self rdna4SyncBackingLocked];
    [_lock unlock];
}

- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained *)buffers
           offsets:(const NSUInteger *)offsets
         withRange:(NSRange)range {
    if (range.length == 0) return;
    if (!buffers || !offsets ||
        range.location > RDNA4RTR_MAX_ATTACH_BUFFERS ||
        range.length > RDNA4RTR_MAX_ATTACH_BUFFERS - range.location) {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable setBuffers:offsets:withRange: invalid range or NULL array");
        return;
    }
    for (NSUInteger i = 0; i < range.length; i++)
        [self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}

- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)functionTable
                  atBufferIndex:(NSUInteger)bufferIndex {
    if (bufferIndex >= RDNA4RTR_MAX_NESTED_VFTS) {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable setVisibleFunctionTable:atBufferIndex:%lu out of bounds",
                      (unsigned long)bufferIndex);
        return;
    }
    if (functionTable &&
        (![functionTable isKindOfClass:[RDNA4VisibleFunctionTable class]] ||
         [(RDNA4VisibleFunctionTable *)functionTable device] != _dev)) {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable: rejected invalid nested VFT class or device");
        return;
    }
    uint64_t bda = functionTable ? [(RDNA4VisibleFunctionTable *)functionTable rdna4BackingGPUAddress] : 0u;
    [_lock lock];
    _nestedVFTs[bufferIndex] = functionTable;
    _attachedBuffers[bufferIndex] = nil;
    _attachedBufferOffsets[bufferIndex] = 0u;
    (void)rdna4rtr_ift_set_attached_bda(&_table, (uint32_t)bufferIndex, bda);
    (void)[self rdna4SyncBackingLocked];
    [_lock unlock];
}

- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained *)functionTables
                 withBufferRange:(NSRange)bufferRange {
    if (bufferRange.length == 0) return;
    if (!functionTables ||
        bufferRange.location > RDNA4RTR_MAX_NESTED_VFTS ||
        bufferRange.length > RDNA4RTR_MAX_NESTED_VFTS - bufferRange.location) {
        RDNA4LOG_WARN("RDNA4IntersectionFunctionTable setVisibleFunctionTables:withBufferRange: invalid range or NULL array");
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
// Bridge 3 (Metal 3 Bindless): return backing GPU BDA for gpuResourceID.
RDNA4_RESOURCE_SPI_CUSTOM_UID(_backing ? [_backing gpuAddress] : rdna4_resource_uid(&_uid))

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
    rtSlots->ift[bufferIndex] = nil;
    rtSlots->iftMask &= ~(1u << bufferIndex);
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
    id primData = [as rdna4CurrentPrimData];
    if (primData) rdna4_rt_cb_retain(cb, primData);
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
    if (!ift) {
        rtSlots->ift[bufferIndex] = nil;
        rtSlots->iftMask &= ~(1u << bufferIndex);
        return YES;
    }
    if (![ift isKindOfClass:[RDNA4IntersectionFunctionTable class]] ||
        [(RDNA4IntersectionFunctionTable *)ift device] != dev) {
        rdna4_rt_cb_fail(cb, [NSString stringWithFormat:@"%s setIntersectionFunctionTable: not an RDNA4IntersectionFunctionTable for this device",
                              stageName ?: "rt"]);
        return NO;
    }
    RDNA4IntersectionFunctionTable *tbl = (RDNA4IntersectionFunctionTable *)ift;
    rtSlots->ift[bufferIndex] = tbl;
    rtSlots->iftMask |= (1u << bufferIndex);
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
        id primData = [as rdna4CurrentPrimData];
        if (primData) rdna4_rt_cb_retain(cb, primData);
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
        id pd = as ? [as rdna4CurrentPrimData] : nil;
        if (pd) rdna4_rt_cb_retain(cb, pd);
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
// (matching RDNA4ArgumentEncoder semantics). Accepts both BLAS and TLAS handles in either
// MTLDataTypeInstanceAccelerationStructure (118) or MTLDataTypePrimitiveAccelerationStructure (117)
// slots (matching Apple Metal; e.g. Cycles encodes mesh BLASes into 1-slot InstanceAS encoders).
// If an AS created via `newAccelerationStructureWithSize:` has not yet materialized a
// VkAccelerationStructureKHR handle, materializes it for the field kind.
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
        RDNA4LOG_ERR("newIntersectionFunctionTableWithDescriptor failed: %s", e.localizedDescription.UTF8String);
    return (id<MTLIntersectionFunctionTable>)tbl;
}

#endif // RDNA4_RT_RUNTIME_IMPL_INC
