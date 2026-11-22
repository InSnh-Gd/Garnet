; ModuleID = 'fixtures/abi8/sampler-fragment-indirect.air'
source_filename = "quad_lod_no_bias"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

%struct.Args = type { %"struct.metal::array", %"struct.metal::array.0" }
%"struct.metal::array" = type { [2 x %"struct.metal::texture2d"] }
%"struct.metal::texture2d" = type { ptr addrspace(1) }
%"struct.metal::array.0" = type { [2 x %"struct.metal::sampler"] }
%"struct.metal::sampler" = type { ptr addrspace(2) }

; Function Attrs: convergent mustprogress nofree nounwind willreturn memory(read)
define <4 x float> @quad_lod_no_bias(<4 x float> %position, ptr addrspace(2) readonly align 8 captures(none) dereferenceable(32) "air-buffer-no-alias" %args, ptr addrspace(2) readonly align 4 captures(none) dereferenceable(4) "air-buffer-no-alias" %flip) local_unnamed_addr #0 {
entry:
  %0 = load i32, ptr addrspace(2) %flip, align 4, !tbaa !24, !alias.scope !28, !noalias !31
  %and = and i32 %0, 1
  %conv = zext i32 %and to i64
  %agg.tmp.sroa.0.0..sroa_idx = getelementptr inbounds %struct.Args, ptr addrspace(2) %args, i64 0, i32 1, i32 0, i64 %conv, i32 0
  %agg.tmp.sroa.0.0.copyload = load ptr addrspace(2), ptr addrspace(2) %agg.tmp.sroa.0.0..sroa_idx, align 8, !tbaa.struct !33, !alias.scope !31, !noalias !28
  %1 = shufflevector <4 x float> %position, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  %mul = fmul fast <2 x float> %1, splat (float 5.000000e-01)
  %t.i = getelementptr inbounds %struct.Args, ptr addrspace(2) %args, i64 0, i32 0, i32 0, i64 %conv, i32 0
  %2 = load ptr addrspace(1), ptr addrspace(2) %t.i, align 8, !tbaa !36, !alias.scope !31, !noalias !28
  %3 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %2, ptr addrspace(2) readonly captures(none) %agg.tmp.sroa.0.0.copyload, <2 x float> %mul, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #2
  %4 = extractvalue { <4 x float>, i8 } %3, 0
  ret <4 x float> %4
}

; Function Attrs: convergent mustprogress nofree nounwind willreturn memory(argmem: read)
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), <2 x float>, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr #1

attributes #0 = { convergent mustprogress nofree nounwind willreturn memory(read) "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { convergent mustprogress nofree nounwind willreturn memory(argmem: read) }
attributes #2 = { convergent nounwind willreturn memory(argmem: read) }

!llvm.module.flags = !{!0, !1, !2, !3, !4, !5, !6, !7}
!llvm.ident = !{!8}
!air.version = !{!9}
!air.language_version = !{!10}
!air.compile_options = !{!11, !12, !13}
!air.fragment = !{!14}

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
!14 = !{ptr @quad_lod_no_bias, !15, !17}
!15 = !{!16}
!16 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!17 = !{!18, !19, !23}
!18 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!19 = !{i32 1, !"air.indirect_buffer", !"air.buffer_size", i32 32, !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !20, !"air.arg_type_size", i32 32, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Args", !"air.arg_name", !"args"}
!20 = !{i32 0, i32 16, i32 0, !"array<texture2d<float, sample>, 2>", !"images", !"air.indirect_argument", !21, i32 16, i32 16, i32 0, !"array<sampler, 2>", !"filters", !"air.indirect_argument", !22}
!21 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 2, !"air.sample", !"air.arg_type_name", !"array<texture2d<float, sample>, 2>", !"air.arg_name", !"images"}
!22 = !{i32 1, !"air.sampler", !"air.location_index", i32 2, i32 2, !"air.arg_type_name", !"array<sampler, 2>", !"air.arg_name", !"filters"}
!23 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"flip"}
!24 = !{!25, !25, i64 0}
!25 = !{!"int", !26, i64 0}
!26 = !{!"omnipotent char", !27, i64 0}
!27 = !{!"Simple C++ TBAA"}
!28 = !{!29}
!29 = distinct !{!29, !30, !"air-alias-scope-arg(2)"}
!30 = distinct !{!30, !"air-alias-scopes(quad_lod_no_bias)"}
!31 = !{!32}
!32 = distinct !{!32, !30, !"air-alias-scope-arg(1)"}
!33 = !{i64 0, i64 8, !34}
!34 = !{!35, !35, i64 0}
!35 = !{!"__metal_sampler_t", !26, i64 0}
!36 = !{!37, !38, i64 0}
!37 = !{!"_ZTSN5metal9texture2dIfLNS_6accessE0EvEE", !38, i64 0}
!38 = !{!"__metal_texture_2d_t", !26, i64 0}
