; ModuleID = 'fixtures/abi8/reference-air/sampler_coordinates_compare.air'
source_filename = "sampler_coordinates_compare"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

; Function Attrs: convergent nofree nounwind
define void @sampler_coordinates_compare(ptr addrspace(1) %depth.coerce, ptr addrspace(2) readonly captures(none) %pixelNever.coerce, ptr addrspace(2) readonly captures(none) %normalizedNever.coerce, ptr addrspace(2) readonly captures(none) %pixelAlways.coerce, ptr addrspace(2) readonly captures(none) %normalizedAlways.coerce, ptr addrspace(2) readonly captures(none) %pixelLess.coerce, ptr addrspace(2) readonly captures(none) %normalizedLess.coerce, ptr addrspace(1) writeonly captures(none) "air-buffer-no-alias" %out) local_unnamed_addr #0 {
entry:
  br label %for.body

for.cond.cleanup:                                 ; preds = %_Z21coordinate_normalizedj.exit
  ret void

for.body:                                         ; preds = %_Z21coordinate_normalizedj.exit, %entry
  %i.077 = phi i32 [ 0, %entry ], [ %inc, %_Z21coordinate_normalizedj.exit ]
  switch i32 %i.077, label %sw.default.i [
    i32 0, label %_Z16coordinate_pixelj.exit
    i32 1, label %sw.bb1.i
    i32 2, label %sw.bb2.i
    i32 3, label %sw.bb3.i
    i32 4, label %sw.bb4.i
    i32 5, label %sw.bb5.i
    i32 6, label %sw.bb6.i
    i32 7, label %sw.bb7.i
    i32 8, label %sw.bb8.i
  ]

sw.bb1.i:                                         ; preds = %for.body
  br label %_Z16coordinate_pixelj.exit

sw.bb2.i:                                         ; preds = %for.body
  br label %_Z16coordinate_pixelj.exit

sw.bb3.i:                                         ; preds = %for.body
  br label %_Z16coordinate_pixelj.exit

sw.bb4.i:                                         ; preds = %for.body
  br label %_Z16coordinate_pixelj.exit

sw.bb5.i:                                         ; preds = %for.body
  br label %_Z16coordinate_pixelj.exit

sw.bb6.i:                                         ; preds = %for.body
  br label %_Z16coordinate_pixelj.exit

sw.bb7.i:                                         ; preds = %for.body
  br label %_Z16coordinate_pixelj.exit

sw.bb8.i:                                         ; preds = %for.body
  br label %_Z16coordinate_pixelj.exit

sw.default.i:                                     ; preds = %for.body
  br label %_Z16coordinate_pixelj.exit

_Z16coordinate_pixelj.exit:                       ; preds = %sw.default.i, %sw.bb8.i, %sw.bb7.i, %sw.bb6.i, %sw.bb5.i, %sw.bb4.i, %sw.bb3.i, %sw.bb2.i, %sw.bb1.i, %for.body
  %retval.0.i = phi <2 x float> [ <float 5.500000e+00, float 6.500000e+00>, %sw.default.i ], [ <float -1.000000e+00, float -2.000000e+00>, %sw.bb8.i ], [ splat (float -5.000000e-01), %sw.bb7.i ], [ splat (float 4.000000e+00), %sw.bb6.i ], [ <float 2.750000e+00, float 1.250000e+00>, %sw.bb5.i ], [ <float 1.250000e+00, float 2.000000e+00>, %sw.bb4.i ], [ splat (float 1.000000e+00), %sw.bb3.i ], [ splat (float 3.500000e+00), %sw.bb2.i ], [ <float 1.500000e+00, float 2.500000e+00>, %sw.bb1.i ], [ splat (float 5.000000e-01), %for.body ]
  switch i32 %i.077, label %sw.default.i.i [
    i32 0, label %_Z21coordinate_normalizedj.exit
    i32 1, label %sw.bb1.i.i
    i32 2, label %sw.bb2.i.i
    i32 3, label %sw.bb3.i.i
    i32 4, label %sw.bb4.i.i
    i32 5, label %sw.bb5.i.i
    i32 6, label %sw.bb6.i.i
    i32 7, label %sw.bb7.i.i
    i32 8, label %sw.bb8.i.i
  ]

sw.bb1.i.i:                                       ; preds = %_Z16coordinate_pixelj.exit
  br label %_Z21coordinate_normalizedj.exit

sw.bb2.i.i:                                       ; preds = %_Z16coordinate_pixelj.exit
  br label %_Z21coordinate_normalizedj.exit

