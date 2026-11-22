; Authored resource graph: record and image indices vary independently.
target triple = "air64-apple-macosx"
%Root = type { ptr addrspace(2) }
%Record = type { [2 x ptr addrspace(1)], ptr addrspace(2) }
define void @resource_graph(ptr addrspace(2) %args, ptr addrspace(1) %out, i32 %i) {
entry:
  %slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0
  %base = load ptr addrspace(2), ptr addrspace(2) %slot, align 8
  %g = lshr i32 %i, 2
  %g1 = and i32 %g, 1
  %group = zext i32 %g1 to i64
  %ix = and i32 %i, 1
  %index = zext i32 %ix to i64
  %image_slot = getelementptr %Record, ptr addrspace(2) %base, i64 %group, i32 0, i64 %index
  %sampler_slot = getelementptr %Record, ptr addrspace(2) %base, i64 %group, i32 1
  %image = load ptr addrspace(1), ptr addrspace(2) %image_slot, align 8
  %sampler = load ptr addrspace(2), ptr addrspace(2) %sampler_slot, align 8
  %sample = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> zeroinitializer, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %rgba = extractvalue { <4 x float>, i8 } %sample, 0
  %red = extractelement <4 x float> %rgba, i32 0
  store float %red, ptr addrspace(1) %out, align 4
  ret void
}
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)
!air.kernel = !{!0}
!0 = !{ptr @resource_graph, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"Root"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"float"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
