target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64"
target triple = "air64_v28-apple-macosx26.7.0"

%Table = type { [1 x ptr addrspace(1)] }

define void @large_buffer_addrspace2_device_pointers(ptr addrspace(2) %table, ptr addrspace(1) %out, i32 %index) {
entry:
  ; Selecting a device pointer from constant data is still an ordinary data-buffer GEP. The
  ; selected pointer's later dereference is independently a device-address operation.
  %slot = getelementptr inbounds %Table, ptr addrspace(2) %table, i64 0, i32 0, i32 %index
  %pointer = load ptr addrspace(1), ptr addrspace(2) %slot, align 8
  %value = load i32, ptr addrspace(1) %pointer, align 4
  store i32 %value, ptr addrspace(1) %out, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @large_buffer_addrspace2_device_pointers, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 8, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Table", !"air.arg_name", !"table"}
!4 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
