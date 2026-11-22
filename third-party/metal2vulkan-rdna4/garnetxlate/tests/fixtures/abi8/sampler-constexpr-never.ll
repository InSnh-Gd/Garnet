; ModuleID = 'fixtures/abi8/sampler-constexpr-never.air'
source_filename = "sampler_constexpr_never"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

@__air_sampler_state = internal addrspace(2) constant [2 x i64] [i64 34901797601017929, i64 0], align 8

; Function Attrs: convergent mustprogress nofree nounwind willreturn memory(argmem: readwrite)
define void @sampler_constexpr_never(ptr addrspace(1) readonly captures(none) %color.coerce, ptr addrspace(1) readonly captures(none) %depth.coerce, ptr addrspace(1) writeonly captures(none) "air-buffer-no-alias" %out) local_unnamed_addr #0 {
entry:
  %0 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %color.coerce, ptr addrspace(2) readonly captures(none) @__air_sampler_state, <2 x float> splat (float 5.000000e-01), i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #2
  %1 = extractvalue { <4 x float>, i8 } %0, 0
  %2 = extractelement <4 x float> %1, i64 0
  store float %2, ptr addrspace(1) %out, align 4, !tbaa !21, !alias.scope !25, !noalias !28
  %3 = tail call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) readonly captures(none) %depth.coerce, ptr addrspace(2) readonly captures(none) @__air_sampler_state, i32 1, <2 x float> splat (float 2.500000e-01), float 5.000000e-01, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0) #2
  %4 = extractvalue { float, i8 } %3, 0
  %arrayidx6 = getelementptr inbounds float, ptr addrspace(1) %out, i64 1
  store float %4, ptr addrspace(1) %arrayidx6, align 4, !tbaa !21, !alias.scope !25, !noalias !28
  ret void
}

; Function Attrs: convergent mustprogress nofree nounwind willreturn memory(argmem: read)
declare { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), i32, <2 x float>, float, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr #1

; Function Attrs: convergent mustprogress nofree nounwind willreturn memory(argmem: read)
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), <2 x float>, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr #1

attributes #0 = { convergent mustprogress nofree nounwind willreturn memory(argmem: readwrite) "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { convergent mustprogress nofree nounwind willreturn memory(argmem: read) }
attributes #2 = { convergent nounwind willreturn memory(argmem: read) }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7}
!llvm.ident = !{!8}
!air.version = !{!9}
!air.language_version = !{!10}
!air.compile_options = !{!11, !12, !13}
!air.kernel = !{!14}
!air.sampler_states = !{!20}

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
!14 = !{ptr @sampler_constexpr_never, !15, !16}
!15 = !{}
!16 = !{!17, !18, !19}
!17 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"color"}
!18 = !{i32 1, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.sample", !"air.arg_type_name", !"depth2d<float, sample>", !"air.arg_name", !"depth"}
!19 = !{i32 2, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!20 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state}
!21 = !{!22, !22, i64 0}
!22 = !{!"float", !23, i64 0}
!23 = !{!"omnipotent char", !24, i64 0}
!24 = !{!"Simple C++ TBAA"}
!25 = !{!26}
!26 = distinct !{!26, !27, !"air-alias-scope-arg(2)"}
!27 = distinct !{!27, !"air-alias-scopes(sampler_constexpr_never)"}
!28 = !{!29}
!29 = distinct !{!29, !27, !"air-alias-scope-textures"}
