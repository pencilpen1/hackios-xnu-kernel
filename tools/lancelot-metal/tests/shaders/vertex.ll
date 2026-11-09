source_filename = "lancelot_vertex"
target triple = "air64-apple-ios16.5.0"
target datalayout = "e-p:64:64:64-i64:64-v128:128-n8:16:32"
define <{<4 x float>}> @lancelot_vertex(i32 %vid) {
  %right = icmp eq i32 %vid, 1
  %bottom = icmp eq i32 %vid, 2
  %x = select i1 %right, float 3.0, float -1.0
  %y = select i1 %bottom, float 3.0, float -1.0
  %p0 = insertelement <4 x float> <float 0.0, float 0.0, float 0.0, float 1.0>, float %x, i32 0
  %p1 = insertelement <4 x float> %p0, float %y, i32 1
  %out = insertvalue <{<4 x float>}> undef, <4 x float> %p1, 0
  ret <{<4 x float>}> %out
}
!air.version = !{!0}
!air.language_version = !{!1}
!air.vertex = !{!2}
!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @lancelot_vertex, !3, !5}
!3 = !{!4}
!4 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!5 = !{!6}
!6 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
