; ModuleID = 'fixtures/abi8/reference-air/sampler_coordinates_gather.air'
source_filename = "sampler_coordinates_gather"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

; Function Attrs: mustprogress nofree nounwind
define void @sampler_coordinates_gather(ptr addrspace(1) %image.coerce, ptr addrspace(2) readonly captures(none) %pixelSampler.coerce, ptr addrspace(2) readonly captures(none) %normalizedSampler.coerce, ptr addrspace(1) writeonly captures(none) "air-buffer-no-alias" %out) local_unnamed_addr #0 {
entry:
  br label %for.body

for.cond.cleanup:                                 ; preds = %_Z21coordinate_normalizedj.exit
  ret void

for.body:                                         ; preds = %_Z21coordinate_normalizedj.exit, %entry
  %i.057 = phi i32 [ 0, %entry ], [ %inc, %_Z21coordinate_normalizedj.exit ]
  switch i32 %i.057, label %sw.default.i [
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
  %0 = tail call { <4 x float>, i8 } @air.gather_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %image.coerce, ptr addrspace(2) readonly captures(none) %pixelSampler.coerce, <2 x float> %retval.0.i, i1 true, <2 x i32> zeroinitializer, i32 0, i32 0) #2, !alias.scope !21, !noalias !25
  %1 = extractvalue { <4 x float>, i8 } %0, 0
  switch i32 %i.057, label %sw.default.i.i [
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
  %2 = tail call { <4 x float>, i8 } @air.gather_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %image.coerce, ptr addrspace(2) readonly captures(none) %normalizedSampler.coerce, <2 x float> %retval.0.i.i, i1 true, <2 x i32> zeroinitializer, i32 0, i32 0) #2, !alias.scope !21, !noalias !25
  %3 = extractvalue { <4 x float>, i8 } %2, 0
  %mul = shl nuw nsw i32 %i.057, 3
  %4 = extractelement <4 x float> %1, i64 0
  %idxprom = zext i32 %mul to i64
  %arrayidx = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom
  store float %4, ptr addrspace(1) %arrayidx, align 4, !tbaa !27, !alias.scope !25, !noalias !21
  %5 = extractelement <4 x float> %1, i64 1
  %add7 = or i32 %mul, 1
  %idxprom8 = zext i32 %add7 to i64
  %arrayidx9 = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom8
  store float %5, ptr addrspace(1) %arrayidx9, align 4, !tbaa !27, !alias.scope !25, !noalias !21
  %6 = extractelement <4 x float> %1, i64 2
  %add10 = or i32 %mul, 2
  %idxprom11 = zext i32 %add10 to i64
  %arrayidx12 = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom11
  store float %6, ptr addrspace(1) %arrayidx12, align 4, !tbaa !27, !alias.scope !25, !noalias !21
  %7 = extractelement <4 x float> %1, i64 3
  %add13 = or i32 %mul, 3
  %idxprom14 = zext i32 %add13 to i64
  %arrayidx15 = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom14
  store float %7, ptr addrspace(1) %arrayidx15, align 4, !tbaa !27, !alias.scope !25, !noalias !21
  %8 = extractelement <4 x float> %3, i64 0
  %add16 = or i32 %mul, 4
  %idxprom17 = zext i32 %add16 to i64
  %arrayidx18 = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom17
  store float %8, ptr addrspace(1) %arrayidx18, align 4, !tbaa !27, !alias.scope !25, !noalias !21
  %9 = extractelement <4 x float> %3, i64 1
  %add19 = or i32 %mul, 5
  %idxprom20 = zext i32 %add19 to i64
  %arrayidx21 = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom20
  store float %9, ptr addrspace(1) %arrayidx21, align 4, !tbaa !27, !alias.scope !25, !noalias !21
  %10 = extractelement <4 x float> %3, i64 2
  %add22 = or i32 %mul, 6
  %idxprom23 = zext i32 %add22 to i64
  %arrayidx24 = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom23
  store float %10, ptr addrspace(1) %arrayidx24, align 4, !tbaa !27, !alias.scope !25, !noalias !21
  %11 = extractelement <4 x float> %3, i64 3
  %add25 = or i32 %mul, 7
  %idxprom26 = zext i32 %add25 to i64
  %arrayidx27 = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom26
  store float %11, ptr addrspace(1) %arrayidx27, align 4, !tbaa !27, !alias.scope !25, !noalias !21
  %inc = add nuw nsw i32 %i.057, 1
  %exitcond.not = icmp eq i32 %inc, 10
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body, !llvm.loop !31
}

; Function Attrs: mustprogress nofree nounwind willreturn memory(argmem: read)
declare { <4 x float>, i8 } @air.gather_texture_2d.v4f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), <2 x float>, i1, <2 x i32>, i32, i32) local_unnamed_addr #1

attributes #0 = { mustprogress nofree nounwind "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { mustprogress nofree nounwind willreturn memory(argmem: read) }
attributes #2 = { nounwind willreturn memory(argmem: read) }

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
!14 = !{ptr @sampler_coordinates_gather, !15, !16}
!15 = !{}
!16 = !{!17, !18, !19, !20}
!17 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"image"}
!18 = !{i32 1, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"pixelSampler"}
!19 = !{i32 2, !"air.sampler", !"air.location_index", i32 1, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"normalizedSampler"}
!20 = !{i32 3, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!21 = !{!22, !24}
!22 = distinct !{!22, !23, !"air-alias-scope-textures"}
!23 = distinct !{!23, !"air-alias-scopes(sampler_coordinates_gather)"}
!24 = distinct !{!24, !23, !"air-alias-scope-samplers"}
!25 = !{!26}
!26 = distinct !{!26, !23, !"air-alias-scope-arg(3)"}
!27 = !{!28, !28, i64 0}
!28 = !{!"float", !29, i64 0}
!29 = !{!"omnipotent char", !30, i64 0}
!30 = !{!"Simple C++ TBAA"}
!31 = distinct !{!31, !32}
!32 = !{!"llvm.loop.mustprogress"}
