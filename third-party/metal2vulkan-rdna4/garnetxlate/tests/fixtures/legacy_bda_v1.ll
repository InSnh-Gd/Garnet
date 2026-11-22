target triple = "air64_v28-apple-macosx26.7.0"

%Args = type { ptr addrspace(1), i32 }

define void @legacy_bda_v1(ptr addrspace(1) %args, ptr addrspace(1) %out, i32 %index) {
entry:
  %pointer_slot = getelementptr inbounds %Args, ptr addrspace(1) %args, i64 0, i32 0
  %pointer = load ptr addrspace(1), ptr addrspace(1) %pointer_slot, align 8
  %input_word = getelementptr inbounds i32, ptr addrspace(1) %pointer, i32 %index
  %value = load i32, ptr addrspace(1) %input_word, align 4
  %bounded = and i32 %index, 1023
  %out_word = getelementptr inbounds i32, ptr addrspace(1) %out, i32 %bounded
  store i32 %value, ptr addrspace(1) %out_word, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @legacy_bda_v1, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Args", !"air.arg_name", !"args"}
!4 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
