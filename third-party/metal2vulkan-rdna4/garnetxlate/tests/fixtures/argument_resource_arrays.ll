; ModuleID = 'reference/argument-arrays.air'
source_filename = "argument_arrays"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.6.0"

%struct.Resources = type { %"struct.metal::array", %"struct.metal::array.0", %"struct.metal::array.1" }
%"struct.metal::array" = type { [2 x %"struct.metal::texture2d"] }
%"struct.metal::texture2d" = type { ptr addrspace(1) }
%"struct.metal::array.0" = type { [2 x %"struct.metal::sampler"] }
%"struct.metal::sampler" = type { ptr addrspace(2) }
%"struct.metal::array.1" = type { [2 x ptr addrspace(1)] }

; Function Attrs: convergent mustprogress nofree nounwind willreturn
define void @argument_arrays(ptr addrspace(2) readonly align 8 captures(none) dereferenceable(48) "air-buffer-no-alias" %r, ptr addrspace(1) writeonly captures(none) "air-buffer-no-alias" %out, ptr addrspace(2) readonly align 4 captures(none) dereferenceable(4) "air-buffer-no-alias" %flip, i32 %i) local_unnamed_addr #0 {
entry:
  %0 = load i32, ptr addrspace(2) %flip, align 4, !tbaa !26, !alias.scope !30, !noalias !33
  %xor = xor i32 %0, %i
  %and = and i32 %xor, 1
  %shr = lshr i32 %i, 1
  %and1 = and i32 %shr, 1
  %conv = zext i32 %and to i64
  %conv2 = zext i32 %and1 to i64
  %agg.tmp.sroa.0.0..sroa_idx = getelementptr inbounds %struct.Resources, ptr addrspace(2) %r, i64 0, i32 1, i32 0, i64 %conv2, i32 0
  %agg.tmp.sroa.0.0.copyload = load ptr addrspace(2), ptr addrspace(2) %agg.tmp.sroa.0.0..sroa_idx, align 8, !tbaa.struct !36, !alias.scope !39, !noalias !40
  %t.i = getelementptr inbounds %struct.Resources, ptr addrspace(2) %r, i64 0, i32 0, i32 0, i64 %conv, i32 0
  %1 = load ptr addrspace(1), ptr addrspace(2) %t.i, align 8, !tbaa !41, !alias.scope !39, !noalias !40
  %2 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %1, ptr addrspace(2) readonly captures(none) %agg.tmp.sroa.0.0.copyload, <2 x float> <float 1.250000e+00, float 5.000000e-01>, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0) #3
  %3 = extractvalue { <4 x float>, i8 } %2, 0
  %4 = extractelement <4 x float> %3, i64 0
  %5 = tail call i32 @air.convert.u.i32.f.f32(float %4) #4
  %arrayidx.i21 = getelementptr inbounds %struct.Resources, ptr addrspace(2) %r, i64 0, i32 2, i32 0, i64 %conv
  %6 = load ptr addrspace(1), ptr addrspace(2) %arrayidx.i21, align 8, !tbaa !44, !alias.scope !39, !noalias !40
  %idxprom = zext i32 %i to i64
  %arrayidx = getelementptr inbounds i32, ptr addrspace(1) %6, i64 %idxprom
  %7 = load i32, ptr addrspace(1) %arrayidx, align 4, !tbaa !26
  %add = add i32 %7, %5
  %arrayidx9 = getelementptr inbounds i32, ptr addrspace(1) %out, i64 %idxprom
  store i32 %add, ptr addrspace(1) %arrayidx9, align 4, !tbaa !26, !alias.scope !46, !noalias !47
  ret void
}

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare i32 @air.convert.u.i32.f.f32(float) local_unnamed_addr #1

; Function Attrs: convergent mustprogress nofree nounwind willreturn memory(argmem: read)
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), <2 x float>, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr #2

attributes #0 = { convergent mustprogress nofree nounwind willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { mustprogress nofree nosync nounwind willreturn memory(none) }
attributes #2 = { convergent mustprogress nofree nounwind willreturn memory(argmem: read) }
attributes #3 = { convergent nounwind willreturn memory(argmem: read) }
attributes #4 = { nounwind willreturn memory(none) }

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
!14 = !{ptr @argument_arrays, !15, !16}
!15 = !{}
!16 = !{!17, !23, !24, !25}
!17 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 48, !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !18, !"air.arg_type_size", i32 48, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Resources", !"air.arg_name", !"r"}
!18 = !{i32 0, i32 16, i32 0, !"array<texture2d<float, sample>, 2>", !"images", !"air.indirect_argument", !19, i32 16, i32 16, i32 0, !"array<sampler, 2>", !"filters", !"air.indirect_argument", !20, !"air.struct_type_info", !21, i32 32, i32 16, i32 0, !"metal::array", !"values", !"air.indirect_argument", i32 11}
!19 = !{i32 0, !"air.texture", !"air.location_index", i32 2, i32 2, !"air.sample", !"air.arg_type_name", !"array<texture2d<float, sample>, 2>", !"air.arg_name", !"images"}
!20 = !{i32 1, !"air.sampler", !"air.location_index", i32 7, i32 2, !"air.arg_type_name", !"array<sampler, 2>", !"air.arg_name", !"filters"}
!21 = !{i32 0, i32 8, i32 2, !"uint", !"__elems", !"air.indirect_argument", !22}
!22 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"__elems"}
!23 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!24 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"flip"}
!25 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"i"}
!26 = !{!27, !27, i64 0}
!27 = !{!"int", !28, i64 0}
!28 = !{!"omnipotent char", !29, i64 0}
!29 = !{!"Simple C++ TBAA"}
!30 = !{!31}
!31 = distinct !{!31, !32, !"air-alias-scope-arg(2)"}
!32 = distinct !{!32, !"air-alias-scopes(argument_arrays)"}
!33 = !{!34, !35}
!34 = distinct !{!34, !32, !"air-alias-scope-arg(0)"}
!35 = distinct !{!35, !32, !"air-alias-scope-arg(1)"}
!36 = !{i64 0, i64 8, !37}
!37 = !{!38, !38, i64 0}
!38 = !{!"__metal_sampler_t", !28, i64 0}
!39 = !{!34}
!40 = !{!35, !31}
!41 = !{!42, !43, i64 0}
!42 = !{!"_ZTSN5metal9texture2dIfLNS_6accessE0EvEE", !43, i64 0}
!43 = !{!"__metal_texture_2d_t", !28, i64 0}
!44 = !{!45, !45, i64 0}
!45 = !{!"any pointer", !28, i64 0}
!46 = !{!35}
!47 = !{!34, !31}
