; Authored dynamic-token image matrix: sampled float/uint/sint/depth operations,
; nonzero mip/layer/offset, every image query family, and storage read/write.
target triple = "air64-apple-macosx"
%Root = type { [2 x %Record] }
%Record = type { [2 x ptr addrspace(1)], ptr addrspace(2), [2 x ptr addrspace(1)], [2 x ptr addrspace(1)], [2 x ptr addrspace(1)], [2 x ptr addrspace(1)], [2 x ptr addrspace(1)], [2 x ptr addrspace(1)], [2 x ptr addrspace(1)], [2 x ptr addrspace(1)], [2 x ptr addrspace(1)], [2 x ptr addrspace(1)] }

define internal float @sample_float_level(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %coord) {
entry:
  %sample = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %coord, i1 true, <2 x i32> <i32 1, i32 -1>, i1 true, float 2.0, float 0.0, i32 0)
  %rgba = extractvalue { <4 x float>, i8 } %sample, 0
  %red = extractelement <4 x float> %rgba, i32 0
  ret float %red
}

define internal float @sample_float_gradient(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %coord, <2 x float> %dx, <2 x float> %dy) {
entry:
  %grad_sample = call { <4 x float>, i8 } @air.sample_texture_2d_grad.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %coord, <2 x float> %dx, <2 x float> %dy, float 0.0, i1 true, <2 x i32> zeroinitializer, i32 0)
  %grad_rgba = extractvalue { <4 x float>, i8 } %grad_sample, 0
  %green = extractelement <4 x float> %grad_rgba, i32 1
  ret float %green
}

