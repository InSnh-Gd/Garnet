target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64"
target triple = "air64_v28-apple-macosx26.7.0"

define void @large_buffer_vec3_tail(ptr addrspace(1) %offsets, ptr addrspace(1) %data, ptr addrspace(1) %out) {
entry:
  ; `<3 x i32>` allocates 16 bytes for GEP stride but a load/store touches only 12. At the
  ; end of a row, all 12 touched bytes are in bounds even though the next 4-byte boundary is not.
  %offset_slot = getelementptr inbounds i64, ptr addrspace(1) %offsets, i64 0
  %index = load i64, ptr addrspace(1) %offset_slot, align 8
  %src = getelementptr inbounds <3 x i32>, ptr addrspace(1) %data, i64 %index
  %value = load <3 x i32>, ptr addrspace(1) %src, align 16
  %dst = getelementptr inbounds <3 x i32>, ptr addrspace(1) %out, i64 %index
  store <3 x i32> %value, ptr addrspace(1) %dst, align 16
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @large_buffer_vec3_tail, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 8, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"ulong", !"air.arg_name", !"offsets"}
!4 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 12, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 12, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"uint3", !"air.arg_name", !"data"}
!5 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 12, !"air.location_index", i32 2, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 12, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"uint3", !"air.arg_name", !"out"}
