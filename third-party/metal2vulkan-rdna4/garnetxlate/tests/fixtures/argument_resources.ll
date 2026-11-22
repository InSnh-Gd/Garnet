; ModuleID = 'fixtures/argument-resources.air'
source_filename = "argument_resources"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

%struct.Resources = type { %"struct.metal::texture2d", %"struct.metal::sampler" }
%"struct.metal::texture2d" = type { ptr addrspace(1) }
%"struct.metal::sampler" = type { ptr addrspace(2) }

; Function Attrs: convergent mustprogress nofree nounwind willreturn
define void @argument_resources(ptr addrspace(1) noundef readonly align 8 captures(none) dereferenceable(16) "air-buffer-no-alias" %args, ptr addrspace(1) noundef writeonly captures(none) "air-buffer-no-alias" %result, i32 noundef %i) local_unnamed_addr #0 {
entry:
  %and = and i32 %i, 1
  %tobool.not = icmp eq i32 %and, 0
  %cond = select fast i1 %tobool.not, float 2.500000e-01, float 7.500000e-01
  %vecinit = insertelement <2 x float> undef, float %cond, i64 0
  %and1 = and i32 %i, 2
  %tobool2.not = icmp eq i32 %and1, 0
  %cond3 = select fast i1 %tobool2.not, float 2.500000e-01, float 7.500000e-01
  %vecinit4 = insertelement <2 x float> %vecinit, float %cond3, i64 1
  %agg.tmp.sroa.0.0..sroa_idx = getelementptr inbounds %struct.Resources, ptr addrspace(1) %args, i64 0, i32 1, i32 0
  %agg.tmp.sroa.0.0.copyload = load ptr addrspace(2), ptr addrspace(1) %agg.tmp.sroa.0.0..sroa_idx, align 8, !tbaa.struct !23, !alias.scope !28, !noalias !31
  %t.i = getelementptr inbounds %struct.Resources, ptr addrspace(1) %args, i64 0, i32 0, i32 0
  %0 = load ptr addrspace(1), ptr addrspace(1) %t.i, align 8, !tbaa !33, !alias.scope !28, !noalias !31
  %1 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %0, ptr addrspace(2) readonly captures(none) %agg.tmp.sroa.0.0.copyload, <2 x float> %vecinit4, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0) #3
  %2 = extractvalue { <4 x float>, i8 } %1, 0
  %3 = tail call <4 x i32> @air.convert.u.v4i32.f.v4f32(<4 x float> %2) #4
  %idxprom = zext i32 %i to i64
  %arrayidx = getelementptr inbounds <4 x i32>, ptr addrspace(1) %result, i64 %idxprom
  store <4 x i32> %3, ptr addrspace(1) %arrayidx, align 16, !tbaa !36, !alias.scope !31, !noalias !28
  ret void
}

; Function Attrs: mustprogress nofree nosync nounwind willreturn memory(none)
declare <4 x i32> @air.convert.u.v4i32.f.v4f32(<4 x float>) local_unnamed_addr #1

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
!10 = !{!"Metal", i32 4, i32 0, i32 0}
!11 = !{!"air.compile.denorms_disable"}
!12 = !{!"air.compile.fast_math_enable"}
!13 = !{!"air.compile.framebuffer_fetch_enable"}
!14 = !{ptr @argument_resources, !15, !16}
!15 = !{}
!16 = !{!17, !21, !22}
!17 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 1, !"air.struct_type_info", !18, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Resources", !"air.arg_name", !"args"}
!18 = !{i32 0, i32 8, i32 0, !"texture2d<float, sample>", !"image", !"air.indirect_argument", !19, i32 8, i32 8, i32 0, !"sampler", !"filtering", !"air.indirect_argument", !20}
!19 = !{i32 0, !"air.texture", !"air.location_index", i32 2, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"image"}
!20 = !{i32 1, !"air.sampler", !"air.location_index", i32 7, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"filtering"}
!21 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"uint4", !"air.arg_name", !"result"}
!22 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"i"}
!23 = !{i64 0, i64 8, !24}
!24 = !{!25, !25, i64 0}
!25 = !{!"__metal_sampler_t", !26, i64 0}
!26 = !{!"omnipotent char", !27, i64 0}
!27 = !{!"Simple C++ TBAA"}
!28 = !{!29}
!29 = distinct !{!29, !30, !"air-alias-scope-arg(0)"}
!30 = distinct !{!30, !"air-alias-scopes(argument_resources)"}
!31 = !{!32}
!32 = distinct !{!32, !30, !"air-alias-scope-arg(1)"}
!33 = !{!34, !35, i64 0}
!34 = !{!"_ZTSN5metal9texture2dIfLNS_6accessE0ELNS_16memory_coherenceE0EvEE", !35, i64 0}
!35 = !{!"__metal_texture_2d_t", !26, i64 0}
!36 = !{!26, !26, i64 0}
