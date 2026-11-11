source_filename = "lancelot_gaussian"
target triple = "air64-apple-ios16.5.0"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-n8:16:32"
declare <4 x float> @air.read_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, i32)
declare void @air.write_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, <4 x float>, i32)
define void @lancelot_gaussian(ptr addrspace(1) %src, ptr addrspace(1) %dst, ptr addrspace(2) %p, <2 x i32> %tid) {
entry:
  %x = extractelement <2 x i32> %tid, i32 0
  %y = extractelement <2 x i32> %tid, i32 1
  %pw = getelementptr i32, ptr addrspace(2) %p, i32 0
  %ph = getelementptr i32, ptr addrspace(2) %p, i32 1
  %pr = getelementptr i32, ptr addrspace(2) %p, i32 2
  %pa = getelementptr i32, ptr addrspace(2) %p, i32 3
  %pe = getelementptr i32, ptr addrspace(2) %p, i32 4
  %w = load i32, ptr addrspace(2) %pw, align 4
  %h = load i32, ptr addrspace(2) %ph, align 4
  %r = load i32, ptr addrspace(2) %pr, align 4
  %axis = load i32, ptr addrspace(2) %pa, align 4
  %edge = load i32, ptr addrspace(2) %pe, align 4
  %ix = icmp ult i32 %x, %w
  %iy = icmp ult i32 %y, %h
  %inside = and i1 %ix, %iy
  br i1 %inside, label %start, label %end
start:
  %wm = sub i32 %w, 1
  %hm = sub i32 %h, 1
  %first = sub i32 0, %r
  %horizontal = icmp eq i32 %axis, 0
  %clamp = icmp ne i32 %edge, 0
  br label %loop
loop:
  %i = phi i32 [ %first, %start ], [ %next, %step ]
  %sum = phi <4 x float> [ zeroinitializer, %start ], [ %added, %step ]
  %dx = select i1 %horizontal, i32 %i, i32 0
  %dy = select i1 %horizontal, i32 0, i32 %i
  %sx = add i32 %x, %dx
  %sy = add i32 %y, %dy
  %xn = icmp slt i32 %sx, 0
  %yn = icmp slt i32 %sy, 0
  %xp = select i1 %xn, i32 0, i32 %sx
  %yp = select i1 %yn, i32 0, i32 %sy
  %xo = icmp sgt i32 %xp, %wm
  %yo = icmp sgt i32 %yp, %hm
  %cx = select i1 %xo, i32 %wm, i32 %xp
  %cy = select i1 %yo, i32 %hm, i32 %yp
  %c0 = insertelement <2 x i32> zeroinitializer, i32 %cx, i32 0
  %coord = insertelement <2 x i32> %c0, i32 %cy, i32 1
  %texel = call <4 x float> @air.read_texture_2d.v4f32(ptr addrspace(1) %src, <2 x i32> %coord, i32 0)
  %badx = or i1 %xn, %xo
  %bady = or i1 %yn, %yo
  %bad = or i1 %badx, %bady
  %good = xor i1 %bad, true
  %use = or i1 %good, %clamp
  %color = select i1 %use, <4 x float> %texel, <4 x float> zeroinitializer
  %wi = add i32 %i, %r
  %index = add i32 %wi, 8
  %weightptr = getelementptr float, ptr addrspace(2) %p, i32 %index
  %weight = load float, ptr addrspace(2) %weightptr, align 4
  %wv = insertelement <4 x float> zeroinitializer, float %weight, i32 0
  %splat = shufflevector <4 x float> %wv, <4 x float> poison, <4 x i32> zeroinitializer
  %weighted = fmul <4 x float> %color, %splat
  %added = fadd <4 x float> %sum, %weighted
  %last = icmp eq i32 %i, %r
  br i1 %last, label %store, label %step
step:
  %next = add i32 %i, 1
  br label %loop
store:
  call void @air.write_texture_2d.v4f32(ptr addrspace(1) %dst, <2 x i32> %tid, <4 x float> %added, i32 0)
  br label %end
end:
  ret void
}
!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}
!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @lancelot_gaussian, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7, !8}
!5 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<float, access::read>", !"air.arg_name", !"src"}
!6 = !{i32 1, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<float, access::write>", !"air.arg_name", !"dst"}
!7 = !{i32 2, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.address_space", i32 2, !"air.read", !"air.buffer_size", i32 2084, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"parameters"}
!8 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"tid"}