define void @image_operations(ptr addrspace(2) %args, ptr addrspace(1) %out, i32 %i) {
entry:
  %group = and i32 %i, 1
  %group64 = zext i32 %group to i64
  %index = and i32 %i, 1
  %index64 = zext i32 %index to i64
  %mip = and i32 %i, 1
  %layer = and i32 %i, 1

  %image_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group64, i32 0, i64 %index64
  %sampler_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group64, i32 1
  %depth_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group64, i32 2, i64 %index64
  %array_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group64, i32 3, i64 %index64
  %image3d_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group64, i32 4, i64 %index64
  %image_ms_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group64, i32 5, i64 %index64
  %storage_f_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group64, i32 6, i64 %index64
  %storage_u_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group64, i32 7, i64 %index64
  %storage_s_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group64, i32 8, i64 %index64
  %image_u_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group64, i32 9, i64 %index64
  %image_s_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group64, i32 10, i64 %index64
  %image_1d_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group64, i32 11, i64 %index64

  %image_token = load i64, ptr addrspace(2) %image_slot, align 8
  %sampler = load ptr addrspace(2), ptr addrspace(2) %sampler_slot, align 8
  %depth_token = load i64, ptr addrspace(2) %depth_slot, align 8
  %array_token = load i64, ptr addrspace(2) %array_slot, align 8
  %image3d_token = load i64, ptr addrspace(2) %image3d_slot, align 8
  %image_ms_token = load i64, ptr addrspace(2) %image_ms_slot, align 8
  %storage_f_token = load i64, ptr addrspace(2) %storage_f_slot, align 8
  %storage_u_token = load i64, ptr addrspace(2) %storage_u_slot, align 8
  %storage_s_token = load i64, ptr addrspace(2) %storage_s_slot, align 8
  %image_u_token = load i64, ptr addrspace(2) %image_u_slot, align 8
  %image_s_token = load i64, ptr addrspace(2) %image_s_slot, align 8
  %image_1d_token = load i64, ptr addrspace(2) %image_1d_slot, align 8

  %image = inttoptr i64 %image_token to ptr addrspace(1)
  %depth = inttoptr i64 %depth_token to ptr addrspace(1)
  %image_array = inttoptr i64 %array_token to ptr addrspace(1)
  %image3d = inttoptr i64 %image3d_token to ptr addrspace(1)
  %image_ms = inttoptr i64 %image_ms_token to ptr addrspace(1)
  %storage_f = inttoptr i64 %storage_f_token to ptr addrspace(1)
  %storage_u = inttoptr i64 %storage_u_token to ptr addrspace(1)
  %storage_s = inttoptr i64 %storage_s_token to ptr addrspace(1)
  %image_u = inttoptr i64 %image_u_token to ptr addrspace(1)
  %image_s = inttoptr i64 %image_s_token to ptr addrspace(1)
  %image_1d = inttoptr i64 %image_1d_token to ptr addrspace(1)

  %coord = insertelement <2 x float> poison, float 0.25, i32 0
  %coord1 = insertelement <2 x float> %coord, float 0.75, i32 1
  %dx = fmul <2 x float> %coord1, <float 0.5, float 0.0>
  %dy = fmul <2 x float> %coord1, <float 0.0, float 0.5>
  %level_value = call float @sample_float_level(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %coord1)
  %gradient_value = call float @sample_float_gradient(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %coord1, <2 x float> %dx, <2 x float> %dy)

  %uint_sample = call { <4 x i32>, i8 } @air.sample_texture_2d.u.v4i32(ptr addrspace(1) %image_u, ptr addrspace(2) %sampler, <2 x float> %coord1, i1 true, <2 x i32> zeroinitializer, i1 true, float 1.0, float 0.0, i32 0)
  %uint_rgba = extractvalue { <4 x i32>, i8 } %uint_sample, 0
  %uint_value = extractelement <4 x i32> %uint_rgba, i32 0

  %sint_sample = call { <4 x i32>, i8 } @air.sample_texture_2d.v4i32(ptr addrspace(1) %image_s, ptr addrspace(2) %sampler, <2 x float> %coord1, i1 true, <2 x i32> zeroinitializer, i1 true, float 1.0, float 0.0, i32 0)
  %sint_rgba = extractvalue { <4 x i32>, i8 } %sint_sample, 0
  %sint_value = extractelement <4 x i32> %sint_rgba, i32 0

  %depth_sample = call { float, i8 } @air.sample_depth_2d.f32(ptr addrspace(1) %depth, ptr addrspace(2) %sampler, i32 0, <2 x float> %coord1, i1 true, <2 x i32> zeroinitializer, i1 true, float 2.0, float 0.0, i32 0)
  %depth_value = extractvalue { float, i8 } %depth_sample, 0
  %depth_compare = call { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1) %depth, ptr addrspace(2) %sampler, i32 0, <2 x float> %coord1, float 0.5, i1 true, <2 x i32> zeroinitializer, i1 true, float 0.0, float 0.0, i32 0)
  %compare_value = extractvalue { float, i8 } %depth_compare, 0

  %array_sample = call { <4 x float>, i8 } @air.sample_texture_2d_array.v4f32(ptr addrspace(1) %image_array, ptr addrspace(2) %sampler, <2 x float> %coord1, i32 %layer, i1 true, <2 x i32> zeroinitializer, i1 true, float 1.0, float 0.0, i32 0)
  %array_rgba = extractvalue { <4 x float>, i8 } %array_sample, 0
  %array_value = extractelement <4 x float> %array_rgba, i32 2

  %float_fetch = call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %image, <2 x i32> zeroinitializer, i32 0, i32 1)
  %float_fetch_rgba = extractvalue { <4 x float>, i8 } %float_fetch, 0
  %float_fetch_value = extractelement <4 x float> %float_fetch_rgba, i32 3
  %uint_fetch = call { <4 x i32>, i8 } @air.read_texture_2d.u.v4i32(ptr addrspace(1) %image_u, <2 x i32> zeroinitializer, i32 0, i32 1)
  %uint_fetch_rgba = extractvalue { <4 x i32>, i8 } %uint_fetch, 0
  %uint_fetch_value = extractelement <4 x i32> %uint_fetch_rgba, i32 1
  %sint_fetch = call { <4 x i32>, i8 } @air.read_texture_2d.v4i32(ptr addrspace(1) %image_s, <2 x i32> zeroinitializer, i32 0, i32 1)
  %sint_fetch_rgba = extractvalue { <4 x i32>, i8 } %sint_fetch, 0
  %sint_fetch_value = extractelement <4 x i32> %sint_fetch_rgba, i32 2

  %width1d = call i32 @air.get_width_texture_1d(ptr addrspace(1) %image_1d, i32 0)
  %width = call i32 @air.get_width_texture_2d(ptr addrspace(1) %image, i32 %mip)
  %height = call i32 @air.get_height_texture_2d(ptr addrspace(1) %image, i32 %mip)
  %width3d = call i32 @air.get_width_texture_3d(ptr addrspace(1) %image3d, i32 %mip)
  %height3d = call i32 @air.get_height_texture_3d(ptr addrspace(1) %image3d, i32 %mip)
  %depth3d = call i32 @air.get_depth_texture_3d(ptr addrspace(1) %image3d, i32 %mip)
  %array_size = call i32 @air.get_array_size_texture_2d_array(ptr addrspace(1) %image_array)
  %mip_count = call i32 @air.get_num_mip_levels_texture_2d(ptr addrspace(1) %image)
  %sample_count = call i32 @air.get_num_samples_texture_2d_ms(ptr addrspace(1) %image_ms)

  %store_read = call { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1) %storage_f, <2 x i32> zeroinitializer, i32 0, i32 3)
  %store_rgba = extractvalue { <4 x float>, i8 } %store_read, 0
  %store_red = extractelement <4 x float> %store_rgba, i32 0
  %write_texel = insertelement <4 x float> %store_rgba, float 91.0, i32 0
  call void @air.write_texture_2d.v4f32(ptr addrspace(1) %storage_f, <2 x i32> zeroinitializer, <4 x float> %write_texel, i32 0, i32 3)

  %uint_read = call { <4 x i32>, i8 } @air.read_texture_2d.u.v4i32(ptr addrspace(1) %storage_u, <2 x i32> zeroinitializer, i32 0, i32 3)
  %uint_read_rgba = extractvalue { <4 x i32>, i8 } %uint_read, 0
  %uint_read_value = extractelement <4 x i32> %uint_read_rgba, i32 0
  %uint_write_texel = insertelement <4 x i32> %uint_read_rgba, i32 4000000000, i32 0
  call void @air.write_texture_2d.u.v4i32(ptr addrspace(1) %storage_u, <2 x i32> zeroinitializer, <4 x i32> %uint_write_texel, i32 0, i32 3)

  %sint_read = call { <4 x i32>, i8 } @air.read_texture_2d.v4i32(ptr addrspace(1) %storage_s, <2 x i32> zeroinitializer, i32 0, i32 3)
  %sint_read_rgba = extractvalue { <4 x i32>, i8 } %sint_read, 0
  %sint_read_value = extractelement <4 x i32> %sint_read_rgba, i32 0
  %sint_write_texel = insertelement <4 x i32> %sint_read_rgba, i32 -91, i32 0
  call void @air.write_texture_2d.v4i32(ptr addrspace(1) %storage_s, <2 x i32> zeroinitializer, <4 x i32> %sint_write_texel, i32 0, i32 3)

  %sum0 = fadd float %level_value, %gradient_value
  %sum1 = fadd float %sum0, %depth_value
  %sum2 = fadd float %sum1, %compare_value
  %sum3 = fadd float %sum2, %array_value
  %sum4 = fadd float %sum3, %store_red
  %sum5 = fadd float %sum4, %float_fetch_value
  %size_1d = add i32 %width1d, %width
  %size_sum = add i32 %size_1d, %height
  %size3d_sum0 = add i32 %width3d, %height3d
  %size3d_sum1 = add i32 %size3d_sum0, %depth3d
  %counts = add i32 %array_size, %mip_count
  %counts1 = add i32 %counts, %sample_count
  %int_sum = add i32 %size_sum, %size3d_sum1
  %int_sum1 = add i32 %int_sum, %counts1
  %int_sum2 = add i32 %int_sum1, %uint_value
  %int_sum3 = add i32 %int_sum2, %uint_read_value
  %int_sum4 = add i32 %int_sum3, %uint_fetch_value
  %int_sum5 = add i32 %int_sum4, %sint_value
  %int_sum6 = add i32 %int_sum5, %sint_read_value
  %int_sum7 = add i32 %int_sum6, %sint_fetch_value
  %int_f = sitofp i32 %int_sum7 to float
  %result = fadd float %sum5, %int_f
  store float %result, ptr addrspace(1) %out, align 4
  ret void
}

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)
declare { <4 x float>, i8 } @air.sample_texture_2d_grad.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, <2 x float>, <2 x float>, float, i1, <2 x i32>, i32)
declare { <4 x i32>, i8 } @air.sample_texture_2d.u.v4i32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)
declare { <4 x i32>, i8 } @air.sample_texture_2d.v4i32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)
declare { float, i8 } @air.sample_depth_2d.f32(ptr addrspace(1), ptr addrspace(2), i32, <2 x float>, i1, <2 x i32>, i1, float, float, i32)
declare { float, i8 } @air.sample_compare_depth_2d.f32(ptr addrspace(1), ptr addrspace(2), i32, <2 x float>, float, i1, <2 x i32>, i1, float, float, i32)
declare { <4 x float>, i8 } @air.sample_texture_2d_array.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i32, i1, <2 x i32>, i1, float, float, i32)
declare i32 @air.get_width_texture_1d(ptr addrspace(1), i32)
declare i32 @air.get_width_texture_2d(ptr addrspace(1), i32)
declare i32 @air.get_height_texture_2d(ptr addrspace(1), i32)
declare i32 @air.get_width_texture_3d(ptr addrspace(1), i32)
declare i32 @air.get_height_texture_3d(ptr addrspace(1), i32)
declare i32 @air.get_depth_texture_3d(ptr addrspace(1), i32)
declare i32 @air.get_array_size_texture_2d_array(ptr addrspace(1))
declare i32 @air.get_num_mip_levels_texture_2d(ptr addrspace(1))
declare i32 @air.get_num_samples_texture_2d_ms(ptr addrspace(1))
declare { <4 x float>, i8 } @air.read_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, i32, i32)
declare void @air.write_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, <4 x float>, i32, i32)
declare { <4 x i32>, i8 } @air.read_texture_2d.u.v4i32(ptr addrspace(1), <2 x i32>, i32, i32)
declare void @air.write_texture_2d.u.v4i32(ptr addrspace(1), <2 x i32>, <4 x i32>, i32, i32)
declare { <4 x i32>, i8 } @air.read_texture_2d.v4i32(ptr addrspace(1), <2 x i32>, i32, i32)
declare void @air.write_texture_2d.v4i32(ptr addrspace(1), <2 x i32>, <4 x i32>, i32, i32)

!air.kernel = !{!0}
!0 = !{ptr @image_operations, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"Root"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_name", !"float"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint"}