sw.bb3.i.i:                                       ; preds = %_Z16coordinate_pixelj.exit
  br label %_Z21coordinate_normalizedj.exit

sw.bb4.i.i:                                       ; preds = %_Z16coordinate_pixelj.exit
  br label %_Z21coordinate_normalizedj.exit

sw.bb5.i.i:                                       ; preds = %_Z16coordinate_pixelj.exit
  br label %_Z21coordinate_normalizedj.exit

sw.bb6.i.i:                                       ; preds = %_Z16coordinate_pixelj.exit
  br label %_Z21coordinate_normalizedj.exit

sw.bb7.i.i:                                       ; preds = %_Z16coordinate_pixelj.exit
  br label %_Z21coordinate_normalizedj.exit

sw.bb8.i.i:                                       ; preds = %_Z16coordinate_pixelj.exit
  br label %_Z21coordinate_normalizedj.exit

sw.default.i.i:                                   ; preds = %_Z16coordinate_pixelj.exit
  br label %_Z21coordinate_normalizedj.exit

_Z21coordinate_normalizedj.exit:                  ; preds = %sw.default.i.i, %sw.bb8.i.i, %sw.bb7.i.i, %sw.bb6.i.i, %sw.bb5.i.i, %sw.bb4.i.i, %sw.bb3.i.i, %sw.bb2.i.i, %sw.bb1.i.i, %_Z16coordinate_pixelj.exit
  %retval.0.i.i = phi <2 x float> [ <float 1.375000e+00, float 1.625000e+00>, %sw.default.i.i ], [ <float -2.500000e-01, float -5.000000e-01>, %sw.bb8.i.i ], [ splat (float -1.250000e-01), %sw.bb7.i.i ], [ splat (float 1.000000e+00), %sw.bb6.i.i ], [ <float 6.875000e-01, float 3.125000e-01>, %sw.bb5.i.i ], [ <float 3.125000e-01, float 5.000000e-01>, %sw.bb4.i.i ], [ splat (float 2.500000e-01), %sw.bb3.i.i ], [ splat (float 8.750000e-01), %sw.bb2.i.i ], [ <float 3.750000e-01, float 6.250000e-01>, %sw.bb1.i.i ], [ splat (float 1.250000e-01), %_Z16coordinate_pixelj.exit ]
  %mul = mul nuw nsw i32 %i.077, 6
  %0 = tail call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) readonly captures(none) %depth.coerce, ptr addrspace(2) readonly captures(none) %pixelNever.coerce, i32 1, <2 x float> %retval.0.i, float 5.000000e-01, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0) #2, !alias.scope !25, !noalias !29
  %1 = extractvalue { float, i8 } %0, 0
  %idxprom = zext i32 %mul to i64
  %arrayidx = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom
  store float %1, ptr addrspace(1) %arrayidx, align 4, !tbaa !31, !alias.scope !29, !noalias !25
  %2 = tail call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) readonly captures(none) %depth.coerce, ptr addrspace(2) readonly captures(none) %normalizedNever.coerce, i32 1, <2 x float> %retval.0.i.i, float 5.000000e-01, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0) #2, !alias.scope !25, !noalias !29
  %3 = extractvalue { float, i8 } %2, 0
  %add13 = or i32 %mul, 1
  %idxprom14 = zext i32 %add13 to i64
  %arrayidx15 = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom14
  store float %3, ptr addrspace(1) %arrayidx15, align 4, !tbaa !31, !alias.scope !29, !noalias !25
  %4 = tail call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) readonly captures(none) %depth.coerce, ptr addrspace(2) readonly captures(none) %pixelAlways.coerce, i32 1, <2 x float> %retval.0.i, float 5.000000e-01, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0) #2, !alias.scope !25, !noalias !29
  %5 = extractvalue { float, i8 } %4, 0
  %add19 = add nuw nsw i32 %mul, 2
  %idxprom20 = zext i32 %add19 to i64
  %arrayidx21 = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom20
  store float %5, ptr addrspace(1) %arrayidx21, align 4, !tbaa !31, !alias.scope !29, !noalias !25
  %6 = tail call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) readonly captures(none) %depth.coerce, ptr addrspace(2) readonly captures(none) %normalizedAlways.coerce, i32 1, <2 x float> %retval.0.i.i, float 5.000000e-01, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0) #2, !alias.scope !25, !noalias !29
  %7 = extractvalue { float, i8 } %6, 0
  %add25 = add nuw nsw i32 %mul, 3
  %idxprom26 = zext i32 %add25 to i64
  %arrayidx27 = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom26
  store float %7, ptr addrspace(1) %arrayidx27, align 4, !tbaa !31, !alias.scope !29, !noalias !25
  %8 = tail call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) readonly captures(none) %depth.coerce, ptr addrspace(2) readonly captures(none) %pixelLess.coerce, i32 1, <2 x float> %retval.0.i, float 5.000000e-01, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0) #2, !alias.scope !25, !noalias !29
  %9 = extractvalue { float, i8 } %8, 0
  %add31 = add nuw nsw i32 %mul, 4
  %idxprom32 = zext i32 %add31 to i64
  %arrayidx33 = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom32
  store float %9, ptr addrspace(1) %arrayidx33, align 4, !tbaa !31, !alias.scope !29, !noalias !25
  %10 = tail call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) readonly captures(none) %depth.coerce, ptr addrspace(2) readonly captures(none) %normalizedLess.coerce, i32 1, <2 x float> %retval.0.i.i, float 5.000000e-01, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0) #2, !alias.scope !25, !noalias !29
  %11 = extractvalue { float, i8 } %10, 0
  %add37 = add nuw nsw i32 %mul, 5
  %idxprom38 = zext i32 %add37 to i64
  %arrayidx39 = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom38
  store float %11, ptr addrspace(1) %arrayidx39, align 4, !tbaa !31, !alias.scope !29, !noalias !25
  %inc = add nuw nsw i32 %i.077, 1
  %exitcond.not = icmp eq i32 %inc, 10
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body, !llvm.loop !35
}

