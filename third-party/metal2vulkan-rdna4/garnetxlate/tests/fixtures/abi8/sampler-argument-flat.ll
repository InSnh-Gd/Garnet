; A flat argument-buffer record with one sampler used by ordinary and depth comparison sampling.
target triple = "air64_v28-apple-macosx26.7.0"

%struct.MixedArgs = type { %"struct.metal::texture2d", %"struct.metal::depth2d", %"struct.metal::sampler" }
%"struct.metal::texture2d" = type { ptr addrspace(1) }
%"struct.metal::depth2d" = type { ptr addrspace(1) }
%"struct.metal::sampler" = type { ptr addrspace(2) }

define void @sampler_argument_flat(ptr addrspace(2) readonly align 8 dereferenceable(24) "air-buffer-no-alias" %args, ptr addrspace(1) writeonly "air-buffer-no-alias" %out, i32 %i) local_unnamed_addr {
entry:
  %state.sroa.0.0..sroa_idx = getelementptr inbounds %struct.MixedArgs, ptr addrspace(2) %args, i64 0, i32 2, i32 0
  %state.sroa.0.0.copyload = load ptr addrspace(2), ptr addrspace(2) %state.sroa.0.0..sroa_idx, align 8
  %t.i = getelementptr inbounds %struct.MixedArgs, ptr addrspace(2) %args, i64 0, i32 0, i32 0
  %0 = load ptr addrspace(1), ptr addrspace(2) %t.i, align 8
  %1 = tail call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none) %0, ptr addrspace(2) readonly captures(none) %state.sroa.0.0.copyload, <2 x float> splat (float 5.000000e-01), i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %2 = extractvalue { <4 x float>, i8 } %1, 0
  %t.i27 = getelementptr inbounds %struct.MixedArgs, ptr addrspace(2) %args, i64 0, i32 1, i32 0
  %3 = load ptr addrspace(1), ptr addrspace(2) %t.i27, align 8
  %4 = tail call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) readonly captures(none) %3, ptr addrspace(2) readonly captures(none) %state.sroa.0.0.copyload, i32 1, <2 x float> splat (float 2.500000e-01), float 5.000000e-01, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.000000e+00, float 0.000000e+00, i32 0)
  %5 = extractvalue { float, i8 } %4, 0
  %6 = extractelement <4 x float> %2, i64 0
  %mul = shl i32 %i, 1
  %idxprom = zext i32 %mul to i64
  %arrayidx = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom
  store float %6, ptr addrspace(1) %arrayidx, align 4
  %add = or i32 %mul, 1
  %idxprom12 = zext i32 %add to i64
  %arrayidx13 = getelementptr inbounds float, ptr addrspace(1) %out, i64 %idxprom12
  store float %5, ptr addrspace(1) %arrayidx13, align 4
  ret void
}

declare { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), i32, <2 x float>, float, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) readonly captures(none), ptr addrspace(2) readonly captures(none), <2 x float>, i1, <2 x i32>, i1, float, float, i32) local_unnamed_addr

!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}

!0 = !{i32 2, i32 8, i32 0}
!1 = !{!"Metal", i32 2, i32 1, i32 0}
!2 = !{ptr @sampler_argument_flat, !3, !4}
!3 = !{}
!4 = !{!5, !10, !11}
!5 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 24, !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !6, !"air.arg_type_size", i32 24, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"MixedArgs", !"air.arg_name", !"args"}
!6 = !{i32 0, i32 8, i32 0, !"texture2d<float, sample>", !"color", !"air.indirect_argument", !7, i32 8, i32 8, i32 0, !"depth2d<float, sample>", !"depth", !"air.indirect_argument", !8, i32 16, i32 8, i32 0, !"sampler", !"state", !"air.indirect_argument", !9}
!7 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"color"}
!8 = !{i32 1, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.sample", !"air.arg_type_name", !"depth2d<float, sample>", !"air.arg_name", !"depth"}
!9 = !{i32 2, !"air.sampler", !"air.location_index", i32 2, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"state"}
!10 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!11 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"i"}
