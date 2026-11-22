target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64"
target triple = "air64_v28-apple-macosx26.7.0"

%Args = type { ptr addrspace(1), [1 x i64] }

define void @large_buffer_mixed_pointer_select(ptr addrspace(1) %args, ptr addrspace(1) %data, ptr addrspace(1) %out) {
entry:
  %pointer_slot = getelementptr inbounds %Args, ptr addrspace(1) %args, i64 0, i32 0
  %unchecked = load ptr addrspace(1), ptr addrspace(1) %pointer_slot, align 8
  %offset_slot = getelementptr inbounds %Args, ptr addrspace(1) %args, i64 0, i32 1, i64 0
  %offset = load i64, ptr addrspace(1) %offset_slot, align 8
  %checked = getelementptr i8, ptr addrspace(1) %data, i64 %offset
  %cond = icmp ne i64 %offset, 0
  %selected = select i1 %cond, ptr addrspace(1) %checked, ptr addrspace(1) %unchecked
  %value = load i32, ptr addrspace(1) %selected, align 4
  store i32 %value, ptr addrspace(1) %out, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @large_buffer_mixed_pointer_select, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 16, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Args", !"air.arg_name", !"args"}
!4 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"data"}
!5 = !{i32 2, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 2, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
