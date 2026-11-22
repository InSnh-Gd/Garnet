; Authored fragment graph: typed sampling establishes the image used by LOD queries.
target triple = "air64-apple-macosx"
%Root = type { [2 x %Record] }
%Record = type { [2 x ptr addrspace(1)], ptr addrspace(2) }
define <4 x float> @lod_graph(ptr addrspace(2) %args, <4 x float> %position, <2 x float> %coord, i32 %i) {
entry:
  %g = lshr i32 %i, 2
  %g1 = and i32 %g, 1
  %group = zext i32 %g1 to i64
  %ix = and i32 %i, 1
  %index = zext i32 %ix to i64
  %image_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group, i32 0, i64 %index
  %sampler_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group, i32 1
  %image = load ptr addrspace(1), ptr addrspace(2) %image_slot, align 8
  %sampler = load ptr addrspace(2), ptr addrspace(2) %sampler_slot, align 8
  %sample = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %coord, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %rgba = extractvalue { <4 x float>, i8 } %sample, 0
  %red = extractelement <4 x float> %rgba, i32 0
  %lod = call float @air.calculate_unclamped_lod_texture_2d(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %coord, i32 0)
  %width = call i32 @air.get_width_texture_2d(ptr addrspace(1) %image, i32 0)
  %size = uitofp i32 %width to float
  %out0 = insertelement <4 x float> zeroinitializer, float %lod, i32 0
  %out1 = insertelement <4 x float> %out0, float %red, i32 1
  %out2 = insertelement <4 x float> %out1, float %size, i32 2
  ret <4 x float> %out2
}
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)
declare float @air.calculate_unclamped_lod_texture_2d(ptr addrspace(1), ptr addrspace(2), <2 x float>, i32)
declare i32 @air.get_width_texture_2d(ptr addrspace(1), i32)
!air.fragment = !{!0}
!0 = !{ptr @lod_graph, !1, !3}
!1 = !{!2}
!2 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!3 = !{!4, !5, !6, !7}
!4 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"Root"}
!5 = !{i32 1, !"air.position", !"air.center", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!6 = !{i32 2, !"air.fragment_input", !"generated(coord)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float2", !"air.arg_name", !"coord"}
!7 = !{i32 3, !"air.fragment_input", !"generated(index)", !"air.flat", !"air.arg_type_name", !"uint", !"air.arg_name", !"index"}
