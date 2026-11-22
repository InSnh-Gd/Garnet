; Authored select between two argument-buffer texture tokens.
target triple = "air64-apple-macosx"
%Root = type { ptr addrspace(2) }
%Record = type { [2 x ptr addrspace(1)], ptr addrspace(2) }

define void @select_graph(ptr addrspace(2) %args, ptr addrspace(1) %out, i32 %i) {
entry:
  %slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0
  %base = load ptr addrspace(2), ptr addrspace(2) %slot, align 8
  %index0 = and i32 %i, 1
  %index1 = xor i32 %index0, 1
  %index0_64 = zext i32 %index0 to i64
  %index1_64 = zext i32 %index1 to i64
  %image0_slot = getelementptr %Record, ptr addrspace(2) %base, i64 0, i32 0, i64 %index0_64
  %image1_slot = getelementptr %Record, ptr addrspace(2) %base, i64 0, i32 0, i64 %index1_64
  %sampler_slot = getelementptr %Record, ptr addrspace(2) %base, i64 0, i32 1
  %token0 = load i64, ptr addrspace(2) %image0_slot, align 8
  %token1 = load i64, ptr addrspace(2) %image1_slot, align 8
  %image0 = inttoptr i64 %token0 to ptr addrspace(1)
  %image1 = inttoptr i64 %token1 to ptr addrspace(1)
  %sampler = load ptr addrspace(2), ptr addrspace(2) %sampler_slot, align 8
  %sample0 = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %image0, ptr addrspace(2) %sampler, <2 x float> zeroinitializer, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %sample1 = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %image1, ptr addrspace(2) %sampler, <2 x float> zeroinitializer, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %rgba0 = extractvalue { <4 x float>, i8 } %sample0, 0
  %rgba1 = extractvalue { <4 x float>, i8 } %sample1, 0
  %red0 = extractelement <4 x float> %rgba0, i32 0
  %red1 = extractelement <4 x float> %rgba1, i32 0
  %arm_sum = fadd float %red0, %red1
  %odd = and i32 %i, 1
  %condition = icmp ne i32 %odd, 0
  %image = select i1 %condition, ptr addrspace(1) %image0, ptr addrspace(1) %image1
  %sample = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> zeroinitializer, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %rgba = extractvalue { <4 x float>, i8 } %sample, 0
  %red = extractelement <4 x float> %rgba, i32 0
  %combined = fadd float %red, %arm_sum
  store float %combined, ptr addrspace(1) %out, align 4
  ret void
}

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)

!air.kernel = !{!0}
!0 = !{ptr @select_graph, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"Root"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"float"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
