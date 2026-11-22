target triple = "air64_v28-apple-macosx26.7.0"

define void @large_buffer_scaled_dynamic_no_layout(ptr addrspace(1) %data, ptr addrspace(1) %out, i32 %index) {
entry:
  ; The default AIR ABI gives i32 an 8-byte GEP stride. A dynamic i32 index can therefore
  ; address beyond 4 GiB even when the module omits an explicit target datalayout.
  %far = getelementptr inbounds i64, ptr addrspace(1) %data, i32 %index
  %value = load i64, ptr addrspace(1) %far, align 8
  store i64 %value, ptr addrspace(1) %out, align 8
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @large_buffer_scaled_dynamic_no_layout, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 8, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"ulong", !"air.arg_name", !"data"}
!4 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 8, !"air.location_index", i32 1, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"ulong", !"air.arg_name", !"out"}
