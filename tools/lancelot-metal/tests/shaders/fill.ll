source_filename = "lancelot_fill"
target triple = "air64-apple-ios16.5.0"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-n8:16:32"
define void @lancelot_fill(ptr addrspace(1) %out, i32 %tid) {
  %address = getelementptr i32, ptr addrspace(1) %out, i32 %tid
  %value = add i32 %tid, 305397760
  store i32 %value, ptr addrspace(1) %address, align 4
  ret void
}
!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}
!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @lancelot_fill, !3, !4}
!3 = !{}
!4 = !{!5, !6}
!5 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.address_space", i32 1, !"air.write", !"air.buffer_size", i32 256, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!6 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
