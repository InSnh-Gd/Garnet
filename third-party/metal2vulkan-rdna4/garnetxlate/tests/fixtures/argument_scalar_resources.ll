; Authored regression matching probe.m: a constant Resources& holds a data pointer,
; texture and sampler. The sampler must become a descriptor, not a BDA pointer.
source_filename = "argument_buffer_sampler"
target datalayout = "e-p:64:64-i64:64-v128:128-n8:16:32"
target triple = "air64_v28-apple-macosx26.7.0"

%Params = type { <4 x float>, float, i32 }
%Resources = type { ptr addrspace(1), %Texture, %Sampler }
%Texture = type { ptr addrspace(1) }
%Sampler = type { ptr addrspace(2) }

define void @argbuf_sampler(ptr addrspace(1) %out, ptr addrspace(2) %p, ptr addrspace(2) %r, i32 %i) {
entry:
  %count.field = getelementptr inbounds %Params, ptr addrspace(2) %p, i64 0, i32 2
  %count = load i32, ptr addrspace(2) %count.field, align 4
  %active = icmp ult i32 %i, %count
  br i1 %active, label %work, label %exit
work:
  %values.field = getelementptr inbounds %Resources, ptr addrspace(2) %r, i64 0, i32 0
  %values = load ptr addrspace(1), ptr addrspace(2) %values.field, align 8
  %index = zext i32 %i to i64
  %element = getelementptr inbounds float, ptr addrspace(1) %values, i64 %index
  %value = load float, ptr addrspace(1) %element, align 4
  %scale.field = getelementptr inbounds %Params, ptr addrspace(2) %p, i64 0, i32 1
  %scale = load float, ptr addrspace(2) %scale.field, align 4
  %scaled = fmul float %value, %scale
  %sampler.field = getelementptr inbounds %Resources, ptr addrspace(2) %r, i64 0, i32 2, i32 0
  %sampler = load ptr addrspace(2), ptr addrspace(2) %sampler.field, align 8
  %texture.field = getelementptr inbounds %Resources, ptr addrspace(2) %r, i64 0, i32 1, i32 0
  %texture = load ptr addrspace(1), ptr addrspace(2) %texture.field, align 8
  %sample = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %texture, ptr addrspace(2) %sampler, <2 x float> <float 0.5, float 0.5>, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %rgba = extractvalue { <4 x float>, i8 } %sample, 0
  %red = extractelement <4 x float> %rgba, i64 0
  %sum = fadd float %scaled, %red
  %dst = getelementptr inbounds float, ptr addrspace(1) %out, i64 %index
  store float %sum, ptr addrspace(1) %dst, align 4
  br label %exit
exit:
  ret void
}

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)

!air.kernel = !{!0}
!0 = !{ptr @argbuf_sampler, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5, !6}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 32, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"Params", !"air.arg_name", !"p"}
!5 = !{i32 2, !"air.indirect_buffer", !"air.buffer_size", i32 24, !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !7, !"air.arg_type_size", i32 24, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Resources", !"air.arg_name", !"r"}
!6 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"i"}
!7 = !{i32 0, i32 8, i32 0, !"float", !"values", !"air.indirect_argument", !8, i32 8, i32 8, i32 0, !"texture2d<float, sample>", !"tex", !"air.indirect_argument", !9, i32 16, i32 8, i32 0, !"sampler", !"smp", !"air.indirect_argument", !10}
!8 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"values"}
!9 = !{i32 1, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"tex"}
!10 = !{i32 2, !"air.sampler", !"air.location_index", i32 2, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"smp"}
