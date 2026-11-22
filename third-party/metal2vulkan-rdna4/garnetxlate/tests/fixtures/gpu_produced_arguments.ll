target triple = "air64_v28-apple-macosx26.5.0"
%PointerArgs = type { ptr addrspace(1), i32 }

define void @produce_arguments(ptr addrspace(1) %destination, ptr addrspace(1) %source) {
entry:
  %source_slot = getelementptr inbounds %PointerArgs, ptr addrspace(1) %source, i64 0, i32 0
  %words = load ptr addrspace(1), ptr addrspace(1) %source_slot, align 8
  %next = getelementptr inbounds i32, ptr addrspace(1) %words, i64 4
  %destination_slot = getelementptr inbounds %PointerArgs, ptr addrspace(1) %destination, i64 0, i32 0
  store ptr addrspace(1) %next, ptr addrspace(1) %destination_slot, align 8
  %source_bias = getelementptr inbounds %PointerArgs, ptr addrspace(1) %source, i64 0, i32 1
  %bias = load i32, ptr addrspace(1) %source_bias, align 4
  %updated = add i32 %bias, 37
  %destination_bias = getelementptr inbounds %PointerArgs, ptr addrspace(1) %destination, i64 0, i32 1
  store i32 %updated, ptr addrspace(1) %destination_bias, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @produce_arguments, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 3, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.struct_type_info", !5, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"PointerArgs", !"air.arg_name", !"destination"}
!4 = !{i32 1, !"air.indirect_buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 5, i32 1, !"air.read", !"air.address_space", i32 1, !"air.struct_type_info", !5, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"PointerArgs", !"air.arg_name", !"source"}
!5 = !{i32 0, i32 8, i32 0, !"uint", !"words", !"air.indirect_argument", !6, i32 8, i32 4, i32 0, !"uint", !"bias", !"air.indirect_argument", !7}
!6 = !{i32 0, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"words"}
!7 = !{i32 1, !"air.indirect_constant", !"air.location_index", i32 5, i32 1, !"air.arg_type_name", !"uint", !"air.arg_name", !"bias"}
