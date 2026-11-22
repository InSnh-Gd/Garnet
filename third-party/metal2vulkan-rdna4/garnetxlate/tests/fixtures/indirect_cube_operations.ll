; Authored test-argument-image-operations.m, color-cube-0; MSL 2.1.
source_filename = "image_ops"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

%struct.Args = type { %"struct.metal::array", %"struct.metal::array", %"struct.metal::sampler" }
%"struct.metal::array" = type { [2 x %"struct.metal::texturecube"] }
%"struct.metal::texturecube" = type { ptr addrspace(1) }
%"struct.metal::sampler" = type { ptr addrspace(2) }

; Function Attrs: convergent mustprogress nounwind willreturn
define void @image_ops(ptr addrspace(2) readonly align 8 captures(none) dereferenceable(40) "air-buffer-no-alias" %a, ptr addrspace(1) writeonly captures(none) "air-buffer-no-alias" %out, ptr addrspace(2) readonly align 4 captures(none) dereferenceable(4) "air-buffer-no-alias" %select) local_unnamed_addr #0 {
entry:
  %0 = load i32, ptr addrspace(2) %select, align 4, !tbaa !24, !alias.scope !28, !noalias !31
  %and = and i32 %0, 1
  %conv = zext i32 %and to i64
  %t.i = getelementptr inbounds %struct.Args, ptr addrspace(2) %a, i64 0, i32 0, i32 0, i64 %conv, i32 0
  %1 = load ptr addrspace(1), ptr addrspace(2) %t.i, align 8, !tbaa !34, !alias.scope !37, !noalias !38
  %2 = tail call ptr addrspace(2) @air.get_read_sampler() #6
  %3 = tail call { <4 x float>, i8 } @air.read_texture_cube.v4f32(ptr addrspace(1) readonly captures(none) %1, ptr addrspace(2) %2, <2 x i32> <i32 2, i32 1>, i32 2, i32 0, i32 0) #7
  %4 = extractvalue { <4 x float>, i8 } %3, 0
  %5 = extractelement <4 x float> %4, i64 0
  store float %5, ptr addrspace(1) %out, align 4, !tbaa !39, !alias.scope !41, !noalias !42
  %agg.tmp.sroa.0.0..sroa_idx = getelementptr inbounds %struct.Args, ptr addrspace(2) %a, i64 0, i32 2, i32 0
  %agg.tmp.sroa.0.0.copyload = load ptr addrspace(2), ptr addrspace(2) %agg.tmp.sroa.0.0..sroa_idx, align 8, !tbaa.struct !43, !alias.scope !37, !noalias !38
  %6 = tail call { <4 x float>, i8 } @air.sample_texture_cube.v4f32(ptr addrspace(1) readonly captures(none) %1, ptr addrspace(2) readonly captures(none) %agg.tmp.sroa.0.0.copyload, <3 x float> <float 2.500000e-01, float 1.000000e+00, float -2.500000e-01>, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0) #8
  %7 = extractvalue { <4 x float>, i8 } %6, 0
  %8 = extractelement <4 x float> %7, i64 0
  %arrayidx8 = getelementptr inbounds float, ptr addrspace(1) %out, i64 1
  store float %8, ptr addrspace(1) %arrayidx8, align 4, !tbaa !39, !alias.scope !41, !noalias !42
  %9 = tail call { <4 x float>, i8 } @air.sample_texture_cube_grad.v4f32(ptr addrspace(1) readonly captures(none) %1, ptr addrspace(2) readonly captures(none) %agg.tmp.sroa.0.0.copyload, <3 x float> <float 2.500000e-01, float 1.000000e+00, float -2.500000e-01>, <3 x float> <float 1.000000e-03, float 2.000000e-03, float 3.000000e-03>, <3 x float> <float 3.000000e-03, float 1.000000e-03, float 2.000000e-03>, float 0.000000e+00, i32 0) #7
  %10 = extractvalue { <4 x float>, i8 } %9, 0
  %11 = extractelement <4 x float> %10, i64 0
  %arrayidx17 = getelementptr inbounds float, ptr addrspace(1) %out, i64 2
  store float %11, ptr addrspace(1) %arrayidx17, align 4, !tbaa !39, !alias.scope !41, !noalias !42
  %arrayidx18 = getelementptr inbounds float, ptr addrspace(1) %out, i64 3
  store float 0.000000e+00, ptr addrspace(1) %arrayidx18, align 4, !tbaa !39, !alias.scope !41, !noalias !42
  %12 = tail call i32 @air.get_width_texture_cube(ptr addrspace(1) readonly captures(none) %1, i32 0) #7
  %13 = tail call fast float @air.convert.f.f32.u.i32(i32 %12) #9
  %arrayidx23 = getelementptr inbounds float, ptr addrspace(1) %out, i64 4
  store float %13, ptr addrspace(1) %arrayidx23, align 4, !tbaa !39, !alias.scope !41, !noalias !42
  %14 = tail call i32 @air.get_height_texture_cube(ptr addrspace(1) readonly captures(none) %1, i32 0) #7
  %15 = tail call fast float @air.convert.f.f32.u.i32(i32 %14) #9
  %arrayidx28 = getelementptr inbounds float, ptr addrspace(1) %out, i64 5
  store float %15, ptr addrspace(1) %arrayidx28, align 4, !tbaa !39, !alias.scope !41, !noalias !42
  %arrayidx29 = getelementptr inbounds float, ptr addrspace(1) %out, i64 6
  store float 1.000000e+00, ptr addrspace(1) %arrayidx29, align 4, !tbaa !39, !alias.scope !41, !noalias !42
  %16 = tail call i32 @air.get_num_mip_levels_texture_cube(ptr addrspace(1) readonly captures(none) %1) #7
  %17 = tail call fast float @air.convert.f.f32.u.i32(i32 %16) #9
  %arrayidx34 = getelementptr inbounds float, ptr addrspace(1) %out, i64 7
  store float %17, ptr addrspace(1) %arrayidx34, align 4, !tbaa !39, !alias.scope !41, !noalias !42
  %arrayidx35 = getelementptr inbounds float, ptr addrspace(1) %out, i64 8
  store float 1.000000e+00, ptr addrspace(1) %arrayidx35, align 4, !tbaa !39, !alias.scope !41, !noalias !42
  %18 = tail call { <4 x float>, i8 } @air.sample_texture_cube.v4f32(ptr addrspace(1) readonly captures(none) %1, ptr addrspace(2) readonly captures(none) %agg.tmp.sroa.0.0.copyload, <3 x float> <float 2.500000e-01, float 1.000000e+00, float -2.500000e-01>, i1 true, float 1.000000e+00, float 0.000000e+00, i32 0) #8
  %19 = extractvalue { <4 x float>, i8 } %18, 0
  %20 = extractelement <4 x float> %19, i64 0
  %arrayidx44 = getelementptr inbounds float, ptr addrspace(1) %out, i64 9
  store float %20, ptr addrspace(1) %arrayidx44, align 4, !tbaa !39, !alias.scope !41, !noalias !42
  %21 = tail call i32 @air.get_width_texture_cube(ptr addrspace(1) readonly captures(none) %1, i32 1) #7
  %22 = tail call fast float @air.convert.f.f32.u.i32(i32 %21) #9
  %arrayidx49 = getelementptr inbounds float, ptr addrspace(1) %out, i64 10
  store float %22, ptr addrspace(1) %arrayidx49, align 4, !tbaa !39, !alias.scope !41, !noalias !42
  %arrayidx50 = getelementptr inbounds float, ptr addrspace(1) %out, i64 11
  store float 0.000000e+00, ptr addrspace(1) %arrayidx50, align 4, !tbaa !39, !alias.scope !41, !noalias !42
  %t.i107 = getelementptr inbounds %struct.Args, ptr addrspace(2) %a, i64 0, i32 1, i32 0, i64 %conv, i32 0
  %23 = load ptr addrspace(1), ptr addrspace(2) %t.i107, align 8, !tbaa !46, !alias.scope !37, !noalias !38
  tail call void @air.write_texture_cube.v4f32(ptr addrspace(1) captures(none) %23, <2 x i32> <i32 2, i32 1>, i32 2, <4 x float> splat (float 7.700000e+01), i32 0, i32 2) #10
  ret void
}