; Function Attrs: convergent mustprogress nofree nounwind willreturn memory(argmem: read)
declare { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), i32, <2 x float>, float, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr #1

attributes #0 = { convergent nofree nounwind "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="64" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { convergent mustprogress nofree nounwind willreturn memory(argmem: read) }
attributes #2 = { convergent nounwind willreturn memory(argmem: read) }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7}
!llvm.ident = !{!8}
!air.version = !{!9}
!air.language_version = !{!10}
!air.compile_options = !{!11, !12, !13}
!air.kernel = !{!14}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 7, !"frame-pointer", i32 2}
!2 = !{i32 7, !"air.max_device_buffers", i32 31}
!3 = !{i32 7, !"air.max_constant_buffers", i32 31}
!4 = !{i32 7, !"air.max_threadgroup_buffers", i32 31}
!5 = !{i32 7, !"air.max_textures", i32 128}
!6 = !{i32 7, !"air.max_read_write_textures", i32 8}
!7 = !{i32 7, !"air.max_samplers", i32 16}
!8 = !{!"Apple metal version 32023.886 (metalfe-32023.886.1)"}
!9 = !{i32 2, i32 8, i32 0}
!10 = !{!"Metal", i32 2, i32 1, i32 0}
!11 = !{!"air.compile.denorms_disable"}
!12 = !{!"air.compile.fast_math_enable"}
!13 = !{!"air.compile.framebuffer_fetch_disable"}
!14 = !{ptr @sampler_coordinates_compare, !15, !16}
!15 = !{}
!16 = !{!17, !18, !19, !20, !21, !22, !23, !24}
!17 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"depth2d<float, sample>", !"air.arg_name", !"depth"}
!18 = !{i32 1, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"pixelNever"}
!19 = !{i32 2, !"air.sampler", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"normalizedNever"}
!20 = !{i32 3, !"air.sampler", !"air.location_index", i32 2, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"pixelAlways"}
!21 = !{i32 4, !"air.sampler", !"air.location_index", i32 3, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"normalizedAlways"}
!22 = !{i32 5, !"air.sampler", !"air.location_index", i32 4, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"pixelLess"}
!23 = !{i32 6, !"air.sampler", !"air.location_index", i32 5, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"normalizedLess"}
!24 = !{i32 7, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!25 = !{!26, !28}
!26 = distinct !{!26, !27, !"air-alias-scope-textures"}
!27 = distinct !{!27, !"air-alias-scopes(sampler_coordinates_compare)"}
!28 = distinct !{!28, !27, !"air-alias-scope-samplers"}
!29 = !{!30}
!30 = distinct !{!30, !27, !"air-alias-scope-arg(7)"}
!31 = !{!32, !32, i64 0}
!32 = !{!"float", !33, i64 0}
!33 = !{!"omnipotent char", !34, i64 0}
!34 = !{!"Simple C++ TBAA"}
!35 = distinct !{!35, !36}
!36 = !{!"llvm.loop.mustprogress"}
