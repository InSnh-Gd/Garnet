target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64"
target triple = "air64_v28-apple-macosx26.7.0"

define void @large_buffer_unaligned_scalar(ptr addrspace(1) %offsets, ptr addrspace(1) %data, ptr addrspace(1) %out) {
entry:
  ; A runtime byte offset above 4 GiB selects large-buffer addressing. The i32 accesses are
  ; explicitly under-aligned, so the physical-storage lowering must not claim Aligned 4.
  %offset_slot = getelementptr inbounds i64, ptr addrspace(1) %offsets, i64 0
  %offset = load i64, ptr addrspace(1) %offset_slot, align 8
  %src = getelementptr inbounds i8, ptr addrspace(1) %data, i64 %offset
  %value = load i32, ptr addrspace(1) %src, align 1
  %dst = getelementptr inbounds i8, ptr addrspace(1) %out, i64 %offset
  store i32 %value, ptr addrspace(1) %dst, align 1
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @large_buffer_unaligned_scalar, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 8, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"ulong", !"air.arg_name", !"offsets"}
!4 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 1, !"air.arg_type_name", !"uint", !"air.arg_name", !"data"}
!5 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 2, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 1, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
