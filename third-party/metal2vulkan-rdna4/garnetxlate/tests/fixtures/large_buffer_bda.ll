target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64"
target triple = "air64_v28-apple-macosx26.7.0"

%Offsets = type { [4 x i64] }
%Data = type { [2 x [4 x i32]], ptr addrspace(1) }

define internal i32 @read_at(ptr addrspace(1) %base, i64 %byte_offset) {
entry:
  %ptr = getelementptr inbounds i8, ptr addrspace(1) %base, i64 %byte_offset
  %value = load i32, ptr addrspace(1) %ptr, align 4
  ret i32 %value
}

define internal void @write_at(ptr addrspace(1) %base, i64 %byte_offset, i32 %value) {
entry:
  %ptr = getelementptr inbounds i8, ptr addrspace(1) %base, i64 %byte_offset
  store i32 %value, ptr addrspace(1) %ptr, align 4
  ret void
}

define void @large_buffer_bda(ptr addrspace(1) %offsets, ptr addrspace(1) %data, ptr addrspace(1) %out) {
entry:
  %before_slot = getelementptr inbounds %Offsets, ptr addrspace(1) %offsets, i64 0, i32 0, i64 0
  %before = load i64, ptr addrspace(1) %before_slot, align 8
  %small_index = and i64 %before, 3
  %dynamic_slot = getelementptr inbounds %Offsets, ptr addrspace(1) %offsets, i64 0, i32 0, i64 %small_index
  %dynamic = load i64, ptr addrspace(1) %dynamic_slot, align 8
  %boundary_slot = getelementptr inbounds %Offsets, ptr addrspace(1) %offsets, i64 0, i32 0, i64 1
  %boundary = load i64, ptr addrspace(1) %boundary_slot, align 8
  %far_slot = getelementptr inbounds %Offsets, ptr addrspace(1) %offsets, i64 0, i32 0, i64 2
  %far = load i64, ptr addrspace(1) %far_slot, align 8
  %end_slot = getelementptr inbounds %Offsets, ptr addrspace(1) %offsets, i64 0, i32 0, i64 3
  %end = load i64, ptr addrspace(1) %end_slot, align 8

  %before_value = call i32 @read_at(ptr addrspace(1) %data, i64 %before)
  %boundary_value = call i32 @read_at(ptr addrspace(1) %data, i64 %boundary)
  %far_value = call i32 @read_at(ptr addrspace(1) %data, i64 %far)
  %end_value = call i32 @read_at(ptr addrspace(1) %data, i64 %end)

  %lane = and i64 %far, 1
  %cell = getelementptr inbounds %Data, ptr addrspace(1) %data, i64 0, i32 0, i64 1, i64 %lane
  %cell_value = load i32, ptr addrspace(1) %cell, align 4

  %nested_slot = getelementptr inbounds %Data, ptr addrspace(1) %data, i64 0, i32 1
  %nested = load ptr addrspace(1), ptr addrspace(1) %nested_slot, align 8
  %nested_value = call i32 @read_at(ptr addrspace(1) %nested, i64 %boundary)

  %sum0 = add i32 %before_value, %boundary_value
  %sum1 = add i32 %far_value, %end_value
  %sum2 = add i32 %cell_value, %nested_value
  %sum3 = add i32 %sum0, %sum1
  %sum = add i32 %sum2, %sum3

  call void @write_at(ptr addrspace(1) %out, i64 %before, i32 %sum)
  call void @write_at(ptr addrspace(1) %out, i64 %boundary, i32 %sum0)
  call void @write_at(ptr addrspace(1) %out, i64 %far, i32 %sum1)
  call void @write_at(ptr addrspace(1) %out, i64 %end, i32 %sum2)
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @large_buffer_bda, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 32, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 32, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Offsets", !"air.arg_name", !"offsets"}
!4 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 40, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 40, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Data", !"air.arg_name", !"data"}
!5 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 40, !"air.location_index", i32 2, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 40, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Data", !"air.arg_name", !"out"}
