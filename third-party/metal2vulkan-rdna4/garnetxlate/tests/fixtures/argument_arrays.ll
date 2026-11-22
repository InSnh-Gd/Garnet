target triple = "air64_v28-apple-macosx26.7.0"
%Args = type { ptr addrspace(1), %Pointers, i32, %Weights }
%Pointers = type { [2 x ptr addrspace(1)] }
%Weights = type { [3 x i32] }

define void @argument_arrays(ptr addrspace(1) %args, i32 %i) {
entry:
  %input_slot = getelementptr inbounds %Args, ptr addrspace(1) %args, i64 0, i32 0
  %input = load ptr addrspace(1), ptr addrspace(1) %input_slot, align 8
  %index = zext i32 %i to i64
  %input_word = getelementptr inbounds i32, ptr addrspace(1) %input, i64 %index
  %value = load i32, ptr addrspace(1) %input_word, align 4
  %bias_slot = getelementptr inbounds %Args, ptr addrspace(1) %args, i64 0, i32 2
  %bias = load i32, ptr addrspace(1) %bias_slot, align 4
  %weight0_slot = getelementptr inbounds %Args, ptr addrspace(1) %args, i64 0, i32 3, i32 0, i64 0
  %weight0 = load i32, ptr addrspace(1) %weight0_slot, align 4
  %flipped = xor i32 %value, %weight0
  %base = add i32 %flipped, %bias
  %result0 = add i32 %base, %i
  %out0_slot = getelementptr inbounds %Args, ptr addrspace(1) %args, i64 0, i32 1, i32 0, i64 0
  %out0 = load ptr addrspace(1), ptr addrspace(1) %out0_slot, align 8
  %out0_word = getelementptr inbounds i32, ptr addrspace(1) %out0, i64 %index
  store i32 %result0, ptr addrspace(1) %out0_word, align 4
  %remainder = urem i32 %i, 3
  %weight_index = zext i32 %remainder to i64
  %weight_slot = getelementptr inbounds %Args, ptr addrspace(1) %args, i64 0, i32 3, i32 0, i64 %weight_index
  %weight = load i32, ptr addrspace(1) %weight_slot, align 4
  %product = mul i32 %value, %weight
  %result1 = add i32 %product, %bias
  %out1_slot = getelementptr inbounds %Args, ptr addrspace(1) %args, i64 0, i32 1, i32 0, i64 1
  %out1 = load ptr addrspace(1), ptr addrspace(1) %out1_slot, align 8
  %out1_word = getelementptr inbounds i32, ptr addrspace(1) %out1, i64 %index
  store i32 %result1, ptr addrspace(1) %out1_word, align 4
  ret void
}

!air.kernel = !{!0}
!0 = !{ptr @argument_arrays, !1, !2}
!1 = !{}
!2 = !{!3, !11}
!3 = !{i32 0, !"air.indirect_buffer", !"air.buffer_size", i32 40, !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 1, !"air.struct_type_info", !4, !"air.arg_type_size", i32 40, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"Args", !"air.arg_name", !"args"}
!4 = !{i32 0, i32 8, i32 0, !"uint", !"input", !"air.indirect_argument", !5, !"air.struct_type_info", !6, i32 8, i32 16, i32 0, !"metal::array", !"outputs", !"air.indirect_argument", i32 5, i32 24, i32 4, i32 0, !"uint", !"bias", !"air.indirect_argument", !8, !"air.struct_type_info", !9, i32 28, i32 12, i32 0, !"metal::array", !"weights", !"air.indirect_argument", i32 16}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 2, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!6 = !{i32 0, i32 8, i32 2, !"uint", !"__elems", !"air.indirect_argument", !7}
!7 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_name", !"uint"}
!8 = !{i32 2, !"air.indirect_constant", !"air.location_index", i32 11, i32 1, !"air.arg_type_name", !"uint"}
!9 = !{i32 0, i32 4, i32 3, !"uint", !"__elems", !"air.indirect_argument", !10}
!10 = !{i32 0, !"air.indirect_constant", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"uint"}
!11 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"i"}
