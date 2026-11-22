target triple = "air64-apple-macosx"

define void @reinterpret_entry(ptr addrspace(2) %input, ptr addrspace(1) %output, i32 %lane) {
entry:
  %input_guard = getelementptr i32, ptr addrspace(2) %input, i32 64
  %input_word = load i32, ptr addrspace(2) %input_guard, align 4
  %output_guard = getelementptr i32, ptr addrspace(1) %output, i32 64
  %output_word = load i32, ptr addrspace(1) %output_guard, align 4
  call void @reinterpret_helper(ptr addrspace(2) %input, ptr addrspace(1) %output, i32 %lane)
  ret void
}

define void @reinterpret_helper(ptr addrspace(2) %input, ptr addrspace(1) %output, i32 %lane) {
entry:
  %index = and i32 %lane, 31
  %source = getelementptr float, ptr addrspace(2) %input, i32 %index
  %value = load float, ptr addrspace(2) %source, align 4
  %negated = fneg float %value
  %destination = getelementptr float, ptr addrspace(1) %output, i32 %index
  store float %negated, ptr addrspace(1) %destination, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @reinterpret_entry, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"uint"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"uint"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
