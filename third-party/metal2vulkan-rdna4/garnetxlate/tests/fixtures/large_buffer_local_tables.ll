target datalayout = "e-p:64:64:64-i64:64:64"
target triple = "air64-apple-macosx"

define void @large_buffer_local_tables(ptr addrspace(1) %a, ptr addrspace(1) %b, ptr addrspace(1) %out, ptr addrspace(1) %control) {
entry:
  %table = alloca [2 x [2 x ptr addrspace(1)]], align 8
  %offset = load i64, ptr addrspace(1) %control, align 8
  %condition = icmp eq i64 %offset, 0
  %ap = getelementptr i8, ptr addrspace(1) %a, i64 %offset
  %bp = getelementptr i8, ptr addrspace(1) %b, i64 %offset
  %p00 = getelementptr [2 x [2 x ptr addrspace(1)]], ptr %table, i64 0, i32 0, i32 0
  %p01 = getelementptr [2 x [2 x ptr addrspace(1)]], ptr %table, i64 0, i32 0, i32 1
  %p10 = getelementptr [2 x [2 x ptr addrspace(1)]], ptr %table, i64 0, i32 1, i32 0
  %p11 = getelementptr [2 x [2 x ptr addrspace(1)]], ptr %table, i64 0, i32 1, i32 1
  store ptr addrspace(1) %ap, ptr %p00, align 8
  store ptr addrspace(1) %bp, ptr %p01, align 8
  store ptr addrspace(1) %bp, ptr %p10, align 8
  store ptr addrspace(1) %ap, ptr %p11, align 8
  br i1 %condition, label %replace, label %read
replace:
  store ptr addrspace(1) %bp, ptr %p00, align 8
  br label %read
read:
  %i = and i64 %offset, 1
  %j = lshr i64 %offset, 1
  %j1 = and i64 %j, 1
  %dynamic = getelementptr [2 x [2 x ptr addrspace(1)]], ptr %table, i64 0, i64 %i, i64 %j1
  store ptr addrspace(1) %bp, ptr %dynamic, align 8
  %fixed = load ptr addrspace(1), ptr %p00, align 8
  %v0 = load i32, ptr addrspace(1) %fixed, align 1
  %selected = load ptr addrspace(1), ptr %dynamic, align 8
  %v1 = load i32, ptr addrspace(1) %selected, align 1
  %sum = add i32 %v0, %v1
  store i32 %sum, ptr addrspace(1) %out, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @large_buffer_local_tables, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5, !6}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 1, !"air.arg_type_align_size", i32 1, !"air.arg_type_name", !"uchar", !"air.arg_name", !"a"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 1, !"air.arg_type_align_size", i32 1, !"air.arg_type_name", !"uchar", !"air.arg_name", !"b"}
!5 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!6 = !{i32 3, !"air.buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"ulong", !"air.arg_name", !"control"}
