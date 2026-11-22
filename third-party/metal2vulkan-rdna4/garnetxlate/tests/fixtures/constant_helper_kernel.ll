%Globals = type { [3 x [2 x float]], [3 x [2 x float]], float, float }

define void @helper_kernel(ptr addrspace(2) %globals, ptr addrspace(1) %output) {
entry:
  %tail = getelementptr inbounds %Globals, ptr addrspace(2) %globals, i64 1, i32 0, i64 0, i64 0
  %first = call <4 x float> @read_tail.MTL_VISIBLE_FN_REF(ptr addrspace(2) %tail)
  store <4 x float> %first, ptr addrspace(1) %output, align 16
  %tail2 = getelementptr inbounds %Globals, ptr addrspace(2) %globals, i64 2, i32 0, i64 0, i64 0
  %second = call <4 x float> @read_tail.MTL_VISIBLE_FN_REF(ptr addrspace(2) %tail2)
  %next = getelementptr inbounds <4 x float>, ptr addrspace(1) %output, i64 1
  store <4 x float> %second, ptr addrspace(1) %next, align 16
  ret void
}
declare <4 x float> @read_tail.MTL_VISIBLE_FN_REF(ptr addrspace(2))

!air.visible_function_references = !{!10}
!10 = !{!"air.visible_function_reference", ptr @read_tail.MTL_VISIBLE_FN_REF, !"read_tail"}
!air.kernel = !{!0}
!0 = !{ptr @helper_kernel, !1, !3}
!1 = !{}
!3 = !{!4, !5}
!4 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 56, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"Globals", !"air.arg_name", !"globals"}
!5 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"output"}
