// garnet_rt_plan.m - GarnetMetal modular compilation unit: AS build/refit plans and BLAS/TLAS sidecars.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_rt_internal.h"

static VkDeviceAddress rdna4_rt_store_motion_keys(RDNA4RTStagingBuffer *bounds, uint64_t used,
        const uint64_t *addresses, uint32_t count, NSMutableArray *retained, NSError **err) {
    if (!bounds) return 0;
    RDNA4RTStagingBuffer *storage = bounds;
    uint64_t bytes = (uint64_t)count * sizeof(uint64_t);
    if (!rdna4rt_motion_table_fits(bounds->_size, used, count)) {
        storage = [[RDNA4RTStagingBuffer alloc] initWithSize:bytes error:err];
        if (!storage) return 0;
        [retained addObject:storage];
        used = 0;
    }
    memcpy((uint8_t *)storage->_map + used, addresses, (size_t)bytes);
    return storage->_addr + used;
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

static BOOL rdna4_rt_curve_cp_fmt_from_mtl(NSUInteger mtlFmt, rdna4rt_curve_cp_fmt_t *outFmt) {
    if (!outFmt) return NO;
    switch (mtlFmt) {
    case MTLAttributeFormatFloat2:  *outFmt = RDNA4RT_CURVE_CP_FLOAT2; return YES;
    case MTLAttributeFormatInvalid:
    case MTLAttributeFormatFloat3:  *outFmt = RDNA4RT_CURVE_CP_FLOAT3; return YES;
    case MTLAttributeFormatFloat4:  *outFmt = RDNA4RT_CURVE_CP_FLOAT4; return YES;
    case MTLAttributeFormatHalf2:   *outFmt = RDNA4RT_CURVE_CP_HALF2;  return YES;
    case MTLAttributeFormatHalf3:   *outFmt = RDNA4RT_CURVE_CP_HALF3;  return YES;
    case MTLAttributeFormatHalf4:   *outFmt = RDNA4RT_CURVE_CP_HALF4;  return YES;
    default: return NO;
    }
}

static BOOL rdna4_rt_curve_rad_fmt_from_mtl(NSUInteger mtlFmt, rdna4rt_curve_rad_fmt_t *outFmt) {
    if (!outFmt) return NO;
    switch (mtlFmt) {
    case MTLAttributeFormatInvalid:
    case MTLAttributeFormatFloat: *outFmt = RDNA4RT_CURVE_RAD_FLOAT; return YES;
    case MTLAttributeFormatHalf:  *outFmt = RDNA4RT_CURVE_RAD_HALF;  return YES;
    default: return NO;
    }
}

GARNET_INTERNAL void rdna4_rt_plan_free(RDNA4RTBuildPlan *p) {
    if (!p) return;
    if (p->geomCount > RDNA4RT_INLINE_GEOMS) {
        free(p->vkGeoms);
        free(p->vkRanges);
        free(p->maxPrimCounts);
        free(p->primPlans);
        free(p->primSrcVkBufs);
        free(p->primSrcVkOffs);
        free(p->curvePlans);
        free(p->motionPlans);
    }
    free(p->xformPCs);
    free(p->curvePCs);
    free(p->motionBlasPCs);
    free(p->motionBlasGeomIdx);
    p->vkGeoms = NULL;
    p->vkRanges = NULL;
    p->maxPrimCounts = NULL;
    p->primPlans = NULL;
    p->primSrcVkBufs = NULL;
    p->primSrcVkOffs = NULL;
    p->curvePlans = NULL;
    p->motionPlans = NULL;
    p->tlasInstStgBuf = VK_NULL_HANDLE;
    p->tlasInstanceCount = 0u;
    p->xformPCs = NULL;
    p->xformDispatchCount = 0u;
    p->curvePCs = NULL;
    p->curveDispatchCount = 0u;
    p->motionBlasPCs = NULL;
    p->motionBlasGeomIdx = NULL;
    p->motionBlasDispatchCount = 0u;
    p->localAabbBuf = VK_NULL_HANDLE;
    p->localAabbAddr = 0u;
    p->hasInstDispatch = NO;
    p->hasCurves = NO;
    p->hasMotionGeoms = NO;
    p->hasMotionTlas = NO;
    p->motionTlasXformVkBuf = VK_NULL_HANDLE;
    p->motionTlasXformVkOff = 0u;
    p->motionTlasXformCount = 0u;
}

static BOOL rdna4_rt_plan_append_xform(RDNA4RTBuildPlan *p, const rdna4rt_xform_transpose_pc_t *pc) {
    uint32_t next = p->xformDispatchCount + 1u;
    rdna4rt_xform_transpose_pc_t *arr = (rdna4rt_xform_transpose_pc_t *)realloc(
        p->xformPCs, (size_t)next * sizeof(*arr));
    if (!arr) return NO;
    arr[p->xformDispatchCount] = *pc;
    p->xformPCs = arr;
    p->xformDispatchCount = next;
    return YES;
}

static BOOL rdna4_rt_plan_append_curve(RDNA4RTBuildPlan *p, const rdna4rt_curve_build_pc_t *pc) {
    uint32_t next = p->curveDispatchCount + 1u;
    rdna4rt_curve_build_pc_t *arr = (rdna4rt_curve_build_pc_t *)realloc(
        p->curvePCs, (size_t)next * sizeof(*arr));
    if (!arr) return NO;
    arr[p->curveDispatchCount] = *pc;
    p->curvePCs = arr;
    p->curveDispatchCount = next;
    return YES;
}

static BOOL rdna4_rt_plan_append_motion_blas(
    RDNA4RTBuildPlan *p, const rdna4rt_motion_blas_build_pc_t *pc, uint32_t geomIdx) {
    uint32_t next = p->motionBlasDispatchCount + 1u;
    rdna4rt_motion_blas_build_pc_t *arr = (rdna4rt_motion_blas_build_pc_t *)realloc(
        p->motionBlasPCs, (size_t)next * sizeof(*arr));
    if (!arr) return NO;
    p->motionBlasPCs = arr;
    uint32_t *garr = (uint32_t *)realloc(p->motionBlasGeomIdx, (size_t)next * sizeof(*garr));
    if (!garr) return NO;
    p->motionBlasGeomIdx = garr;
    p->motionBlasPCs[p->motionBlasDispatchCount] = *pc;
    p->motionBlasGeomIdx[p->motionBlasDispatchCount] = geomIdx;
    p->motionBlasDispatchCount = next;
    return YES;
}

GARNET_INTERNAL BOOL rdna4_rt_dispatch_pre_build(
    VkCommandBuffer cmd, const RDNA4RTBuildPlan *plan, BOOL isComputeQueue, NSError **err) {
    if (plan->localAabbBuf != VK_NULL_HANDLE && RDNA4RTFn.cmdUpdateBuffer) {
        static const uint32_t emptyOrd[6] = {
            0xFF7FFFFFu, 0xFF7FFFFFu, 0xFF7FFFFFu,
            0x00800000u, 0x00800000u, 0x00800000u
        };
        RDNA4RTFn.cmdUpdateBuffer(cmd, plan->localAabbBuf, 0u, 24u, emptyOrd);
    }
    rdna4_rt_emit_barrier(cmd, RDNA4RT_BARRIER_PRE_BUILD, isComputeQueue);
    if (!plan->hasInstDispatch && plan->xformDispatchCount == 0u &&
        plan->curveDispatchCount == 0u && plan->motionBlasDispatchCount == 0u &&
        !plan->hasMotionTlas) {
        return YES;
    }
    if (plan->hasInstDispatch || plan->xformDispatchCount > 0u) {
        VkPipelineLayout instLayout = VK_NULL_HANDLE, xformLayout = VK_NULL_HANDLE;
        VkPipeline instPipe = VK_NULL_HANDLE, xformPipe = VK_NULL_HANDLE;
        if (!rdna4_rt_ensure_compute_pipes(&instLayout, &instPipe, &xformLayout, &xformPipe, err))
            return NO;
        if (plan->xformDispatchCount > 0u) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, xformPipe);
            for (uint32_t i = 0; i < plan->xformDispatchCount; i++) {
                vkCmdPushConstants(cmd, xformLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                                   (uint32_t)sizeof(plan->xformPCs[i]), &plan->xformPCs[i]);
                vkCmdDispatch(cmd, 1u, 1u, 1u);
            }
        }
        if (plan->hasInstDispatch && plan->instPC.max_count > 0u) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, instPipe);
            vkCmdPushConstants(cmd, instLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                               (uint32_t)sizeof(plan->instPC), &plan->instPC);
            uint32_t groups = (plan->instPC.max_count + 63u) / 64u;
            vkCmdDispatch(cmd, groups, 1u, 1u);
        }
    }
    if (plan->curveDispatchCount > 0u) {
        VkPipelineLayout curveLayout = VK_NULL_HANDLE;
        VkPipeline curvePipe = VK_NULL_HANDLE;
        if (!rdna4_rt_ensure_curve_pipe(&curveLayout, &curvePipe, err))
            return NO;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, curvePipe);
        for (uint32_t i = 0; i < plan->curveDispatchCount; i++) {
            uint32_t sub = (plan->curvePCs[i].packed_flags >> 20u) & 0xFu;
            if (!sub) sub = 1u;
            uint64_t n = (uint64_t)plan->curvePCs[i].segment_count * (uint64_t)sub;
            if (!n) continue;
            vkCmdPushConstants(cmd, curveLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                               (uint32_t)sizeof(plan->curvePCs[i]), &plan->curvePCs[i]);
            uint32_t groups = (uint32_t)((n + 63u) / 64u);
            vkCmdDispatch(cmd, groups, 1u, 1u);
        }
    }
    if ((plan->xformDispatchCount > 0u || plan->curveDispatchCount > 0u) &&
        plan->motionBlasDispatchCount > 0u) {
        rdna4_rt_emit_barrier(cmd, RDNA4RT_BARRIER_PRE_BUILD, isComputeQueue);
    }
    if (plan->motionBlasDispatchCount > 0u || plan->hasMotionTlas) {
        VkPipelineLayout mBlasLayout = VK_NULL_HANDLE, mTlasLayout = VK_NULL_HANDLE;
        VkPipeline mBlasPipe = VK_NULL_HANDLE, mTlasPipe = VK_NULL_HANDLE;
        if (!rdna4_rt_ensure_motion_pipes(&mBlasLayout, &mBlasPipe, &mTlasLayout, &mTlasPipe, err))
            return NO;
        if (plan->motionBlasDispatchCount > 0u) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mBlasPipe);
            for (uint32_t i = 0; i < plan->motionBlasDispatchCount; i++) {
                uint32_t n = plan->motionBlasPCs[i].prim_count;
                if (!n) continue;
                vkCmdPushConstants(cmd, mBlasLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                                   (uint32_t)sizeof(plan->motionBlasPCs[i]), &plan->motionBlasPCs[i]);
                uint32_t groups = (n + 63u) / 64u;
                vkCmdDispatch(cmd, groups, 1u, 1u);
            }
        }
        if (plan->hasMotionTlas && plan->motionTlasPC.max_count > 0u) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTlasPipe);
            vkCmdPushConstants(cmd, mTlasLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                               (uint32_t)sizeof(plan->motionTlasPC), &plan->motionTlasPC);
            uint32_t groups = (plan->motionTlasPC.max_count + 63u) / 64u;
            vkCmdDispatch(cmd, groups, 1u, 1u);
        }
    }
    rdna4_rt_emit_barrier(cmd, RDNA4RT_BARRIER_COMP_TO_BUILD, isComputeQueue);
    return YES;
}

GARNET_INTERNAL BOOL rdna4_rt_check_buffer(id obj, id dev, BOOL writable, RDNA4Buffer **outBuf, NSString **why) {
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

static BOOL rdna4_rt_extract_prim_data_geom(
    id gd, id dev, uint32_t primCount, BOOL collectPrimBuffers,
    NSMutableArray *retainedObjects,
    rdna4rt_prim_data_geom_plan_t *outPlan,
    VkBuffer *outVkBuf, uint64_t *outVkOff,
    NSError **err)
{
    memset(outPlan, 0, sizeof(*outPlan));
    *outVkBuf = VK_NULL_HANDLE;
    *outVkOff = 0u;
    SEL bufSel = sel_registerName("primitiveDataBuffer");
    if (![gd respondsToSelector:bufSel]) return YES;
    id rawPbuf = ((id (*)(id, SEL))objc_msgSend)(gd, bufSel);
    if (!rawPbuf) return YES;
    RDNA4Buffer *pbuf = nil;
    NSString *bwhy = nil;
    if (!rdna4_rt_check_buffer(rawPbuf, dev, NO, &pbuf, &bwhy)) {
        if (err) *err = rdna4_err(145, bwhy ?: @"invalid primitiveDataBuffer");
        return NO;
    }
    NSUInteger pOff = 0, pElemSz = 0, pStride = 0;
    SEL offSel = sel_registerName("primitiveDataBufferOffset");
    SEL elemSel = sel_registerName("primitiveDataElementSize");
    SEL strideSel = sel_registerName("primitiveDataStride");
    if ([gd respondsToSelector:offSel])
        pOff = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, offSel);
    if ([gd respondsToSelector:elemSel])
        pElemSz = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, elemSel);
    if ([gd respondsToSelector:strideSel])
        pStride = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, strideSel);
    rdna4rt_prim_data_geom_desc_t pdesc = {
        .has_buf = 1,
        .buf_len = (uint64_t)[pbuf length],
        .buf_offset = (uint64_t)pOff,
        .element_size = (uint64_t)pElemSz,
        .stride = (uint64_t)pStride,
        .prim_count = (uint64_t)primCount
    };
    rdna4rt_err_t pe = rdna4rt_validate_prim_data_geom(&pdesc, collectPrimBuffers ? 1 : 0, outPlan);
    if (pe != RDNA4RT_OK) {
        if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"primitiveDataBuffer: %s",
                                        rdna4rt_err_str(pe)]);
        return NO;
    }
    if (collectPrimBuffers && outPlan->present && outPlan->copy_bytes > 0u) {
        VkDeviceSize srcVkOff = 0u;
        if (__builtin_add_overflow([pbuf rdna4VkOffset], (VkDeviceSize)outPlan->src_offset, &srcVkOff)) {
            if (err) *err = rdna4_err(145, @"primitiveDataBuffer offset overflow");
            return NO;
        }
        *outVkBuf = [pbuf vkBuffer];
        *outVkOff = (uint64_t)srcVkOff;
        if (retainedObjects) [retainedObjects addObject:pbuf];
    }
    return YES;
}

