target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64"
target triple = "air64_v28-apple-macosx26.7.0"

define void @large_buffer_chained_constant(ptr addrspace(1) %data, ptr addrspace(1) %out) {
entry:
  ; Each offset fits in u32, but the provenance chain totals 0x1_0001_0000 bytes.
  %first = getelementptr inbounds i8, ptr addrspace(1) %data, i64 4294901760
  %second = getelementptr inbounds i8, ptr addrspace(1) %first, i64 131072
  %value = load i32, ptr addrspace(1) %second, align 4
  store i32 %value, ptr addrspace(1) %out, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @large_buffer_chained_constant, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"data"}
!4 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
