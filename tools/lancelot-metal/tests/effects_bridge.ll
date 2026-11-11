target triple = "x86_64-unknown-linux-gnu"
declare float @effect_read(ptr, i32, i32, i32)
declare void @effect_write(ptr, i32, i32, float, float, float, float)
declare void @lancelot_gaussian(ptr addrspace(1), ptr addrspace(1), ptr addrspace(2), <2 x i32>)
declare void @lancelot_scale(ptr addrspace(1), ptr addrspace(1), ptr addrspace(2), <2 x i32>)
define <4 x float> @air.read_texture_2d.v4f32(ptr addrspace(1) %t, <2 x i32> %c, i32 %lod) {
  %p = addrspacecast ptr addrspace(1) %t to ptr
  %x = extractelement <2 x i32> %c, i32 0
  %y = extractelement <2 x i32> %c, i32 1
  %r = call float @effect_read(ptr %p, i32 %x, i32 %y, i32 0)
  %g = call float @effect_read(ptr %p, i32 %x, i32 %y, i32 1)
  %b = call float @effect_read(ptr %p, i32 %x, i32 %y, i32 2)
  %a = call float @effect_read(ptr %p, i32 %x, i32 %y, i32 3)
  %v0 = insertelement <4 x float> zeroinitializer, float %r, i32 0
  %v1 = insertelement <4 x float> %v0, float %g, i32 1
  %v2 = insertelement <4 x float> %v1, float %b, i32 2
  %v3 = insertelement <4 x float> %v2, float %a, i32 3
  ret <4 x float> %v3
}
define void @air.write_texture_2d.v4f32(ptr addrspace(1) %t, <2 x i32> %c, <4 x float> %v, i32 %lod) {
  %p = addrspacecast ptr addrspace(1) %t to ptr
  %x = extractelement <2 x i32> %c, i32 0
  %y = extractelement <2 x i32> %c, i32 1
  %r = extractelement <4 x float> %v, i32 0
  %g = extractelement <4 x float> %v, i32 1
  %b = extractelement <4 x float> %v, i32 2
  %a = extractelement <4 x float> %v, i32 3
  call void @effect_write(ptr %p, i32 %x, i32 %y, float %r, float %g, float %b, float %a)
  ret void
}
define void @shader_call(i32 %kind, ptr %source, ptr %dest, ptr %params, i32 %x, i32 %y) {
  %s = addrspacecast ptr %source to ptr addrspace(1)
  %d = addrspacecast ptr %dest to ptr addrspace(1)
  %p = addrspacecast ptr %params to ptr addrspace(2)
  %v0 = insertelement <2 x i32> zeroinitializer, i32 %x, i32 0
  %v1 = insertelement <2 x i32> %v0, i32 %y, i32 1
  %gauss = icmp eq i32 %kind, 0
  br i1 %gauss, label %g, label %b
g:
  call void @lancelot_gaussian(ptr addrspace(1) %s, ptr addrspace(1) %d, ptr addrspace(2) %p, <2 x i32> %v1)
  ret void
b:
  call void @lancelot_scale(ptr addrspace(1) %s, ptr addrspace(1) %d, ptr addrspace(2) %p, <2 x i32> %v1)
  ret void
}
