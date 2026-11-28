// garnet_rt_encoder.m - GarnetMetal modular compilation unit: AS command encoder and device/heap AS factories.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_rt_internal.h"

static uint32_t rdna4_rt_compacted_size_bytes(MTLDataType dt) {
    if (dt == MTLDataTypeUInt) return RDNA4RT_SIZE_TYPE_UINT32;
    if (dt == MTLDataTypeULong) return RDNA4RT_SIZE_TYPE_UINT64;
    return 0u;
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
                RDNA4LOG_WARN("accel pass: sampleBufferAttachments[%lu] invalid; refused", (unsigned long)k);
                _ended = YES;
                [super endEncoding];
                return nil;
            }
            NSUInteger count = ((NSUInteger (*)(id, SEL))objc_msgSend)(sb, @selector(sampleCount));
            if ((startIdx != MTLCounterDontSample && startIdx >= count) ||
                (endIdx != MTLCounterDontSample && endIdx >= count)) {
                RDNA4LOG_WARN("accel pass: sampleBufferAttachments[%lu] sample index out of range; refused", (unsigned long)k);
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
    NSMutableArray<GarnetASPrimData *> *tlasBlasPrimDatas = [NSMutableArray array];
    RDNA4RTBuildPlan plan;
    NSError *err = nil;
    if (!rdna4_rt_build_plan_make_ex(dev, descriptor, YES, YES, YES, NO, 0u, retained, tlasBlasPrimDatas, &plan, &err)) {
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
    const BOOL isCompQ = [_cb rdna4CurrentSegmentIsCompute];
    if (plan.kind == RDNA4RT_KIND_BLAS && plan.motionBlasDispatchCount > 0u) {
        VkDeviceAddress laAddr = [dstHandle rdna4EnsureLocalAabbAddress:&err];
        if (!laAddr || !dstHandle->_localAabbBuf) {
            rdna4_rt_plan_free(&plan);
            [_cb rdna4Fail:err.localizedDescription ?: @"buildAccelerationStructure: local AABB buffer failed"];
            return;
        }
        plan.localAabbBuf = dstHandle->_localAabbBuf->_buf;
        plan.localAabbAddr = laAddr;
        [_cb rdna4RetainEncoded:dstHandle->_localAabbBuf];
        for (uint32_t k = 0; k < plan.motionBlasDispatchCount; k++)
            plan.motionBlasPCs[k].blas_aabb_addr = (uint64_t)laAddr;
    }
    const BOOL needsBlasSidecar = (plan.kind == RDNA4RT_KIND_BLAS && (plan.hasCurves || plan.hasMotionGeoms));
    GarnetASPrimData *preparedCurvePd = nil;
    if (needsBlasSidecar) {
        if (!rdna4_rt_prepare_blas_sidecar(cmd, _cb, &plan, nil, YES, NO, &preparedCurvePd, &err)) {
            rdna4_rt_plan_free(&plan);
            [_cb rdna4Fail:err.localizedDescription ?: @"buildAccelerationStructure: curve/motion sidecar preparation failed"];
            return;
        }
    }
    GarnetASPrimData *preparedMotionTlasPd = nil;
    if (plan.kind == RDNA4RT_KIND_TLAS && plan.hasMotionTlas && plan.tlasInstanceCount > 0u) {
        if (!rdna4_rt_prepare_motion_tlas_sidecar(cmd, _cb, &plan, tlasBlasPrimDatas, &preparedMotionTlasPd, &err)) {
            rdna4_rt_plan_free(&plan);
            [_cb rdna4Fail:err.localizedDescription ?: @"buildAccelerationStructure: motion TLAS sidecar preparation failed"];
            return;
        }
    }
    if (!rdna4_rt_dispatch_pre_build(cmd, &plan, isCompQ, &err)) {
        rdna4_rt_plan_free(&plan);
        [_cb rdna4Fail:err.localizedDescription ?: @"buildAccelerationStructure: pre-build compute conversion failed"];
        return;
    }
    RDNA4RTFn.cmdBuildAS(cmd, 1u, &buildInfo, &pRanges);
    BOOL primOk = YES;
    if (needsBlasSidecar) {
        primOk = [dstAS rdna4SetPrimData:preparedCurvePd forAsAddress:(uint64_t)dstHandle->_deviceAddress commandBuffer:_cb vkCmdBuffer:cmd error:&err];
    } else if (plan.kind == RDNA4RT_KIND_TLAS && plan.hasMotionTlas) {
        if (preparedMotionTlasPd && plan.tlasInstStgBuf && plan.tlasInstanceCount > 0u) {
            VkBufferCopy instSnapCopy = {
                .srcOffset = 0u,
                .dstOffset = 0u,
                .size = (uint64_t)plan.tlasInstanceCount * RDNA4RT_VK_INSTANCE_SIZE
            };
            vkCmdCopyBuffer(cmd, plan.tlasInstStgBuf, preparedMotionTlasPd->_buf, 1u, &instSnapCopy);
            primOk = [dstAS rdna4SetPrimData:preparedMotionTlasPd forAsAddress:(uint64_t)dstHandle->_deviceAddress commandBuffer:_cb vkCmdBuffer:cmd error:&err];
        } else {
            primOk = [dstAS rdna4SetPrimData:nil forAsAddress:(uint64_t)dstHandle->_deviceAddress commandBuffer:_cb vkCmdBuffer:cmd error:&err];
        }
    } else {
        primOk = (plan.kind == RDNA4RT_KIND_BLAS)
            ? rdna4_rt_record_blas_prim_snapshot(cmd, _cb, dstAS, (uint64_t)dstHandle->_deviceAddress, &plan, &err)
            : rdna4_rt_record_tlas_prim_snapshot(cmd, _cb, dstAS, (uint64_t)dstHandle->_deviceAddress, &plan, tlasBlasPrimDatas, &err);
    }
    if (!primOk) {
        rdna4_rt_plan_free(&plan);
        [_cb rdna4Fail:err.localizedDescription ?: @"buildAccelerationStructure: primitive data snapshot failed"];
        return;
    }
    rdna4_rt_emit_barrier(cmd, RDNA4RT_BARRIER_POST_BUILD, isCompQ);

    (void)rdna4rt_state_apply_build_ex(&dstMeta, plan.kind, plan.metalSizes.acceleration_structure_size,
                                       plan.usage, plan.geomCount, plan.topologyHash, plan.geomIftBase);
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

    if (options == 0u || ((uint64_t)options & ~(uint64_t)RDNA4RT_REFIT_OPT_KNOWN_MASK) != 0u) {
        [_cb rdna4Fail:@"refitAccelerationStructure: invalid refit options"];
        return;
    }
    const BOOL updVerts = rdna4rt_refit_updates_vertices((uint64_t)options) != 0;
    const BOOL updPrim = rdna4rt_refit_updates_prim_data((uint64_t)options) != 0;
    const BOOL primOnly = rdna4rt_refit_is_prim_data_only((uint64_t)options) != 0;

    GarnetAccelerationStructure *srcAS = (GarnetAccelerationStructure *)sourceAccelerationStructure;
    GarnetAccelerationStructure *dstAS = (GarnetAccelerationStructure *)effectiveDst;
    rdna4rt_accel_meta_t srcMeta = [srcAS rdna4MetaSnapshot];
    if (srcMeta.state != RDNA4RT_STATE_BUILT || !srcMeta.allow_refit) {
        [_cb rdna4Fail:@"refitAccelerationStructure: source acceleration structure was not built with MTLAccelerationStructureUsageRefit or was compacted"];
        return;
    }

    NSMutableArray *retained = [NSMutableArray array];
    NSMutableArray<GarnetASPrimData *> *tlasBlasPrimDatas = [NSMutableArray array];
    RDNA4RTBuildPlan plan;
    NSError *err = nil;
    if (!rdna4_rt_build_plan_make_ex(dev, descriptor, updVerts, updPrim, updPrim,
                                     YES, srcMeta.usage, retained, tlasBlasPrimDatas, &plan, &err)) {
        [_cb rdna4Fail:err.localizedDescription ?: @"refitAccelerationStructure: invalid descriptor"];
        return;
    }

    rdna4rt_accel_meta_t dstMeta = (dstAS == srcAS) ? srcMeta : [dstAS rdna4MetaSnapshot];
    const uint64_t reqAsSize = primOnly ? 0u : plan.metalSizes.acceleration_structure_size;
    const uint64_t checkTopo = primOnly ? 0u : plan.topologyHash;
    rdna4rt_err_t re = rdna4rt_state_can_refit(
        &srcMeta, &dstMeta, plan.kind, reqAsSize,
        plan.geomCount, checkTopo, (uint64_t)options);
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

    uint64_t scratchAddr = 0;
    if (updVerts) {
        const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
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

    VkCommandBuffer cmd = [_cb vk];
    const BOOL isCompQ = [_cb rdna4CurrentSegmentIsCompute];
    if (primOnly) {
        rdna4_rt_emit_barrier(cmd, RDNA4RT_BARRIER_PRE_BUILD, isCompQ);
        if (dstAS != srcAS) {
            VkCopyAccelerationStructureInfoKHR ci = {
                .sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
                .src = srcHandle->_vkAS,
                .dst = dstHandle->_vkAS,
                .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR
            };
            RDNA4RTFn.cmdCopyAS(cmd, &ci);
        }
        BOOL primOk = YES;
        const BOOL needsBlasSidecar = (plan.kind == RDNA4RT_KIND_BLAS && (plan.hasCurves || plan.hasMotionGeoms));
        if (needsBlasSidecar) {
            GarnetASPrimData *preparedPd = nil;
            primOk = rdna4_rt_clone_local_aabb(cmd, _cb, srcAS, dstAS, &err) &&
                     rdna4_rt_prepare_blas_sidecar(cmd, _cb, &plan, [srcAS rdna4CurrentPrimData], YES, YES, &preparedPd, &err) &&
                     [dstAS rdna4SetPrimData:preparedPd forAsAddress:(uint64_t)dstHandle->_deviceAddress commandBuffer:_cb vkCmdBuffer:cmd error:&err];
        } else if (plan.kind == RDNA4RT_KIND_BLAS) {
            primOk = rdna4_rt_clone_local_aabb(cmd, _cb, srcAS, dstAS, &err) &&
                     rdna4_rt_record_blas_prim_snapshot(cmd, _cb, dstAS, (uint64_t)dstHandle->_deviceAddress, &plan, &err);
        } else if (dstAS != srcAS) {
            primOk = rdna4_rt_record_prim_data_clone(cmd, _cb, srcAS, dstAS, (uint64_t)dstHandle->_deviceAddress, &err);
        } else {
            GarnetASPrimData *curPd = [srcAS rdna4CurrentPrimData];
            if (curPd) [_cb rdna4RetainEncoded:curPd];
        }
        if (!primOk) {
            rdna4_rt_plan_free(&plan);
            [_cb rdna4Fail:err.localizedDescription ?: @"refitAccelerationStructure: per-primitive data update failed"];
            return;
        }
        rdna4_rt_emit_barrier(cmd, RDNA4RT_BARRIER_POST_BUILD, isCompQ);
    } else {
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

        if (plan.kind == RDNA4RT_KIND_BLAS && plan.motionBlasDispatchCount > 0u) {
            VkDeviceAddress laAddr = [dstHandle rdna4EnsureLocalAabbAddress:&err];
            if (!laAddr || !dstHandle->_localAabbBuf) {
                rdna4_rt_plan_free(&plan);
                [_cb rdna4Fail:err.localizedDescription ?: @"refitAccelerationStructure: local AABB buffer failed"];
                return;
            }
            plan.localAabbBuf = dstHandle->_localAabbBuf->_buf;
            plan.localAabbAddr = laAddr;
            [_cb rdna4RetainEncoded:dstHandle->_localAabbBuf];
            for (uint32_t k = 0; k < plan.motionBlasDispatchCount; k++)
                plan.motionBlasPCs[k].blas_aabb_addr = (uint64_t)laAddr;
        }
        const BOOL needsBlasSidecar = (plan.kind == RDNA4RT_KIND_BLAS && (plan.hasCurves || plan.hasMotionGeoms));
        GarnetASPrimData *preparedCurvePd = nil;
        if (needsBlasSidecar) {
            if (!rdna4_rt_prepare_blas_sidecar(cmd, _cb, &plan, [srcAS rdna4CurrentPrimData], updPrim, NO, &preparedCurvePd, &err)) {
                rdna4_rt_plan_free(&plan);
                [_cb rdna4Fail:err.localizedDescription ?: @"refitAccelerationStructure: curve/motion sidecar preparation failed"];
                return;
            }
        }
        GarnetASPrimData *preparedMotionTlasPd = nil;
        if (plan.kind == RDNA4RT_KIND_TLAS && plan.hasMotionTlas && plan.tlasInstanceCount > 0u) {
            if (!rdna4_rt_prepare_motion_tlas_sidecar(cmd, _cb, &plan, tlasBlasPrimDatas, &preparedMotionTlasPd, &err)) {
                rdna4_rt_plan_free(&plan);
                [_cb rdna4Fail:err.localizedDescription ?: @"refitAccelerationStructure: motion TLAS sidecar preparation failed"];
                return;
            }
        }
        if (!rdna4_rt_dispatch_pre_build(cmd, &plan, isCompQ, &err)) {
            rdna4_rt_plan_free(&plan);
            [_cb rdna4Fail:err.localizedDescription ?: @"refitAccelerationStructure: pre-build compute conversion failed"];
            return;
        }
        RDNA4RTFn.cmdBuildAS(cmd, 1u, &buildInfo, &pRanges);

        BOOL primOk = YES;
        if (needsBlasSidecar) {
            primOk = [dstAS rdna4SetPrimData:preparedCurvePd forAsAddress:(uint64_t)dstHandle->_deviceAddress commandBuffer:_cb vkCmdBuffer:cmd error:&err];
        } else if (plan.kind == RDNA4RT_KIND_TLAS && plan.hasMotionTlas) {
            if (preparedMotionTlasPd && plan.tlasInstStgBuf && plan.tlasInstanceCount > 0u) {
                VkBufferCopy instSnapCopy = {
                    .srcOffset = 0u,
                    .dstOffset = 0u,
                    .size = (uint64_t)plan.tlasInstanceCount * RDNA4RT_VK_INSTANCE_SIZE
                };
                vkCmdCopyBuffer(cmd, plan.tlasInstStgBuf, preparedMotionTlasPd->_buf, 1u, &instSnapCopy);
                primOk = [dstAS rdna4SetPrimData:preparedMotionTlasPd forAsAddress:(uint64_t)dstHandle->_deviceAddress commandBuffer:_cb vkCmdBuffer:cmd error:&err];
            } else {
                primOk = [dstAS rdna4SetPrimData:nil forAsAddress:(uint64_t)dstHandle->_deviceAddress commandBuffer:_cb vkCmdBuffer:cmd error:&err];
            }
        } else if (plan.kind == RDNA4RT_KIND_TLAS) {
            primOk = rdna4_rt_record_tlas_prim_snapshot(cmd, _cb, dstAS, (uint64_t)dstHandle->_deviceAddress, &plan, tlasBlasPrimDatas, &err);
        } else if (updPrim) {
            primOk = rdna4_rt_record_blas_prim_snapshot(cmd, _cb, dstAS, (uint64_t)dstHandle->_deviceAddress, &plan, &err);
        } else if (dstAS != srcAS) {
            primOk = rdna4_rt_record_prim_data_clone(cmd, _cb, srcAS, dstAS, (uint64_t)dstHandle->_deviceAddress, &err);
        } else {
            GarnetASPrimData *curPd = [srcAS rdna4CurrentPrimData];
            if (curPd) [_cb rdna4RetainEncoded:curPd];
        }
        if (!primOk) {
            rdna4_rt_plan_free(&plan);
            [_cb rdna4Fail:err.localizedDescription ?: @"refitAccelerationStructure: primitive data update failed"];
            return;
        }
        rdna4_rt_emit_barrier(cmd, RDNA4RT_BARRIER_POST_BUILD, isCompQ);
    }

    (void)rdna4rt_state_apply_refit_ex(
        &srcMeta, &dstMeta, plan.kind, reqAsSize,
        plan.geomCount, checkTopo, (uint64_t)options, plan.geomIftBase);
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
    const BOOL isCompQ = [_cb rdna4CurrentSegmentIsCompute];
    rdna4_rt_emit_barrier(cmd, RDNA4RT_BARRIER_PRE_BUILD, isCompQ);
    RDNA4RTFn.cmdCopyAS(cmd, &ci);
    if (!rdna4_rt_record_prim_data_clone(cmd, _cb, srcAS, dstAS, (uint64_t)dstHandle->_deviceAddress, &err)) {
        [_cb rdna4Fail:err.localizedDescription ?: @"copyAccelerationStructure: primitive data clone failed"];
        return;
    }
    rdna4_rt_emit_barrier(cmd, RDNA4RT_BARRIER_POST_BUILD, isCompQ);

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
    const BOOL isCompQ = [_cb rdna4CurrentSegmentIsCompute];
    rdna4_rt_emit_barrier(cmd, RDNA4RT_BARRIER_PRE_BUILD, isCompQ);
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
    rdna4_rt_emit_barrier(cmd, RDNA4RT_BARRIER_TRANSFER_TO_HOST, isCompQ);

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
    const BOOL isCompQ = [_cb rdna4CurrentSegmentIsCompute];
    rdna4_rt_emit_barrier(cmd, RDNA4RT_BARRIER_PRE_BUILD, isCompQ);
    RDNA4RTFn.cmdCopyAS(cmd, &ci);
    if (!rdna4_rt_record_prim_data_clone(cmd, _cb, srcAS, dstAS, (uint64_t)dstHandle->_deviceAddress, &err)) {
        [_cb rdna4Fail:err.localizedDescription ?: @"copyAndCompactAccelerationStructure: primitive data clone failed"];
        return;
    }
    rdna4_rt_emit_barrier(cmd, RDNA4RT_BARRIER_POST_BUILD, isCompQ);

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
        GarnetASPrimData *pd = [(GarnetAccelerationStructure *)resource rdna4CurrentPrimData];
        if (pd) [_cb rdna4RetainEncoded:pd];
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
    const BOOL isCompQ = [_cb rdna4CurrentSegmentIsCompute];
    if (_nCsbEnd) {
        VkCommandBuffer cmd = [_cb vk];
        rdna4_rt_emit_barrier(cmd, RDNA4RT_BARRIER_POST_BUILD, isCompQ);
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
        rdna4_rt_emit_barrier([_cb vk], RDNA4RT_BARRIER_POST_BUILD, isCompQ);
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
        RDNA4LOG_ERR("accelerationStructureSizesWithDescriptor: RADV open failed: %s",
                     err.localizedDescription.UTF8String);
        return zero;
    }
    RDNA4RTBuildPlan plan;
    if (!rdna4_rt_build_plan_make(dev, descriptor, NO, NO, 0u, nil, &plan, &err)) {
        RDNA4LOG_WARN("accelerationStructureSizesWithDescriptor: refused: %s",
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
    if (!as) RDNA4LOG_ERR("newAccelerationStructureWithSize:%lu failed: %s",
                          (unsigned long)size, err.localizedDescription.UTF8String);
    return (id<MTLAccelerationStructure>)as;
}

GARNET_INTERNAL __attribute__((unused)) id<MTLAccelerationStructure> rdna4_rt_device_new_with_descriptor(
    id dev, MTLAccelerationStructureDescriptor *descriptor) {
    if (!dev || !descriptor) return nil;
    NSError *err = nil;
    if (!rdna4_radv_open(&err)) {
        RDNA4LOG_ERR("newAccelerationStructureWithDescriptor: RADV open failed: %s",
                     err.localizedDescription.UTF8String);
        return nil;
    }
    RDNA4RTBuildPlan plan;
    if (!rdna4_rt_build_plan_make(dev, descriptor, NO, NO, 0u, nil, &plan, &err)) {
        RDNA4LOG_WARN("newAccelerationStructureWithDescriptor: refused: %s",
                      err.localizedDescription.UTF8String);
        return nil;
    }
    rdna4rt_kind_t kind = plan.kind;
    NSUInteger reqSize = (NSUInteger)plan.metalSizes.acceleration_structure_size;
    rdna4_rt_plan_free(&plan);
    GarnetAccelerationStructure *as = [[GarnetAccelerationStructure alloc]
        initWithDevice:dev size:reqSize initialKind:kind error:&err];
    if (!as) RDNA4LOG_ERR("newAccelerationStructureWithDescriptor: alloc %lu failed: %s",
                          (unsigned long)reqSize, err.localizedDescription.UTF8String);
    return (id<MTLAccelerationStructure>)as;
}

GARNET_INTERNAL __attribute__((unused)) MTLSizeAndAlign rdna4_rt_device_heap_as_size_align_with_size(
    id dev, NSUInteger size) {
    MTLSizeAndAlign zero = { 0, 0 };
    if (!dev || size == 0) return zero;
    NSError *err = nil;
    if (!rdna4_radv_open(&err)) {
        RDNA4LOG_ERR("heapAccelerationStructureSizeAndAlignWithSize: RADV open failed: %s",
                     err.localizedDescription.UTF8String);
        return zero;
    }
    if (!rdna4_rt_resolve_funcs() || !RDNA4P.bmr2) {
        RDNA4LOG_WARN("heapAccelerationStructureSizeAndAlignWithSize: acceleration structure or requirements2 unavailable");
        return zero;
    }
    uint64_t backingSize = 0u;
    if (!rdna4rt_align_up_u64((uint64_t)size, RDNA4RT_HEAP_AS_ALIGN, &backingSize)) return zero;
    rdna4dc_caps caps = rdna4_device_caps();
    if (!rdna4dc_buffer_ok(&caps, backingSize)) return zero;
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = backingSize,
        .usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
    };
    VkBuffer tmpBuf = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(RDNA4R.dev, &bci, NULL, &tmpBuf);
    if (r != VK_SUCCESS || !tmpBuf) return zero;
    VkMemoryRequirements mr;
    BOOL queried = rdna4_requirements2(tmpBuf, VK_NULL_HANDLE, &mr, NULL, NULL, YES);
    vkDestroyBuffer(RDNA4R.dev, tmpBuf, NULL);
    if (!queried) return zero;
    uint64_t gran = RDNA4R.lim.bufferImageGranularity ? RDNA4R.lim.bufferImageGranularity : 1u;
    rdna4rt_heap_size_align_t sa;
    if (rdna4rt_heap_as_size_align(backingSize, (uint64_t)mr.size, (uint64_t)mr.alignment, gran, &sa) != RDNA4RT_OK ||
        sa.size > NSUIntegerMax || sa.align > NSUIntegerMax)
        return zero;
    return (MTLSizeAndAlign){ .size = (NSUInteger)sa.size, .align = (NSUInteger)sa.align };
}

GARNET_INTERNAL __attribute__((unused)) MTLSizeAndAlign rdna4_rt_device_heap_as_size_align_with_descriptor(
    id dev, MTLAccelerationStructureDescriptor *descriptor) {
    MTLSizeAndAlign zero = { 0, 0 };
    if (!dev || !descriptor) return zero;
    MTLAccelerationStructureSizes sz = rdna4_rt_device_sizes(dev, descriptor);
    if (sz.accelerationStructureSize == 0) return zero;
    return rdna4_rt_device_heap_as_size_align_with_size(dev, sz.accelerationStructureSize);
}

GARNET_INTERNAL __attribute__((unused)) id<MTLAccelerationStructure> rdna4_rt_heap_new_as_with_size(
    RDNA4Heap *heap, id dev, NSUInteger size, BOOL hasOffset, NSUInteger offset) {
    if (!heap || !dev) return nil;
    uint64_t alignedSize = 0u;
    rdna4rt_err_t ve = rdna4rt_heap_as_validate_alloc(
        (uint32_t)[heap storageMode], (uint32_t)[heap type], (uint64_t)[heap size],
        (uint64_t)size, hasOffset ? 1 : 0, (uint64_t)offset, RDNA4RT_HEAP_AS_ALIGN, &alignedSize);
    if (ve != RDNA4RT_OK) {
        RDNA4LOG_WARN("heap newAccelerationStructureWithSize:%lu offset:%lu(has=%d) refused: %s",
                      (unsigned long)size, (unsigned long)offset, hasOffset ? 1 : 0, rdna4rt_err_str(ve));
        return nil;
    }
    NSError *err = nil;
    GarnetAccelerationStructure *as = [[GarnetAccelerationStructure alloc]
        initWithDevice:dev size:size initialKind:RDNA4RT_KIND_UNKNOWN
                  heap:heap offset:offset hasOffset:hasOffset error:&err];
    if (!as) {
        RDNA4LOG_ERR("heap newAccelerationStructureWithSize:%lu failed: %s",
                     (unsigned long)size, err ? err.localizedDescription.UTF8String : "unknown");
        return nil;
    }
    [heap rdna4TrackResource:as];
    return (id<MTLAccelerationStructure>)as;
}

GARNET_INTERNAL __attribute__((unused)) id<MTLAccelerationStructure> rdna4_rt_heap_new_as_with_descriptor(
    RDNA4Heap *heap, id dev, MTLAccelerationStructureDescriptor *descriptor, BOOL hasOffset, NSUInteger offset) {
    if (!heap || !dev || !descriptor) return nil;
    /* Pre-check storageMode and Automatic vs Placement heap gating before Vulkan size query. */
    if ([heap storageMode] != MTLStorageModePrivate) {
        RDNA4LOG_WARN("heap newAccelerationStructureWithDescriptor refused: heap must use MTLStorageModePrivate");
        return nil;
    }
    if ((hasOffset && [heap type] != MTLHeapTypePlacement) ||
        (!hasOffset && [heap type] != MTLHeapTypeAutomatic)) {
        RDNA4LOG_WARN("heap newAccelerationStructureWithDescriptor refused: heap type mismatch (hasOffset=%d, type=%lu)",
                      hasOffset ? 1 : 0, (unsigned long)[heap type]);
        return nil;
    }
    NSError *err = nil;
    if (!rdna4_radv_open(&err)) {
        RDNA4LOG_ERR("heap newAccelerationStructureWithDescriptor: RADV open failed: %s",
                     err.localizedDescription.UTF8String);
        return nil;
    }
    RDNA4RTBuildPlan plan;
    if (!rdna4_rt_build_plan_make(dev, descriptor, NO, NO, 0u, nil, &plan, &err)) {
        RDNA4LOG_WARN("heap newAccelerationStructureWithDescriptor: refused: %s",
                      err.localizedDescription.UTF8String);
        return nil;
    }
    rdna4rt_kind_t kind = plan.kind;
    NSUInteger reqSize = (NSUInteger)plan.metalSizes.acceleration_structure_size;
    rdna4_rt_plan_free(&plan);

    uint64_t alignedSize = 0u;
    rdna4rt_err_t ve = rdna4rt_heap_as_validate_alloc(
        (uint32_t)[heap storageMode], (uint32_t)[heap type], (uint64_t)[heap size],
        (uint64_t)reqSize, hasOffset ? 1 : 0, (uint64_t)offset, RDNA4RT_HEAP_AS_ALIGN, &alignedSize);
    if (ve != RDNA4RT_OK) {
        RDNA4LOG_WARN("heap newAccelerationStructureWithDescriptor:%lu offset:%lu(has=%d) refused: %s",
                      (unsigned long)reqSize, (unsigned long)offset, hasOffset ? 1 : 0, rdna4rt_err_str(ve));
        return nil;
    }
    GarnetAccelerationStructure *as = [[GarnetAccelerationStructure alloc]
        initWithDevice:dev size:reqSize initialKind:kind
                  heap:heap offset:offset hasOffset:hasOffset error:&err];
    if (!as) {
        RDNA4LOG_ERR("heap newAccelerationStructureWithDescriptor: alloc %lu failed: %s",
                     (unsigned long)reqSize, err ? err.localizedDescription.UTF8String : "unknown");
        return nil;
    }
    [heap rdna4TrackResource:as];
    return (id<MTLAccelerationStructure>)as;
}
