target triple = "air64_v28-apple-macosx26.7.0"

define void @ordinary_i32(ptr addrspace(1) %input, ptr addrspace(1) %output, i32 %index) {
entry:
  ; The mask makes the whole byte range provable: 1023 * sizeof(i32) is below 4 GiB.
  %bounded = and i32 %index, 1023
  %input_word = getelementptr inbounds i32, ptr addrspace(1) %input, i32 %bounded
  %value = load i32, ptr addrspace(1) %input_word, align 4
  %output_word = getelementptr inbounds i32, ptr addrspace(1) %output, i32 %bounded
  store i32 %value, ptr addrspace(1) %output_word, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @ordinary_i32, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"input"}
!4 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 2, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"output"}
