; Authored fragment graph: a varying position selects the image/sampler tuple used by a LOD query.
target triple = "air64-apple-macosx"
%Root = type { [2 x %Record] }
%Record = type { [2 x ptr addrspace(1)], ptr addrspace(2) }
define <4 x float> @lod_graph_nonuniform(ptr addrspace(2) %args, <4 x float> %position, <2 x float> %coord) {
entry:
  %px = extractelement <4 x float> %position, i32 0
  %py = extractelement <4 x float> %position, i32 1
  %scaled = fmul float %px, 5.000000e-01
  %selection = fptoui float %scaled to i32
  %g = lshr i32 %selection, 2
  %g1 = and i32 %g, 1
  %group = zext i32 %g1 to i64
  %ix = and i32 %selection, 1
  %index = zext i32 %ix to i64
  %image_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group, i32 0, i64 %index
  %sampler_slot = getelementptr %Root, ptr addrspace(2) %args, i64 0, i32 0, i64 %group, i32 1
  %image = load ptr addrspace(1), ptr addrspace(2) %image_slot, align 8
  %sampler = load ptr addrspace(2), ptr addrspace(2) %sampler_slot, align 8
  %sample = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %coord, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %rgba = extractvalue { <4 x float>, i8 } %sample, 0
  %red = extractelement <4 x float> %rgba, i32 0
  %lod = call float @air.calculate_unclamped_lod_texture_2d(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %coord, i32 0)
  %out0 = insertelement <4 x float> zeroinitializer, float %lod, i32 0
  %out1 = insertelement <4 x float> %out0, float %red, i32 1
  %out2 = insertelement <4 x float> %out1, float %py, i32 2
  ret <4 x float> %out2
}
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)
declare float @air.calculate_unclamped_lod_texture_2d(ptr addrspace(1), ptr addrspace(2), <2 x float>, i32)
!air.fragment = !{!0}
!0 = !{ptr @lod_graph_nonuniform, !1, !3}
!1 = !{!2}
!2 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!3 = !{!4, !5, !6}
!4 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"Root"}
!5 = !{i32 1, !"air.position", !"air.center", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!6 = !{i32 2, !"air.fragment_input", !"generated(coord)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float2", !"air.arg_name", !"coord"}
