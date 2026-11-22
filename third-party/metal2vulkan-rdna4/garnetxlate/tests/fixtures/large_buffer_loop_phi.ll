target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64"
target triple = "air64_v28-apple-macosx26.7.0"

define void @large_buffer_loop_phi(ptr addrspace(1) %offsets, ptr addrspace(1) %data, ptr addrspace(1) %out) {
entry:
  %offset_slot = getelementptr inbounds i64, ptr addrspace(1) %offsets, i64 0
  %offset = load i64, ptr addrspace(1) %offset_slot, align 8
  br label %loop

loop:
  %ptr = phi ptr addrspace(1) [ %data, %entry ], [ %next, %latch ]
  %i = phi i64 [ 0, %entry ], [ %i_next, %latch ]
  %sum = phi i32 [ 0, %entry ], [ %sum_next, %latch ]
  %value = load i32, ptr addrspace(1) %ptr, align 4
  %sum_next = add i32 %sum, %value
  %next = getelementptr i8, ptr addrspace(1) %ptr, i64 %offset
  %i_next = add i64 %i, 1
  %more = icmp ult i64 %i_next, 4
  br i1 %more, label %latch, label %exit

latch:
  br label %loop

exit:
  store i32 %sum_next, ptr addrspace(1) %out, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @large_buffer_loop_phi, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 8, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"ulong", !"air.arg_name", !"offsets"}
!4 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"data"}
!5 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 2, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