; Function Attrs: mustprogress nounwind willreturn memory(argmem: readwrite)
declare void @air.write_texture_cube.v4f32(ptr addrspace(1) captures(none), <2 x i32>, i32, <4 x float>, i32, i32) local_unnamed_addr #1

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare float @air.convert.f.f32.u.i32(i32) local_unnamed_addr #2

; Function Attrs: mustprogress nofree nounwind willreturn memory(argmem: read)
declare i32 @air.get_width_texture_cube(ptr addrspace(1) readonly captures(none), i32) local_unnamed_addr #3

; Function Attrs: convergent mustprogress nofree nounwind willreturn memory(argmem: read)
declare { <4 x float>, i8 } @air.sample_texture_cube.v4f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), <3 x float>, i1, float, float, i32) local_unnamed_addr #4

; Function Attrs: mustprogress nofree nounwind willreturn memory(argmem: read)
declare i32 @air.get_num_mip_levels_texture_cube(ptr addrspace(1) readonly captures(none)) local_unnamed_addr #3

; Function Attrs: mustprogress nofree nounwind willreturn memory(argmem: read)
declare i32 @air.get_height_texture_cube(ptr addrspace(1) readonly captures(none), i32) local_unnamed_addr #3

; Function Attrs: mustprogress nofree nounwind willreturn memory(argmem: read)
declare { <4 x float>, i8 } @air.sample_texture_cube_grad.v4f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), <3 x float>, <3 x float>, <3 x float>, float, i32) local_unnamed_addr #3

