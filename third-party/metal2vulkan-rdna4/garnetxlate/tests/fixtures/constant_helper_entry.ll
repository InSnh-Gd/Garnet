%Globals = type { [3 x [2 x float]], [3 x [2 x float]], float, float }

define <4 x float> @sample_entry(ptr addrspace(2) %globals) {
entry:
  %tail = getelementptr inbounds %Globals, ptr addrspace(2) %globals, i64 1, i32 0, i64 0, i64 0
  %color = call <4 x float> @read_tail.MTL_VISIBLE_FN_REF(ptr addrspace(2) %tail)
  ret <4 x float> %color
}
declare <4 x float> @read_tail.MTL_VISIBLE_FN_REF(ptr addrspace(2))

!air.visible_function_references = !{!10}
!10 = !{!"air.visible_function_reference", ptr @read_tail.MTL_VISIBLE_FN_REF, !"read_tail"}
!air.fragment = !{!0}
!0 = !{ptr @sample_entry, !1, !3}
!1 = !{!2}
!2 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!3 = !{!4}
!4 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 56, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"Globals", !"air.arg_name", !"globals"}
