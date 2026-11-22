target triple = "air64_v28-apple-macosx26.6.0"
%Resources = type { [2 x ptr addrspace(1)], [2 x ptr addrspace(2)], ptr addrspace(2) }

define void @copy_resources(ptr addrspace(2) %source, ptr addrspace(1) %destination, i32 %index, ptr addrspace(1) %observed) {
entry:
  %element = and i32 %index, 1
  %wide = zext i32 %element to i64
  %texture_source = getelementptr %Resources, ptr addrspace(2) %source, i64 0, i32 0, i64 %wide
  %texture = load ptr addrspace(1), ptr addrspace(2) %texture_source, align 8
  %texture_destination = getelementptr %Resources, ptr addrspace(1) %destination, i64 0, i32 0, i64 %wide
  store ptr addrspace(1) %texture, ptr addrspace(1) %texture_destination, align 8
  %sampler_source = getelementptr %Resources, ptr addrspace(2) %source, i64 0, i32 1, i64 %wide
  %sampler = load ptr addrspace(2), ptr addrspace(2) %sampler_source, align 8
  %sampler_destination = getelementptr %Resources, ptr addrspace(1) %destination, i64 0, i32 1, i64 %wide
  store ptr addrspace(2) %sampler, ptr addrspace(1) %sampler_destination, align 8
  %data_source = getelementptr %Resources, ptr addrspace(2) %source, i64 0, i32 2
  %data = load ptr addrspace(2), ptr addrspace(2) %data_source, align 8
  %data_destination = getelementptr %Resources, ptr addrspace(1) %destination, i64 0, i32 2
  store ptr addrspace(2) %data, ptr addrspace(1) %data_destination, align 8
  %word = getelementptr i32, ptr addrspace(2) %data, i64 %wide
  %value = load i32, ptr addrspace(2) %word, align 4
  %result = getelementptr i32, ptr addrspace(1) %observed, i64 %wide
  store i32 %value, ptr addrspace(1) %result, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @copy_resources, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5, !9}
!3 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 40, !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.struct_type_info", !6, !"air.arg_type_size", i32 40, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Resources", !"air.arg_name", !"source"}
!4 = !{i32 1, !"air.indirect_buffer", !"air.buffer_size", i32 40, !"air.location_index", i32 5, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.struct_type_info", !6, !"air.arg_type_size", i32 40, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Resources", !"air.arg_name", !"destination"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"index"}
!6 = !{i32 0, i32 16, i32 0, !"array<texture2d<float, sample>, 2>", !"images", !"air.indirect_argument", !7, i32 16, i32 16, i32 0, !"array<sampler, 2>", !"filters", !"air.indirect_argument", !8, i32 32, i32 8, i32 0, !"uint", !"data", !"air.indirect_argument", !10}
!7 = !{i32 0, !"air.texture", !"air.location_index", i32 2, i32 2, !"air.sample", !"air.arg_type_name", !"array<texture2d<float, sample>, 2>", !"air.arg_name", !"images"}
!8 = !{i32 1, !"air.sampler", !"air.location_index", i32 7, i32 2, !"air.arg_type_name", !"array<sampler, 2>", !"air.arg_name", !"filters"}
!9 = !{i32 3, !"air.buffer", !"air.location_index", i32 7, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_name", !"uint", !"air.arg_name", !"observed"}
!10 = !{i32 2, !"air.buffer", !"air.location_index", i32 9, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"data"}
