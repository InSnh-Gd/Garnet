target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64"
target triple = "air64_v28-apple-macosx26.7.0"

define <4 x float> @large_buffer_fragment(ptr addrspace(1) %offsets, ptr addrspace(1) %data) {
entry:
  %offset = load i64, ptr addrspace(1) %offsets, align 8
  %source = getelementptr inbounds i8, ptr addrspace(1) %data, i64 %offset
  %value = load i32, ptr addrspace(1) %source, align 4
  %float = uitofp i32 %value to float
  %out0 = insertelement <4 x float> zeroinitializer, float %float, i32 0
  ret <4 x float> %out0
}

!air.fragment = !{!0}
!0 = !{ptr @large_buffer_fragment, !1, !2}
!1 = !{!3}
!2 = !{!4, !5}
!3 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!4 = !{i32 0, !"air.buffer", !"air.buffer_size", i32 8, !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 8, !"air.arg_type_align_size", i32 8, !"air.arg_type_name", !"ulong", !"air.arg_name", !"offsets"}
!5 = !{i32 1, !"air.buffer", !"air.buffer_size", i32 4, !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"data"}
