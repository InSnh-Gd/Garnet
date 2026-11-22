target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64"
target triple = "air64_v28-apple-macosx26.7.0"

define internal i32 @load_nullable(ptr addrspace(1) %maybe) {
entry:
  %is_null = icmp eq ptr addrspace(1) %maybe, null
  br i1 %is_null, label %empty, label %read

empty:
  ret i32 0

read:
  %value = load i32, ptr addrspace(1) %maybe, align 4
  ret i32 %value
}

define void @large_buffer_nullable_merge(ptr addrspace(1) %data, ptr addrspace(1) %out, <3 x i32> %gid) {
entry:
  %lane = extractelement <3 x i32> %gid, i64 0
  %wide = zext i32 %lane to i64
  %offset = shl i64 %wide, 32
  %take = icmp ne i64 %offset, 0
  ; `%shared` must select large-buffer addressing even though a nullable merge also consumes it.
  %shared = getelementptr inbounds i8, ptr addrspace(1) %data, i64 %offset
  %direct = load i32, ptr addrspace(1) %shared, align 4
  %maybe_select = select i1 %take, ptr addrspace(1) %shared, ptr addrspace(1) null
  %selected = call i32 @load_nullable(ptr addrspace(1) %maybe_select)
  br i1 %take, label %left, label %right

left:
  %left_ptr = getelementptr inbounds i8, ptr addrspace(1) %data, i64 %offset
  br label %merge

right:
  br label %merge

merge:
  %maybe_phi = phi ptr addrspace(1) [ %left_ptr, %left ], [ null, %right ]
  %phied = call i32 @load_nullable(ptr addrspace(1) %maybe_phi)
  %sum0 = add i32 %direct, %selected
  %sum = add i32 %sum0, %phied
  store i32 %sum, ptr addrspace(1) %out, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @large_buffer_nullable_merge, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"data"}
!4 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint3", !"air.arg_name", !"gid"}
