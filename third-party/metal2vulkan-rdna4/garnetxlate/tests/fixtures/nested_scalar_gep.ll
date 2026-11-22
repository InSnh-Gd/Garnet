target triple = "air64_v28-apple-macosx26.7.0"

define <4 x float> @nested_scalar_gep(ptr addrspace(1) %data, ptr addrspace(1) %other, i32 %index) {
entry:
  %first = getelementptr inbounds [4 x [4 x float]], ptr addrspace(1) %data, i32 0, i32 1, i32 0
  %second = getelementptr inbounds [4 x [4 x float]], ptr addrspace(1) %other, i32 0, i32 1, i32 0
  %choose = icmp eq i32 %index, 0
  %selected = select i1 %choose, ptr addrspace(1) %first, ptr addrspace(1) %second
  %next = getelementptr inbounds float, ptr addrspace(1) %selected, i32 1
  %value = load float, ptr addrspace(1) %next, align 4
  %out = insertelement <4 x float> zeroinitializer, float %value, i32 0
  ret <4 x float> %out
}

!air.vertex = !{!0}
!0 = !{ptr @nested_scalar_gep, !1, !2}
!1 = !{!3}
!2 = !{!4, !5, !6}
!3 = !{!"air.position", !"air.arg_type_name", !"float4"}
!4 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 64, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float[4][4]", !"air.arg_name", !"data"}
!5 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 64, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 64, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float[4][4]", !"air.arg_name", !"other"}
!6 = !{i32 2, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"index"}
