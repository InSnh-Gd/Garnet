; Authored helper clones: air.is_uniform facts must follow both clones through inlining and
; canonical id remapping.
target triple = "air64-apple-macosx"

define <4 x float> @uniform_helper(float %selector) {
entry:
  %left = tail call float @scaled_uniform(float %selector)
  %right = tail call float @scaled_uniform(float %selector)
  %sum = fadd float %left, %right
  %positive = fcmp ogt float %sum, 0.0
  br i1 %positive, label %above, label %below

above:
  %above_value = insertelement <4 x float> zeroinitializer, float %sum, i32 0
  br label %exit

below:
  %negated = fsub float 0.0, %sum
  %below_value = insertelement <4 x float> zeroinitializer, float %negated, i32 0
  br label %exit

exit:
  %result = phi <4 x float> [ %above_value, %above ], [ %below_value, %below ]
  ret <4 x float> %result
}

define internal float @scaled_uniform(float %value) {
entry:
  %scaled = fmul float %value, 2.0
  %same = tail call i1 @air.is_uniform.f32(float %scaled)
  tail call void @llvm.assume(i1 %same)
  ret float %scaled
}

declare i1 @air.is_uniform.f32(float)
declare void @llvm.assume(i1)

!air.fragment = !{!0}
!0 = !{ptr @uniform_helper, !1, !2}
!1 = !{!3}
!2 = !{!4}
!3 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!4 = !{i32 0, !"air.fragment_input", !"air.center", !"air.perspective", !"air.arg_type_name", !"float", !"air.arg_name", !"selector"}
