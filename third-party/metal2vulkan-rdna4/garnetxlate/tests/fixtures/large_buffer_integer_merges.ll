target datalayout = "e-p:64:64:64-i64:64:64"
target triple = "air64-apple-macosx"

define void @integer_merges(ptr addrspace(1) %a, ptr addrspace(1) %b, ptr addrspace(1) %control, ptr addrspace(1) %out) {
entry:
  %offset = load i64, ptr addrspace(1) %control, align 8
  %condition = icmp ne i64 %offset, 0
  %a_address = ptrtoint ptr addrspace(1) %a to i64
  %b_address = ptrtoint ptr addrspace(1) %b to i64
  %advanced = add i64 %a_address, %offset
  %back = sub i64 %advanced, 4
  %selected = select i1 %condition, i64 %back, i64 %b_address
  %frozen = freeze i64 %selected
  %restored = inttoptr i64 %frozen to ptr addrspace(1)
  %value0 = load i32, ptr addrspace(1) %restored, align 4
  br i1 %condition, label %left, label %right
left:
  %left_address = add i64 %a_address, %offset
  br label %merge
right:
  %right_address = sub i64 %b_address, 4
  br label %merge
merge:
  %merged = phi i64 [ %left_address, %left ], [ %right_address, %right ]
  %merged_ptr = inttoptr i64 %merged to ptr addrspace(1)
  %value1 = load i32, ptr addrspace(1) %merged_ptr, align 4
  br label %loop
loop:
  %cursor = phi i64 [ %advanced, %merge ], [ %next, %latch ]
  %iteration = phi i32 [ 0, %merge ], [ %iteration_next, %latch ]
  %cursor_ptr = inttoptr i64 %cursor to ptr addrspace(1)
  %value2 = load i32, ptr addrspace(1) %cursor_ptr, align 4
  br label %latch
latch:
  %next = sub i64 %cursor, 4
  %iteration_next = add i32 %iteration, 1
  %again = icmp ult i32 %iteration_next, 2
  br i1 %again, label %loop, label %exit
exit:
  %sum0 = add i32 %value0, %value1
  %sum1 = add i32 %sum0, %value2
  store i32 %sum1, ptr addrspace(1) %out, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @integer_merges, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5, !6}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"a"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"b"}
!5 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"ulong", !"air.arg_name", !"control"}
!6 = !{i32 3, !"air.buffer", !"air.location_index", i32 3, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