GARNET_INTERNAL BOOL rdna4_rt_build_plan_make_ex(
    id dev, MTLAccelerationStructureDescriptor *desc,
    BOOL requireBuffers, BOOL validatePrimData, BOOL collectPrimBuffers,
    BOOL useOverrideUsage, uint64_t overrideUsage,
    NSMutableArray *retainedObjects,
    NSMutableArray<GarnetASPrimData *> *tlasBlasPrimDatas,
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
    const int allowMotion = rdna4_rt_motion_enabled() ? 1 : 0;
    const int allowCurves = rdna4_rt_curves_enabled() ? 1 : 0;

    Class indirectInstCls = objc_getClass("MTLIndirectInstanceAccelerationStructureDescriptor");

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
        uint32_t inlineSigs[RDNA4RT_INLINE_GEOMS];
        uint32_t *geomSigs = NULL;
        if (gcount > 0 && gcount <= RDNA4RT_INLINE_GEOMS) {
            plan->vkGeoms = plan->small.vkGeoms;
            plan->vkRanges = plan->small.vkRanges;
            plan->maxPrimCounts = plan->small.maxPrimCounts;
            plan->primPlans = plan->small.primPlans;
            plan->primSrcVkBufs = plan->small.primSrcVkBufs;
            plan->primSrcVkOffs = plan->small.primSrcVkOffs;
            plan->curvePlans = plan->small.curvePlans;
            plan->motionPlans = plan->small.motionPlans;
        } else if (gcount > RDNA4RT_INLINE_GEOMS) {
            plan->vkGeoms = (VkAccelerationStructureGeometryKHR *)calloc(gcount, sizeof(*plan->vkGeoms));
            plan->vkRanges = (VkAccelerationStructureBuildRangeInfoKHR *)calloc(gcount, sizeof(*plan->vkRanges));
            plan->maxPrimCounts = (uint32_t *)calloc(gcount, sizeof(*plan->maxPrimCounts));
            plan->primPlans = (rdna4rt_prim_data_geom_plan_t *)calloc(gcount, sizeof(*plan->primPlans));
            plan->primSrcVkBufs = (VkBuffer *)calloc(gcount, sizeof(*plan->primSrcVkBufs));
            plan->primSrcVkOffs = (uint64_t *)calloc(gcount, sizeof(*plan->primSrcVkOffs));
            plan->curvePlans = (RDNA4RTCurvePlanEntry *)calloc(gcount, sizeof(*plan->curvePlans));
            plan->motionPlans = (RDNA4RTMotionGeomPlanEntry *)calloc(gcount, sizeof(*plan->motionPlans));
            geomSigs = (uint32_t *)calloc(gcount, sizeof(*geomSigs));
            if (!plan->vkGeoms || !plan->vkRanges || !plan->maxPrimCounts ||
                !plan->primPlans || !plan->primSrcVkBufs || !plan->primSrcVkOffs ||
                !plan->curvePlans || !plan->motionPlans || !geomSigs) {
                free(geomSigs);
                rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, @"out of host memory building BLAS plan");
                return NO;
            }
        }

        uint32_t *signatures = geomSigs ? geomSigs : inlineSigs;
        RDNA4RTStagingBuffer *transformSlab = nil;
        uint64_t nextTransform = 0u;
        Class motionTriCls = objc_getClass("MTLAccelerationStructureMotionTriangleGeometryDescriptor");
        Class motionBoxCls = objc_getClass("MTLAccelerationStructureMotionBoundingBoxGeometryDescriptor");
        Class curveCls = objc_getClass("MTLAccelerationStructureCurveGeometryDescriptor");
        int hasTriangles = 0;
        rdna4rt_geom_ift_acc_t geomIftAcc = { 0u, 0u, 0 };

        for (NSUInteger i = 0; i < gcount; i++) {
            MTLAccelerationStructureGeometryDescriptor *gd = geoms[i];
            if (!gd) {
                free(geomSigs); rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, @"nil geometry descriptor in BLAS");
                return NO;
            }
            NSUInteger geomIftOff = [gd respondsToSelector:@selector(intersectionFunctionTableOffset)]
                ? gd.intersectionFunctionTableOffset : 0u;
            rdna4rt_err_t iftErr = rdna4rt_geom_ift_acc_add(&geomIftAcc, (uint64_t)geomIftOff);
            if (iftErr != RDNA4RT_OK) {
                free(geomSigs); rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"geometry[%lu] intersectionFunctionTableOffset: %s",
                                                (unsigned long)i, rdna4rt_err_str(iftErr)]);
                return NO;
            }
            if (motionTriCls && [gd isKindOfClass:motionTriCls]) {
                NSArray *kfArray = [gd respondsToSelector:sel_registerName("vertexBuffers")]
                    ? ((id (*)(id, SEL))objc_msgSend)(gd, sel_registerName("vertexBuffers")) : nil;
                uint32_t kfCount = (uint32_t)MIN(kfArray.count, (NSUInteger)UINT32_MAX);
                rdna4rt_motion_params_t mp = {
                    .is_motion = 1,
                    .keyframe_count = motionKeyframes > 1 ? (uint32_t)MIN(motionKeyframes, (NSUInteger)UINT32_MAX) : kfCount,
                    .keyframe_buffer_count = kfCount,
                    .start_border_mode = startBorder,
                    .end_border_mode = endBorder,
                    .start_time = startTime,
                    .end_time = endTime
                };
                rdna4rt_err_t me = rdna4rt_validate_motion_params(&mp, allowMotion);
                if (me != RDNA4RT_OK || (motionKeyframes > 1 && kfArray.count != motionKeyframes)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithUTF8String:rdna4rt_err_str(me != RDNA4RT_OK ? me : RDNA4RT_ERR_INVALID_ARG)]);
                    return NO;
                }

                id rawIbuf = [gd respondsToSelector:sel_registerName("indexBuffer")]
                    ? ((id (*)(id, SEL))objc_msgSend)(gd, sel_registerName("indexBuffer")) : nil;
                NSUInteger idxOff = [gd respondsToSelector:sel_registerName("indexBufferOffset")]
                    ? ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("indexBufferOffset")) : 0u;
                NSUInteger idxMtlType = [gd respondsToSelector:sel_registerName("indexType")]
                    ? ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("indexType")) : 0u;
                NSUInteger triCount = [gd respondsToSelector:sel_registerName("triangleCount")]
                    ? ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("triangleCount")) : 0u;
                NSUInteger vStride = [gd respondsToSelector:sel_registerName("vertexStride")]
                    ? ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("vertexStride")) : 0u;
                NSUInteger mtlVfmt = MTLAttributeFormatFloat3;
                if ([gd respondsToSelector:sel_registerName("vertexFormat")])
                    mtlVfmt = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("vertexFormat"));
                rdna4rt_vfmt_t pureVfmt = RDNA4RT_VFMT_FLOAT3;
                VkFormat vkVfmt = VK_FORMAT_R32G32B32_SFLOAT;
                if (!rdna4_rt_vfmt_from_mtl(mtlVfmt, &pureVfmt, &vkVfmt) ||
                    !rdna4_rt_vk_vfmt_supported(vkVfmt)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"unsupported motion triangle vertexFormat %lu", (unsigned long)mtlVfmt]);
                    return NO;
                }

                RDNA4Buffer *ibuf = nil, *tbuf = nil;
                NSString *bwhy = nil;
                if (!rdna4_rt_check_buffer(rawIbuf, dev, NO, &ibuf, &bwhy)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, bwhy ?: @"invalid motion triangle index buffer");
                    return NO;
                }
                id rawTbuf = [gd respondsToSelector:sel_registerName("transformationMatrixBuffer")]
                    ? ((id (*)(id, SEL))objc_msgSend)(gd, sel_registerName("transformationMatrixBuffer")) : nil;
                NSUInteger tbufOff = [gd respondsToSelector:sel_registerName("transformationMatrixBufferOffset")]
                    ? ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("transformationMatrixBufferOffset")) : 0u;
                NSUInteger tlayout = RDNA4RT_MATRIX_COLUMN_MAJOR;
                if ([gd respondsToSelector:sel_registerName("transformationMatrixLayout")])
                    tlayout = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("transformationMatrixLayout"));
                if (!rdna4_rt_check_buffer(rawTbuf, dev, NO, &tbuf, &bwhy)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, bwhy ?: @"invalid motion triangle transform buffer");
                    return NO;
                }

                rdna4rt_index_type_t idxType = RDNA4RT_INDEX_NONE;
                if (ibuf) {
                    if (idxMtlType == MTLIndexTypeUInt16) idxType = RDNA4RT_INDEX_UINT16;
                    else if (idxMtlType == MTLIndexTypeUInt32) idxType = RDNA4RT_INDEX_UINT32;
                    else {
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        if (err) *err = rdna4_err(145, @"unsupported motion triangle indexType");
                        return NO;
                    }
                }

                int *hasVbs = kfCount ? (int *)calloc(kfCount, sizeof(*hasVbs)) : NULL;
                uint64_t *vbAddrs = kfCount ? (uint64_t *)calloc(kfCount, sizeof(*vbAddrs)) : NULL;
                uint64_t *vbLens = kfCount ? (uint64_t *)calloc(kfCount, sizeof(*vbLens)) : NULL;
                uint64_t *vbOffs = kfCount ? (uint64_t *)calloc(kfCount, sizeof(*vbOffs)) : NULL;
                uint64_t *kfEffAddrs = kfCount ? (uint64_t *)calloc(kfCount, sizeof(*kfEffAddrs)) : NULL;
                if (kfCount && (!hasVbs || !vbAddrs || !vbLens || !vbOffs || !kfEffAddrs)) {
                    free(hasVbs); free(vbAddrs); free(vbLens); free(vbOffs); free(kfEffAddrs);
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, @"out of host memory validating motion triangle keyframes");
                    return NO;
                }
                for (uint32_t k = 0u; k < kfCount; k++) {
                    id kfItem = kfArray[k];
                    id rawKfBuf = [kfItem respondsToSelector:sel_registerName("buffer")]
                        ? ((id (*)(id, SEL))objc_msgSend)(kfItem, sel_registerName("buffer")) : nil;
                    NSUInteger kfOff = [kfItem respondsToSelector:sel_registerName("offset")]
                        ? ((NSUInteger (*)(id, SEL))objc_msgSend)(kfItem, sel_registerName("offset")) : 0u;
                    RDNA4Buffer *kfBuf = nil;
                    if (!rdna4_rt_check_buffer(rawKfBuf, dev, NO, &kfBuf, &bwhy)) {
                        free(hasVbs); free(vbAddrs); free(vbLens); free(vbOffs); free(kfEffAddrs);
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        if (err) *err = rdna4_err(145, bwhy ?: @"invalid motion triangle keyframe vertex buffer");
                        return NO;
                    }
                    hasVbs[k] = kfBuf != nil ? 1 : 0;
                    vbAddrs[k] = kfBuf ? [kfBuf gpuAddress] : 0u;
                    vbLens[k] = kfBuf ? (uint64_t)[kfBuf length] : 0u;
                    vbOffs[k] = (uint64_t)kfOff;
                    kfEffAddrs[k] = vbAddrs[k] + vbOffs[k];
                    if (requireBuffers && kfBuf) [retainedObjects addObject:kfBuf];
                }

                rdna4rt_motion_tri_geom_desc_t mtd = {
                    .vertex_format = pureVfmt,
                    .keyframe_count = kfCount,
                    .has_vertex_bufs = hasVbs,
                    .vertex_buf_gpu_addrs = vbAddrs,
                    .vertex_buf_lens = vbLens,
                    .vertex_buf_offsets = vbOffs,
                    .vertex_stride = (uint64_t)vStride,
                    .has_index_buf = ibuf != nil ? 1 : 0,
                    .index_type = idxType,
                    .index_buf_gpu_addr = ibuf ? [ibuf gpuAddress] : 0u,
                    .index_buf_len = ibuf ? (uint64_t)[ibuf length] : 0u,
                    .index_buf_offset = (uint64_t)idxOff,
                    .triangle_count = (uint64_t)triCount,
                    .has_transform_buf = tbuf != nil ? 1 : 0,
                    .transform_buf_gpu_addr = tbuf ? [tbuf gpuAddress] : 0u,
                    .transform_buf_len = tbuf ? (uint64_t)[tbuf length] : 0u,
                    .transform_buf_offset = (uint64_t)tbufOff,
                    .transform_layout = (rdna4rt_matrix_layout_t)tlayout,
                    .opaque = gd.opaque ? 1 : 0,
                    .allow_duplicate_any_hit = gd.allowDuplicateIntersectionFunctionInvocation ? 1 : 0
                };
                rdna4rt_motion_tri_plan_t mtplan;
                rdna4rt_err_t mte = rdna4rt_validate_motion_tri_geom(&mtd, requireBuffers ? 1 : 0, allowMotion, &m3, &mtplan);
                free(hasVbs); free(vbAddrs); free(vbLens); free(vbOffs);
                if (mte != RDNA4RT_OK) {
                    free(kfEffAddrs);
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"motion triangle geometry[%lu]: %s", (unsigned long)i, rdna4rt_err_str(mte)]);
                    return NO;
                }
                if (validatePrimData) {
                    if (!rdna4_rt_extract_prim_data_geom(gd, dev, mtplan.primitive_count, collectPrimBuffers,
                                                         retainedObjects, &plan->primPlans[i],
                                                         &plan->primSrcVkBufs[i], &plan->primSrcVkOffs[i], err)) {
                        free(kfEffAddrs);
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        return NO;
                    }
                }
                if (requireBuffers) {
                    if (ibuf) [retainedObjects addObject:ibuf];
                    if (tbuf) [retainedObjects addObject:tbuf];
                }

                plan->hasMotionGeoms = YES;
                plan->motionPlans[i] = (RDNA4RTMotionGeomPlanEntry){
                    .isMotion = 1,
                    .geomKind = RDNA4RT_MOTION_GEOM_TRIANGLES,
                    .keyframeCount = mtplan.keyframe_count,
                    .primCount = mtplan.primitive_count,
                    .startBorder = startBorder,
                    .endBorder = endBorder,
                    .startTime = startTime,
                    .endTime = endTime,
                    .primStride = RDNA4RT_MOTION_TRI_STRIDE,
                    .flags = gd.opaque ? 1u : 0u,
                    .payloadBytes = mtplan.payload_bytes
                };

                VkDeviceAddress aabbAddr = 0u;
                if (requireBuffers && mtplan.primitive_count > 0u) {
                    RDNA4RTStagingBuffer *aabbStg = [[RDNA4RTStagingBuffer alloc]
                        initWithSize:(uint64_t)mtplan.primitive_count * RDNA4RT_AABB_STRIDE_MIN error:err];
                    VkDeviceAddress keyTable = rdna4_rt_store_motion_keys(aabbStg,
                        (uint64_t)mtplan.primitive_count * RDNA4RT_AABB_STRIDE_MIN,
                        kfEffAddrs, kfCount, retainedObjects, err);
                    if (!keyTable) {
                        free(kfEffAddrs);
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        return NO;
                    }
                    [retainedObjects addObject:aabbStg];
                    aabbAddr = aabbStg->_addr;
                    rdna4rt_motion_blas_build_pc_t mpc = {
                        .kf_addrs_bda = keyTable,
                        .idx_addr = mtplan.index_addr,
                        .xform_addr = mtplan.transform_addr,
                        .aabb_dst_addr = aabbAddr,
                        .payload_dst_addr = 0u,
                        .blas_aabb_addr = 0u,
                        .prim_count = mtplan.primitive_count,
                        .keyframe_count = mtplan.keyframe_count,
                        .src_stride = (uint32_t)mtplan.vertex_stride,
                        .packed_flags = rdna4rt_motion_blas_pack_flags(
                            (uint32_t)pureVfmt, (uint32_t)mtplan.effective_index_type, 0u,
                            (tbuf && mtd.transform_layout == RDNA4RT_MATRIX_ROW_MAJOR) ? 1u : 0u)
                    };
                    if (!rdna4_rt_plan_append_motion_blas(plan, &mpc, (uint32_t)i)) {
                        free(kfEffAddrs);
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        if (err) *err = rdna4_err(145, @"out of host memory recording motion triangle dispatch");
                        return NO;
                    }
                }
                free(kfEffAddrs);

                plan->vkGeoms[i] = (VkAccelerationStructureGeometryKHR){
                    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                    .geometryType = VK_GEOMETRY_TYPE_AABBS_KHR,
                    .flags = mtplan.vk_geom_flags,
                    .geometry.aabbs = {
                        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
                        .data.deviceAddress = aabbAddr,
                        .stride = RDNA4RT_AABB_STRIDE_MIN
                    }
                };
                plan->vkRanges[i] = (VkAccelerationStructureBuildRangeInfoKHR){
                    .primitiveCount = mtplan.primitive_count,
                    .primitiveOffset = 0u,
                    .firstVertex = 0u,
                    .transformOffset = 0u
                };
                plan->maxPrimCounts[i] = mtplan.primitive_count;
                signatures[i] = rdna4rt_geom_sig(RDNA4RT_GEOM_MOTION_TRIANGLES, mtplan.effective_index_type, pureVfmt, mtplan.vk_geom_flags);
                if (!rdna4rt_add_u64(totalPrims, (uint64_t)mtplan.primitive_count, &totalPrims)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, @"total BLAS primitive count overflow");
                    return NO;
                }
            } else if (motionBoxCls && [gd isKindOfClass:motionBoxCls]) {
                NSArray *kfArray = [gd respondsToSelector:sel_registerName("boundingBoxBuffers")]
                    ? ((id (*)(id, SEL))objc_msgSend)(gd, sel_registerName("boundingBoxBuffers")) : nil;
                uint32_t kfCount = (uint32_t)MIN(kfArray.count, (NSUInteger)UINT32_MAX);
                rdna4rt_motion_params_t mp = {
                    .is_motion = 1,
                    .keyframe_count = motionKeyframes > 1 ? (uint32_t)MIN(motionKeyframes, (NSUInteger)UINT32_MAX) : kfCount,
                    .keyframe_buffer_count = kfCount,
                    .start_border_mode = startBorder,
                    .end_border_mode = endBorder,
                    .start_time = startTime,
                    .end_time = endTime
                };
                rdna4rt_err_t me = rdna4rt_validate_motion_params(&mp, allowMotion);
                if (me != RDNA4RT_OK || (motionKeyframes > 1 && kfArray.count != motionKeyframes)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithUTF8String:rdna4rt_err_str(me != RDNA4RT_OK ? me : RDNA4RT_ERR_INVALID_ARG)]);
                    return NO;
                }
                NSUInteger boxCount = [gd respondsToSelector:sel_registerName("boundingBoxCount")]
                    ? ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("boundingBoxCount")) : 0u;
                NSUInteger boxStride = [gd respondsToSelector:sel_registerName("boundingBoxStride")]
                    ? ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("boundingBoxStride")) : 0u;

                int *hasBbs = kfCount ? (int *)calloc(kfCount, sizeof(*hasBbs)) : NULL;
                uint64_t *bbAddrs = kfCount ? (uint64_t *)calloc(kfCount, sizeof(*bbAddrs)) : NULL;
                uint64_t *bbLens = kfCount ? (uint64_t *)calloc(kfCount, sizeof(*bbLens)) : NULL;
                uint64_t *bbOffs = kfCount ? (uint64_t *)calloc(kfCount, sizeof(*bbOffs)) : NULL;
                uint64_t *kfEffAddrs = kfCount ? (uint64_t *)calloc(kfCount, sizeof(*kfEffAddrs)) : NULL;
                if (kfCount && (!hasBbs || !bbAddrs || !bbLens || !bbOffs || !kfEffAddrs)) {
                    free(hasBbs); free(bbAddrs); free(bbLens); free(bbOffs); free(kfEffAddrs);
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, @"out of host memory validating motion bounding-box keyframes");
                    return NO;
                }
                NSString *bwhy = nil;
                for (uint32_t k = 0u; k < kfCount; k++) {
                    id kfItem = kfArray[k];
                    id rawKfBuf = [kfItem respondsToSelector:sel_registerName("buffer")]
                        ? ((id (*)(id, SEL))objc_msgSend)(kfItem, sel_registerName("buffer")) : nil;
                    NSUInteger kfOff = [kfItem respondsToSelector:sel_registerName("offset")]
                        ? ((NSUInteger (*)(id, SEL))objc_msgSend)(kfItem, sel_registerName("offset")) : 0u;
                    RDNA4Buffer *kfBuf = nil;
                    if (!rdna4_rt_check_buffer(rawKfBuf, dev, NO, &kfBuf, &bwhy)) {
                        free(hasBbs); free(bbAddrs); free(bbLens); free(bbOffs); free(kfEffAddrs);
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        if (err) *err = rdna4_err(145, bwhy ?: @"invalid motion bounding-box keyframe buffer");
                        return NO;
                    }
                    hasBbs[k] = kfBuf != nil ? 1 : 0;
                    bbAddrs[k] = kfBuf ? [kfBuf gpuAddress] : 0u;
                    bbLens[k] = kfBuf ? (uint64_t)[kfBuf length] : 0u;
                    bbOffs[k] = (uint64_t)kfOff;
                    kfEffAddrs[k] = bbAddrs[k] + bbOffs[k];
                    if (requireBuffers && kfBuf) [retainedObjects addObject:kfBuf];
                }

                rdna4rt_motion_aabb_geom_desc_t mad = {
                    .keyframe_count = kfCount,
                    .has_bufs = hasBbs,
                    .buf_gpu_addrs = bbAddrs,
                    .buf_lens = bbLens,
                    .buf_offsets = bbOffs,
                    .stride = (uint64_t)boxStride,
                    .box_count = (uint64_t)boxCount,
                    .opaque = gd.opaque ? 1 : 0,
                    .allow_duplicate_any_hit = gd.allowDuplicateIntersectionFunctionInvocation ? 1 : 0
                };
                rdna4rt_motion_aabb_plan_t maplan;
                rdna4rt_err_t mae = rdna4rt_validate_motion_aabb_geom(&mad, requireBuffers ? 1 : 0, allowMotion, &m3, &maplan);
                free(hasBbs); free(bbAddrs); free(bbLens); free(bbOffs);
                if (mae != RDNA4RT_OK) {
                    free(kfEffAddrs);
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"motion bounding-box geometry[%lu]: %s", (unsigned long)i, rdna4rt_err_str(mae)]);
                    return NO;
                }
                if (validatePrimData) {
                    if (!rdna4_rt_extract_prim_data_geom(gd, dev, maplan.primitive_count, collectPrimBuffers,
                                                         retainedObjects, &plan->primPlans[i],
                                                         &plan->primSrcVkBufs[i], &plan->primSrcVkOffs[i], err)) {
                        free(kfEffAddrs);
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        return NO;
                    }
                }

                plan->hasMotionGeoms = YES;
                plan->motionPlans[i] = (RDNA4RTMotionGeomPlanEntry){
                    .isMotion = 1,
                    .geomKind = RDNA4RT_MOTION_GEOM_AABBS,
                    .keyframeCount = maplan.keyframe_count,
                    .primCount = maplan.primitive_count,
                    .startBorder = startBorder,
                    .endBorder = endBorder,
                    .startTime = startTime,
                    .endTime = endTime,
                    .primStride = RDNA4RT_MOTION_AABB_STRIDE,
                    .flags = gd.opaque ? 1u : 0u,
                    .payloadBytes = maplan.payload_bytes
                };

                VkDeviceAddress aabbAddr = 0u;
                if (requireBuffers && maplan.primitive_count > 0u) {
                    RDNA4RTStagingBuffer *aabbStg = [[RDNA4RTStagingBuffer alloc]
                        initWithSize:(uint64_t)maplan.primitive_count * RDNA4RT_AABB_STRIDE_MIN error:err];
                    VkDeviceAddress keyTable = rdna4_rt_store_motion_keys(aabbStg,
                        (uint64_t)maplan.primitive_count * RDNA4RT_AABB_STRIDE_MIN,
                        kfEffAddrs, kfCount, retainedObjects, err);
                    if (!keyTable) {
                        free(kfEffAddrs);
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        return NO;
                    }
                    [retainedObjects addObject:aabbStg];
                    aabbAddr = aabbStg->_addr;
                    rdna4rt_motion_blas_build_pc_t mpc = {
                        .kf_addrs_bda = keyTable,
                        .idx_addr = 0u,
                        .xform_addr = 0u,
                        .aabb_dst_addr = aabbAddr,
                        .payload_dst_addr = 0u,
                        .blas_aabb_addr = 0u,
                        .prim_count = maplan.primitive_count,
                        .keyframe_count = maplan.keyframe_count,
                        .src_stride = (uint32_t)maplan.stride,
                        .packed_flags = rdna4rt_motion_blas_pack_flags(0u, 0u, 1u, 0u)
                    };
                    if (!rdna4_rt_plan_append_motion_blas(plan, &mpc, (uint32_t)i)) {
                        free(kfEffAddrs);
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        if (err) *err = rdna4_err(145, @"out of host memory recording motion AABB dispatch");
                        return NO;
                    }
                }
                free(kfEffAddrs);

                plan->vkGeoms[i] = (VkAccelerationStructureGeometryKHR){
                    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                    .geometryType = VK_GEOMETRY_TYPE_AABBS_KHR,
                    .flags = maplan.vk_geom_flags,
                    .geometry.aabbs = {
                        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
                        .data.deviceAddress = aabbAddr,
                        .stride = RDNA4RT_AABB_STRIDE_MIN
                    }
                };
                plan->vkRanges[i] = (VkAccelerationStructureBuildRangeInfoKHR){
                    .primitiveCount = maplan.primitive_count,
                    .primitiveOffset = 0u,
                    .firstVertex = 0u,
                    .transformOffset = 0u
                };
                plan->maxPrimCounts[i] = maplan.primitive_count;
                signatures[i] = rdna4rt_geom_sig(RDNA4RT_GEOM_MOTION_AABBS, RDNA4RT_INDEX_NONE, RDNA4RT_VFMT_FLOAT3, maplan.vk_geom_flags);
                if (!rdna4rt_add_u64(totalPrims, (uint64_t)maplan.primitive_count, &totalPrims)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, @"total BLAS primitive count overflow");
                    return NO;
                }
            } else if ([gd isKindOfClass:[MTLAccelerationStructureTriangleGeometryDescriptor class]]) {
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
                if (validatePrimData) {
                    if (!rdna4_rt_extract_prim_data_geom(gd, dev, tplan.primitive_count, collectPrimBuffers,
                                                         retainedObjects, &plan->primPlans[i],
                                                         &plan->primSrcVkBufs[i], &plan->primSrcVkOffs[i], err)) {
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        return NO;
                    }
                }

                VkDeviceAddress transformAddr = tplan.transform_addr;
                if (requireBuffers && tbuf) {
                    const uint8_t *tmap = (const uint8_t *)[tbuf contents];
                    if (tplan.needs_transform_staging) {
                        if (!transformSlab || !rdna4rt_take_transform_slice(transformSlab->_size, &nextTransform)) {
                            transformSlab = [[RDNA4RTStagingBuffer alloc] initWithSize:RDNA4RT_TRANSFORM_SIZE error:err];
                            if (!transformSlab) { free(geomSigs); rdna4_rt_plan_free(plan); return NO; }
                            [retainedObjects addObject:transformSlab];
                            nextTransform = RDNA4RT_TRANSFORM_SIZE;
                        }
                        uint64_t offset = nextTransform - RDNA4RT_TRANSFORM_SIZE;
                        transformAddr = transformSlab->_addr + offset;
                        if (!tmap) {
                            /* GPU compute transpose/copy for Private transformationMatrixBuffer. */
                            rdna4rt_xform_transpose_pc_t xpc = {
                                .src_addr = tplan.transform_addr,
                                .dst_addr = transformAddr,
                                .layout_mode = (tdesc.transform_layout == RDNA4RT_MATRIX_ROW_MAJOR) ? 1u : 0u,
                                ._pad = 0u
                            };
                            if (!rdna4_rt_plan_append_xform(plan, &xpc)) {
                                free(geomSigs); rdna4_rt_plan_free(plan);
                                if (err) *err = rdna4_err(145, @"out of host memory recording Private transform dispatch");
                                return NO;
                            }
                        } else {
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
                            memcpy((uint8_t *)transformSlab->_map + offset, &vkXf, sizeof(vkXf));
                        }
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
                signatures[i] = rdna4rt_geom_sig(RDNA4RT_GEOM_TRIANGLES, tplan.effective_index_type, pureVfmt, tplan.vk_geom_flags);
                if (!rdna4rt_add_u64(totalPrims, (uint64_t)tplan.primitive_count, &totalPrims)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, @"total BLAS primitive count overflow");
                    return NO;
                }
                if (allowMotion && requireBuffers && tplan.primitive_count > 0u) {
                    rdna4rt_motion_blas_build_pc_t bpc = {
                        .kf_addrs_bda = tplan.vertex_addr,
                        .idx_addr = tplan.index_addr,
                        .xform_addr = transformAddr,
                        .aabb_dst_addr = 0u,
                        .payload_dst_addr = 0u,
                        .blas_aabb_addr = 0u,
                        .prim_count = tplan.primitive_count,
                        .keyframe_count = 1u,
                        .src_stride = (uint32_t)tplan.vertex_stride,
                        .packed_flags = rdna4rt_motion_blas_pack_flags(
                            (uint32_t)pureVfmt, (uint32_t)tplan.effective_index_type, 2u,
                            transformAddr ? 1u : 0u)
                    };
                    if (!rdna4_rt_plan_append_motion_blas(plan, &bpc, (uint32_t)i)) {
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        if (err) *err = rdna4_err(145, @"out of host memory recording static triangle local AABB dispatch");
                        return NO;
                    }
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
                if (validatePrimData) {
                    if (!rdna4_rt_extract_prim_data_geom(gd, dev, aplan.primitive_count, collectPrimBuffers,
                                                         retainedObjects, &plan->primPlans[i],
                                                         &plan->primSrcVkBufs[i], &plan->primSrcVkOffs[i], err)) {
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        return NO;
                    }
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
                signatures[i] = rdna4rt_geom_sig(RDNA4RT_GEOM_AABBS, RDNA4RT_INDEX_NONE, RDNA4RT_VFMT_FLOAT3, aplan.vk_geom_flags);
                if (!rdna4rt_add_u64(totalPrims, (uint64_t)aplan.primitive_count, &totalPrims)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, @"total BLAS primitive count overflow");
                    return NO;
                }
                if (allowMotion && requireBuffers && aplan.primitive_count > 0u) {
                    rdna4rt_motion_blas_build_pc_t bpc = {
                        .kf_addrs_bda = aplan.data_addr,
                        .idx_addr = 0u,
                        .xform_addr = 0u,
                        .aabb_dst_addr = 0u,
                        .payload_dst_addr = 0u,
                        .blas_aabb_addr = 0u,
                        .prim_count = aplan.primitive_count,
                        .keyframe_count = 1u,
                        .src_stride = (uint32_t)aplan.stride,
                        .packed_flags = rdna4rt_motion_blas_pack_flags(0u, 0u, 3u, 0u)
                    };
                    if (!rdna4_rt_plan_append_motion_blas(plan, &bpc, (uint32_t)i)) {
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        if (err) *err = rdna4_err(145, @"out of host memory recording static bounding-box local AABB dispatch");
                        return NO;
                    }
                }
            } else if (curveCls && [gd isKindOfClass:curveCls]) {
                if (!allowCurves) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, @"curve geometry requires GARNET_RT_CURVES=1");
                    return NO;
                }
                id rawCpBuf = ((id (*)(id, SEL))objc_msgSend)(gd, sel_registerName("controlPointBuffer"));
                NSUInteger cpOff = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("controlPointBufferOffset"));
                NSUInteger cpCount = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("controlPointCount"));
                NSUInteger cpStride = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("controlPointStride"));
                NSUInteger cpMtlFmt = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("controlPointFormat"));
                id rawRadBuf = ((id (*)(id, SEL))objc_msgSend)(gd, sel_registerName("radiusBuffer"));
                NSUInteger radOff = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("radiusBufferOffset"));
                NSUInteger radStride = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("radiusStride"));
                NSUInteger radMtlFmt = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("radiusFormat"));
                id rawIdxBuf = ((id (*)(id, SEL))objc_msgSend)(gd, sel_registerName("indexBuffer"));
                NSUInteger idxOff = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("indexBufferOffset"));
                NSUInteger idxMtlType = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("indexType"));
                NSUInteger segCount = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("segmentCount"));
                NSUInteger segCpCount = ((NSUInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("segmentControlPointCount"));
                NSInteger curveType = ((NSInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("curveType"));
                NSInteger curveBasis = ((NSInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("curveBasis"));
                NSInteger curveCaps = ((NSInteger (*)(id, SEL))objc_msgSend)(gd, sel_registerName("curveEndCaps"));

                RDNA4Buffer *cpBuf = nil, *radBuf = nil, *idxBuf = nil;
                NSString *bwhy = nil;
                if (!rdna4_rt_check_buffer(rawCpBuf, dev, NO, &cpBuf, &bwhy) ||
                    !rdna4_rt_check_buffer(rawRadBuf, dev, NO, &radBuf, &bwhy) ||
                    !rdna4_rt_check_buffer(rawIdxBuf, dev, NO, &idxBuf, &bwhy)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, bwhy ?: @"invalid curve geometry buffer");
                    return NO;
                }

                rdna4rt_curve_cp_fmt_t cpFmt = RDNA4RT_CURVE_CP_FLOAT3;
                rdna4rt_curve_rad_fmt_t radFmt = RDNA4RT_CURVE_RAD_FLOAT;
                if (!rdna4_rt_curve_cp_fmt_from_mtl(cpMtlFmt, &cpFmt) ||
                    !rdna4_rt_curve_rad_fmt_from_mtl(radMtlFmt, &radFmt)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"unsupported curve format cp=%lu rad=%lu", (unsigned long)cpMtlFmt, (unsigned long)radMtlFmt]);
                    return NO;
                }

                rdna4rt_index_type_t cIdxType = RDNA4RT_INDEX_NONE;
                if (idxBuf) {
                    if (idxMtlType == MTLIndexTypeUInt16) cIdxType = RDNA4RT_INDEX_UINT16;
                    else if (idxMtlType == MTLIndexTypeUInt32) cIdxType = RDNA4RT_INDEX_UINT32;
                    else {
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        if (err) *err = rdna4_err(145, @"unsupported curve indexType");
                        return NO;
                    }
                }

                rdna4rt_curve_geom_t cdesc = {
                    .curve_type = (rdna4rt_curve_type_t)curveType,
                    .curve_basis = (rdna4rt_curve_basis_t)curveBasis,
                    .curve_end_caps = (rdna4rt_curve_endcaps_t)curveCaps,
                    .segment_control_point_count = (uint32_t)MIN(segCpCount, (NSUInteger)UINT32_MAX),
                    .segment_count = (uint64_t)segCount,
                    .control_point_count = (uint64_t)cpCount,
                    .control_point_format = cpFmt,
                    .has_control_point_buf = cpBuf != nil ? 1 : 0,
                    .control_point_buf_gpu_addr = cpBuf ? [cpBuf gpuAddress] : 0u,
                    .control_point_buf_len = cpBuf ? (uint64_t)[cpBuf length] : 0u,
                    .control_point_buf_offset = (uint64_t)cpOff,
                    .control_point_stride = (uint64_t)cpStride,
                    .radius_format = radFmt,
                    .has_radius_buf = radBuf != nil ? 1 : 0,
                    .radius_buf_gpu_addr = radBuf ? [radBuf gpuAddress] : 0u,
                    .radius_buf_len = radBuf ? (uint64_t)[radBuf length] : 0u,
                    .radius_buf_offset = (uint64_t)radOff,
                    .radius_stride = (uint64_t)radStride,
                    .has_index_buf = idxBuf != nil ? 1 : 0,
                    .index_type = cIdxType,
                    .index_buf_gpu_addr = idxBuf ? [idxBuf gpuAddress] : 0u,
                    .index_buf_len = idxBuf ? (uint64_t)[idxBuf length] : 0u,
                    .index_buf_offset = (uint64_t)idxOff,
                    .opaque = gd.opaque ? 1 : 0,
                    .allow_duplicate_any_hit = gd.allowDuplicateIntersectionFunctionInvocation ? 1 : 0
                };
                rdna4rt_curve_plan_t cplan;
                rdna4rt_err_t ce = rdna4rt_validate_curve_geom_ex(&cdesc, m3.rt_max_primitives, requireBuffers ? 1 : 0, &cplan);
                if (ce != RDNA4RT_OK) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"curve geometry[%lu]: %s", (unsigned long)i, rdna4rt_err_str(ce)]);
                    return NO;
                }
                uint32_t subSplits = rdna4rt_curve_sub_splits(cdesc.curve_basis);
                uint32_t aabbCount = 0u;
                rdna4rt_err_t ace = rdna4rt_curve_aabb_count(cplan.segment_count, subSplits, &aabbCount);
                if (ace != RDNA4RT_OK || (m3.rt_max_primitives > 0u && aabbCount > m3.rt_max_primitives)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"curve geometry[%lu]: %s", (unsigned long)i, rdna4rt_err_str(ace != RDNA4RT_OK ? ace : RDNA4RT_ERR_LIMIT)]);
                    return NO;
                }
                if (validatePrimData) {
                    if (!rdna4_rt_extract_prim_data_geom(gd, dev, cplan.segment_count, collectPrimBuffers,
                                                         retainedObjects, &plan->primPlans[i],
                                                         &plan->primSrcVkBufs[i], &plan->primSrcVkOffs[i], err)) {
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        return NO;
                    }
                }
                if (requireBuffers) {
                    if (cpBuf) [retainedObjects addObject:cpBuf];
                    if (radBuf) [retainedObjects addObject:radBuf];
                    if (idxBuf) [retainedObjects addObject:idxBuf];
                }
                plan->curvePlans[i] = (RDNA4RTCurvePlanEntry){
                    .isCurve = 1,
                    .desc = cdesc,
                    .subSplits = subSplits,
                    .aabbCount = aabbCount,
                    .vkGeomFlags = cplan.vk_geom_flags
                };
                if (aabbCount > 0u) plan->hasCurves = YES;

                VkDeviceAddress aabbAddr = 0u;
                if (requireBuffers && aabbCount > 0u) {
                    RDNA4RTStagingBuffer *aabbStg = [[RDNA4RTStagingBuffer alloc]
                        initWithSize:(uint64_t)aabbCount * RDNA4RT_AABB_STRIDE_MIN error:err];
                    if (!aabbStg) {
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        return NO;
                    }
                    [retainedObjects addObject:aabbStg];
                    aabbAddr = aabbStg->_addr;
                    rdna4rt_curve_build_pc_t cpc = {
                        .cp_addr = cplan.cp_addr,
                        .rad_addr = cplan.rad_addr,
                        .idx_addr = cplan.index_addr,
                        .aabb_dst_addr = (uint64_t)aabbAddr,
                        .seg_dst_addr = 0u,
                        .segment_count = cplan.segment_count,
                        .control_point_count = (uint32_t)cdesc.control_point_count,
                        .cp_stride = (uint32_t)cplan.cp_stride,
                        .rad_stride = (uint32_t)cplan.rad_stride,
                        .geom_index = (uint32_t)i,
                        .packed_flags = rdna4rt_curve_pack_flags(
                            (uint32_t)cdesc.control_point_format,
                            (uint32_t)cdesc.radius_format,
                            (uint32_t)cplan.effective_index_type,
                            (uint32_t)cdesc.curve_basis,
                            cplan.segment_cp_count,
                            subSplits)
                    };
                    if (!rdna4_rt_plan_append_curve(plan, &cpc)) {
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        if (err) *err = rdna4_err(145, @"out of host memory recording curve dispatch");
                        return NO;
                    }
                }

                plan->vkGeoms[i] = (VkAccelerationStructureGeometryKHR){
                    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                    .geometryType = VK_GEOMETRY_TYPE_AABBS_KHR,
                    .flags = cplan.vk_geom_flags,
                    .geometry.aabbs = {
                        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
                        .data.deviceAddress = aabbAddr,
                        .stride = RDNA4RT_AABB_STRIDE_MIN
                    }
                };
                plan->vkRanges[i] = (VkAccelerationStructureBuildRangeInfoKHR){
                    .primitiveCount = aabbCount,
                    .primitiveOffset = 0u,
                    .firstVertex = 0u,
                    .transformOffset = 0u
                };
                plan->maxPrimCounts[i] = aabbCount;
                signatures[i] = rdna4rt_geom_sig(RDNA4RT_GEOM_CURVES, cplan.effective_index_type, RDNA4RT_VFMT_FLOAT3, cplan.vk_geom_flags);
                if (!rdna4rt_add_u64(totalPrims, (uint64_t)aabbCount, &totalPrims)) {
                    free(geomSigs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, @"total BLAS primitive count overflow");
                    return NO;
                }
                if (allowMotion && requireBuffers && aabbCount > 0u) {
                    rdna4rt_motion_blas_build_pc_t bpc = {
                        .kf_addrs_bda = (uint64_t)aabbAddr,
                        .idx_addr = 0u,
                        .xform_addr = 0u,
                        .aabb_dst_addr = 0u,
                        .payload_dst_addr = 0u,
                        .blas_aabb_addr = 0u,
                        .prim_count = aabbCount,
                        .keyframe_count = 1u,
                        .src_stride = RDNA4RT_AABB_STRIDE_MIN,
                        .packed_flags = rdna4rt_motion_blas_pack_flags(0u, 0u, 3u, 0u)
                    };
                    if (!rdna4_rt_plan_append_motion_blas(plan, &bpc, (uint32_t)i)) {
                        free(geomSigs); rdna4_rt_plan_free(plan);
                        if (err) *err = rdna4_err(145, @"out of host memory recording curve local AABB dispatch");
                        return NO;
                    }
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
        plan->geomIftBase = geomIftAcc.base;
        if (requireBuffers && geomIftAcc.non_consecutive) {
            RDNA4LOGR("rt-accel: multi-geometry BLAS (%u geoms) uses non-consecutive intersectionFunctionTableOffset values (base=%u); Vulkan ray queries index IFT as instanceSBT + geomIdx",
                      plan->geomCount, geomIftAcc.base);
        }
        plan->vkBuildFlags = rdna4rt_blas_build_flags_with_caps(plan->vkBuildFlags, hasTriangles, &m3);
        plan->topologyHash = rdna4rt_topology_hash(signatures, plan->maxPrimCounts, plan->geomCount);
        if (geomSigs) free(geomSigs);
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
        rdna4rt_xform_type_t xformType = RDNA4RT_XFORM_PACKED_FLOAT4X3;
        if ([idesc respondsToSelector:@selector(instanceDescriptorType)]) {
            descType = (NSUInteger)idesc.instanceDescriptorType;
            if (!rdna4_rt_check_buffer(idesc.motionTransformBuffer, dev, NO, &motionBuf, &bwhy)) {
                if (err) *err = rdna4_err(145, bwhy ?: @"invalid TLAS motionTransformBuffer");
                return NO;
            }
            motionCount = idesc.motionTransformCount;
            motionOff = idesc.motionTransformBufferOffset;
            SEL mtSel = sel_registerName("motionTransformType");
            if ([idesc respondsToSelector:mtSel])
                xformType = (rdna4rt_xform_type_t)((NSUInteger (*)(id, SEL))objc_msgSend)(idesc, mtSel);
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
            .motion_transform_count = (uint64_t)motionCount,
            .motion_transform_type = xformType
        };
        rdna4rt_tlas_plan_t tplan;
        rdna4rt_err_t te = rdna4rt_validate_tlas_desc(&tdesc, requireBuffers ? 1 : 0, allowMotion, &m3, &tplan);
        if (te != RDNA4RT_OK) {
            if (err) *err = rdna4_err(145, [NSString stringWithUTF8String:rdna4rt_err_str(te)]);
            return NO;
        }

        /* Vulkan requires geometryCount == 1 for TOP_LEVEL even when instanceCount == 0. */
        plan->geomCount = 1u;
        plan->vkGeoms = plan->small.vkGeoms;
        plan->vkRanges = plan->small.vkRanges;
        plan->maxPrimCounts = plan->small.maxPrimCounts;

        VkDeviceAddress vkInstAddr = 0;
        if (requireBuffers && tplan.instance_count > 0u) {
            const BOOL isMotionTlas = (descType == RDNA4RT_INST_DESC_MOTION ||
                                       descType == RDNA4RT_INST_DESC_INDIRECT_MOTION);
            NSUInteger blasCount = (descType == RDNA4RT_INST_DESC_INDIRECT ||
                                    descType == RDNA4RT_INST_DESC_INDIRECT_MOTION) ? 0u : blasList.count;
            uint64_t *blasAddrs = blasCount ? (uint64_t *)calloc(blasCount, sizeof(*blasAddrs)) : NULL;
            uint32_t *blasIftOffsets = blasCount ? (uint32_t *)calloc(blasCount, sizeof(*blasIftOffsets)) : NULL;
            uint64_t *blasLocalAabbs = blasCount ? (uint64_t *)calloc(blasCount, sizeof(*blasLocalAabbs)) : NULL;
            if (blasCount && (!blasAddrs || !blasIftOffsets || !blasLocalAabbs)) {
                free(blasAddrs); free(blasIftOffsets); free(blasLocalAabbs); rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, @"out of host memory collecting BLAS addresses");
                return NO;
            }
            for (NSUInteger bi = 0; bi < blasCount; bi++) {
                id obj = blasList[bi];
                if (![obj isKindOfClass:[GarnetAccelerationStructure class]] || [(GarnetAccelerationStructure *)obj device] != dev) {
                    free(blasAddrs); free(blasIftOffsets); free(blasLocalAabbs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"instancedAccelerationStructures[%lu] is not an GarnetAccelerationStructure on this device", (unsigned long)bi]);
                    return NO;
                }
                GarnetAccelerationStructure *blas = (GarnetAccelerationStructure *)obj;
                rdna4rt_accel_meta_t bmeta = [blas rdna4MetaSnapshot];
                GarnetASHandle *bhandle = [blas rdna4CurrentHandle];
                if (bmeta.kind != RDNA4RT_KIND_BLAS || bmeta.state == RDNA4RT_STATE_UNBUILT ||
                    !bhandle || !bhandle->_deviceAddress) {
                    free(blasAddrs); free(blasIftOffsets); free(blasLocalAabbs); rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"instancedAccelerationStructures[%lu] is not a built BLAS", (unsigned long)bi]);
                    return NO;
                }
                blasAddrs[bi] = bhandle->_deviceAddress;
                blasIftOffsets[bi] = bmeta.geom_ift_base;
                blasLocalAabbs[bi] = bhandle->_localAabbBuf ? (uint64_t)bhandle->_localAabbBuf->_addr : 0u;
                [retainedObjects addObject:blas];
                [retainedObjects addObject:bhandle];
                if (bhandle->_localAabbBuf) [retainedObjects addObject:bhandle->_localAabbBuf];
                GarnetASPrimData *bprim = [blas rdna4CurrentPrimData];
                if (bprim) {
                    [retainedObjects addObject:bprim];
                    if (tlasBlasPrimDatas) [tlasBlasPrimDatas addObject:bprim];
                }
            }

            uint64_t stageBytes = 0;
            if (!rdna4rt_mul_u64((uint64_t)tplan.instance_count, RDNA4RT_VK_INSTANCE_SIZE, &stageBytes)) {
                free(blasAddrs); free(blasIftOffsets); free(blasLocalAabbs); rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, @"TLAS instance staging size overflow");
                return NO;
            }
            RDNA4RTStagingBuffer *stg = [[RDNA4RTStagingBuffer alloc] initWithSize:stageBytes error:err];
            if (!stg) { free(blasAddrs); free(blasIftOffsets); free(blasLocalAabbs); rdna4_rt_plan_free(plan); return NO; }

            if (isMotionTlas) {
                VkDeviceAddress unitBoxBda = rdna4_rt_ensure_unit_box_blas(err);
                if (!unitBoxBda) {
                    free(blasAddrs); free(blasIftOffsets); free(blasLocalAabbs); rdna4_rt_plan_free(plan);
                    return NO;
                }
                uint64_t blasTableAddr = 0u;
                if (blasCount > 0u) {
                    uint64_t tblBytes = 0u;
                    if (!rdna4rt_mul_u64((uint64_t)blasCount, (uint64_t)sizeof(rdna4rt_motion_blas_entry_t), &tblBytes)) {
                        free(blasAddrs); free(blasIftOffsets); free(blasLocalAabbs); rdna4_rt_plan_free(plan);
                        if (err) *err = rdna4_err(145, @"motion TLAS BLAS table size overflow");
                        return NO;
                    }
                    RDNA4RTStagingBuffer *blasTbl = [[RDNA4RTStagingBuffer alloc] initWithSize:tblBytes error:err];
                    if (!blasTbl) {
                        free(blasAddrs); free(blasIftOffsets); free(blasLocalAabbs); rdna4_rt_plan_free(plan);
                        return NO;
                    }
                    rdna4rt_motion_blas_entry_t *entries = (rdna4rt_motion_blas_entry_t *)blasTbl->_map;
                    for (NSUInteger bi = 0; bi < blasCount; bi++) {
                        entries[bi] = (rdna4rt_motion_blas_entry_t){
                            .blas_bda = blasAddrs[bi],
                            .local_aabb_bda = blasLocalAabbs[bi],
                            .geom_ift_base = blasIftOffsets[bi],
                            ._pad = 0u
                        };
                    }
                    blasTableAddr = blasTbl->_addr;
                    [retainedObjects addObject:blasTbl];
                }
                free(blasAddrs);
                free(blasIftOffsets);
                free(blasLocalAabbs);
                uint64_t srcAddr = 0u, motionAddr = 0u;
                VkDeviceSize xformVkOff = 0u;
                if (!rdna4rt_add_u64([instBuf gpuAddress], tdesc.inst_buf_offset, &srcAddr) ||
                    (motionBuf && !rdna4rt_add_u64([motionBuf gpuAddress], tdesc.motion_transform_buf_offset, &motionAddr)) ||
                    (motionBuf && __builtin_add_overflow([motionBuf rdna4VkOffset], (VkDeviceSize)motionOff, &xformVkOff))) {
                    rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, @"motion TLAS buffer address overflow");
                    return NO;
                }
                plan->hasMotionTlas = YES;
                plan->motionTlasXformType = xformType;
                plan->motionTlasXformVkBuf = motionBuf ? [motionBuf vkBuffer] : VK_NULL_HANDLE;
                plan->motionTlasXformVkOff = (uint64_t)xformVkOff;
                plan->motionTlasXformCount = (uint32_t)MIN(motionCount, (NSUInteger)UINT32_MAX);
                plan->motionTlasPC = (rdna4rt_motion_tlas_build_pc_t){
                    .inst_src_addr = srcAddr,
                    .xform_src_addr = motionAddr,
                    .blas_table_addr = blasTableAddr,
                    .count_addr = 0u,
                    .vk_inst_dst_addr = stg->_addr,
                    .inst_meta_dst_addr = 0u,
                    .unit_box_blas_bda = unitBoxBda,
                    .max_count = tplan.instance_count,
                    .src_stride = (uint32_t)tplan.stride,
                    .blas_count = (uint32_t)blasCount,
                    .xform_count = plan->motionTlasXformCount,
                    .packed_flags = ((descType == RDNA4RT_INST_DESC_INDIRECT_MOTION) ? 1u : 0u) |
                                    ((xformType == RDNA4RT_XFORM_COMPONENT ? 1u : 0u) << 1u)
                };
                if (motionBuf) [retainedObjects addObject:motionBuf];
            } else if (![instBuf contents]) {
                /* GPU compute conversion for MTLStorageModePrivate instanceDescriptorBuffer. */
                uint64_t blasTableAddr = 0u;
                if (blasCount > 0u) {
                    uint64_t tblBytes = 0u;
                    if (!rdna4rt_mul_u64((uint64_t)blasCount, (uint64_t)sizeof(rdna4rt_blas_table_entry_t), &tblBytes)) {
                        free(blasAddrs); free(blasIftOffsets); free(blasLocalAabbs); rdna4_rt_plan_free(plan);
                        if (err) *err = rdna4_err(145, @"TLAS BLAS table size overflow");
                        return NO;
                    }
                    RDNA4RTStagingBuffer *blasTbl = [[RDNA4RTStagingBuffer alloc] initWithSize:tblBytes error:err];
                    if (!blasTbl) { free(blasAddrs); free(blasIftOffsets); free(blasLocalAabbs); rdna4_rt_plan_free(plan); return NO; }
                    rdna4rt_blas_table_entry_t *entries = (rdna4rt_blas_table_entry_t *)blasTbl->_map;
                    for (NSUInteger bi = 0; bi < blasCount; bi++) {
                        entries[bi] = (rdna4rt_blas_table_entry_t){
                            .bda = blasAddrs[bi],
                            .geom_ift_base = blasIftOffsets[bi],
                            ._pad = 0u
                        };
                    }
                    blasTableAddr = blasTbl->_addr;
                    [retainedObjects addObject:blasTbl];
                }
                free(blasAddrs);
                free(blasIftOffsets);
                free(blasLocalAabbs);
                uint64_t srcAddr = 0u;
                if (!rdna4rt_add_u64([instBuf gpuAddress], tdesc.inst_buf_offset, &srcAddr)) {
                    rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, @"TLAS Private instance buffer address overflow");
                    return NO;
                }
                plan->hasInstDispatch = YES;
                plan->instPC = (rdna4rt_inst_convert_pc_t){
                    .src_addr = srcAddr,
                    .dst_addr = stg->_addr,
                    .blas_table_addr = blasTableAddr,
                    .count_addr = 0u,
                    .max_count = tplan.instance_count,
                    .src_stride = (uint32_t)tplan.stride,
                    .desc_type = (uint32_t)tdesc.descriptor_type,
                    .blas_count = (uint32_t)blasCount
                };
            } else {
                rdna4rt_err_t ce = rdna4rt_convert_instances_ex(
                    tdesc.descriptor_type, (const uint8_t *)[instBuf contents], (size_t)[instBuf length],
                    tdesc.inst_buf_offset, tplan.stride, tplan.instance_count,
                    blasAddrs, blasIftOffsets, (uint32_t)blasCount, (rdna4rt_vk_instance_t *)stg->_map);
                free(blasAddrs);
                free(blasIftOffsets);
                free(blasLocalAabbs);
                if (ce != RDNA4RT_OK) {
                    rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, [NSString stringWithFormat:@"TLAS instance conversion failed: %s", rdna4rt_err_str(ce)]);
                    return NO;
                }
            }
            vkInstAddr = stg->_addr;
            plan->tlasInstStgBuf = stg->_buf;
            plan->tlasInstanceCount = tplan.instance_count;
            [retainedObjects addObject:stg];
            [retainedObjects addObject:instBuf];
        } else if (requireBuffers) {
            if (instBuf) [retainedObjects addObject:instBuf];
            if (motionBuf) [retainedObjects addObject:motionBuf];
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
    } else if (indirectInstCls && [desc isKindOfClass:indirectInstCls]) {
        plan->kind = RDNA4RT_KIND_TLAS;
        id rawInstBuf = ((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("instanceDescriptorBuffer"));
        NSUInteger instOff = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("instanceDescriptorBufferOffset"));
        NSUInteger instStride = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("instanceDescriptorStride"));
        NSUInteger maxCount = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("maxInstanceCount"));
        id rawCountBuf = ((id (*)(id, SEL))objc_msgSend)(desc, sel_registerName("instanceCountBuffer"));
        NSUInteger countOff = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, sel_registerName("instanceCountBufferOffset"));
        NSUInteger descType = RDNA4RT_INST_DESC_INDIRECT;
        SEL dtSel = sel_registerName("instanceDescriptorType");
        if ([desc respondsToSelector:dtSel])
            descType = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, dtSel);
        id rawMotionBuf = nil;
        NSUInteger motionCount = 0, motionOff = 0;
        rdna4rt_xform_type_t xformType = RDNA4RT_XFORM_PACKED_FLOAT4X3;
        SEL mbSel = sel_registerName("motionTransformBuffer");
        if ([desc respondsToSelector:mbSel])
            rawMotionBuf = ((id (*)(id, SEL))objc_msgSend)(desc, mbSel);
        SEL moSel = sel_registerName("motionTransformBufferOffset");
        if ([desc respondsToSelector:moSel])
            motionOff = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, moSel);
        SEL mcSel = sel_registerName("motionTransformCount");
        if ([desc respondsToSelector:mcSel])
            motionCount = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, mcSel);
        SEL mtSel = sel_registerName("motionTransformType");
        if ([desc respondsToSelector:mtSel])
            xformType = (rdna4rt_xform_type_t)((NSUInteger (*)(id, SEL))objc_msgSend)(desc, mtSel);

        RDNA4Buffer *instBuf = nil, *countBuf = nil, *motionBuf = nil;
        NSString *bwhy = nil;
        if (!rdna4_rt_check_buffer(rawInstBuf, dev, NO, &instBuf, &bwhy)) {
            if (err) *err = rdna4_err(145, bwhy ?: @"invalid indirect TLAS instanceDescriptorBuffer");
            return NO;
        }
        if (!rdna4_rt_check_buffer(rawCountBuf, dev, NO, &countBuf, &bwhy)) {
            if (err) *err = rdna4_err(145, bwhy ?: @"invalid indirect TLAS instanceCountBuffer");
            return NO;
        }
        if (!rdna4_rt_check_buffer(rawMotionBuf, dev, NO, &motionBuf, &bwhy)) {
            if (err) *err = rdna4_err(145, bwhy ?: @"invalid indirect TLAS motionTransformBuffer");
            return NO;
        }

        rdna4rt_indirect_tlas_desc_t idesc = {
            .descriptor_type = (rdna4rt_inst_desc_type_t)descType,
            .usage = plan->usage,
            .max_instance_count = (uint64_t)maxCount,
            .has_inst_buf = instBuf != nil ? 1 : 0,
            .inst_buf_len = instBuf ? (uint64_t)[instBuf length] : 0u,
            .inst_buf_offset = (uint64_t)instOff,
            .inst_stride = (uint64_t)instStride,
            .has_count_buf = countBuf != nil ? 1 : 0,
            .count_buf_len = countBuf ? (uint64_t)[countBuf length] : 0u,
            .count_buf_offset = (uint64_t)countOff,
            .has_motion_transform_buf = motionBuf != nil ? 1 : 0,
            .motion_transform_buf_len = motionBuf ? (uint64_t)[motionBuf length] : 0u,
            .motion_transform_buf_offset = (uint64_t)motionOff,
            .motion_transform_count = (uint64_t)motionCount,
            .motion_transform_type = xformType
        };
        rdna4rt_tlas_plan_t tplan;
        rdna4rt_err_t te = rdna4rt_validate_indirect_tlas_desc(&idesc, requireBuffers ? 1 : 0, allowMotion, &m3, &tplan);
        if (te != RDNA4RT_OK) {
            if (err) *err = rdna4_err(145, [NSString stringWithUTF8String:rdna4rt_err_str(te)]);
            return NO;
        }

        plan->geomCount = 1u;
        plan->vkGeoms = plan->small.vkGeoms;
        plan->vkRanges = plan->small.vkRanges;
        plan->maxPrimCounts = plan->small.maxPrimCounts;

        VkDeviceAddress vkInstAddr = 0;
        if (requireBuffers && tplan.instance_count > 0u) {
            uint64_t stageBytes = 0;
            if (!rdna4rt_mul_u64((uint64_t)tplan.instance_count, RDNA4RT_VK_INSTANCE_SIZE, &stageBytes)) {
                rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, @"indirect TLAS staging size overflow");
                return NO;
            }
            RDNA4RTStagingBuffer *stg = [[RDNA4RTStagingBuffer alloc] initWithSize:stageBytes error:err];
            if (!stg) { rdna4_rt_plan_free(plan); return NO; }
            uint64_t srcAddr = 0u, cntAddr = 0u;
            if (!rdna4rt_add_u64([instBuf gpuAddress], idesc.inst_buf_offset, &srcAddr) ||
                !rdna4rt_add_u64([countBuf gpuAddress], idesc.count_buf_offset, &cntAddr)) {
                rdna4_rt_plan_free(plan);
                if (err) *err = rdna4_err(145, @"indirect TLAS buffer address overflow");
                return NO;
            }
            if (descType == RDNA4RT_INST_DESC_INDIRECT_MOTION) {
                VkDeviceAddress unitBoxBda = rdna4_rt_ensure_unit_box_blas(err);
                if (!unitBoxBda) { rdna4_rt_plan_free(plan); return NO; }
                uint64_t motionAddr = 0u;
                VkDeviceSize xformVkOff = 0u;
                if ((motionBuf && !rdna4rt_add_u64([motionBuf gpuAddress], idesc.motion_transform_buf_offset, &motionAddr)) ||
                    (motionBuf && __builtin_add_overflow([motionBuf rdna4VkOffset], (VkDeviceSize)motionOff, &xformVkOff))) {
                    rdna4_rt_plan_free(plan);
                    if (err) *err = rdna4_err(145, @"indirect motion TLAS transform buffer address overflow");
                    return NO;
                }
                plan->hasMotionTlas = YES;
                plan->motionTlasXformType = xformType;
                plan->motionTlasXformVkBuf = motionBuf ? [motionBuf vkBuffer] : VK_NULL_HANDLE;
                plan->motionTlasXformVkOff = (uint64_t)xformVkOff;
                plan->motionTlasXformCount = (uint32_t)MIN(motionCount, (NSUInteger)UINT32_MAX);
                plan->motionTlasPC = (rdna4rt_motion_tlas_build_pc_t){
                    .inst_src_addr = srcAddr,
                    .xform_src_addr = motionAddr,
                    .blas_table_addr = 0u,
                    .count_addr = cntAddr,
                    .vk_inst_dst_addr = stg->_addr,
                    .inst_meta_dst_addr = 0u,
                    .unit_box_blas_bda = unitBoxBda,
                    .max_count = tplan.instance_count,
                    .src_stride = (uint32_t)tplan.stride,
                    .blas_count = 0u,
                    .xform_count = plan->motionTlasXformCount,
                    .packed_flags = 1u | ((xformType == RDNA4RT_XFORM_COMPONENT ? 1u : 0u) << 1u)
                };
                if (motionBuf) [retainedObjects addObject:motionBuf];
            } else {
                /* Always translate on GPU with zero CPU readback (count/descriptors may be written earlier in the same CB). */
                plan->hasInstDispatch = YES;
                plan->instPC = (rdna4rt_inst_convert_pc_t){
                    .src_addr = srcAddr,
                    .dst_addr = stg->_addr,
                    .blas_table_addr = 0u,
                    .count_addr = cntAddr,
                    .max_count = tplan.instance_count,
                    .src_stride = (uint32_t)tplan.stride,
                    .desc_type = RDNA4RT_INST_DESC_INDIRECT,
                    .blas_count = 0u
                };
            }
            vkInstAddr = stg->_addr;
            plan->tlasInstStgBuf = stg->_buf;
            plan->tlasInstanceCount = tplan.instance_count;
            [retainedObjects addObject:stg];
            [retainedObjects addObject:instBuf];
            [retainedObjects addObject:countBuf];
        } else if (requireBuffers) {
            if (instBuf) [retainedObjects addObject:instBuf];
            if (countBuf) [retainedObjects addObject:countBuf];
            if (motionBuf) [retainedObjects addObject:motionBuf];
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

    if (!requireBuffers && collectPrimBuffers) {
        /* PerPrimitiveData-only refit skips vkCmdBuildAccelerationStructuresKHR and needs no scratch sizing. */
        return YES;
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

GARNET_INTERNAL BOOL rdna4_rt_build_plan_make(
    id dev, MTLAccelerationStructureDescriptor *desc, BOOL requireBuffers,
    BOOL useOverrideUsage, uint64_t overrideUsage,
    NSMutableArray *retainedObjects,
    RDNA4RTBuildPlan *plan, NSError **err) {
    return rdna4_rt_build_plan_make_ex(dev, desc, requireBuffers, YES, requireBuffers,
                                       useOverrideUsage, overrideUsage, retainedObjects, nil, plan, err);
}

static BOOL rdna4_rt_upload_prim_header(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb, VkBuffer dstBuf,
    const void *hdrData, uint64_t hdrBytes, NSError **err)
{
    return rdna4_rt_cmd_write_buf_bytes(cmd, cb, dstBuf, 0u, hdrData, hdrBytes, err);
}

GARNET_INTERNAL BOOL rdna4_rt_record_blas_prim_snapshot(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb,
    GarnetAccelerationStructure *dstAS, uint64_t dstAsBda,
    const RDNA4RTBuildPlan *plan, NSError **err)
{
    if (!plan->primPlans || plan->geomCount == 0u)
        return [dstAS rdna4SetPrimData:nil forAsAddress:dstAsBda commandBuffer:cb vkCmdBuffer:cmd error:err];
    uint64_t *sliceOffsets = (uint64_t *)calloc(plan->geomCount, sizeof(*sliceOffsets));
    if (!sliceOffsets) {
        if (err) *err = rdna4_err(146, @"out of host memory computing BLAS primitive data layout");
        return NO;
    }
    uint64_t headerBytes = 0u, totalBytes = 0u;
    int anyPrim = 0;
    rdna4rt_err_t le = rdna4rt_blas_prim_layout_compute(
        plan->primPlans, plan->geomCount, sliceOffsets, &headerBytes, &totalBytes, &anyPrim);
    if (le != RDNA4RT_OK) {
        free(sliceOffsets);
        if (err) *err = rdna4_err(146, [NSString stringWithUTF8String:rdna4rt_err_str(le)]);
        return NO;
    }
    if (!anyPrim) {
        free(sliceOffsets);
        return [dstAS rdna4SetPrimData:nil forAsAddress:dstAsBda commandBuffer:cb vkCmdBuffer:cmd error:err];
    }
    GarnetASPrimData *pd = [[GarnetASPrimData alloc]
        initWithSize:totalBytes kind:RDNA4RT_PRIM_KIND_BLAS count:plan->geomCount
         entryStride:0u refOffset:0u error:err];
    if (!pd) {
        free(sliceOffsets);
        return NO;
    }
    rdna4rt_geom_prim_entry_t *headers = (rdna4rt_geom_prim_entry_t *)calloc(plan->geomCount, sizeof(*headers));
    if (!headers) {
        free(sliceOffsets);
        if (err) *err = rdna4_err(146, @"out of host memory building BLAS primitive headers");
        return NO;
    }
    rdna4rt_err_t he = rdna4rt_blas_prim_build_headers(
        (uint64_t)pd->_bda, plan->primPlans, sliceOffsets, plan->geomCount, headers);
    if (he != RDNA4RT_OK) {
        free(headers); free(sliceOffsets);
        if (err) *err = rdna4_err(146, [NSString stringWithUTF8String:rdna4rt_err_str(he)]);
        return NO;
    }
    pd->_blasHeaders = headers;

    if (!rdna4_rt_upload_prim_header(cmd, cb, pd->_buf, headers, headerBytes, err)) {
        free(sliceOffsets);
        return NO;
    }
    for (uint32_t i = 0; i < plan->geomCount; i++) {
        if (!plan->primPlans[i].present || plan->primPlans[i].copy_bytes == 0u) continue;
        VkBufferCopy copy = {
            .srcOffset = plan->primSrcVkOffs[i],
            .dstOffset = sliceOffsets[i],
            .size = plan->primPlans[i].copy_bytes
        };
        vkCmdCopyBuffer(cmd, plan->primSrcVkBufs[i], pd->_buf, 1u, &copy);
    }
    free(sliceOffsets);

    if (![dstAS rdna4SetPrimData:pd forAsAddress:dstAsBda commandBuffer:cb vkCmdBuffer:cmd error:err]) return NO;
    [cb rdna4RetainEncoded:pd];
    return YES;
}

GARNET_INTERNAL BOOL rdna4_rt_prepare_blas_sidecar(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb,
    RDNA4RTBuildPlan *plan, GarnetASPrimData *cloneSrcPd,
    BOOL updPrim, BOOL cloneTailCurveSegs,
    GarnetASPrimData **outPd, NSError **err)
{
    if (outPd) *outPd = nil;
    if (!plan || plan->geomCount == 0u || !plan->curvePlans) return YES;

    uint32_t *curveAabbs = (uint32_t *)calloc(plan->geomCount, sizeof(*curveAabbs));
    uint64_t *primOffsets = (uint64_t *)calloc(plan->geomCount, sizeof(*primOffsets));
    uint64_t *segOffsets = (uint64_t *)calloc(plan->geomCount, sizeof(*segOffsets));
    uint64_t *motionPayBytes = (uint64_t *)calloc(plan->geomCount, sizeof(*motionPayBytes));
    uint64_t *motionGeomOffsets = (uint64_t *)calloc(plan->geomCount, sizeof(*motionGeomOffsets));
    if (!curveAabbs || !primOffsets || !segOffsets || !motionPayBytes || !motionGeomOffsets) {
        free(curveAabbs); free(primOffsets); free(segOffsets); free(motionPayBytes); free(motionGeomOffsets);
        if (err) *err = rdna4_err(146, @"out of host memory planning BLAS curve/motion sidecar");
        return NO;
    }
    for (uint32_t i = 0; i < plan->geomCount; i++) {
        curveAabbs[i] = plan->curvePlans[i].isCurve ? plan->curvePlans[i].aabbCount : 0u;
        motionPayBytes[i] = (plan->motionPlans && plan->motionPlans[i].isMotion)
            ? plan->motionPlans[i].payloadBytes : 0u;
    }
    const rdna4rt_prim_data_geom_plan_t *primSrc = updPrim
        ? plan->primPlans
        : NULL;
    uint64_t headerBytes = 0u, auxCurveOff = 0u, auxMotionOff = 0u, totalBytes = 0u;
    int anyPrim = 0, anyCurve = 0, anyMotion = 0;
    rdna4rt_prim_data_geom_plan_t *clonedPlans = NULL;
    if (!updPrim && cloneSrcPd && cloneSrcPd->_blasHeaders && cloneSrcPd->_count == plan->geomCount) {
        clonedPlans = (rdna4rt_prim_data_geom_plan_t *)calloc(plan->geomCount, sizeof(*clonedPlans));
        if (!clonedPlans) {
            free(curveAabbs); free(primOffsets); free(segOffsets); free(motionPayBytes); free(motionGeomOffsets);
            if (err) *err = rdna4_err(146, @"out of host memory cloning prim headers");
            return NO;
        }
        for (uint32_t i = 0; i < plan->geomCount; i++) {
            uint32_t st = cloneSrcPd->_blasHeaders[i].stride;
            uint32_t pc = cloneSrcPd->_blasHeaders[i].prim_count;
            if (cloneSrcPd->_blasHeaders[i].data_bda != 0u && pc > 0u) {
                clonedPlans[i].present = 1;
                clonedPlans[i].stride = st;
                clonedPlans[i].prim_count = pc;
                clonedPlans[i].copy_bytes = (uint64_t)pc * (uint64_t)st;
            }
        }
        primSrc = clonedPlans;
    }

    rdna4rt_err_t le = rdna4rt_blas_sidecar_layout_compute(
        primSrc, curveAabbs, motionPayBytes, plan->geomCount,
        primOffsets, &headerBytes, &auxCurveOff, segOffsets,
        &auxMotionOff, motionGeomOffsets, &totalBytes, &anyPrim, &anyCurve, &anyMotion);
    if (le != RDNA4RT_OK || auxCurveOff > UINT32_MAX || auxMotionOff > UINT32_MAX) {
        free(clonedPlans); free(curveAabbs); free(primOffsets); free(segOffsets); free(motionPayBytes); free(motionGeomOffsets);
        if (err) *err = rdna4_err(146, [NSString stringWithUTF8String:rdna4rt_err_str(le != RDNA4RT_OK ? le : RDNA4RT_ERR_LIMIT)]);
        return NO;
    }
    if (!anyPrim && !anyCurve && !anyMotion) {
        free(clonedPlans); free(curveAabbs); free(primOffsets); free(segOffsets); free(motionPayBytes); free(motionGeomOffsets);
        return YES;
    }

    GarnetASPrimData *pd = [[GarnetASPrimData alloc]
        initWithSize:totalBytes kind:RDNA4RT_PRIM_KIND_BLAS count:plan->geomCount
         entryStride:(uint32_t)auxCurveOff refOffset:(uint32_t)auxMotionOff error:err];
    if (!pd) {
        free(clonedPlans); free(curveAabbs); free(primOffsets); free(segOffsets); free(motionPayBytes); free(motionGeomOffsets);
        return NO;
    }

    rdna4rt_geom_prim_entry_t *headers = (rdna4rt_geom_prim_entry_t *)calloc(plan->geomCount, sizeof(*headers));
    rdna4rt_curve_meta_hdr_t *curveHdrs = anyCurve
        ? (rdna4rt_curve_meta_hdr_t *)calloc(plan->geomCount, sizeof(*curveHdrs))
        : NULL;
    rdna4rt_motion_geom_hdr_t *motionHdrs = anyMotion
        ? (rdna4rt_motion_geom_hdr_t *)calloc(plan->geomCount, sizeof(*motionHdrs))
        : NULL;
    if (!headers || (anyCurve && !curveHdrs) || (anyMotion && !motionHdrs)) {
        free(headers); free(curveHdrs); free(motionHdrs); free(clonedPlans);
        free(curveAabbs); free(primOffsets); free(segOffsets); free(motionPayBytes); free(motionGeomOffsets);
        if (err) *err = rdna4_err(146, @"out of host memory building BLAS curve/motion headers");
        return NO;
    }
    if (primSrc && anyPrim) {
        rdna4rt_err_t he = rdna4rt_blas_prim_build_headers(
            (uint64_t)pd->_bda, primSrc, primOffsets, plan->geomCount, headers);
        if (he != RDNA4RT_OK) {
            free(headers); free(curveHdrs); free(motionHdrs); free(clonedPlans);
            free(curveAabbs); free(primOffsets); free(segOffsets); free(motionPayBytes); free(motionGeomOffsets);
            if (err) *err = rdna4_err(146, [NSString stringWithUTF8String:rdna4rt_err_str(he)]);
            return NO;
        }
    }
    pd->_blasHeaders = headers;
    if (!rdna4_rt_upload_prim_header(cmd, cb, pd->_buf, headers, headerBytes, err)) {
        free(curveHdrs); free(motionHdrs); free(clonedPlans);
        free(curveAabbs); free(primOffsets); free(segOffsets); free(motionPayBytes); free(motionGeomOffsets);
        return NO;
    }
    if (anyPrim) {
        for (uint32_t i = 0; i < plan->geomCount; i++) {
            if (!primSrc[i].present || primSrc[i].copy_bytes == 0u) continue;
            if (updPrim) {
                VkBufferCopy copy = {
                    .srcOffset = plan->primSrcVkOffs[i],
                    .dstOffset = primOffsets[i],
                    .size = primSrc[i].copy_bytes
                };
                vkCmdCopyBuffer(cmd, plan->primSrcVkBufs[i], pd->_buf, 1u, &copy);
            } else if (cloneSrcPd && cloneSrcPd->_blasHeaders[i].data_bda >= (uint64_t)cloneSrcPd->_bda) {
                VkBufferCopy copy = {
                    .srcOffset = cloneSrcPd->_blasHeaders[i].data_bda - (uint64_t)cloneSrcPd->_bda,
                    .dstOffset = primOffsets[i],
                    .size = primSrc[i].copy_bytes
                };
                vkCmdCopyBuffer(cmd, cloneSrcPd->_buf, pd->_buf, 1u, &copy);
            }
        }
    }
    if (anyCurve) {
        for (uint32_t i = 0; i < plan->geomCount; i++) {
            if (!plan->curvePlans[i].isCurve || plan->curvePlans[i].aabbCount == 0u) continue;
            const rdna4rt_curve_geom_t *cg = &plan->curvePlans[i].desc;
            curveHdrs[i] = (rdna4rt_curve_meta_hdr_t){
                .magic = RDNA4RT_CURVE_META_MAGIC,
                .curve_type = (uint32_t)cg->curve_type,
                .curve_basis = (uint32_t)cg->curve_basis,
                .curve_end_caps = (uint32_t)cg->curve_end_caps,
                .segment_cp_count = cg->segment_control_point_count,
                .segment_count = (uint32_t)cg->segment_count,
                .sub_splits = plan->curvePlans[i].subSplits,
                .flags = cg->opaque ? 1u : 0u,
                .seg_table_off = segOffsets[i],
                .sub_map_off = 0u
            };
        }
        uint64_t curveHdrBytes = (uint64_t)plan->geomCount * (uint64_t)sizeof(rdna4rt_curve_meta_hdr_t);
        if (!rdna4_rt_cmd_write_buf_bytes(cmd, cb, pd->_buf, auxCurveOff, curveHdrs, curveHdrBytes, err)) {
            free(curveHdrs); free(motionHdrs); free(clonedPlans);
            free(curveAabbs); free(primOffsets); free(segOffsets); free(motionPayBytes); free(motionGeomOffsets);
            return NO;
        }
        uint64_t srcEnd = (cloneSrcPd && cloneSrcPd->_refOffset > 0u &&
                           (uint64_t)cloneSrcPd->_refOffset < cloneSrcPd->_totalBytes)
            ? (uint64_t)cloneSrcPd->_refOffset
            : (cloneSrcPd ? cloneSrcPd->_totalBytes : 0u);
        if (cloneTailCurveSegs && cloneSrcPd && cloneSrcPd->_entryStride > 0u &&
            srcEnd > (uint64_t)cloneSrcPd->_entryStride + curveHdrBytes) {
            uint64_t srcTailOff = (uint64_t)cloneSrcPd->_entryStride + curveHdrBytes;
            uint64_t dstTailOff = auxCurveOff + curveHdrBytes;
            uint64_t availSrc = srcEnd - srcTailOff;
            uint64_t dstEnd = (anyMotion && auxMotionOff > dstTailOff) ? auxMotionOff : pd->_totalBytes;
            uint64_t availDst = (dstEnd > dstTailOff) ? (dstEnd - dstTailOff) : 0u;
            uint64_t tailCopySz = MIN(availSrc, availDst);
            if (tailCopySz > 0u) {
                VkBufferCopy tcopy = { .srcOffset = srcTailOff, .dstOffset = dstTailOff, .size = tailCopySz };
                vkCmdCopyBuffer(cmd, cloneSrcPd->_buf, pd->_buf, 1u, &tcopy);
            }
        } else {
            for (uint32_t k = 0; k < plan->curveDispatchCount; k++) {
                uint32_t gi = plan->curvePCs[k].geom_index;
                if (gi < plan->geomCount && segOffsets[gi] > 0u) {
                    plan->curvePCs[k].seg_dst_addr = (uint64_t)pd->_bda + segOffsets[gi];
                }
            }
        }
        free(curveHdrs);
    }
    if (anyMotion) {
        for (uint32_t i = 0; i < plan->geomCount; i++) {
            if (!plan->motionPlans || !plan->motionPlans[i].isMotion) continue;
            const RDNA4RTMotionGeomPlanEntry *mp = &plan->motionPlans[i];
            motionHdrs[i] = (rdna4rt_motion_geom_hdr_t){
                .magic = RDNA4RT_MOTION_GEOM_MAGIC,
                .geom_kind = mp->geomKind,
                .keyframe_count = mp->keyframeCount,
                .prim_count = mp->primCount,
                .start_border = mp->startBorder,
                .end_border = mp->endBorder,
                .start_time = mp->startTime,
                .end_time = mp->endTime,
                .data_off = motionGeomOffsets[i],
                .prim_stride = mp->primStride,
                .flags = mp->flags
            };
        }
        uint64_t motHdrBytes = (uint64_t)plan->geomCount * (uint64_t)sizeof(rdna4rt_motion_geom_hdr_t);
        if (!rdna4_rt_cmd_write_buf_bytes(cmd, cb, pd->_buf, auxMotionOff, motionHdrs, motHdrBytes, err)) {
            free(motionHdrs); free(clonedPlans);
            free(curveAabbs); free(primOffsets); free(segOffsets); free(motionPayBytes); free(motionGeomOffsets);
            return NO;
        }
        if (cloneTailCurveSegs && cloneSrcPd && cloneSrcPd->_refOffset > 0u &&
            cloneSrcPd->_totalBytes > (uint64_t)cloneSrcPd->_refOffset + motHdrBytes) {
            uint64_t srcMotTail = (uint64_t)cloneSrcPd->_refOffset + motHdrBytes;
            uint64_t dstMotTail = auxMotionOff + motHdrBytes;
            uint64_t availSrc = cloneSrcPd->_totalBytes - srcMotTail;
            uint64_t availDst = (pd->_totalBytes > dstMotTail) ? (pd->_totalBytes - dstMotTail) : 0u;
            uint64_t tailCopySz = MIN(availSrc, availDst);
            if (tailCopySz > 0u) {
                VkBufferCopy mcopy = { .srcOffset = srcMotTail, .dstOffset = dstMotTail, .size = tailCopySz };
                vkCmdCopyBuffer(cmd, cloneSrcPd->_buf, pd->_buf, 1u, &mcopy);
            }
        } else {
            for (uint32_t k = 0; k < plan->motionBlasDispatchCount; k++) {
                uint32_t gi = plan->motionBlasGeomIdx ? plan->motionBlasGeomIdx[k] : UINT32_MAX;
                uint32_t gk = (plan->motionBlasPCs[k].packed_flags >> 8u) & 0xFu;
                if (gk <= 1u && gi < plan->geomCount && motionGeomOffsets[gi] > 0u) {
                    plan->motionBlasPCs[k].payload_dst_addr = (uint64_t)pd->_bda + motionGeomOffsets[gi];
                }
            }
        }
        free(motionHdrs);
    }

    free(clonedPlans); free(curveAabbs); free(primOffsets); free(segOffsets); free(motionPayBytes); free(motionGeomOffsets);
    [cb rdna4RetainEncoded:pd];
    if (outPd) *outPd = pd;
    return YES;
}

GARNET_INTERNAL BOOL rdna4_rt_prepare_motion_tlas_sidecar(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb,
    RDNA4RTBuildPlan *plan, NSArray<GarnetASPrimData *> *referencedBlasPrimData,
    GarnetASPrimData **outPd, NSError **err)
{
    if (outPd) *outPd = nil;
    if (!plan || !plan->hasMotionTlas || plan->tlasInstanceCount == 0u) return YES;
    uint64_t hdrOff = 0u, instMetaOff = 0u, xformDataOff = 0u, totalBytes = 0u;
    rdna4rt_err_t le = rdna4rt_motion_tlas_layout_compute(
        plan->tlasInstanceCount, plan->motionTlasXformCount, plan->motionTlasXformType,
        &hdrOff, &instMetaOff, &xformDataOff, &totalBytes);
    if (le != RDNA4RT_OK || instMetaOff > UINT32_MAX) {
        if (err) *err = rdna4_err(146, [NSString stringWithUTF8String:rdna4rt_err_str(le != RDNA4RT_OK ? le : RDNA4RT_ERR_LIMIT)]);
        return NO;
    }
    GarnetASPrimData *pd = [[GarnetASPrimData alloc]
        initWithSize:totalBytes kind:RDNA4RT_PRIM_KIND_TLAS_MOTION count:plan->tlasInstanceCount
         entryStride:(uint32_t)sizeof(rdna4rt_motion_tlas_inst_t) refOffset:(uint32_t)instMetaOff error:err];
    if (!pd) return NO;
    if (referencedBlasPrimData.count > 0)
        pd->_referencedPrimData = [referencedBlasPrimData copy];
    rdna4rt_motion_tlas_hdr_t hdr = {
        .magic = RDNA4RT_MOTION_TLAS_MAGIC,
        .instance_count = plan->tlasInstanceCount,
        .xform_type = (uint32_t)plan->motionTlasXformType,
        .xform_count = plan->motionTlasXformCount,
        .inst_meta_off = instMetaOff,
        .xform_data_off = xformDataOff
    };
    if (!rdna4_rt_cmd_write_buf_bytes(cmd, cb, pd->_buf, hdrOff, &hdr, sizeof(hdr), err))
        return NO;
    uint64_t xfElem = (plan->motionTlasXformType == RDNA4RT_XFORM_COMPONENT)
        ? RDNA4RT_MTL_COMPONENT_TRANSFORM_SIZE : RDNA4RT_TRANSFORM_SIZE;
    uint64_t xformBytes = (uint64_t)plan->motionTlasXformCount * xfElem;
    if (xformBytes > 0u && plan->motionTlasXformVkBuf != VK_NULL_HANDLE) {
        VkBufferCopy xfCopy = {
            .srcOffset = plan->motionTlasXformVkOff,
            .dstOffset = xformDataOff,
            .size = xformBytes
        };
        vkCmdCopyBuffer(cmd, plan->motionTlasXformVkBuf, pd->_buf, 1u, &xfCopy);
    }
    plan->motionTlasPC.inst_meta_dst_addr = (uint64_t)pd->_bda + instMetaOff;
    [cb rdna4RetainEncoded:pd];
    if (outPd) *outPd = pd;
    return YES;
}

GARNET_INTERNAL BOOL rdna4_rt_clone_local_aabb(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb,
    GarnetAccelerationStructure *srcAS, GarnetAccelerationStructure *dstAS, NSError **err)
{
    if (!srcAS || !dstAS || srcAS == dstAS) return YES;
    GarnetASHandle *srcH = [srcAS rdna4CurrentHandle];
    if (!srcH || !srcH->_localAabbBuf) return YES;
    GarnetASHandle *dstH = [dstAS rdna4CurrentHandle];
    if (!dstH || ![dstH rdna4EnsureLocalAabbAddress:err] || !dstH->_localAabbBuf) return NO;
    [cb rdna4RetainEncoded:srcH->_localAabbBuf];
    [cb rdna4RetainEncoded:dstH->_localAabbBuf];
    VkBufferCopy c = { .srcOffset = 0u, .dstOffset = 0u, .size = 24u };
    vkCmdCopyBuffer(cmd, srcH->_localAabbBuf->_buf, dstH->_localAabbBuf->_buf, 1u, &c);
    return YES;
}

GARNET_INTERNAL BOOL rdna4_rt_record_tlas_prim_snapshot(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb,
    GarnetAccelerationStructure *dstAS, uint64_t dstAsBda,
    const RDNA4RTBuildPlan *plan, NSArray<GarnetASPrimData *> *referencedBlasPrimData,
    NSError **err)
{
    if (!plan->tlasInstStgBuf || plan->tlasInstanceCount == 0u)
        return [dstAS rdna4SetPrimData:nil forAsAddress:dstAsBda commandBuffer:cb vkCmdBuffer:cmd error:err];
    uint64_t totalBytes = 0u;
    if (!rdna4rt_mul_u64((uint64_t)plan->tlasInstanceCount, RDNA4RT_PRIM_TLAS_ENTRY_STRIDE, &totalBytes)) {
        if (err) *err = rdna4_err(146, @"TLAS primitive table snapshot size overflow");
        return NO;
    }
    GarnetASPrimData *pd = [[GarnetASPrimData alloc]
        initWithSize:totalBytes kind:RDNA4RT_PRIM_KIND_TLAS count:plan->tlasInstanceCount
         entryStride:RDNA4RT_PRIM_TLAS_ENTRY_STRIDE refOffset:RDNA4RT_PRIM_TLAS_REF_OFFSET error:err];
    if (!pd) return NO;
    if (referencedBlasPrimData.count > 0)
        pd->_referencedPrimData = [referencedBlasPrimData copy];
    VkBufferCopy copy = { .srcOffset = 0u, .dstOffset = 0u, .size = totalBytes };
    vkCmdCopyBuffer(cmd, plan->tlasInstStgBuf, pd->_buf, 1u, &copy);
    if (![dstAS rdna4SetPrimData:pd forAsAddress:dstAsBda commandBuffer:cb vkCmdBuffer:cmd error:err]) return NO;
    [cb rdna4RetainEncoded:pd];
    return YES;
}
GARNET_INTERNAL BOOL rdna4_rt_record_prim_data_clone(
    VkCommandBuffer cmd, RDNA4CommandBuffer *cb,
    GarnetAccelerationStructure *srcAS,
    GarnetAccelerationStructure *dstAS, uint64_t dstAsBda,
    NSError **err)
{
    if (!rdna4_rt_clone_local_aabb(cmd, cb, srcAS, dstAS, err)) return NO;
    GarnetASPrimData *srcPd = [srcAS rdna4CurrentPrimData];
    if (!srcPd)
        return [dstAS rdna4SetPrimData:nil forAsAddress:dstAsBda commandBuffer:cb vkCmdBuffer:cmd error:err];
    [cb rdna4RetainEncoded:srcPd];
    GarnetASPrimData *dstPd = [[GarnetASPrimData alloc]
        initWithSize:srcPd->_totalBytes kind:srcPd->_kind count:srcPd->_count
         entryStride:srcPd->_entryStride refOffset:srcPd->_refOffset error:err];
    if (!dstPd) return NO;
    if (srcPd->_referencedPrimData.count > 0)
        dstPd->_referencedPrimData = [srcPd->_referencedPrimData copy];

    if (srcPd->_kind == RDNA4RT_PRIM_KIND_BLAS) {
        uint64_t headerBytes = (uint64_t)srcPd->_count * (uint64_t)sizeof(rdna4rt_geom_prim_entry_t);
        if (!srcPd->_blasHeaders || headerBytes > srcPd->_totalBytes) {
            if (err) *err = rdna4_err(146, @"BLAS primitive data clone: missing or corrupt source headers");
            return NO;
        }
        rdna4rt_geom_prim_entry_t *rebased = (rdna4rt_geom_prim_entry_t *)calloc(srcPd->_count, sizeof(*rebased));
        if (!rebased) {
            if (err) *err = rdna4_err(146, @"out of host memory rebasing BLAS primitive headers");
            return NO;
        }
        rdna4rt_err_t re = rdna4rt_blas_prim_rebase_headers(
            srcPd->_blasHeaders, srcPd->_count,
            (uint64_t)srcPd->_bda, (uint64_t)dstPd->_bda, srcPd->_totalBytes,
            rebased);
        if (re != RDNA4RT_OK) {
            free(rebased);
            if (err) *err = rdna4_err(146, [NSString stringWithUTF8String:rdna4rt_err_str(re)]);
            return NO;
        }
        dstPd->_blasHeaders = rebased;
        if (!rdna4_rt_upload_prim_header(cmd, cb, dstPd->_buf, rebased, headerBytes, err))
            return NO;
        if (srcPd->_totalBytes > headerBytes) {
            VkBufferCopy tailCopy = {
                .srcOffset = headerBytes,
                .dstOffset = headerBytes,
                .size = srcPd->_totalBytes - headerBytes
            };
            vkCmdCopyBuffer(cmd, srcPd->_buf, dstPd->_buf, 1u, &tailCopy);
        }
    } else {
        VkBufferCopy fullCopy = { .srcOffset = 0u, .dstOffset = 0u, .size = srcPd->_totalBytes };
        vkCmdCopyBuffer(cmd, srcPd->_buf, dstPd->_buf, 1u, &fullCopy);
    }

    if (![dstAS rdna4SetPrimData:dstPd forAsAddress:dstAsBda commandBuffer:cb vkCmdBuffer:cmd error:err]) return NO;
    [cb rdna4RetainEncoded:dstPd];
    return YES;
}

