
target triple = "air64-apple-macosx"
%Args = type { [2 x ptr addrspace(1)], [2 x ptr addrspace(2)] }
define <4 x float> @main(ptr addrspace(2) %args, <4 x float> %position, ptr addrspace(2) readonly align 4 %layers) {
entry:
  %x = extractelement <4 x float> %position, i32 0
  %pixel = call i32 @air.convert.u.i32.f.f32(float %x)
  %quad = and i32 %pixel, 2
  %condition = icmp ne i32 %quad, 0
  %index32 = and i32 %pixel, 1
  %index = zext i32 %index32 to i64
  %image_slot = getelementptr %Args, ptr addrspace(2) %args, i64 0, i32 0, i64 %index
  %sampler_slot = getelementptr %Args, ptr addrspace(2) %args, i64 0, i32 1, i64 %index
  %image = load ptr addrspace(1), ptr addrspace(2) %image_slot, align 8
  %sampler = load ptr addrspace(2), ptr addrspace(2) %sampler_slot, align 8
  %xy = shufflevector <4 x float> %position, <4 x float> poison, <2 x i32> <i32 0, i32 1>
  br i1 %condition, label %arm, label %skip
arm:
  %layer = load i32, ptr addrspace(2) %layers, align 4
  %scale = call float @scale_helper.MTL_VISIBLE_FN_REF(ptr addrspace(2) %layers)
  %scale0 = insertelement <2 x float> poison, float %scale, i32 0
  %scale2 = shufflevector <2 x float> %scale0, <2 x float> poison, <2 x i32> zeroinitializer
  %coord = fmul <2 x float> %xy, %scale2
  %sample = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %image, ptr addrspace(2) %sampler, <2 x float> %coord,  i1 true, <2 x i32> zeroinitializer, i1 false, float 0.0, float 0.0, i32 0)
  %rgba = extractvalue { <4 x float>, i8 } %sample, 0
  br label %merge
skip:
  br label %merge
merge:
  %color = phi <4 x float> [ %rgba, %arm ], [ zeroinitializer, %skip ]
  ret <4 x float> %color
}
declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>,  i1, <2 x i32>, i1, float, float, i32)
declare i32 @air.convert.u.i32.f.f32(float)
!air.fragment = !{!0}
!0 = !{ptr @main, !1, !2}
!1 = !{!6}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.indirect_buffer", !"air.location_index", i32 3, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"Args"}
!4 = !{i32 1, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4"}
!5 = !{i32 2, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read", !"air.address_space", i32 2, !"air.arg_type_name", !"uint"}
!6 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}

declare float @scale_helper.MTL_VISIBLE_FN_REF(ptr addrspace(2)) section "air.externally_defined"
!air.visible_function_references = !{!7}
!7 = !{!"air.visible_function_reference", ptr @scale_helper.MTL_VISIBLE_FN_REF, !"scale_helper"}
