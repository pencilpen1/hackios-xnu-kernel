source_filename = "lancelot_fragment"
target triple = "air64-apple-ios16.5.0"
target datalayout = "e-p:64:64:64-i64:64-v128:128-n8:16:32"
define <{<4 x float>}> @lancelot_fragment() {
  ret <{<4 x float>}> <{<4 x float> <float 0.25, float 0.5, float 0.75, float 1.0>}>
}
!air.version = !{!0}
!air.language_version = !{!1}
!air.fragment = !{!2}
!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @lancelot_fragment, !3, !5}
!3 = !{!4}
!4 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!5 = !{}
