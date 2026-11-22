target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64"
target triple = "air64_v28-apple-macosx26.7.0"

%Offsets = type { [1 x i64] }

define void @large_buffer_atomic(ptr addrspace(1) %offsets, ptr addrspace(1) %data, ptr addrspace(1) %out) {
entry:
  %offset_slot = getelementptr inbounds %Offsets, ptr addrspace(1) %offsets, i64 0, i32 0, i64 0
  %offset = load i64, ptr addrspace(1) %offset_slot, align 8
  %slot = getelementptr inbounds i8, ptr addrspace(1) %data, i64 %offset
  %expected = alloca i32, align 4
  %loaded = call i32 @air.atomic.global.load.i32(ptr addrspace(1) %slot, i32 0, i32 1, i1 true)
  call void @air.atomic.global.store.i32(ptr addrspace(1) %slot, i32 %loaded, i32 0, i32 2, i1 true)
  %rmw = call i32 @air.atomic.global.add.u.i32(ptr addrspace(1) %slot, i32 1, i32 0, i32 2, i1 true)
  store i32 %rmw, ptr %expected, align 4
  %old = call i32 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1) %slot, ptr %expected, i32 7, i32 0, i32 0, i32 2, i1 true)
  store i32 %old, ptr addrspace(1) %out, align 4
  ret void
}

declare i32 @air.atomic.global.load.i32(ptr addrspace(1), i32, i32, i1)
declare void @air.atomic.global.store.i32(ptr addrspace(1), i32, i32, i32, i1)
declare i32 @air.atomic.global.add.u.i32(ptr addrspace(1), i32, i32, i32, i1)
declare i32 @air.atomic.global.cmpxchg.weak.i32(ptr addrspace(1), ptr, i32, i32, i32, i32, i1)

!air.kernel = !{!0}
!0 = !{ptr @large_buffer_atomic, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Offsets", !"air.arg_name", !"offsets"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"data"}
!5 = !{i32 2, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
