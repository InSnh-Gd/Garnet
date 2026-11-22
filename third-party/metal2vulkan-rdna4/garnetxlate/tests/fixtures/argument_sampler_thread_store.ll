target triple = "air64_v28-apple-macosx26.5.0"
%ResourceArgs = type { %Sampler }
%Sampler = type { ptr addrspace(2) }

define void @store_thread_sampler(ptr addrspace(1) %destination, ptr addrspace(2) %input) {
entry:
  %slot = getelementptr inbounds %ResourceArgs, ptr addrspace(1) %destination, i64 0, i32 0, i32 0
  store ptr addrspace(2) %input, ptr addrspace(1) %slot, align 8
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @store_thread_sampler, !1, !2}
!1 = !{}
!2 = !{!3, !7}
!3 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 8, !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.struct_type_info", !4, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"ResourceArgs", !"air.arg_name", !"destination"}
!4 = !{i32 0, i32 8, i32 0, !"sampler", !"filter", !"air.indirect_argument", !5}
!5 = !{i32 0, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"filter"}
!7 = !{i32 1, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"input"}
