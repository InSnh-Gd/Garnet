; ModuleID = 'fixtures/abi8/sampler-mixed.air'
source_filename = "sampler_mixed"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

%struct.MixedArgs = type { %"struct.metal::texture2d", %"struct.metal::depth2d", %"struct.metal::array" }
%"struct.metal::texture2d" = type { ptr addrspace(1) }
%"struct.metal::depth2d" = type { ptr addrspace(1) }
%"struct.metal::array" = type { [8 x %"struct.metal::sampler"] }
%"struct.metal::sampler" = type { ptr addrspace(2) }

; Function Attrs: convergent mustprogress nofree nounwind willreturn
define void @sampler_mixed(ptr addrspace(2) readonly align 8 captures(none) dereferenceable(80) "air-buffer-no-alias" %args, ptr addrspace(1) writeonly captures(none) "air-buffer-no-alias" %out, i32 %i) local_unnamed_addr #0 {
entry:
  %shr = lshr i32 %i, 2
  %and1 = and i32 %i, 1
  %tobool.not = icmp eq i32 %and1, 0
  %cond = select fast i1 %tobool.not, float 2.500000e-01, float 7.500000e-01
  %vecinit = insertelement <2 x float> undef, float %cond, i64 0
  %and2 = and i32 %i, 2
  %tobool3.not = icmp eq i32 %and2, 0
  %cond4 = select fast i1 %tobool3.not, float 2.500000e-01, float 7.500000e-01
  %vecinit5 = insertelement <2 x float> %vecinit, float %cond4, i64 1
  %conv = zext i32 %shr to i64
  %state.sroa.0.0..sroa_idx = getelementptr inbounds %struct.MixedArgs, ptr addrspace(2) %args, i64 0, i32 2, i32 0, i64 %conv, i32 0
  %state.sroa.0.0.copyload = load ptr addrspace(2), ptr addrspace(2) %state.sroa.0.0..sroa_idx, align 8, !tbaa.struct !24, !alias.scope !29, !noalias !32
  %t.i = getelementptr inbounds %struct.MixedArgs, ptr addrspace(2) %args, i64 0, i32 0, i32 0
  %0 = load ptr addrspace(1), ptr addrspace(2) %t.i, align 8, !tbaa !34, !alias.scope !29, !noalias !32
  %1 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %0, ptr addrspace(2) readonly captures(none) %state.sroa.0.0.copyload, <2 x float> splat (float 5.000000e-01), i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0) #2
  %2 = extractvalue { <4 x float>, i8 } %1, 0
  %t.i27 = getelementptr inbounds %struct.MixedArgs, ptr addrspace(2) %args, i64 0, i32 1, i32 0
  %3 = load ptr addrspace(1), ptr addrspace(2) %t.i27, align 8, !tbaa !37, !alias.scope !29, !noalias !32
  %4 = tail call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) readonly captures(none) %3, ptr addrspace(2) readonly captures(none) %state.sroa.0.0.copyload, i32 1, <2 x float> %vecinit5, float 5.000000e-01, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0) #2
  %5 = extractvalue { float, i8 } %4, 0
  %6 = extractelement <4 x float> %2, i64 0
  %mul = shl i32 %i, 1
  %idxprom = zext i32 %mul to i64
  %arrayidx = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom
  store float %6, ptr addrspace(1) %arrayidx, align 4, !tbaa !40, !alias.scope !32, !noalias !29
  %add = or i32 %mul, 1
  %idxprom12 = zext i32 %add to i64
  %arrayidx13 = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom12
  store float %5, ptr addrspace(1) %arrayidx13, align 4, !tbaa !40, !alias.scope !32, !noalias !29
  ret void
}

; Function Attrs: convergent mustprogress nofree nounwind willreturn memory(argmem: read)
declare { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), i32, <2 x float>, float, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr #1

; Function Attrs: convergent mustprogress nofree nounwind willreturn memory(argmem: read)
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), <2 x float>, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr #1

attributes #0 = { convergent mustprogress nofree nounwind willreturn "approx-func-fp-math"="true" "frame-pointer"="all" "min-legal-vector-width"="128" "no-builtins" "no-infs-fp-math"="true" "no-nans-fp-math"="true" "no-signed-zeros-fp-math"="true" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "unsafe-fp-math"="true" }
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
!14 = !{ptr @sampler_mixed, !15, !16}
!15 = !{}
!16 = !{!17, !22, !23}
!17 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 80, !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !18, !"air.arg_type_size", i32 80, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"MixedArgs", !"air.arg_name", !"args"}
!18 = !{i32 0, i32 8, i32 0, !"texture2d<float, sample>", !"color", !"air.indirect_argument", !19, i32 8, i32 8, i32 0, !"depth2d<float, sample>", !"depth", !"air.indirect_argument", !20, i32 16, i32 64, i32 0, !"array<sampler, 8>", !"filters", !"air.indirect_argument", !21}
!19 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"color"}
!20 = !{i32 1, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.sample", !"air.arg_type_name", !"depth2d<float, sample>", !"air.arg_name", !"depth"}
!21 = !{i32 2, !"air.sampler", !"air.location_index", i32 2, i32 8, !"air.arg_type_name", !"array<sampler, 8>", !"air.arg_name", !"filters"}
!22 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!23 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"i"}
!24 = !{i64 0, i64 8, !25}
!25 = !{!26, !26, i64 0}
!26 = !{!"__metal_sampler_t", !27, i64 0}
!27 = !{!"omnipotent char", !28, i64 0}
!28 = !{!"Simple C++ TBAA"}
!29 = !{!30}
!30 = distinct !{!30, !31, !"air-alias-scope-arg(0)"}
!31 = distinct !{!31, !"air-alias-scopes(sampler_mixed)"}
!32 = !{!33}
!33 = distinct !{!33, !31, !"air-alias-scope-arg(1)"}
!34 = !{!35, !36, i64 0}
!35 = !{!"_ZTSN5metal9texture2dIfLNS_6accessE0EvEE", !36, i64 0}
!36 = !{!"__metal_texture_2d_t", !27, i64 0}
!37 = !{!38, !39, i64 0}
!38 = !{!"_ZTSN5metal7depth2dIfLNS_6accessE0EvEE", !39, i64 0}
!39 = !{!"__metal_depth_2d_t", !27, i64 0}
!40 = !{!41, !41, i64 0}
!41 = !{!"float", !27, i64 0}
