source_filename = "lancelot_scale"
target triple = "air64-apple-ios16.5.0"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-n8:16:32"
declare <4 x float> @air.read_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, i32)
declare void @air.write_texture_2d.v4f32(ptr addrspace(1), <2 x i32>, <4 x float>, i32)
define void @lancelot_scale(ptr addrspace(1) %src, ptr addrspace(1) %dst, ptr addrspace(2) %p, <2 x i32> %tid) {
entry:
  %x = extractelement <2 x i32> %tid, i32 0
  %y = extractelement <2 x i32> %tid, i32 1
  %p1 = getelementptr i32, ptr addrspace(2) %p, i32 1
  %p2 = getelementptr i32, ptr addrspace(2) %p, i32 2
  %p3 = getelementptr i32, ptr addrspace(2) %p, i32 3
  %sw = load i32, ptr addrspace(2) %p, align 4
  %sh = load i32, ptr addrspace(2) %p1, align 4
  %dw = load i32, ptr addrspace(2) %p2, align 4
  %dh = load i32, ptr addrspace(2) %p3, align 4
  %okx = icmp ult i32 %x, %dw
  %oky = icmp ult i32 %y, %dh
  %ok = and i1 %okx, %oky
  br i1 %ok, label %scale, label %end
scale:
  %xf = uitofp i32 %x to float
  %yf = uitofp i32 %y to float
  %swf = uitofp i32 %sw to float
  %shf = uitofp i32 %sh to float
  %dwf = uitofp i32 %dw to float
  %dhf = uitofp i32 %dh to float
  %xc = fadd float %xf, 0.5
  %yc = fadd float %yf, 0.5
  %xs = fmul float %xc, %swf
  %ys = fmul float %yc, %shf
  %xx = fdiv float %xs, %dwf
  %yy = fdiv float %ys, %dhf
  %xr = fsub float %xx, 0.5
  %yr = fsub float %yy, 0.5
  %xn = fcmp olt float %xr, 0.0
  %yn = fcmp olt float %yr, 0.0
  %xp = select i1 %xn, float 0.0, float %xr
  %yp = select i1 %yn, float 0.0, float %yr
  %wm = sub i32 %sw, 1
  %hm = sub i32 %sh, 1
  %x0 = fptoui float %xp to i32
  %y0 = fptoui float %yp to i32
  %xt = add i32 %x0, 1
  %yt = add i32 %y0, 1
  %xo = icmp ugt i32 %xt, %wm
  %yo = icmp ugt i32 %yt, %hm
  %x1 = select i1 %xo, i32 %wm, i32 %xt
  %y1 = select i1 %yo, i32 %hm, i32 %yt
  %x0f = uitofp i32 %x0 to float
  %y0f = uitofp i32 %y0 to float
  %wx = fsub float %xp, %x0f
  %wy = fsub float %yp, %y0f
  %vx = insertelement <4 x float> zeroinitializer, float %wx, i32 0
  %vy = insertelement <4 x float> zeroinitializer, float %wy, i32 0
  %wx4 = shufflevector <4 x float> %vx, <4 x float> poison, <4 x i32> zeroinitializer
  %wy4 = shufflevector <4 x float> %vy, <4 x float> poison, <4 x i32> zeroinitializer
  %c0 = insertelement <2 x i32> zeroinitializer, i32 %x0, i32 0
  %c1 = insertelement <2 x i32> zeroinitializer, i32 %x1, i32 0
  %c00 = insertelement <2 x i32> %c0, i32 %y0, i32 1
  %c10 = insertelement <2 x i32> %c1, i32 %y0, i32 1
  %c01 = insertelement <2 x i32> %c0, i32 %y1, i32 1
  %c11 = insertelement <2 x i32> %c1, i32 %y1, i32 1
  %a = call <4 x float> @air.read_texture_2d.v4f32(ptr addrspace(1) %src, <2 x i32> %c00, i32 0)
  %b = call <4 x float> @air.read_texture_2d.v4f32(ptr addrspace(1) %src, <2 x i32> %c10, i32 0)
  %c = call <4 x float> @air.read_texture_2d.v4f32(ptr addrspace(1) %src, <2 x i32> %c01, i32 0)
  %d = call <4 x float> @air.read_texture_2d.v4f32(ptr addrspace(1) %src, <2 x i32> %c11, i32 0)
  %ab = fsub <4 x float> %b, %a
  %cd = fsub <4 x float> %d, %c
  %abt = fmul <4 x float> %ab, %wx4
  %cdt = fmul <4 x float> %cd, %wx4
  %top = fadd <4 x float> %a, %abt
  %bottom = fadd <4 x float> %c, %cdt
  %delta = fsub <4 x float> %bottom, %top
  %blend = fmul <4 x float> %delta, %wy4
  %color = fadd <4 x float> %top, %blend
  call void @air.write_texture_2d.v4f32(ptr addrspace(1) %dst, <2 x i32> %tid, <4 x float> %color, i32 0)
  br label %end
end:
  ret void
}
!air.version = !{!0}
!air.language_version = !{!1}
!air.kernel = !{!2}
!0 = !{i32 2, i32 5, i32 0}
!1 = !{!"Metal", i32 3, i32 0, i32 0}
!2 = !{ptr @lancelot_scale, !3, !4}
!3 = !{}
!4 = !{!5, !6, !7, !8}
!5 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"texture2d<float, access::read>", !"air.arg_name", !"src"}
!6 = !{i32 1, !"air.texture", !"air.location_index", i32 1, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<float, access::write>", !"air.arg_name", !"dst"}
!7 = !{i32 2, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.address_space", i32 2, !"air.read", !"air.buffer_size", i32 16, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"dimensions"}
!8 = !{i32 3, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"tid"}
