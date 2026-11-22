; Authored query-only image graph: no component or access type is carried by the intrinsic.
target triple = "air64-apple-macosx"
%Root = type { [2 x %Record] }
%Record = type { [2 x ptr addrspace(1)] }
define void @query_graph(ptr addrspace(2) %args, ptr addrspace(1) %out, i32 %i) {
entry:
  %g = lshr i32 %i, 2
  %g1 = and i32 %g, 1
  %group = zext i32 %g1 to i64
  %ix = and i32 %i, 1
  %index = zext i32 %ix to i64
  %slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group, i32 0, i64 %index
  %token = load i64, ptr addrspace(2) %slot, align 8
  %image = inttoptr i64 %token to ptr addrspace(1)
  %mip = and i32 %i, 1
  %width = call i32 @air.get_width_texture_2d(ptr addrspace(1) %image, i32 %mip)
  %height = call i32 @air.get_height_texture_2d(ptr addrspace(1) %image, i32 %mip)
  %levels = call i32 @air.get_num_mip_levels_texture_2d(ptr addrspace(1) %image)
  %size = add i32 %width, %height
  %result = add i32 %size, %levels
  store i32 %result, ptr addrspace(1) %out, align 4
  ret void
}
declare i32 @air.get_width_texture_2d(ptr addrspace(1), i32)
declare i32 @air.get_height_texture_2d(ptr addrspace(1), i32)
declare i32 @air.get_num_mip_levels_texture_2d(ptr addrspace(1))
!air.kernel = !{!0}
!0 = !{ptr @query_graph, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"Root"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"uint"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
