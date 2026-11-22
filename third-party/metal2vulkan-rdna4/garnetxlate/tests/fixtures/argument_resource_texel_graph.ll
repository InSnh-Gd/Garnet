; Authored integer token -> opaque texture, with independently indexed records and images.
target triple = "air64-apple-macosx"
%Root = type { [2 x %Record] }
%Record = type { [2 x ptr addrspace(1)] }
define void @texel_graph(ptr addrspace(2) %args, ptr addrspace(1) %out, i32 %i) {
entry:
  %g = lshr i32 %i, 2
  %g1 = and i32 %g, 1
  %group = zext i32 %g1 to i64
  %ix = and i32 %i, 1
  %index = zext i32 %ix to i64
  %slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group, i32 0, i64 %index
  %token = load i64, ptr addrspace(2) %slot, align 8
  %image = inttoptr i64 %token to ptr addrspace(1)
  %sampler = call ptr addrspace(2) @air.get_read_sampler()
  %read = call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x i32> zeroinitializer, <2 x i32> zeroinitializer, i32 0, i32 3)
  %rgba = extractvalue { <4 x float>, i8 } %read, 0
  %red = extractelement <4 x float> %rgba, i32 0
  %width = call i32 @air.get_width_texture_2d(ptr addrspace(1) %image, i32 0)
  %height = call i32 @air.get_height_texture_2d(ptr addrspace(1) %image, i32 0)
  %size = add i32 %width, %height
  %size_f = uitofp i32 %size to float
  %result = fadd float %red, %size_f
  store float %result, ptr addrspace(1) %out, align 4
  call void @air.write_texture_2d.v4f32(ptr addrspace(1) %image, <2 x i32> zeroinitializer, <4 x float> %rgba, i32 0, i32 3)
  ret void
}
declare ptr addrspace(2) @air.get_read_sampler()
declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x i32>, <2 x i32>, i32, i32)
declare i32 @air.get_width_texture_2d(ptr addrspace(1), i32)
declare i32 @air.get_height_texture_2d(ptr addrspace(1), i32)
declare void @air.write_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, <4 x float>, i32, i32)
!air.kernel = !{!0}
!0 = !{ptr @texel_graph, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"Root"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"float"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
