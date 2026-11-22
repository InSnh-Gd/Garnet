target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64"
target triple = "air64_v28-apple-macosx26.7.0"

%Offsets = type { [4 x i64] }

define void @large_buffer_pointer_merge(ptr addrspace(1) %offsets, ptr addrspace(1) %data, ptr addrspace(1) %out) {
entry:
  ; Runtime offsets 0x100000000 and 0x200000000 exercise addresses above 4 GiB.
  %slot0 = getelementptr inbounds %Offsets, ptr addrspace(1) %offsets, i64 0, i32 0, i64 0
  %off4 = load i64, ptr addrspace(1) %slot0, align 8
  %slot1 = getelementptr inbounds %Offsets, ptr addrspace(1) %offsets, i64 0, i32 0, i64 1
  %off8 = load i64, ptr addrspace(1) %slot1, align 8
  %cond = icmp ne i64 %off4, %off8

  %select_low = getelementptr i8, ptr addrspace(1) %data, i64 %off4
  %select_high = getelementptr i8, ptr addrspace(1) %data, i64 %off8
  %selected = select i1 %cond, ptr addrspace(1) %select_low, ptr addrspace(1) %select_high
  %selected_value = load i32, ptr addrspace(1) %selected, align 4
  br i1 %cond, label %phi_low, label %phi_high

phi_low:
  %low = getelementptr i8, ptr addrspace(1) %data, i64 %off4
  br label %merge

phi_high:
  %high = getelementptr i8, ptr addrspace(1) %data, i64 %off8
  br label %merge

merge:
  %merged = phi ptr addrspace(1) [ %low, %phi_low ], [ %high, %phi_high ]
  %merged_value = load i32, ptr addrspace(1) %merged, align 4
  %out0 = getelementptr i32, ptr addrspace(1) %out, i32 0
  store i32 %selected_value, ptr addrspace(1) %out0, align 4
  %out1 = getelementptr i32, ptr addrspace(1) %out, i32 1
  store i32 %merged_value, ptr addrspace(1) %out1, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @large_buffer_pointer_merge, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 32, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 32, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Offsets", !"air.arg_name", !"offsets"}
!4 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"data"}
!5 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 8, !"air.location_index", i32 2, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint2", !"air.arg_name", !"out"}
