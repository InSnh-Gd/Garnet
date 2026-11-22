target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64"
target triple = "air64_v28-apple-macosx26.7.0"

%Offsets = type { [2 x i64] }

define void @large_buffer_cross_root_phi(ptr addrspace(1) %offsets, ptr addrspace(1) %first, ptr addrspace(1) %second, ptr addrspace(1) %out) {
entry:
  %slot0 = getelementptr inbounds %Offsets, ptr addrspace(1) %offsets, i64 0, i32 0, i64 0
  %off4 = load i64, ptr addrspace(1) %slot0, align 8
  %slot1 = getelementptr inbounds %Offsets, ptr addrspace(1) %offsets, i64 0, i32 0, i64 1
  %off8 = load i64, ptr addrspace(1) %slot1, align 8
  %cond = icmp ult i64 %off4, %off8
  br i1 %cond, label %use_first, label %use_second

use_first:
  %first_ptr = getelementptr i8, ptr addrspace(1) %first, i64 %off4
  br label %merge

use_second:
  %second_ptr = getelementptr i8, ptr addrspace(1) %second, i64 %off8
  br label %merge

merge:
  %selected = phi ptr addrspace(1) [ %first_ptr, %use_first ], [ %second_ptr, %use_second ]
  %value = load i32, ptr addrspace(1) %selected, align 4
  store i32 %value, ptr addrspace(1) %out, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @large_buffer_cross_root_phi, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5, !6}
!3 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Offsets", !"air.arg_name", !"offsets"}
!4 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"first"}
!5 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"second"}
!6 = !{i32 3, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 3, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
