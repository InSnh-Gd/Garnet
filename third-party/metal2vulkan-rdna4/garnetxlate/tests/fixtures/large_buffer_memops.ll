target datalayout = "e-p:64:64:64-i64:64:64"
target triple = "air64-apple-macosx"

define void @large_buffer_memops(ptr addrspace(1) %data, ptr addrspace(1) %control) {
entry:
  %local = alloca [8 x i8], align 8
  %offset = load i64, ptr addrspace(1) %control, align 8
  %source = getelementptr i8, ptr addrspace(1) %data, i64 %offset
  call void @llvm.memcpy.p0.p1.i64(ptr align 1 %local, ptr addrspace(1) align 1 %source, i64 8, i1 false)
  %destination = getelementptr i8, ptr addrspace(1) %source, i64 32
  call void @llvm.memcpy.p1.p0.i64(ptr addrspace(1) align 1 %destination, ptr align 1 %local, i64 8, i1 false)
  %clear = getelementptr i8, ptr addrspace(1) %source, i64 48
  call void @llvm.memset.p1.i64(ptr addrspace(1) align 1 %clear, i8 0, i64 5, i1 false)
  ret void
}

declare void @llvm.memcpy.p0.p1.i64(ptr, ptr addrspace(1), i64, i1)
declare void @llvm.memcpy.p1.p0.i64(ptr addrspace(1), ptr, i64, i1)
declare void @llvm.memset.p1.i64(ptr addrspace(1), i8, i64, i1)

!air.kernel = !{!0}
!0 = !{ptr @large_buffer_memops, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 1, !"air.arg_type_align_size", i32 1, !"air.arg_type_name", !"uchar", !"air.arg_name", !"data"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"ulong", !"air.arg_name", !"control"}
