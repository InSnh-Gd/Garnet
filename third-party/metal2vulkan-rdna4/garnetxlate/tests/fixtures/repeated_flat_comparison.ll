; Authored flat fields: ordinary sampling and two distinct comparisons share one sampler.
target triple = "air64-apple-macosx"
%Args = type { ptr addrspace(1), ptr addrspace(1), ptr addrspace(2) }
define void @repeated_flat(ptr addrspace(2) %args, ptr addrspace(1) %out) {
entry:
  %color_slot = getelementptr %Args, ptr addrspace(2) %args, i64 0, i32 0
  %depth_slot = getelementptr %Args, ptr addrspace(2) %args, i64 0, i32 1
  %sampler_slot = getelementptr %Args, ptr addrspace(2) %args, i64 0, i32 2
  %color = load ptr addrspace(1), ptr addrspace(2) %color_slot, align 8
  %depth = load ptr addrspace(1), ptr addrspace(2) %depth_slot, align 8
  %sampler = load ptr addrspace(2), ptr addrspace(2) %sampler_slot, align 8
  %normal = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %color, ptr addrspace(2) %sampler, <2 x float> <float 0.25, float 0.75>, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.0, float 0.0, i32 0)
  %normal4 = extractvalue { <4 x float>, i8 } %normal, 0
  %normal0 = extractelement <4 x float> %normal4, i32 0
  %first = call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) %depth, ptr addrspace(2) %sampler, i32 1, <2 x float> <float 0.25, float 0.75>, float 0.5, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.0, float 0.0, i32 0)
  %first0 = extractvalue { float, i8 } %first, 0
  %second = call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) %depth, ptr addrspace(2) %sampler, i32 1, <2 x float> <float 0.75, float 0.25>, float 0.25, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.0, float 0.0, i32 0)
  %second0 = extractvalue { float, i8 } %second, 0
  %sum = fadd float %first0, %second0
  %result = fadd float %normal0, %sum
  store float %result, ptr addrspace(1) %out, align 4
  ret void
}
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)
declare { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1), ptr addrspace(2), i32, <2 x float>, float, i1, <2 x i32>, i1, float, float, i32)
!air.kernel = !{!0}
!0 = !{ptr @repeated_flat, !1, !2}
!1 = !{}
!2 = !{!3, !8}
!3 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !4, !"air.arg_type_size", i32 24, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Args"}
!4 = !{i32 0, i32 8, i32 0, !"texture2d<float, sample>", !"color", !"air.indirect_argument", !5, i32 8, i32 8, i32 0, !"depth2d<float, sample>", !"depth", !"air.indirect_argument", !6, i32 16, i32 8, i32 0, !"sampler", !"state", !"air.indirect_argument", !7}
!5 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>"}
!6 = !{i32 1, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.sample", !"air.arg_type_name", !"depth2d<float, sample>"}
!7 = !{i32 2, !"air.sampler", !"air.location_index", i32 2, i32 1, !"air.arg_type_name", !"sampler"}
!8 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"float"}