; Function Attrs: mustprogress nofree nounwind willreturn memory(argmem: read)
declare { <4 x float>, i8 } @air.read_texture_cube.v4f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2), <2 x i32>, i32, i32, i32) local_unnamed_addr #3

; Function Attrs: mustprogress nofree nounwind willreturn memory(inaccessiblemem: read)
declare ptr addrspace(2) @air.get_read_sampler() local_unnamed_addr #5

attributes #0 = { convergent mustprogress nounwind willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
attributes #1 = { mustprogress nounwind willreturn memory(argmem: readwrite) }
attributes #2 = { mustprogress nofree nosync nounwind willreturn memory(none) }
attributes #3 = { mustprogress nofree nounwind willreturn memory(argmem: read) }
attributes #4 = { convergent mustprogress nofree nounwind willreturn memory(argmem: read) }
attributes #5 = { mustprogress nofree nounwind willreturn memory(inaccessiblemem: read) }
attributes #6 = { nounwind willreturn memory(inaccessiblemem: read) }
attributes #7 = { nounwind willreturn memory(argmem: read) }
attributes #8 = { convergent nounwind willreturn memory(argmem: read) }
attributes #9 = { nounwind willreturn memory(none) }
attributes #10 = { nounwind willreturn memory(argmem: readwrite) }

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
!14 = !{ptr @image_ops, !15, !16}
!15 = !{}
!16 = !{!17, !22, !23}
!17 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 40, !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !18, !"air.arg_type_size", i32 40, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Args", !"air.arg_name", !"a"}
!18 = !{i32 0, i32 16, i32 0, !"array<texturecube<float, sample>, 2>", !"src", !"air.indirect_argument", !19, i32 16, i32 16, i32 0, !"array<texturecube<float, write>, 2>", !"dst", !"air.indirect_argument", !20, i32 32, i32 8, i32 0, !"sampler", !"filter", !"air.indirect_argument", !21}
!19 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 2, !"air.sample", !"air.arg_type_name", !"array<texturecube<float, sample>, 2>", !"air.arg_name", !"src"}
!20 = !{i32 1, !"air.texture", !"air.location_index", i32 2, i32 2, !"air.write", !"air.arg_type_name", !"array<texturecube<float, write>, 2>", !"air.arg_name", !"dst"}
!21 = !{i32 2, !"air.sampler", !"air.location_index", i32 4, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"filter"}
!22 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!23 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"select"}
!24 = !{!25, !25, i64 0}
!25 = !{!"int", !26, i64 0}
!26 = !{!"omnipotent char", !27, i64 0}
!27 = !{!"Simple C++ TBAA"}
!28 = !{!29}
!29 = distinct !{!29, !30, !"air-alias-scope-arg(2)"}
!30 = distinct !{!30, !"air-alias-scopes(image_ops)"}
!31 = !{!32, !33}
!32 = distinct !{!32, !30, !"air-alias-scope-arg(0)"}
!33 = distinct !{!33, !30, !"air-alias-scope-arg(1)"}
!34 = !{!35, !36, i64 0}
!35 = !{!"_ZTSN5metal11texturecubeIfLNS_6accessE0EvEE", !36, i64 0}
!36 = !{!"__metal_texture_cube_t", !26, i64 0}
!37 = !{!32}
!38 = !{!33, !29}
!39 = !{!40, !40, i64 0}
!40 = !{!"float", !26, i64 0}
!41 = !{!33}
!42 = !{!32, !29}
!43 = !{i64 0, i64 8, !44}
!44 = !{!45, !45, i64 0}
!45 = !{!"__metal_sampler_t", !26, i64 0}
!46 = !{!47, !36, i64 0}
!47 = !{!"_ZTSN5metal11texturecubeIfLNS_6accessE2EvEE", !36, i64 0}
