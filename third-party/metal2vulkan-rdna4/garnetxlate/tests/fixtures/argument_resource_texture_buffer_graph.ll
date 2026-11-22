; Synthetic dynamic texture-buffer argument array. No captured AIR source.
target triple = "air64-apple-macosx"
%Resources = type { [2 x %TextureBuffer] }
%TextureBuffer = type { ptr addrspace(1) }
define void @texel_graph(ptr addrspace(2) %resources, ptr addrspace(1) %out, i32 %choice) {
entry:
  %index = and i32 %choice, 1
  %index64 = zext i32 %index to i64
  %slot = getelementptr %Resources, ptr addrspace(2) %resources, i64 0, i32 0, i64 %index64, i32 0
  %texture = load ptr addrspace(1), ptr addrspace(2) %slot, align 8
  %read = call { <4 x float>, i8 } @air.read_texture_buffer_1d.v4f32(ptr addrspace(1) %texture, i32 %index, i32 1)
  %rgba = extractvalue { <4 x float>, i8 } %read, 0
  %red = extractelement <4 x float> %rgba, i32 0
  store float %red, ptr addrspace(1) %out, align 4
  ret void
}
declare { <4 x float>, i8 } @air.read_texture_buffer_1d.v4f32(ptr addrspace(1), i32, i32)
!air.kernel = !{!0}
!0 = !{ptr @texel_graph, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !6, !"air.arg_type_name", !"Resources", !"air.arg_name", !"resources"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"choice"}
!6 = !{i32 0, i32 16, i32 0, !"array<texture_buffer<float, read>, 2>", !"buffers", !"air.indirect_argument", !7}
!7 = !{i32 0, !"air.texture", !"air.location_index", i32 2, i32 2, !"air.read", !"air.arg_type_name", !"array<texture_buffer<float, read>, 2>", !"air.arg_name", !"buffers"}
