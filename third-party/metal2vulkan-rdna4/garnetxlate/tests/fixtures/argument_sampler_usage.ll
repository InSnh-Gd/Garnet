; Authored nested resource graph: one sampled image and one depth image share one sampler.
target triple = "air64-apple-macosx"
%Root = type { ptr addrspace(2) }
%Record = type { [2 x ptr addrspace(1)], ptr addrspace(2) }

define void @sampler_usage(ptr addrspace(2) %args, ptr addrspace(1) %out, i32 %i) {
entry:
  %slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0
  %base = load ptr addrspace(2), ptr addrspace(2) %slot, align 8
  %one = and i32 %i, 1
  %index = zext i32 %one to i64
  %color_slot = getelementptr %Record, ptr addrspace(2) %base, i64 0, i32 0, i64 %index
  %depth_slot = getelementptr %Record, ptr addrspace(2) %base, i64 0, i32 0, i64 1
  %sampler_slot = getelementptr %Record, ptr addrspace(2) %base, i64 0, i32 1
  %color = load ptr addrspace(1), ptr addrspace(2) %color_slot, align 8
  %depth = load ptr addrspace(1), ptr addrspace(2) %depth_slot, align 8
  %sampler = load ptr addrspace(2), ptr addrspace(2) %sampler_slot, align 8
  ; SAMPLE_OPERATIONS
  ret void
}

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)
declare { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1), ptr addrspace(2), i32, <2 x float>, float, i1, <2 x i32>, i1, float, float, i32)

!air.kernel = !{!0}
!0 = !{ptr @sampler_usage, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"Root"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"float"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
