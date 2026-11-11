#!/usr/bin/env python3
"""Apply the Lancelot-only Mesa overlay to a clean archive tree."""
from pathlib import Path
import shutil, sys, re
root=Path(__file__).resolve().parent
src=Path(sys.argv[1]).resolve()
if not (src/'meson.build').exists() or not (src/'src/gallium').is_dir(): raise SystemExit('Mesa source tree expected (meson.build + src/gallium)')
def change(path,old,new):
 p=src/path; data=p.read_text()
 if old in data:
  p.write_text(data.replace(old,new,1)); return
 # Continuation indentation differs among generated/upstream Mesa files. Only
 # tolerate leading whitespace: every identifier, operator and literal must
 # still match, at exactly one location. Semantic/ambiguous hunks stay fatal.
 if '\n' in old:
  lines=old.split('\n')
  pattern='(?m)^'+r'[ \t]*'+re.escape(lines[0].lstrip(' \t'))
  for line in lines[1:]:pattern+='\n'+r'[ \t]*'+re.escape(line.lstrip(' \t'))
  matches=list(re.finditer(pattern,data))
  if len(matches)==1:
   m=matches[0];p.write_text(data[:m.start()]+new+data[m.end():]);return
 raise SystemExit(f'{path}: expected exact source hunk missing: {old[:80]!r}')
change(Path('meson.build'),'if dep_libdrm.found()\n',"if host_machine.system() == 'darwin' and with_gallium_panfrost\n  dep_libdrm = declare_dependency(include_directories : include_directories('src/lancelot-drm'), link_args : ['-framework', 'IOKit', '-framework', 'CoreFoundation'])\n  pre_args += '-DHAVE_LIBDRM'\nelif dep_libdrm.found()\n")
change(Path('src/compiler/meson.build'),"subdir('spirv')","subdir('spirv')\nsubdir('air')")
# Byte-address expressions must be evaluated before casting to a packed
# descriptor pointer. Otherwise cpu + 16 becomes packed_cpu + 16 (256 bytes
# for ATTRIBUTE_BUFFER), corrupting a 32-byte preload allocation on Bifrost.
change(Path('src/panfrost/lib/genxml/pan_pack_helpers.h'),
       'pan_pack((PREFIX2(T, PACKED_T) *)dst, T, name)',
       'pan_pack((PREFIX2(T, PACKED_T) *)(dst), T, name)')
change(Path('src/panfrost/lib/genxml/pan_pack_helpers.h'),
       'pan_pack_nodefaults((PREFIX2(T, PACKED_T) *)dst, T, name)',
       'pan_pack_nodefaults((PREFIX2(T, PACKED_T) *)(dst), T, name)')
change(Path('src/panfrost/lib/kmod/pan_kmod.h'),'   off_t mmap_offset;\n','''#ifdef __APPLE__
   return lancelot_bo_map(bo->dev->fd, bo->handle, bo->size, bo_offset,
                          size, prot, flags, host_addr);
#else
   off_t mmap_offset;
''')
change(Path('src/panfrost/lib/kmod/pan_kmod.h'),'   return host_addr;\n}','   return host_addr;\n#endif\n}')
p=src/'src/gallium/drivers/panfrost/pan_bo.c'; data=p.read_text()
pat=r'(?ms)^\s*if \(os_munmap\(\(void \*\)\(uintptr_t\)bo->ptr\.cpu, panfrost_bo_size\(bo\)\)\) \{\s*mesa_loge\("munmap failed: %s", strerror\(errno\)\);\s*abort\(\);\s*\}'
rep='''#ifdef __APPLE__
   if (lancelot_bo_unmap(panfrost_device_fd(bo->dev), (void *)(uintptr_t)bo->ptr.cpu,
                         panfrost_bo_size(bo))) {
#else
   if (os_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {
#endif
      mesa_loge("munmap failed: %s", strerror(errno));
      abort();
   }'''
data,n=re.subn(pat,rep,data,count=1)
if n!=1: raise SystemExit('pan_bo.c: munmap block missing')
p.write_text(data)
change(Path('src/panfrost/lib/kmod/pan_kmod.c'),'size_t size = lseek(fd, 0, SEEK_END);','''size_t size;
#ifdef __APPLE__
       size = lancelot_bo_import_size(dev->fd, handle);
#else
       size = lseek(fd, 0, SEEK_END);
#endif''')
shutil.copytree(root/'air',src/'src/compiler/air',dirs_exist_ok=True)
shutil.copytree(root/'lancelot-drm',src/'src/lancelot-drm',dirs_exist_ok=True)
change(Path('src/panfrost/lib/kmod/meson.build'),"'panfrost_kmod.c',","'panfrost_kmod.c',\n  '../../../lancelot-drm/lancelot_drm.c',")
with (src/'src/gallium/drivers/panfrost/meson.build').open('a') as f: f.write('\n'+(root/'air-targets.meson').read_text())
change(Path('src/gallium/targets/osmesa/meson.build'),'driver_swrast, idep_mesautil,','driver_swrast, driver_panfrost, dep_libdrm, idep_mesautil,')
change(Path('src/gallium/targets/osmesa/osmesa_target.c'),'#include "sw/null/null_sw_winsys.h"','''#include "sw/null/null_sw_winsys.h"
#ifdef GALLIUM_PANFROST
#include "panfrost/drm/panfrost_drm_public.h"
#include <xf86drm.h>
#endif''')
change(Path('src/gallium/targets/osmesa/osmesa_target.c'),'''   struct sw_winsys *winsys;
   struct pipe_screen *screen;

   /* We use a null software winsys''','''#ifdef GALLIUM_PANFROST
   if (!debug_get_bool_option("OSMESA_SOFTWARE", false)) {
      int fd = drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER);
      if (fd >= 0) {
         struct pipe_screen_config config = {0};
         struct pipe_screen *gpu = panfrost_drm_screen_create(fd, &config);
         drmClose(fd);
         if (gpu) return debug_screen_wrap(gpu);
      }
      if (debug_get_bool_option("OSMESA_REQUIRE_HARDWARE", false)) return NULL;
   }
#endif
   struct sw_winsys *winsys;
   struct pipe_screen *screen;

   /* We use a null software winsys''')

# Bridge sources and shared wire header are copied, never symlinked to another tree.
shutil.copyfile(root.parents[1]/'xnu/files/iokit/Kernel/lancelot_mali_uapi.h',src/'src/lancelot-drm/lancelot_mali_uapi.h')
shutil.copyfile(root.parents[1]/'xnu/files/iokit/Kernel/lancelot_mali_queue_uapi.h',src/'src/lancelot-drm/lancelot_mali_queue_uapi.h')
change(Path('src/panfrost/lib/kmod/meson.build'),"'../../../lancelot-drm/lancelot_drm.c',", "'../../../lancelot-drm/lancelot_drm.c',\n  '../../../lancelot-drm/lancelot_transport.c',")
# Explicit fd aliases preserve the IOKit connection; no /dev/null inode screen cache.
change(Path('src/panfrost/lib/kmod/pan_kmod_backend.h'),'      close(dev->fd);','''#ifdef __APPLE__
      drmClose(dev->fd);
#else
      close(dev->fd);
#endif''')
change(Path('src/gallium/drivers/panfrost/pan_device.c'),'      close(fd);','''#ifdef __APPLE__
      drmClose(fd);
#else
      close(fd);
#endif''')
change(Path('src/gallium/winsys/panfrost/drm/panfrost_drm_winsys.c'),'''   return u_pipe_screen_lookup_or_create(os_dupfd_cloexec(fd), config, NULL,
                                         panfrost_create_screen);''','''#ifdef __APPLE__
   int alias = lancelot_drm_dup(fd);
   return alias < 0 ? NULL : panfrost_create_screen(alias, config, NULL);
#else
   return u_pipe_screen_lookup_or_create(os_dupfd_cloexec(fd), config, NULL,
                                         panfrost_create_screen);
#endif''')
change(Path('src/gallium/winsys/panfrost/drm/panfrost_drm_winsys.c'),'''   return u_pipe_screen_lookup_or_create(os_dupfd_cloexec(fd), config, ro,
                                         panfrost_create_screen);''','''#ifdef __APPLE__
   int alias = lancelot_drm_dup(fd);
   return alias < 0 ? NULL : panfrost_create_screen(alias, config, ro);
#else
   return u_pipe_screen_lookup_or_create(os_dupfd_cloexec(fd), config, ro,
                                         panfrost_create_screen);
#endif''')
change(Path('src/gallium/drivers/panfrost/pan_device.c'),'''         dev, 128 * 1024 * 1024, PAN_BO_INVISIBLE | PAN_BO_GROWABLE, "Tiler heap");''','''#ifdef __APPLE__
         dev, 128 * 1024 * 1024, PAN_BO_INVISIBLE | PAN_BO_GROWABLE, "Tiler heap");
#else
         dev, 128 * 1024 * 1024, PAN_BO_INVISIBLE | PAN_BO_GROWABLE, "Tiler heap");
#endif''')
# Panfrost queries DRI options even with XML parsing disabled: the generated
# no-XML implementation must still be linked, not patched into a build by hand.
change(Path('src/gallium/drivers/panfrost/meson.build'),
       '    idep_mesautil,\n    idep_nir,',
       '    idep_mesautil,\n    idep_xmlconfig,\n    idep_nir,')
change(Path('include/GL/osmesa.h'),
'#define OSMESA_CONTEXT_MINOR_VERSION 0x37\n',
'#define OSMESA_CONTEXT_MINOR_VERSION 0x37\n#define OSMESA_ES_PROFILE            0x38  /* an OpenGL ES 2 or 3 context, by the version asked for */\n')

change(Path('src/gallium/frontends/osmesa/osmesa.c'),
'         if (profile != OSMESA_CORE_PROFILE &&\n             profile != OSMESA_COMPAT_PROFILE)\n',
'         if (profile != OSMESA_CORE_PROFILE &&\n             profile != OSMESA_COMPAT_PROFILE &&\n             profile != OSMESA_ES_PROFILE)\n')

change(Path('src/gallium/frontends/osmesa/osmesa.c'),
'   attribs.profile = (profile == OSMESA_CORE_PROFILE)\n      ? API_OPENGL_CORE : API_OPENGL_COMPAT;\n',
'   attribs.profile = (profile == OSMESA_ES_PROFILE) ? API_OPENGLES2 :\n      (profile == OSMESA_CORE_PROFILE) ? API_OPENGL_CORE : API_OPENGL_COMPAT;\n')

# Hardware-required creation can legitimately return NULL after GPU failure.
# A non-NULL manager is not proof that its pipe_screen exists (backboardd's
# observed st_api_create_context load at screen+0x2c8). Fail closed before ST,
# without substituting a software screen or publishing a half-created context.
change(Path('src/gallium/frontends/osmesa/osmesa.c'),
'''   osmesa->st = st_api_create_context(get_st_manager(),
                                         &attribs, &st_error, st_shared);''',
'''   /* Lancelot: a failed hardware-required screen is not a usable manager. */
   struct pipe_frontend_screen *fscreen = get_st_manager();
   if (!fscreen || !fscreen->screen) {
      FREE(osmesa);
      return NULL;
   }

   osmesa->st = st_api_create_context(fscreen,
                                     &attribs, &st_error, st_shared);''')
change(Path('src/gallium/targets/osmesa/osmesa_target.c'),'''#ifdef GALLIUM_PANFROST
#include "panfrost/drm/panfrost_drm_public.h"
#include <xf86drm.h>
#endif''','''#ifdef GALLIUM_PANFROST
#include "panfrost/drm/panfrost_drm_public.h"
#include "frontend/winsys_handle.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "drm-uapi/drm_fourcc.h"
#include <xf86drm.h>

/*
 * The memory of an IOSurface as a texture (Darwin on lancelot): the kernel's Mali service
 * makes a buffer of the surface, and Panfrost lays a linear resource over it. The image the
 * GL frontend hands us (glEGLImageTargetTexture2DOES) is the IOSurface's ID.
 */
struct pipe_resource *
osmesa_surface_resource(struct pipe_screen *screen, uint32_t surface)
{
   struct lancelot_surface_info info;
   struct pipe_resource templ = {
      .target = PIPE_TEXTURE_2D,
      .format = PIPE_FORMAT_B8G8R8A8_UNORM,
      .depth0 = 1,
      .array_size = 1,
      .bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET,
   };
   struct winsys_handle handle = {
      .type = WINSYS_HANDLE_TYPE_FD,
      .modifier = DRM_FORMAT_MOD_LINEAR,
   };
   static int query_fd = -1;

   if (query_fd < 0)
      query_fd = drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER);
   if (query_fd < 0 || lancelot_surface_query(query_fd, surface, &info))
      return NULL;
   if (info.pixel_format != 0x42475241 /* 'BGRA' */ || info.planes != 0 ||
       info.width == 0 || info.height == 0 ||
       (uint64_t)info.width * 4 > info.row_bytes || info.offset > info.size ||
       (uint64_t)info.row_bytes * info.height > info.size - info.offset)
      return NULL;
   templ.width0 = info.width;
   templ.height0 = info.height;
   handle.handle = surface;    /* the shim's drmPrimeFDToHandle takes the IOSurface ID */
   handle.stride = info.row_bytes;
   handle.offset = info.offset;
   handle.size = info.size;
   return screen->resource_from_handle(screen, &templ, &handle, 0);
}
#else
struct pipe_resource *
osmesa_surface_resource(struct pipe_screen *screen, uint32_t surface)
{
   (void)screen;
   (void)surface;
   return NULL;
}
#endif''')

change(Path('src/gallium/frontends/osmesa/osmesa.c'),
'static struct pipe_frontend_screen *global_fscreen = NULL;\n',
'''static struct pipe_frontend_screen *global_fscreen = NULL;

extern struct pipe_resource *
osmesa_surface_resource(struct pipe_screen *screen, uint32_t surface);

/*
 * There is no EGL here. What glEGLImageTargetTexture2DOES is given as an image is the ID of an
 * IOSurface, whose memory becomes the texture's where the driver can do that (Darwin on lancelot).
 */
static bool
osmesa_st_get_egl_image(struct pipe_frontend_screen *fscreen, void *image,
                        struct st_egl_image *out)
{
   out->texture = osmesa_surface_resource(fscreen->screen,
                                          (uint32_t)(uintptr_t)image);
   if (!out->texture)
      return false;
   out->format = out->texture->format;
   return true;
}

static bool
osmesa_st_validate_egl_image(struct pipe_frontend_screen *fscreen, void *image)
{
   return true;
}
''')

change(Path('src/gallium/frontends/osmesa/osmesa.c'),
'      global_fscreen->get_egl_image = NULL;\n',
'      global_fscreen->get_egl_image = osmesa_st_get_egl_image;\n      global_fscreen->validate_egl_image = osmesa_st_validate_egl_image;\n')

# Uniforms and sysvals did not reach the shader reliably through the push constants on this
# driver: a mat4 uniform moved nothing (attempt 39h draw 9), and a viewport set for one draw was
# used by the next (draw 1). The uniform buffer descriptors are ordinary memory the kernel keeps
# coherent before every submit, so read the uniforms from there instead.
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'#include "gallium/auxiliary/util/u_blend.h"\n',
'#include <stdio.h>\n#include <stdlib.h>\n\n#include "gallium/auxiliary/util/u_blend.h"\n')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''      if (usz > 0) {
         address = panfrost_map_constant_buffer_gpu(batch, stage, buf, ubo);
      }

      panfrost_emit_ubo(ubos.cpu, ubo, address, usz);''',
'''      if (usz > 0) {
         address = panfrost_map_constant_buffer_gpu(batch, stage, buf, ubo);
      }

#ifdef __APPLE__
      /* Lancelot: the bytes the shader will read as this UBO and the address it will read them
       * at, for following a uniform that does not move from the program's storage
       * (glGetUniformfv) to the constant buffer the GPU is given. Enabled with the boot argument
       * glenv=LANCELOT_UBO_DUMP=1 (eagl passes glenv to the environment), or setenv in a test. */
      if (usz >= 16 && getenv("LANCELOT_UBO_DUMP") != NULL) {
         static unsigned dumps;
         struct panfrost_resource *dumped = pan_resource(buf->cb[ubo].buffer);
         const uint8_t *base = dumped && dumped->bo ? (const uint8_t *)(uintptr_t)dumped->bo->ptr.cpu : NULL;
         const float *f = NULL;
         unsigned count = usz / 4 < 32 ? usz / 4 : 32;

         if (base != NULL)
            f = (const float *)(base + buf->cb[ubo].buffer_offset);
         else if (buf->cb[ubo].user_buffer != NULL)
            f = (const float *)((const uint8_t *)buf->cb[ubo].user_buffer + buf->cb[ubo].buffer_offset);
         if (f != NULL && dumps < 80) {
            fprintf(stderr, "lancelot-ubo: %s ubo %u size %zu address %llx:",
                    stage == PIPE_SHADER_VERTEX ? "vertex" : "fragment", ubo, usz,
                    (unsigned long long)address);
            for (unsigned k = 0; k < count; k++)
               fprintf(stderr, " %g", (double)f[k]);
            fprintf(stderr, "\\n");
            dumps++;
         }
      }
#endif

      panfrost_emit_ubo(ubos.cpu, ubo, address, usz);''')

# Public MTK UBO push behavior is applied by apply-hakios-mtk.py.
# The read side, made visible: what the CPU finds in a resource's memory when it is mapped for
# reading, before and after the flush that is meant to make the GPU's writes visible, and every
# batch submit with the framebuffer it draws into. Enabled with LANCELOT_READ_DUMP=1 (gl_test
# draw4 sets it), so the log says whether a pixel read back was missing, early, or correct.
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
'#include <fcntl.h>\n#include <xf86drm.h>\n',
'#include <fcntl.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <xf86drm.h>\n')
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
'''static void *
panfrost_ptr_map(struct pipe_context *pctx, struct pipe_resource *resource,''',
'''#ifdef __APPLE__
/* Lancelot: the four bytes at the centre of a read box, in the resource's own memory, as the
 * CPU sees them. LANCELOT_READ_DUMP=1 prints this before and after the read flush: a centre
 * that only appears after the flush ran before the GPU's write was visible, a centre that
 * never appears was never written, and two equal centres are a plain stale read. */
static const char *
lancelot_read_centre(struct panfrost_resource *rsrc, unsigned level, const struct pipe_box *box)
{
   static char text[48];
   struct panfrost_bo *bo = rsrc->bo;
   unsigned bpp = util_format_get_blocksize(rsrc->image.layout.format);
   const uint8_t *line;

   if (bo == NULL || bo->ptr.cpu == NULL || bpp == 0 || box->width == 0 || box->height == 0 ||
       drm_is_afbc(rsrc->image.layout.modifier) || drm_is_afrc(rsrc->image.layout.modifier) ||
       rsrc->image.layout.modifier != DRM_FORMAT_MOD_LINEAR)
      return "not-linear";

   line = (const uint8_t *)(uintptr_t)bo->ptr.cpu +
          rsrc->image.layout.slices[level].offset +
          (size_t)box->z * (size_t)panfrost_get_layer_stride(&rsrc->image.layout, level) +
          (size_t)(box->y + box->height / 2) * rsrc->image.layout.slices[level].row_stride +
          (size_t)(box->x + box->width / 2) * bpp;
   snprintf(text, sizeof(text), "%02x%02x%02x%02x", line[0], line[1], line[2], line[3]);
   return text;
}
#endif

static void *
panfrost_ptr_map(struct pipe_context *pctx, struct pipe_resource *resource,''')
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
'''      } else if (usage & PIPE_MAP_READ) {
         panfrost_flush_writer(ctx, rsrc, "Synchronized read");
         panfrost_bo_wait(bo, INT64_MAX, false);
      }''',
'''      } else if (usage & PIPE_MAP_READ) {
#ifdef __APPLE__
         const bool lancelot_dump = getenv("LANCELOT_READ_DUMP") != NULL;
         char lancelot_before[48];

         lancelot_before[0] = 0;
         if (lancelot_dump)
            snprintf(lancelot_before, sizeof(lancelot_before), "%s",
                     lancelot_read_centre(rsrc, level, box));
#endif
         panfrost_flush_writer(ctx, rsrc, "Synchronized read");
         panfrost_bo_wait(bo, INT64_MAX, false);
#ifdef __APPLE__
         if (lancelot_dump)
            fprintf(stderr, "lancelot-map: bo %llx mod %llx box %d,%d %dx%d before %s after %s\\n",
                    (unsigned long long)bo->ptr.gpu,
                    (unsigned long long)rsrc->image.layout.modifier,
                    box->x, box->y, box->width, box->height, lancelot_before,
                    lancelot_read_centre(rsrc, level, box));
#endif
      }''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'#include "pan_job.h"\n',
'#include "pan_job.h"\n\n#ifdef __APPLE__\n#include <stdio.h>\n#include <stdlib.h>\n#endif\n')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''      ret = drmIoctl(panfrost_device_fd(dev), DRM_IOCTL_PANFROST_SUBMIT, &submit);
   free(bo_handles);
''',
'''      ret = drmIoctl(panfrost_device_fd(dev), DRM_IOCTL_PANFROST_SUBMIT, &submit);
   free(bo_handles);

#ifdef __APPLE__
   if (getenv("LANCELOT_READ_DUMP") != NULL)
      fprintf(stderr, "lancelot-submit: jc %llx reqs %#x out %u ret %d\\n",
              (unsigned long long)submit.jc, reqs, out_sync, ret);
#endif
''')
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
'#include "pan_util.h"\n',
'#include "pan_util.h"\n\n#ifdef __APPLE__\n#include <stdio.h>\n#include <stdlib.h>\n#endif\n')
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
'''   panfrost_batch_to_fb_info(batch, &fb, rts, &zs, &s, false);
   panfrost_emit_tile_map(batch, &fb);
''',
'''   panfrost_batch_to_fb_info(batch, &fb, rts, &zs, &s, false);
   panfrost_emit_tile_map(batch, &fb);

#ifdef __APPLE__
   if (getenv("LANCELOT_READ_DUMP") != NULL) {
      struct pipe_resource *cbuf0 = batch->key.nr_cbufs ? batch->key.cbufs[0]->texture : NULL;

      fprintf(stderr, "lancelot-batch: submit seq %u cbuf0 %p %ux%u draws %#x\\n",
              batch->seqnum, (void *)cbuf0, cbuf0 ? cbuf0->width0 : 0,
              cbuf0 ? cbuf0->height0 : 0, batch->draws);
   }
#endif
''')
# A tiled resource cannot be peeked through its BO (its pixels are in 16x16 U-interleaved order),
# so the centre texel of the de-tiled box is printed instead. Same switch as the linear peek.
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
'''      if (usage & PIPE_MAP_READ)
         panfrost_load_tiled_images(transfer, rsrc);

      return transfer->map;''',
'''      if (usage & PIPE_MAP_READ)
         panfrost_load_tiled_images(transfer, rsrc);

#ifdef __APPLE__
      if ((usage & PIPE_MAP_READ) && getenv("LANCELOT_READ_DUMP") != NULL &&
          box_blocks.width > 0 && box_blocks.height > 0 &&
          util_format_get_blocksize(format) >= 4) {
         unsigned lancelot_bpp = util_format_get_blocksize(format);
         const uint8_t *lancelot_centre = (const uint8_t *)transfer->map +
            (size_t)(box_blocks.height / 2) * transfer->base.stride +
            (size_t)(box_blocks.width / 2) * lancelot_bpp;

         fprintf(stderr, "lancelot-map: tiled bo %llx centre %02x%02x%02x%02x box %d,%d %dx%d\\n",
                 (unsigned long long)bo->ptr.gpu,
                 lancelot_centre[0], lancelot_centre[1], lancelot_centre[2], lancelot_centre[3],
                 box->x, box->y, box->width, box->height);
      }
#endif

      return transfer->map;''')
# Review 2026-10-29: explicit layouts keep the importer's pitch; validate their
# extent against the actual imported BO as well. winsys_handle.size is otherwise
# ignored by Panfrost (upstream documents it as an export output).
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
'''   rsc->image.data.base = rsc->bo->ptr.gpu;
   rsc->modifier_constant = true;''',
'''#ifdef __APPLE__
   if (rsc->image.layout.data_size > panfrost_bo_size(rsc->bo) ||
       (whandle->size && (rsc->image.layout.data_size > whandle->size ||
                         whandle->size > panfrost_bo_size(rsc->bo)))) {
      panfrost_bo_unreference(rsc->bo);
      FREE(rsc);
      return NULL;
   }
   if (debug_get_bool_option("LANCELOT_REVIEW_DUMP", false)) {
      static unsigned dumps;
      if (dumps++ < 32)
         fprintf(stderr, "lancelot-import: surface %u bo %llx %ux%u mod %llx stride %u/%u offset %u/%llu size %llu/%llu/%llu\\n",
                 whandle->handle, (unsigned long long)rsc->bo->ptr.gpu,
                 prsc->width0, prsc->height0, (unsigned long long)mod,
                 whandle->stride, rsc->image.layout.slices[0].row_stride,
                 whandle->offset, (unsigned long long)rsc->image.layout.slices[0].offset,
                 (unsigned long long)whandle->size,
                 (unsigned long long)rsc->image.layout.data_size,
                 (unsigned long long)panfrost_bo_size(rsc->bo));
   }
#endif
   rsc->image.data.base = rsc->bo->ptr.gpu;
   rsc->modifier_constant = true;''')

# These probes are off by default, bounded, and enabled by one boolean option.
# They record state, not a speculative change to tiling, shader lowering or jobs.
change(Path('src/gallium/drivers/panfrost/pan_blit.c'),
'#include "pan_util.h"\n',
'#include "pan_util.h"\n#ifdef __APPLE__\n#include <stdio.h>\n#endif\n')
change(Path('src/gallium/drivers/panfrost/pan_blit.c'),
'''   panfrost_blit_no_afbc_legalization(pipe, info);
}''',
'''#ifdef __APPLE__
   if (debug_get_bool_option("LANCELOT_REVIEW_DUMP", false)) {
      static unsigned dumps;
      if (dumps++ < 32)
         fprintf(stderr, "lancelot-blit: src %llx mod %llx stride %u box %d,%d %dx%d dst %llx mod %llx stride %u box %d,%d %dx%d scissor %u\\n",
                 (unsigned long long)src->image.data.base,
                 (unsigned long long)src->image.layout.modifier,
                 src->image.layout.slices[info->src.level].row_stride,
                 info->src.box.x, info->src.box.y, info->src.box.width, info->src.box.height,
                 (unsigned long long)dst->image.data.base,
                 (unsigned long long)dst->image.layout.modifier,
                 dst->image.layout.slices[info->dst.level].row_stride,
                 info->dst.box.x, info->dst.box.y, info->dst.box.width, info->dst.box.height,
                 info->scissor_enable);
   }
#endif
   panfrost_blit_no_afbc_legalization(pipe, info);
}''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''      panfrost_emit_ubo(ubos.cpu, ubo, address, usz);
   }''',
'''      panfrost_emit_ubo(ubos.cpu, ubo, address, usz);
#if defined(__APPLE__) && PAN_ARCH <= 7
      if (debug_get_bool_option("LANCELOT_REVIEW_DUMP", false)) {
         static unsigned dumps;
         if (dumps++ < 128) {
            const uint32_t *raw = (const uint32_t *)((const uint8_t *)ubos.cpu + ubo * desc_size);
            pan_unpack((const struct mali_uniform_buffer_packed *)raw, UNIFORM_BUFFER, record);
            fprintf(stderr, "lancelot-ubo-desc: seq %u stage %u slot %u table %llx raw %08x %08x pointer %llx entries %u bytes %zu shader-count %u mask %x sys-slot %u\\n",
                    batch->seqnum, stage, ubo, (unsigned long long)ubos.gpu,
                    raw[0], raw[1], (unsigned long long)record.pointer,
                    record.entries, usz, shader->info.ubo_count, ss->info.ubo_mask, sysval_ubo);
         }
      }
#endif
   }''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''   if (sys_size)
      panfrost_emit_ubo(ubos.cpu, ubo_count, transfer.gpu, sys_size);''',
'''   if (sys_size)
      panfrost_emit_ubo(ubos.cpu, ubo_count, transfer.gpu, sys_size);
#ifdef __APPLE__
   if (sys_size && debug_get_bool_option("LANCELOT_REVIEW_DUMP", false)) {
      static unsigned dumps;
      if (dumps++ < 64) {
         const float *v = (const float *)transfer.cpu;
         fprintf(stderr, "lancelot-sysval: seq %u stage %u slot %u pointer %llx bytes %zu first %g %g %g\\n",
                 batch->seqnum, stage, ubo_count, (unsigned long long)transfer.gpu,
                 sys_size, (double)v[0], (double)v[1], (double)v[2]);
      }
   }
#endif''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''   if (panfrost_batch_skip_rasterization(batch))
      return;''',
'''   if (panfrost_batch_skip_rasterization(batch)) {
#ifdef __APPLE__
      if (debug_get_bool_option("LANCELOT_REVIEW_DUMP", false)) {
         static unsigned dumps;
         if (dumps++ < 32)
            fprintf(stderr, "lancelot-draw-skip: seq %u count %u scissor-empty %u\\n",
                    batch->seqnum, draw->count, batch->scissor_culls_everything);
      }
#endif
      return;
   }''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''   JOBX(launch_draw)(batch, info, drawid_offset, draw, vertex_count);
   batch->draw_count++;''',
'''#ifdef __APPLE__
   if (debug_get_bool_option("LANCELOT_REVIEW_DUMP", false)) {
      static unsigned dumps;
      if (dumps++ < 64)
         fprintf(stderr, "lancelot-draw: seq %u draw %u mode %u count %u start %u fb %ux%u viewport %g,%g + %g,%g vs %llx ubos %llx pos %llx tls %llx\\n",
                 batch->seqnum, batch->draw_count, info->mode, draw->count, draw->start,
                 batch->key.width, batch->key.height,
                 (double)ctx->pipe_viewport.scale[0], (double)ctx->pipe_viewport.scale[1],
                 (double)ctx->pipe_viewport.translate[0], (double)ctx->pipe_viewport.translate[1],
                 (unsigned long long)batch->rsd[PIPE_SHADER_VERTEX],
                 (unsigned long long)batch->uniform_buffers[PIPE_SHADER_VERTEX],
                 (unsigned long long)batch->varyings.pos, (unsigned long long)batch->tls.gpu);
   }
#endif
   JOBX(launch_draw)(batch, info, drawid_offset, draw, vertex_count);
   batch->draw_count++;''')
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
'''   panfrost_emit_tile_map(batch, &fb);
''',
'''   panfrost_emit_tile_map(batch, &fb);
#ifdef __APPLE__
   if (debug_get_bool_option("LANCELOT_REVIEW_DUMP", false)) {
      static unsigned dumps;
      if (dumps++ < 64 && batch->key.nr_cbufs && batch->key.cbufs[0]) {
         struct pipe_surface *surf = batch->key.cbufs[0];
         struct panfrost_resource *r = pan_resource(surf->texture);
         const struct pan_image_slice_layout *s = &r->image.layout.slices[surf->u.tex.level];
         fprintf(stderr, "lancelot-batch-layout: seq %u draws %u fb %ux%u bo %llx mod %llx offset %llu stride %u extent %u,%u-%u,%u\\n",
                 batch->seqnum, batch->draw_count, batch->key.width, batch->key.height,
                 (unsigned long long)r->image.data.base, (unsigned long long)r->image.layout.modifier,
                 (unsigned long long)s->offset, s->row_stride,
                 batch->minx, batch->miny, batch->maxx, batch->maxy);
      }
   }
#endif
''')
# LMAL submit is synchronous. Inspect the actual position allocation after VTC,
# before fragment submission, only for a single-draw batch with at least 4 vertices.
# These are post-viewport positions, not necessarily raw GLSL gl_Position.
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''static int
jm_submit_jc(''',
'''#if defined(__APPLE__) && PAN_ARCH <= 7
static void
lancelot_dump_positions(struct panfrost_batch *batch)
{
   static unsigned dumps;
   uint64_t address = batch->varyings.pos;
   if (!debug_get_bool_option("LANCELOT_REVIEW_DUMP", false) || dumps >= 32 ||
       batch->draw_count != 1 || batch->vertex_count < 4 || !address)
      return;
   util_dynarray_foreach(&batch->invisible_pool.bos, struct panfrost_bo *, entry) {
      struct panfrost_bo *bo = *entry;
      if (address < bo->ptr.gpu || address - bo->ptr.gpu > panfrost_bo_size(bo) ||
          64 > panfrost_bo_size(bo) - (address - bo->ptr.gpu))
         continue;
      if (panfrost_bo_mmap(bo))
         return;
      const float *p = (const float *)((const uint8_t *)bo->ptr.cpu + address - bo->ptr.gpu);
      for (unsigned i = 0; i < 4; i++)
         fprintf(stderr, "lancelot-position: seq %u vertex %u address %llx value %g %g %g %g\\n",
                 batch->seqnum, i, (unsigned long long)(address + 16 * i),
                 (double)p[4*i], (double)p[4*i+1], (double)p[4*i+2], (double)p[4*i+3]);
      dumps++;
      return;
   }
}
#endif

static int
jm_submit_jc(''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''   if (has_frag) {
      ret = jm_submit_jc''',
'''#if defined(__APPLE__) && PAN_ARCH <= 7
   if (has_draws && !batch->ctx->is_noop)
      lancelot_dump_positions(batch);
#endif
   if (has_frag) {
      ret = jm_submit_jc''')
# Persistent framebuffer-preload shaders/RSDs live in screen pools, not in the
# batch pools. Register their BO references after emitting preload descriptors,
# before either VTC or fragment submission snapshots the batch's BO list.
# Otherwise a strictly isolated submit can fault even though ordinary draws
# and clear-only batches work (attempt43 draw4 fragment at 0x318c000).
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''void
GENX(jm_preload_fb)''',
'''#if defined(__APPLE__) && PAN_ARCH <= 7
static void
lancelot_retain_preload_pool(struct panfrost_batch *batch,
                             struct panfrost_pool *pool, const char *kind)
{
   static unsigned dumps;
   util_dynarray_foreach(&pool->bos, struct panfrost_bo *, entry) {
      struct panfrost_bo *bo = *entry;
      panfrost_batch_add_bo(batch, bo, PIPE_SHADER_FRAGMENT);
      if (getenv("LANCELOT_READ_DUMP") != NULL && dumps < 48) {
         dumps++;
         fprintf(stderr, "lancelot-preload-bo: seq %u kind %s handle %u va %llx size %llu\\n",
                 batch->seqnum, kind, panfrost_bo_handle(bo),
                 (unsigned long long)bo->ptr.gpu,
                 (unsigned long long)panfrost_bo_size(bo));
      }
   }
}
#endif

void
GENX(jm_preload_fb)''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''   assert(PAN_ARCH < 6 || !preload_job_count);

   for (unsigned j''',
'''#if defined(__APPLE__) && PAN_ARCH <= 7
   /* Lancelot: pool lock order is RSD -> shader, matching preload_get_rsd.
    * Retain the complete current pools while their owners exclude allocation;
    * batch_add_bo deduplicates handles and retains BOs through completion. */
   pthread_mutex_lock(&dev->fb_preload_cache.rsds.lock);
   pthread_mutex_lock(&dev->fb_preload_cache.shaders.lock);
   lancelot_retain_preload_pool(batch,
      to_panfrost_pool(dev->fb_preload_cache.rsds.pool), "rsd");
   lancelot_retain_preload_pool(batch,
      to_panfrost_pool(dev->fb_preload_cache.shaders.pool), "shader");
   pthread_mutex_unlock(&dev->fb_preload_cache.shaders.lock);
   pthread_mutex_unlock(&dev->fb_preload_cache.rsds.lock);
#endif

   assert(PAN_ARCH < 6 || !preload_job_count);

   for (unsigned j''')
# Release builds must propagate import failures, not compile away their checks.
change(Path('src/gallium/drivers/panfrost/pan_bo.c'),
'''   ret = drmPrimeFDToHandle(dev->kmod.dev->fd, fd, &gem_handle);
   assert(!ret);

   bo = pan_lookup_bo(dev, gem_handle);

   if (!bo->dev) {
      bo->dev = dev;
      bo->kmod_bo = pan_kmod_bo_import(dev->kmod.dev, fd, 0);
''',
'''   ret = drmPrimeFDToHandle(dev->kmod.dev->fd, fd, &gem_handle);
   if (ret) {
      pthread_mutex_unlock(&dev->bo_map_lock);
      return NULL;
   }

   bo = pan_lookup_bo(dev, gem_handle);
   if (!bo) {
      drmCloseBufferHandle(dev->kmod.dev->fd, gem_handle);
      pthread_mutex_unlock(&dev->bo_map_lock);
      return NULL;
   }

   if (!bo->dev) {
      struct pan_kmod_bo *kmod_bo = pan_kmod_bo_import(dev->kmod.dev, fd, 0);
      if (!kmod_bo) {
         pthread_mutex_unlock(&dev->bo_map_lock);
         return NULL;
      }
''')
change(Path('src/gallium/drivers/panfrost/pan_bo.c'),
'''               .size = bo->kmod_bo->size,
            },
         .map =
            {
               .bo = bo->kmod_bo,''',
'''               .size = kmod_bo->size,
            },
         .map =
            {
               .bo = kmod_bo,''')
change(Path('src/gallium/drivers/panfrost/pan_bo.c'),
'''      ASSERTED int ret = pan_kmod_vm_bind(
         dev->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &vm_op, 1);
      assert(!ret);

      bo->ptr.gpu = vm_op.va.start;''',
'''      int ret = pan_kmod_vm_bind(
         dev->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &vm_op, 1);
      if (ret) {
         pan_kmod_bo_put(kmod_bo);
         pthread_mutex_unlock(&dev->bo_map_lock);
         return NULL;
      }

      bo->dev = dev;
      bo->kmod_bo = kmod_bo;
      bo->ptr.gpu = vm_op.va.start;''')

# Make the actual SubData destination and final packed job visible for M1/M2.
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
'''void
panfrost_resource_context_init(struct pipe_context *pctx)''',
'''#ifdef __APPLE__
static void
lancelot_buffer_subdata(struct pipe_context *pctx, struct pipe_resource *resource,
                       unsigned usage, unsigned offset, unsigned size, const void *data)
{
   struct panfrost_resource *r = pan_resource(resource);
   uint64_t before = r->bo->ptr.gpu;
   u_default_buffer_subdata(pctx, resource, usage, offset, size, data);
   if (debug_get_bool_option("LANCELOT_REVIEW_DUMP", false) && size && data) {
      static unsigned dumps;
      size_t n = MIN2(size, 32);
      if (dumps++ < 64 && r->bo->ptr.cpu && offset <= panfrost_bo_size(r->bo) &&
          n <= panfrost_bo_size(r->bo) - offset) {
         uint32_t wanted[8] = {0}, actual[8] = {0};
         memcpy(wanted, data, n);
         memcpy(actual, (const uint8_t *)r->bo->ptr.cpu + offset, n);
         fprintf(stderr, "lancelot-vbo-update: resource %p bo %llx/%llx offset %u size %u words",
                 (void *)resource, (unsigned long long)before,
                 (unsigned long long)r->bo->ptr.gpu, offset, size);
         for (unsigned i = 0; i < n / 4; i++)
            fprintf(stderr, " %08x/%08x", wanted[i], actual[i]);
         fprintf(stderr, "\\n");
      }
   }
}
#endif

void
panfrost_resource_context_init(struct pipe_context *pctx)''')
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
'''   pctx->buffer_subdata = u_default_buffer_subdata;''',
'''#ifdef __APPLE__
   pctx->buffer_subdata = lancelot_buffer_subdata;
#else
   pctx->buffer_subdata = u_default_buffer_subdata;
#endif''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''      jm_emit_draw_descs(batch, &cfg, PIPE_SHADER_VERTEX);
   }
}''',
'''      jm_emit_draw_descs(batch, &cfg, PIPE_SHADER_VERTEX);
   }
#ifdef __APPLE__
   if (debug_get_bool_option("LANCELOT_REVIEW_DUMP", false)) {
      static unsigned dumps;
      if (dumps++ < 64) {
         pan_unpack(section, DRAW, actual);
         fprintf(stderr, "lancelot-vs-job: seq %u state %llx ubos %llx attribs %llx buffers %llx varyings %llx tls %llx\\n",
                 batch->seqnum, (unsigned long long)actual.state,
                 (unsigned long long)actual.uniform_buffers,
                 (unsigned long long)actual.attributes,
                 (unsigned long long)actual.attribute_buffers,
                 (unsigned long long)actual.varying_buffers,
                 (unsigned long long)actual.thread_storage);
      }
   }
#endif
}''')
# Observe every actual batch submission, including resource dependencies which
# upstream perf_debug does not label. No flush, dependency or wait is removed.
change(Path('src/gallium/drivers/panfrost/pan_context.h'),
       'struct panfrost_context {\n',
       '''#ifdef __APPLE__
#include "../../../lancelot-drm/lancelot_batch_profile.h"
#endif
struct panfrost_context {
''')
change(Path('src/gallium/drivers/panfrost/pan_context.h'),
       '   struct pipe_context base;\n',
       '''   struct pipe_context base;
#ifdef __APPLE__
   struct lm_batch_profile lancelot_batches;
#endif
''')
change(Path('src/gallium/drivers/panfrost/pan_context.h'),
       '''pan_context(struct pipe_context *pcontext)
{''',
       '''pan_context(struct pipe_context *pcontext)
{
   _Static_assert(offsetof(struct panfrost_context, base) == 0,
                  "pipe_context base must stay at offset zero");''')
p=src/'src/gallium/drivers/panfrost/pan_job.c';data=p.read_text()
old='''static void panfrost_batch_submit(struct panfrost_context *ctx,
                                  struct panfrost_batch *batch);'''
new='''static void panfrost_batch_submit(struct panfrost_context *ctx,
                                  struct panfrost_batch *batch, unsigned reason);'''
assert data.count(old)==1;data=data.replace(old,new,1)
replacements=[
('''perf_debug(ctx, "Flushing batch due to seqnum overflow");
      panfrost_batch_submit(ctx, batch);''','''perf_debug(ctx, "Flushing batch due to seqnum overflow");
      panfrost_batch_submit(ctx, batch, LM_BATCH_EVICT);'''),
('''perf_debug(ctx, "Flushing the current FBO due to: %s", reason);
      panfrost_batch_submit(ctx, batch);''','''perf_debug(ctx, "Flushing the current FBO due to: %s", reason);
      panfrost_batch_submit(ctx, batch, LM_BATCH_FRESH);'''),
('''if (writer != NULL && writer != batch)
      panfrost_batch_submit(ctx, writer);''','''if (writer != NULL && writer != batch)
      panfrost_batch_submit(ctx, writer, LM_BATCH_READ_WRITER);'''),
('''if (panfrost_batch_uses_resource(batch, rsrc))
            panfrost_batch_submit(ctx, batch);''','''if (panfrost_batch_uses_resource(batch, rsrc))
            panfrost_batch_submit(ctx, batch, LM_BATCH_WRITE_USER);'''),
('''panfrost_batch_submit(ctx, batch);

   for (unsigned i = 0; i < PAN_MAX_BATCHES; i++)''','''panfrost_batch_submit(ctx, batch, LM_BATCH_FLUSH_CURRENT);

   for (unsigned i = 0; i < PAN_MAX_BATCHES; i++)'''),
('''panfrost_batch_submit(ctx, &ctx->batches.slots[i]);''','''panfrost_batch_submit(ctx, &ctx->batches.slots[i], LM_BATCH_FLUSH_OTHER);'''),
('''perf_debug(ctx, "Flushing writer due to: %s", reason);
      panfrost_batch_submit(ctx, entry->data);''','''perf_debug(ctx, "Flushing writer due to: %s", reason);
      panfrost_batch_submit(ctx, entry->data, LM_BATCH_SYNC_WRITER);'''),
('''perf_debug(ctx, "Flushing user due to: %s", reason);
      panfrost_batch_submit(ctx, batch);''','''perf_debug(ctx, "Flushing user due to: %s", reason);
      panfrost_batch_submit(ctx, batch, LM_BATCH_SYNC_USERS);'''),
]
for old,new in replacements:
    if data.count(old)!=1:raise SystemExit('Batch reason hunk mismatch: '+old)
    data=data.replace(old,new,1)
data='''#include "../../../lancelot-drm/lancelot_batch_profile.h"
#ifdef __APPLE__
#include "util/os_time.h"
#endif
'''+data
old='''panfrost_batch_submit(struct panfrost_context *ctx,
                      struct panfrost_batch *batch)
{'''
new='''panfrost_batch_submit(struct panfrost_context *ctx,
                      struct panfrost_batch *batch, unsigned reason)
{'''
assert data.count(old)==1;data=data.replace(old,new,1)
old='''   int ret;

   /* Nothing to do! */'''
new='''   int ret;
#ifdef __APPLE__
   bool measured = (pan_device(pscreen)->debug & PAN_DBG_PERF) &&
                   (has_frag || batch->compute_count || batch->has_time_query);
   uint64_t begin_ns = measured ? os_time_get_nano() : 0;
   unsigned original_draws = batch->draw_count;
   unsigned original_bos = batch->num_bos + panfrost_pool_num_bos(&batch->pool) +
                           panfrost_pool_num_bos(&batch->invisible_pool);
#else
   (void)reason;
#endif

   /* Nothing to do! */'''
assert data.count(old)==1;data=data.replace(old,new,1)
old='''out:
   panfrost_batch_cleanup(ctx, batch);
}'''
new='''out:
#ifdef __APPLE__
   if (measured) {
      lm_batch_record(&ctx->lancelot_batches, reason, begin_ns,
                      os_time_get_nano(), original_draws, original_bos);
      if (lm_batch_report_due(&ctx->lancelot_batches)) {
         for (unsigned k = 0; k < LM_BATCH_REASONS; k++) {
            const struct lm_batch_cost *c = &ctx->lancelot_batches.reason[k];
            if (!c->count) continue;
            fprintf(stderr, "lancelot-batch-sum: id=%llx from=%llu to=%llu kind=%u count=%llu us=%llu draws=%llu bos=%llu max=%llu\\n",
                    (unsigned long long)(uintptr_t)ctx,
                    (unsigned long long)(ctx->lancelot_batches.from_ns / 1000),
                    (unsigned long long)(ctx->lancelot_batches.to_ns / 1000), k,
                    (unsigned long long)c->count, (unsigned long long)(c->ns / 1000),
                    (unsigned long long)c->draws, (unsigned long long)c->bos,
                    (unsigned long long)(c->max_ns / 1000));
         }
         memset(&ctx->lancelot_batches, 0, sizeof(ctx->lancelot_batches));
      }
   }
#endif
   panfrost_batch_cleanup(ctx, batch);
}'''
assert data.count(old)==1;data=data.replace(old,new,1)
assert data.count('panfrost_batch_submit(')==10
p.write_text(data)

# Cached varying ATTRIBUTE tables live in the context's unowned descriptor
# pool, not in either batch pool. The CSO reference keeps their VA alive but
# does not declare it to a strictly isolated JM submission. Retain precisely
# the linkage BO for both users, on first emission AND cached reuse. Do not
# make a whole descriptor pool visible or resurrect cache entries by address.
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''static void
panfrost_emit_varying_descs(''',
'''static bool
panfrost_emit_varying_descs(''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''   /* Take a reference if we're being put on the CSO */
   if (!pool->owned) {''',
'''   if (!T.cpu)
      return false;

   /* Take a reference if we're being put on the CSO */
   if (!pool->owned) {''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''         point_coord_mask, offset, PAN_VARY_FRAGCOORD);
   }
}''',
'''         point_coord_mask, offset, PAN_VARY_FRAGCOORD);
   }
   return true;
}''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''static void
panfrost_emit_varying_descriptor(''',
'''static bool
panfrost_emit_varying_descriptor(''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''   struct pan_linkage _linkage;
   struct pan_linkage *linkage = prelink ? &vs->linkage : &_linkage;''',
'''   struct pan_linkage _linkage = {0};
   struct pan_linkage *linkage = prelink ? &vs->linkage : &_linkage;''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''      panfrost_emit_varying_descs(pool, vs, fs, point_coord_mask, linkage);
   }

   unsigned present = linkage->present, stride = linkage->stride;''',
'''      if (!panfrost_emit_varying_descs(pool, vs, fs, point_coord_mask, linkage)) {
         mesa_loge("panfrost_emit_varying_descs failed");
         return false;
      }
   }

#ifdef __APPLE__
   if (prelink) {
      /* CSO ownership is not batch ownership. Both jobs dereference the
       * cached ATTRIBUTE table; batch cleanup releases these exact retains. */
      panfrost_batch_add_bo(batch, linkage->bo, PIPE_SHADER_VERTEX);
      panfrost_batch_add_bo(batch, linkage->bo, PIPE_SHADER_FRAGMENT);
      if (pan_device(ctx->base.screen)->debug & PAN_DBG_PERF) {
         static unsigned dumps;
         if (dumps++ < 48)
            fprintf(stderr, "lancelot-linkage: seq=%u handle=%u va=%llx size=%llu producer=%llx consumer=%llx refs=%d\\n",
                    batch->seqnum, (unsigned)panfrost_bo_handle(linkage->bo),
                    (unsigned long long)linkage->bo->ptr.gpu,
                    (unsigned long long)panfrost_bo_size(linkage->bo),
                    (unsigned long long)linkage->producer,
                    (unsigned long long)linkage->consumer,
                    p_atomic_read(&linkage->bo->refcnt));
      }
   }
#endif

   unsigned present = linkage->present, stride = linkage->stride;''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''      mesa_loge("panfrost_emit_varying_descriptor failed");
      return;''',
'''      mesa_loge("panfrost_emit_varying_descriptor failed");
      return false;''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''   batch->varyings.fs = linkage->consumer;
}''',
'''   batch->varyings.fs = linkage->consumer;
   return true;
}''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''   panfrost_emit_varying_descriptor(batch,
                                    ctx->padded_count * ctx->instance_count,
                                    info->mode == MESA_PRIM_POINTS);''',
'''   if (!panfrost_emit_varying_descriptor(batch,
                                        ctx->padded_count * ctx->instance_count,
                                        info->mode == MESA_PRIM_POINTS))
      return;''')

# Darwin LMAL returns BOs rounded to a 16 KiB CPU page. Looking up the 4 KiB
# request bucket but returning the object to a 16 KiB bucket defeats reuse.
# Match the native backing granule BEFORE the cache lookup, and consume the
# 16 KiB descriptor slab rather than allocating a new 16 KiB BO every 4 KiB.
# All waits, initial clean, CPU/GPU handovers and MMU isolation remain intact.
change(Path('src/gallium/drivers/panfrost/pan_bo.c'),
'''   size = ALIGN_POT(size, 4096);''',
'''#ifdef __APPLE__
   size = ALIGN_POT(size, 16384);
#else
   size = ALIGN_POT(size, 4096);
#endif''')
change(Path('src/gallium/drivers/panfrost/pan_mempool.c'),
'''   pan_pool_init(&pool->base, slab_size);''',
'''#ifdef __APPLE__
   slab_size = ALIGN_POT(slab_size, 16384);
#endif
   pan_pool_init(&pool->base, slab_size);''')

# Existing contexts can reach fresh-batch allocation after the original GPU
# failure. The NULL-screen creation guard does not cover this path. Refuse
# the failed batch without dereferencing it; no offline-device recovery.
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
'''   batch = panfrost_get_batch(ctx, &ctx->pipe_framebuffer);
   panfrost_dirty_state_all(ctx);''',
'''   batch = panfrost_get_batch(ctx, &ctx->pipe_framebuffer);
   if (!batch)
      return NULL;
   panfrost_dirty_state_all(ctx);''')

# The hardware RSD texture_count is BITSET_LAST_BIT(final NIR textures_used),
# not the context's largest bound slot. Views above it cannot be fetched by this
# shader. Do not create resource readers/flush writers/maintain those unrelated
# BOs. Keep null descriptors below the required count and all real dependencies.
change(Path('src/gallium/drivers/panfrost/pan_context.h'),
'''#include "../../../lancelot-drm/lancelot_batch_profile.h"
''',
'''#include "../../../lancelot-drm/lancelot_batch_profile.h"
#include "../../../lancelot-drm/lancelot_dependency_profile.h"
''')
change(Path('src/gallium/drivers/panfrost/pan_context.h'),
'''   struct lm_batch_profile lancelot_batches;
''',
'''   struct lm_batch_profile lancelot_batches;
   struct lm_dependency_profile lancelot_dependencies;
   struct lm_texture_profile lancelot_textures;
''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''static uint64_t
panfrost_emit_texture_descriptors(''',
'''#ifdef __APPLE__
#include "util/os_time.h"
static void
lancelot_texture_extent(struct panfrost_context *ctx, enum pipe_shader_type stage,
                        unsigned bound, unsigned needed)
{
   if (!(pan_device(ctx->base.screen)->debug & PAN_DBG_PERF))
      return;
   struct lm_texture_profile *p = &ctx->lancelot_textures;
   uint64_t now = os_time_get_nano();
   if (!p->started) { p->started = true; p->first_ns = now; }
   p->last_ns = now;
   p->calls++;
   if (!needed) p->zero_shader_calls++;
   if (bound > needed) {
      p->trimmed_calls++;
      p->omitted_slots += bound - needed;
      for (unsigned i = needed; i < bound; i++) {
         struct panfrost_sampler_view *v = ctx->sampler_views[stage][i];
         if (!v) continue;
         struct panfrost_resource *r = pan_resource(v->base.texture);
         p->omitted_views++;
         if (r && r->bo) p->omitted_bytes += panfrost_bo_size(r->bo);
      }
   }
   if (now - p->first_ns >= UINT64_C(1000000000)) {
      fprintf(stderr, "lancelot-texture-workset: id=%llx from=%llu to=%llu calls=%llu trims=%llu zero=%llu slots=%llu views=%llu bytes=%llu\\n",
              (unsigned long long)(uintptr_t)ctx, (unsigned long long)(p->first_ns / 1000),
              (unsigned long long)(p->last_ns / 1000), (unsigned long long)p->calls,
              (unsigned long long)p->trimmed_calls, (unsigned long long)p->zero_shader_calls,
              (unsigned long long)p->omitted_slots, (unsigned long long)p->omitted_views,
              (unsigned long long)p->omitted_bytes);
      memset(p, 0, sizeof(*p));
   }
}
#endif

static uint64_t
panfrost_emit_texture_descriptors(''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
'''   unsigned actual_count = ctx->sampler_view_count[stage];
   unsigned needed_count = ctx->prog[stage]->info.texture_count;
   unsigned alloc_count = MAX2(actual_count, needed_count);''',
'''   unsigned actual_count = ctx->sampler_view_count[stage];
   unsigned needed_count = ctx->prog[stage]->info.texture_count;
#ifdef __APPLE__
   lancelot_texture_extent(ctx, stage, actual_count, needed_count);
   actual_count = MIN2(actual_count, needed_count);
   unsigned alloc_count = needed_count;
#else
   unsigned alloc_count = MAX2(actual_count, needed_count);
#endif''')

# Observe the exact dependency resource before cleanup, using only scalars.
# Submission order and the underlying synchronous submit are unchanged. Wall
# cost here includes batch cleanup (unlike the existing batch-sum); never mix
# the two definitions or claim a pointer/VA is a lifetime generation identifier.
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
'''static void
panfrost_batch_update_access(''',
'''#ifdef __APPLE__
static uint64_t
lancelot_dependency_fb(struct panfrost_batch *batch)
{
   if (!batch->key.nr_cbufs || !batch->key.cbufs[0]) return 0;
   struct panfrost_resource *r = pan_resource(batch->key.cbufs[0]->texture);
   return r ? r->image.data.base : 0;
}
#endif

static void
lancelot_submit_dependency(struct panfrost_context *ctx,
                           struct panfrost_batch *producer,
                           struct panfrost_batch *consumer,
                           struct panfrost_resource *rsrc, unsigned reason)
{
#ifdef __APPLE__
   bool measured = (pan_device(ctx->base.screen)->debug & PAN_DBG_PERF) != 0;
   struct lm_dep_identity key = {0};
   uint32_t pd = 0, cd = 0;
   uint64_t begin = 0;
   if (measured) {
      key = (struct lm_dep_identity){
         .resource = (uintptr_t)rsrc, .va = rsrc->bo->ptr.gpu,
         .bytes = panfrost_bo_size(rsrc->bo), .handle = panfrost_bo_handle(rsrc->bo),
         .kind = reason, .bind = rsrc->base.bind, .width = rsrc->base.width0,
         .height = rsrc->base.height0, .target = rsrc->base.target,
         .producer_fb = lancelot_dependency_fb(producer),
         .consumer_fb = lancelot_dependency_fb(consumer),
      };
      pd = producer->draw_count;
      cd = consumer->draw_count;
      begin = os_time_get_nano();
   }
#else
   (void)consumer; (void)rsrc;
#endif
   panfrost_batch_submit(ctx, producer, reason);
#ifdef __APPLE__
   if (!measured) return;
   struct lm_dependency_profile *p = &ctx->lancelot_dependencies;
   lm_dep_record(p, &key, begin, os_time_get_nano(), pd, cd);
   if (!lm_dep_due(p)) return;
   bool emitted[LM_DEP_SLOTS] = {false};
   uint64_t other_count = p->overflow_count, other_ns = p->overflow_ns;
   for (unsigned top = 0; top < 4; top++) {
      unsigned best = LM_DEP_SLOTS;
      for (unsigned i = 0; i < LM_DEP_SLOTS; i++)
         if (!emitted[i] && p->slots[i].count &&
             (best == LM_DEP_SLOTS || p->slots[i].ns > p->slots[best].ns)) best = i;
      if (best == LM_DEP_SLOTS) break;
      emitted[best] = true;
      const struct lm_dep_cost *c = &p->slots[best];
      const struct lm_dep_identity *k = &c->key;
      fprintf(stderr, "lancelot-dep-sum: id=%llx from=%llu to=%llu kind=%u r=%llx h=%u va=%llx bytes=%llu bind=%x target=%u w=%u ht=%u producer=%llx consumer=%llx count=%llu us=%llu pd=%llu cd=%llu max=%llu\\n",
              (unsigned long long)(uintptr_t)ctx, (unsigned long long)(p->first_ns / 1000),
              (unsigned long long)(p->last_ns / 1000), k->kind,
              (unsigned long long)k->resource, k->handle, (unsigned long long)k->va,
              (unsigned long long)k->bytes, k->bind, k->target, k->width, k->height,
              (unsigned long long)k->producer_fb, (unsigned long long)k->consumer_fb,
              (unsigned long long)c->count, (unsigned long long)(c->ns / 1000),
              (unsigned long long)c->producer_draws, (unsigned long long)c->consumer_draws,
              (unsigned long long)(c->max_ns / 1000));
   }
   for (unsigned i = 0; i < LM_DEP_SLOTS; i++) if (!emitted[i]) {
      other_count += p->slots[i].count; other_ns += p->slots[i].ns;
   }
   fprintf(stderr, "lancelot-dep-other: id=%llx from=%llu to=%llu count=%llu us=%llu overflow=%llu\\n",
           (unsigned long long)(uintptr_t)ctx, (unsigned long long)(p->first_ns / 1000),
           (unsigned long long)(p->last_ns / 1000), (unsigned long long)other_count,
           (unsigned long long)(other_ns / 1000), (unsigned long long)p->overflow_count);
   memset(p, 0, sizeof(*p));
#endif
}

static void
panfrost_batch_update_access(''')
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
'''      panfrost_batch_submit(ctx, writer, LM_BATCH_READ_WRITER);''',
'''      lancelot_submit_dependency(ctx, writer, batch, rsrc, LM_BATCH_READ_WRITER);''')
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
'''            panfrost_batch_submit(ctx, batch, LM_BATCH_WRITE_USER);''',
'''            lancelot_submit_dependency(ctx, batch, &ctx->batches.slots[batch_idx],
                                       rsrc, LM_BATCH_WRITE_USER);''')
# A flush drains work; it must not create work. get_batch_for_fbo previously
# allocated a64K pool and registered attachment writers even when the current
# FBO had no batch. After mirror-blit binding restoration this fabricated a
# source-FBO writer, submitting the necessary blit as "write-user". Worse, an
# allocation failure returned before draining unrelated real pending batches.
# Find an existing current-FBO batch (if any), then drain every existing slot.
# No real GPU job, dependency, flush or completion wait is removed.
change(Path('src/gallium/drivers/panfrost/pan_context.h'),
'''#include "../../../lancelot-drm/lancelot_dependency_profile.h"
''',
'''#include "../../../lancelot-drm/lancelot_dependency_profile.h"
#include "../../../lancelot-drm/lancelot_flush_profile.h"
''')
change(Path('src/gallium/drivers/panfrost/pan_context.h'),
'''   struct lm_texture_profile lancelot_textures;
''',
'''   struct lm_texture_profile lancelot_textures;
   struct lm_flush_profile lancelot_flushes;
''')
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
'''void
panfrost_flush_all_batches(''',
'''#ifdef __APPLE__
static void
lancelot_flush_lookup(struct panfrost_context *ctx, bool missing, bool idle)
{
   if (!(pan_device(ctx->base.screen)->debug & PAN_DBG_PERF)) return;
   struct lm_flush_profile *p = &ctx->lancelot_flushes;
   uint64_t now = os_time_get_nano();
   if (!p->calls) p->from_ns = now;
   p->calls++; p->missing_fbo += missing; p->idle += idle;
   if (now - p->from_ns < UINT64_C(1000000000)) return;
   fprintf(stderr, "lancelot-flush-lookup: id=%llx from=%llu to=%llu calls=%llu missing=%llu idle=%llu\\n",
           (unsigned long long)(uintptr_t)ctx, (unsigned long long)(p->from_ns / 1000),
           (unsigned long long)(now / 1000), (unsigned long long)p->calls,
           (unsigned long long)p->missing_fbo, (unsigned long long)p->idle);
   memset(p, 0, sizeof(*p));
}
#endif

void
panfrost_flush_all_batches(''')
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
'''   struct panfrost_batch *batch = panfrost_get_batch_for_fbo(ctx);
   if (!batch)
      return;

   panfrost_batch_submit(ctx, batch, LM_BATCH_FLUSH_CURRENT);''',
'''#ifdef __APPLE__
   struct panfrost_batch *batch = ctx->batch;
   if (!batch) {
      for (unsigned i = 0; i < PAN_MAX_BATCHES; i++) {
         struct panfrost_batch *candidate = &ctx->batches.slots[i];
         if (candidate->seqnum &&
             util_framebuffer_state_equal(&candidate->key, &ctx->pipe_framebuffer)) {
            batch = candidate;
            break;
         }
      }
   }
   bool missing = batch == NULL, idle = true;
   for (unsigned i = 0; i < PAN_MAX_BATCHES; i++)
      if (ctx->batches.slots[i].seqnum) idle = false;
#else
   struct panfrost_batch *batch = panfrost_get_batch_for_fbo(ctx);
   if (!batch) return;
#endif
   if (batch)
      panfrost_batch_submit(ctx, batch, LM_BATCH_FLUSH_CURRENT);''')
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
'''         panfrost_batch_submit(ctx, &ctx->batches.slots[i], LM_BATCH_FLUSH_OTHER);
   }
}''',
'''         panfrost_batch_submit(ctx, &ctx->batches.slots[i], LM_BATCH_FLUSH_OTHER);
   }
#ifdef __APPLE__
   lancelot_flush_lookup(ctx, missing, idle);
#endif
}''')
# First stage-workset repair is deliberately asymmetric. G52 tiler descriptors
# name fragment state; never prune its full VTC list using stage bits alone.
# Only a resource-level READ of a PIPE_BUFFER in the vertex stage earns an
# observational input tag. A fragment reference, a write, unknown flags, pools,
# structural BOs or JC always win and keep the BO in the FS workset. Kernel
# ownership rules, actual dependencies, all pools and every flush/wait unchanged.
change(Path('src/gallium/drivers/panfrost/pan_bo.h'),
'''typedef uint8_t pan_bo_access;''',
'''#ifdef __APPLE__
/* Internal role tag only. Never sent as a wire permission/GPU-only flag. */
#define PAN_BO_ACCESS_VERTEX_INPUT (1 << 5)
#endif

typedef uint8_t pan_bo_access;''')
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
'''   uint32_t access = PAN_BO_ACCESS_READ | panfrost_access_for_stage(stage);

   panfrost_batch_add_bo_old(batch, rsrc->bo, access);''',
'''   uint32_t access = PAN_BO_ACCESS_READ | panfrost_access_for_stage(stage);
#ifdef __APPLE__
   if (stage == PIPE_SHADER_VERTEX && rsrc->base.target == PIPE_BUFFER)
      access |= PAN_BO_ACCESS_VERTEX_INPUT;
#endif

   panfrost_batch_add_bo_old(batch, rsrc->bo, access);''')
change(Path('src/gallium/drivers/panfrost/pan_context.h'),
'''#include "../../../lancelot-drm/lancelot_flush_profile.h"
''',
'''#include "../../../lancelot-drm/lancelot_flush_profile.h"
#include "../../../lancelot-drm/lancelot_stage_profile.h"
''')
change(Path('src/gallium/drivers/panfrost/pan_context.h'),
'''   struct lm_flush_profile lancelot_flushes;
''',
'''   struct lm_flush_profile lancelot_flushes;
   struct lm_stage_profile lancelot_stages;
''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''static int
jm_submit_jc(''',
'''#ifdef __APPLE__
#include "util/os_time.h"
static bool
lancelot_fs_input_only(pan_bo_access flags)
{
   const unsigned needed = PAN_BO_ACCESS_READ | PAN_BO_ACCESS_VERTEX_TILER |
                           PAN_BO_ACCESS_VERTEX_INPUT;
   const unsigned allowed = needed | PAN_BO_ACCESS_SHARED;
   return (flags & needed) == needed && !(flags & ~allowed);
}

static bool
lancelot_structural_bo(struct panfrost_batch *batch,
                       struct panfrost_device *dev, struct panfrost_bo *bo)
{
   if (bo == dev->tiler_heap || bo == dev->sample_positions ||
       bo == batch->scratchpad || bo == batch->shared_memory ||
       bo == batch->polygon_list_bo)
      return true;
   util_dynarray_foreach(&batch->pool.bos, struct panfrost_bo *, entry)
      if (*entry == bo) return true;
   util_dynarray_foreach(&batch->invisible_pool.bos, struct panfrost_bo *, entry)
      if (*entry == bo) return true;
   return false;
}

static void
lancelot_stage_record(struct panfrost_context *ctx, unsigned reqs,
                      unsigned original, unsigned kept, unsigned omitted,
                      uint64_t bytes, int error)
{
   if (!(pan_device(ctx->base.screen)->debug & PAN_DBG_PERF)) return;
   struct lm_stage_profile *p = &ctx->lancelot_stages;
   uint64_t now = os_time_get_nano();
   if (!p->vt_calls && !p->fs_calls) p->from_ns = now;
   if (reqs & PANFROST_JD_REQ_FS) p->fs_calls++; else p->vt_calls++;
   p->original_refs += original; p->kept_refs += kept;
   p->omitted_refs += omitted; p->omitted_bytes += bytes; p->errors += error != 0;
   if (now - p->from_ns < UINT64_C(1000000000)) return;
   fprintf(stderr, "lancelot-stage-workset: id=%llx from=%llu to=%llu vt=%llu fs=%llu original=%llu kept=%llu omitted=%llu bytes=%llu errors=%llu\\n",
           (unsigned long long)(uintptr_t)ctx, (unsigned long long)(p->from_ns / 1000),
           (unsigned long long)(now / 1000), (unsigned long long)p->vt_calls,
           (unsigned long long)p->fs_calls, (unsigned long long)p->original_refs,
           (unsigned long long)p->kept_refs, (unsigned long long)p->omitted_refs,
           (unsigned long long)p->omitted_bytes, (unsigned long long)p->errors);
   memset(p, 0, sizeof(*p));
}
#endif

static int
jm_submit_jc(''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''   int ret;

   /* If we trace, we always need a syncobj,''',
'''   int ret;
#ifdef __APPLE__
   const bool trim = PAN_ARCH == 7 && reqs == PANFROST_JD_REQ_FS &&
      panfrost_device_gpu_id(dev) == 0x7402 &&
      !(dev->debug & (PAN_DBG_DUMP | PAN_DBG_TRACE | PAN_DBG_SYNC)) &&
      debug_get_bool_option("LANCELOT_FS_INPUT_TRIM", true);
   unsigned original_refs = 0, kept_refs = 0, omitted_refs = 0;
   uint64_t omitted_bytes = 0;
#endif

   /* If we trace, we always need a syncobj,''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''      assert(submit.bo_handle_count < batch->num_bos);
      bo_handles[submit.bo_handle_count++] = i;''',
'''      struct panfrost_bo *bo = pan_lookup_bo(dev, i);
#ifdef __APPLE__
      original_refs++;
      bool contains_job = first_job_desc >= bo->ptr.gpu &&
         first_job_desc - bo->ptr.gpu < panfrost_bo_size(bo);
      if (trim && lancelot_fs_input_only(flags[i]) && !contains_job &&
          !lancelot_structural_bo(batch, dev, bo)) {
         omitted_refs++; omitted_bytes += panfrost_bo_size(bo);
         continue;
      }
      kept_refs++;
#endif
      assert(submit.bo_handle_count < batch->num_bos);
      bo_handles[submit.bo_handle_count++] = i;''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''      struct panfrost_bo *bo = pan_lookup_bo(dev, i);

      bo->gpu_access |= flags[i] & (PAN_BO_ACCESS_RW);''',
'''      bo->gpu_access |= flags[i] & (PAN_BO_ACCESS_RW);''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''   free(bo_handles);

#ifdef __APPLE__''',
'''   free(bo_handles);

#ifdef __APPLE__
   lancelot_stage_record(ctx, reqs, original_refs, kept_refs, omitted_refs,
                         omitted_bytes, ret);
#endif

#ifdef __APPLE__''')
# Combined follow-up: classify pixel DATA separately from fragment descriptors,
# queries and uniform buffers. G52 VTC never executes the fragment shader. Keep
# every descriptor/pool and any vertex/compute use; do not blanket-prune FS bits
# (the tiler occlusion counter is a fragment-tagged BUFFER, for example).
change(Path('src/gallium/drivers/panfrost/pan_bo.h'),
'''#define PAN_BO_ACCESS_VERTEX_INPUT (1 << 5)''',
'''#define PAN_BO_ACCESS_VERTEX_INPUT (1 << 5)
#define PAN_BO_ACCESS_FRAGMENT_PIXELS (1 << 6)''')
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
'''   if (stage == PIPE_SHADER_VERTEX && rsrc->base.target == PIPE_BUFFER)
      access |= PAN_BO_ACCESS_VERTEX_INPUT;
#endif''',
'''   if (stage == PIPE_SHADER_VERTEX && rsrc->base.target == PIPE_BUFFER)
      access |= PAN_BO_ACCESS_VERTEX_INPUT;
   else if (stage == PIPE_SHADER_FRAGMENT && rsrc->base.target != PIPE_BUFFER &&
            !(rsrc->base.bind & PIPE_BIND_DEPTH_STENCIL) && !rsrc->base.next &&
            !rsrc->separate_stencil && !rsrc->shadow_image &&
            (rsrc->image.layout.modifier == DRM_FORMAT_MOD_LINEAR ||
             rsrc->image.layout.modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED))
      access |= PAN_BO_ACCESS_FRAGMENT_PIXELS;
#endif''')
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
'''   uint32_t access = PAN_BO_ACCESS_WRITE | panfrost_access_for_stage(stage);

   panfrost_batch_add_bo_old(batch, rsrc->bo, access);''',
'''   uint32_t access = PAN_BO_ACCESS_WRITE | panfrost_access_for_stage(stage);
#ifdef __APPLE__
   if (stage == PIPE_SHADER_FRAGMENT && rsrc->base.target != PIPE_BUFFER &&
       !(rsrc->base.bind & PIPE_BIND_DEPTH_STENCIL) && !rsrc->base.next &&
       !rsrc->separate_stencil && !rsrc->shadow_image &&
       (rsrc->image.layout.modifier == DRM_FORMAT_MOD_LINEAR ||
        rsrc->image.layout.modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED))
      access |= PAN_BO_ACCESS_FRAGMENT_PIXELS;
#endif

   panfrost_batch_add_bo_old(batch, rsrc->bo, access);''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''static bool
lancelot_structural_bo(''',
'''static bool
lancelot_vtc_pixel_only(pan_bo_access flags)
{
   const unsigned needed = PAN_BO_ACCESS_FRAGMENT | PAN_BO_ACCESS_FRAGMENT_PIXELS;
   const unsigned allowed = needed | PAN_BO_ACCESS_RW | PAN_BO_ACCESS_SHARED;
   return (flags & needed) == needed && (flags & PAN_BO_ACCESS_RW) &&
          !(flags & ~allowed);
}

static bool
lancelot_structural_bo(''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''   const bool trim = PAN_ARCH == 7 && reqs == PANFROST_JD_REQ_FS &&''',
'''   const bool trim = PAN_ARCH == 7 && reqs <= PANFROST_JD_REQ_FS &&''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''      debug_get_bool_option("LANCELOT_FS_INPUT_TRIM", true);''',
'''      debug_get_bool_option("LANCELOT_STAGE_TRIM", true);''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''      if (trim && lancelot_fs_input_only(flags[i]) && !contains_job &&''',
'''      bool unused = reqs == PANFROST_JD_REQ_FS ? lancelot_fs_input_only(flags[i]) :
                    reqs == 0 && lancelot_vtc_pixel_only(flags[i]);
      if (trim && unused && !contains_job &&''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''   p->omitted_refs += omitted; p->omitted_bytes += bytes; p->errors += error != 0;''',
'''   p->omitted_refs += omitted; p->omitted_bytes += bytes; p->errors += error != 0;
   if (reqs == PANFROST_JD_REQ_FS) p->fs_omitted_refs += omitted;
   else if (reqs == 0) p->vt_omitted_refs += omitted;''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''kept=%llu omitted=%llu bytes=%llu errors=%llu\\n",''',
'''kept=%llu omitted=%llu bytes=%llu errors=%llu vt_omitted=%llu fs_omitted=%llu\\n",''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''           (unsigned long long)p->omitted_bytes, (unsigned long long)p->errors);''',
'''           (unsigned long long)p->omitted_bytes, (unsigned long long)p->errors,
           (unsigned long long)p->vt_omitted_refs, (unsigned long long)p->fs_omitted_refs);''')

# One retained scratch handle array for both synchronous chains, not two calloc
# calls while holding the tiler lock. Each chain still builds its own exact list.
# Allocation failure is returned BEFORE either chain/lock is published.
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''uint32_t reqs, uint32_t out_sync)''',
'''uint32_t reqs, uint32_t out_sync, uint32_t *bo_handles)''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''   uint32_t *bo_handles;
   int ret;''',
'''   int ret;''')
p = src/'src/gallium/drivers/panfrost/pan_jm.c'
data, count = re.subn(
    r'(?m)^   bo_handles = calloc\(panfrost_pool_num_bos\(&batch->pool\) \+\n'
    r' +panfrost_pool_num_bos\(&batch->invisible_pool\) \+\n'
    r' +batch->num_bos \+ 2,\n +sizeof\(\*bo_handles\)\);\n   assert\(bo_handles\);\n',
    '   assert(bo_handles);\n', p.read_text(), count=1)
if count != 1: raise SystemExit('pan_jm.c: exact retained handle-array allocation hunk missing')
p.write_text(data)
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''   free(bo_handles);

#ifdef __APPLE__
   lancelot_stage_record''',
'''#ifdef __APPLE__
   lancelot_stage_record''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''   int ret = 0;

   unsigned reqs =''',
'''   int ret = 0;
   uint32_t *bo_handles = NULL;
   if (has_draws || has_frag) {
      size_t capacity = (size_t)panfrost_pool_num_bos(&batch->pool) +
         panfrost_pool_num_bos(&batch->invisible_pool) + batch->num_bos + 2;
      if (capacity > SIZE_MAX / sizeof(*bo_handles)) return ENOMEM;
      bo_handles = malloc(capacity * sizeof(*bo_handles));
      if (!bo_handles) return ENOMEM;
   }

   unsigned reqs =''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''has_frag ? 0 : out_sync);''',
'''has_frag ? 0 : out_sync, bo_handles);''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''out_sync);
      if (ret)
         goto done;''',
'''out_sync, bo_handles);
      if (ret)
         goto done;''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''   if (has_tiler)
      pthread_mutex_unlock(&dev->submit_lock);

   return ret;''',
'''   if (has_tiler)
      pthread_mutex_unlock(&dev->submit_lock);
   free(bo_handles);

   return ret;''')
# Full-discard writes must choose their final backing BEFORE exporting the old
# BO to the CPU. Otherwise an abandoned GPU-only BO acquires permanent export
# history and the caller pays an unnecessary ownership handoff. Keep every
# writer/read wait, partial shadow copy, shared/persistent and read path intact.
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
'''static void *
panfrost_ptr_map(''',
'''#ifdef __APPLE__
static void *
lancelot_transfer_fail(struct panfrost_transfer *transfer,
                       struct pipe_transfer **out_transfer)
{
   pipe_resource_reference(&transfer->staging.rsrc, NULL);
   pipe_resource_reference(&transfer->base.resource, NULL);
   ralloc_free(transfer);
   *out_transfer = NULL;
   return NULL;
}
#endif

static void *
panfrost_ptr_map(''')
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
'''   struct panfrost_transfer *transfer = rzalloc(pctx, struct panfrost_transfer);
   transfer->base.level = level;''',
'''   struct panfrost_transfer *transfer = rzalloc(pctx, struct panfrost_transfer);
#ifdef __APPLE__
   *out_transfer = NULL;
   if (!transfer) return NULL;
#endif
   transfer->base.level = level;''')
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
'''   /* If we haven't already mmaped, now's the time */
   if (panfrost_bo_mmap(bo))
      return NULL;
''',
'''#ifdef __APPLE__
   uintptr_t original_bo = (uintptr_t)bo;
   bool defer_map = !already_mapped && (usage & PIPE_MAP_WRITE) &&
      !(usage & PIPE_MAP_READ) && !(resource->flags & PIPE_RESOURCE_FLAG_MAP_PERSISTENT) &&
      !(bo->flags & PAN_BO_SHARED) && !rsrc->separate_stencil && !rsrc->shadow_image &&
      !(dev->debug & (PAN_DBG_TRACE | PAN_DBG_SYNC | PAN_DBG_DUMP)) &&
      ((usage & PIPE_MAP_DISCARD_WHOLE_RESOURCE) || panfrost_can_discard(resource, box, usage));
   if (!defer_map && panfrost_bo_mmap(bo))
      return lancelot_transfer_fail(transfer, out_transfer);
   if (defer_map && (dev->debug & PAN_DBG_PERF)) ctx->lancelot_stages.deferred_maps++;
#else
   if (panfrost_bo_mmap(bo)) return NULL;
#endif
''')
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
'''   /* For access to compressed textures, we want the (x, y, w, h)''',
'''#ifdef __APPLE__
   if (defer_map) {
      if (panfrost_bo_mmap(bo)) return lancelot_transfer_fail(transfer, out_transfer);
      if ((uintptr_t)bo != original_bo && (dev->debug & PAN_DBG_PERF))
         ctx->lancelot_stages.avoided_old_maps++;
   }
#endif

   /* For access to compressed textures, we want the (x, y, w, h)''')
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
'''      if (panfrost_bo_mmap(staging->bo))
         return NULL;''',
'''      if (panfrost_bo_mmap(staging->bo))
#ifdef __APPLE__
         return lancelot_transfer_fail(transfer, out_transfer);
#else
         return NULL;
#endif''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''vt_omitted=%llu fs_omitted=%llu\\n",''',
'''vt_omitted=%llu fs_omitted=%llu deferred_maps=%llu avoided_maps=%llu\\n",''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''           (unsigned long long)p->vt_omitted_refs, (unsigned long long)p->fs_omitted_refs);''',
'''           (unsigned long long)p->vt_omitted_refs, (unsigned long long)p->fs_omitted_refs,
           (unsigned long long)p->deferred_maps, (unsigned long long)p->avoided_old_maps);''')
# Stable per-process shader ID + CPU-generated binary hash, once at compile/cache
# materialization. Batch observations copy IDs DURING each draw, never infer the
# old queued batch's shaders from a subsequently changed current context.
change(Path('src/gallium/drivers/panfrost/pan_context.h'),
'''#include "../../../lancelot-drm/lancelot_stage_profile.h"
''',
'''#include "../../../lancelot-drm/lancelot_stage_profile.h"
#include "../../../lancelot-drm/lancelot_shader_profile.h"
''')
change(Path('src/gallium/drivers/panfrost/pan_context.h'),
'''   struct lm_stage_profile lancelot_stages;
''',
'''   struct lm_stage_profile lancelot_stages;
   bool lancelot_report_batch;
''')
change(Path('src/gallium/drivers/panfrost/pan_context.h'),
'''   unsigned dirty_3d, dirty_shader;
};''',
'''   unsigned dirty_3d, dirty_shader;
#ifdef __APPLE__
   uint32_t lancelot_id;
#endif
};''')
change(Path('src/gallium/drivers/panfrost/pan_job.h'),
'''struct panfrost_batch {''',
'''struct panfrost_batch {
#ifdef __APPLE__
   struct lm_shader_mix lancelot_shader_mix;
#endif''')
change(Path('src/gallium/drivers/panfrost/pan_job.h'),
'''#include "pan_mempool.h"''',
'''#include "pan_mempool.h"
#include "../../../lancelot-drm/lancelot_shader_profile.h"''')
change(Path('src/gallium/drivers/panfrost/pan_shader.c'),
'''   struct panfrost_shader_binary res = {0};''',
'''#ifdef __APPLE__
   static uint32_t lancelot_shader_serial;
   uint64_t lancelot_began = os_time_get_nano();
   uint64_t lancelot_compile_ns = 0, lancelot_compile_begin = 0;
#endif
   struct panfrost_shader_binary res = {0};''')
change(Path('src/gallium/drivers/panfrost/pan_shader.c'),
'''   if (!panfrost_disk_cache_retrieve(screen->disk_cache, uncompiled,
                                      &state->key, &res)) {''',
'''   bool lancelot_cache_hit = panfrost_disk_cache_retrieve(screen->disk_cache, uncompiled,
                                                       &state->key, &res);
   if (!lancelot_cache_hit) {''')
change(Path('src/gallium/drivers/panfrost/pan_shader.c'),
'''      panfrost_shader_compile(screen, uncompiled->nir, dbg, &state->key,
                              req_local_mem, uncompiled->fixed_varying_mask,
                              &res);''',
'''#ifdef __APPLE__
      lancelot_compile_begin = os_time_get_nano();
#endif
      panfrost_shader_compile(screen, uncompiled->nir, dbg, &state->key,
                              req_local_mem, uncompiled->fixed_varying_mask,
                              &res);
#ifdef __APPLE__
      lancelot_compile_ns = os_time_get_nano() - lancelot_compile_begin;
#endif''')
change(Path('src/gallium/drivers/panfrost/pan_shader.c'),
'''   util_dynarray_fini(&res.binary);

   /* Don't upload RSD''',
'''#ifdef __APPLE__
   if (dev->debug & PAN_DBG_PERF) {
      unsigned char digest[20]; char hash[41];
      _mesa_sha1_compute(res.binary.data, res.binary.size, digest);
      for (unsigned i=0;i<20;i++) snprintf(hash+2*i,3,"%02x",digest[i]);
      state->lancelot_id = p_atomic_inc_return(&lancelot_shader_serial);
      fprintf(stderr,"lancelot-shader: pid=%d id=%u stage=%u hash=%s bytes=%llu regs=%u tls=%u ubos=%u textures=%u samplers=%u cache=%u compile_us=%llu materialize_us=%llu at=%llu\\n",
              getpid(),state->lancelot_id,uncompiled->nir->info.stage,hash,
              (unsigned long long)res.binary.size,res.info.work_reg_count,res.info.tls_size,
              res.info.ubo_count,res.info.texture_count,res.info.sampler_count,lancelot_cache_hit,
              (unsigned long long)(lancelot_compile_ns/1000),
              (unsigned long long)((os_time_get_nano()-lancelot_began)/1000),
              (unsigned long long)(os_time_get_nano()/1000));
   }
#endif
   util_dynarray_fini(&res.binary);

   /* Don't upload RSD''')
change(Path('src/gallium/drivers/panfrost/pan_shader.c'),
'''#include "pan_context.h"''',
'''#include "pan_context.h"
#ifdef __APPLE__
#include "util/os_time.h"
#include "util/u_atomic.h"
#include <stdio.h>
#include <unistd.h>
#endif''')
# There are two actual draw launch sites (indexed/indirect and direct). Exact
# count keeps their location explicit and catches a changed upstream contract.
p=src/'src/gallium/drivers/panfrost/pan_cmdstream.c';data=p.read_text()
needle='   batch->draw_count++;'
if data.count(needle)!=2: raise SystemExit('pan_cmdstream.c: expected exactly two draw count sites')
data=data.replace(needle,'''#ifdef __APPLE__
   if (pan_device(ctx->base.screen)->debug & PAN_DBG_PERF) {
      uint32_t vsid = ctx->prog[PIPE_SHADER_VERTEX] ? ctx->prog[PIPE_SHADER_VERTEX]->lancelot_id : 0;
      uint32_t fsid = ctx->prog[PIPE_SHADER_FRAGMENT] &&
         panfrost_fs_required(ctx->prog[PIPE_SHADER_FRAGMENT],ctx->blend,&batch->key,ctx->depth_stencil) ?
         ctx->prog[PIPE_SHADER_FRAGMENT]->lancelot_id : 0;
      lm_shader_mix_record(&batch->lancelot_shader_mix,vsid,fsid);
   }
#endif
'''+needle)
p.write_text(data)
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''   memset(p, 0, sizeof(*p));
}
#endif

static int
jm_submit_jc''',
'''   memset(p, 0, sizeof(*p));
   ctx->lancelot_report_batch = true;
}
#endif

static int
jm_submit_jc''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''   int ret = 0;
   uint32_t *bo_handles = NULL;''',
'''   int ret = 0;
#ifdef __APPLE__
   uint64_t lancelot_began = os_time_get_nano();
   batch->ctx->lancelot_report_batch = false;
#endif
   uint32_t *bo_handles = NULL;''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
'''   free(bo_handles);

   return ret;''',
'''   free(bo_handles);
#ifdef __APPLE__
   if (batch->ctx->lancelot_report_batch) {
      uint64_t now = os_time_get_nano();
      const struct lm_shader_mix *m = &batch->lancelot_shader_mix;
      fprintf(stderr,"lancelot-shader-batch: pid=%d ctx=%llx seq=%llu at=%llu done=%llu wall_us=%llu vtjc=%llx fsjc=%llx draws=%u other=%u v0=%u f0=%u n0=%u v1=%u f1=%u n1=%u v2=%u f2=%u n2=%u v3=%u f3=%u n3=%u error=%d helpers=untracked\\n",
              getpid(),(unsigned long long)(uintptr_t)batch->ctx,(unsigned long long)batch->seqnum,
              (unsigned long long)(lancelot_began/1000),(unsigned long long)(now/1000),
              (unsigned long long)((now-lancelot_began)/1000),
              (unsigned long long)batch->jm.jobs.vtc_jc.first_job,(unsigned long long)batch->jm.jobs.frag,
              batch->draw_count,m->other,m->pair[0].vs,m->pair[0].fs,m->pair[0].draws,
              m->pair[1].vs,m->pair[1].fs,m->pair[1].draws,m->pair[2].vs,m->pair[2].fs,m->pair[2].draws,
              m->pair[3].vs,m->pair[3].fs,m->pair[3].draws,ret);
   }
#endif

   return ret;''')
# Diagnostic clock v2: these 14 calls were all introduced by this overlay.
# Bound the replacements per file; global util clocks and timeout/cache users
# are untouched. Every sampled batch emits an explicit paired clock anchor.
for name,count in [('pan_job.c',5),('pan_cmdstream.c',1),('pan_jm.c',3),('pan_shader.c',5)]:
    p=src/'src/gallium/drivers/panfrost'/name;data=p.read_text()
    assert data.count('os_time_get_nano()')==count,(name,'diagnostic clock sites changed')
    data=data.replace('os_time_get_nano()','lancelot_diag_ns()')
    p.write_text('#ifdef __APPLE__\n#include "../../../lancelot-drm/lancelot_diag.h"\n#endif\n'+data)
change(Path('src/gallium/drivers/panfrost/pan_context.h'),
       '   uint32_t lancelot_id;', '   uint32_t lancelot_id;\n   struct lm_shader_meta lancelot_meta;')
change(Path('src/gallium/drivers/panfrost/pan_job.h'),
       '   struct lm_shader_mix lancelot_shader_mix;',
       '   struct lm_shader_mix lancelot_shader_mix;\n   uint64_t lancelot_vtq, lancelot_fsq;')
change(Path('src/gallium/drivers/panfrost/pan_shader.c'),
       '      state->lancelot_id = p_atomic_inc_return(&lancelot_shader_serial);',
       '''      state->lancelot_id = p_atomic_inc_return(&lancelot_shader_serial);
      state->lancelot_meta = (struct lm_shader_meta){
         .id=state->lancelot_id,.stage=uncompiled->nir->info.stage,
         .bytes=res.binary.size,.regs=res.info.work_reg_count,.tls=res.info.tls_size,
         .ubos=res.info.ubo_count,.textures=res.info.texture_count,.samplers=res.info.sampler_count,
         .cache=lancelot_cache_hit,.compile_us=lancelot_compile_ns/1000,
         .materialize_us=(lancelot_diag_ns()-lancelot_began)/1000};
      memcpy(state->lancelot_meta.hash,hash,sizeof(hash));''')
p=src/'src/gallium/drivers/panfrost/pan_cmdstream.c';data=p.read_text()
needle='      lm_shader_mix_record(&batch->lancelot_shader_mix,vsid,fsid);'
assert data.count(needle)==2
data=data.replace(needle,'''      lm_shader_mix_record_meta(&batch->lancelot_shader_mix,
         vsid ? &ctx->prog[PIPE_SHADER_VERTEX]->lancelot_meta : NULL,
         fsid ? &ctx->prog[PIPE_SHADER_FRAGMENT]->lancelot_meta : NULL);''')
p.write_text(data)
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
       '      ret = drmIoctl(panfrost_device_fd(dev), DRM_IOCTL_PANFROST_SUBMIT, &submit);',
       '''      ret = drmIoctl(panfrost_device_fd(dev), DRM_IOCTL_PANFROST_SUBMIT, &submit);
#ifdef __APPLE__
      if (reqs & PANFROST_JD_REQ_FS) batch->lancelot_fsq = lancelot_drm_last_submit();
      else batch->lancelot_vtq = lancelot_drm_last_submit();
#endif''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
       '      const struct lm_shader_mix *m = &batch->lancelot_shader_mix;',
       '''      const struct lm_shader_mix *m = &batch->lancelot_shader_mix;
       lancelot_diag_anchor();
       fprintf(stderr,"lancelot-batch-q: v=2 clock=mach_absolute_time pid=%d ctx=%llx seq=%llu vtq=%llu fsq=%llu at=%llu done=%llu\\n",
               getpid(),(unsigned long long)(uintptr_t)batch->ctx,(unsigned long long)batch->seqnum,
               (unsigned long long)batch->lancelot_vtq,(unsigned long long)batch->lancelot_fsq,
               (unsigned long long)(lancelot_began/1000),(unsigned long long)(now/1000));
       for (unsigned i=0;i<LM_SHADER_PAIRS;i++) for (unsigned s=0;s<2;s++) {
          const struct lm_shader_meta *meta=s?&m->fs[i]:&m->vs[i];
          if (!m->pair[i].draws || !meta->id) continue;
          fprintf(stderr,"lancelot-shader-meta: v=2 pid=%d seq=%llu id=%u stage=%u hash=%s bytes=%u regs=%u tls=%u ubos=%u textures=%u samplers=%u cache=%u compile_us=%llu materialize_us=%llu source=CPU_binary_saved\\n",
                  getpid(),(unsigned long long)batch->seqnum,meta->id,meta->stage,meta->hash,
                  meta->bytes,meta->regs,meta->tls,meta->ubos,meta->textures,meta->samplers,meta->cache,
                  (unsigned long long)meta->compile_us,(unsigned long long)meta->materialize_us);
       }''')
# Per-job read-only access is a negotiated LMAL extension. Keep unknown roles,
# pools, tiler/shader scratch and the writable job chain conservative. Kernel
# PTE permissions, alias checks and detach/reset lifetime enforce the hint.
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
       'uint32_t reqs, uint32_t out_sync, uint32_t *bo_handles)',
       'uint32_t reqs, uint32_t out_sync, uint32_t *bo_handles, uint32_t *bo_readonly)')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
       '   assert(bo_handles);',
       '''   assert(bo_handles && bo_readonly);
   size_t access_capacity = (size_t)panfrost_pool_num_bos(&batch->pool) +
      panfrost_pool_num_bos(&batch->invisible_pool) + batch->num_bos + 2;
   memset(bo_readonly, 0, access_capacity * sizeof(*bo_readonly));''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
       '      bo_handles[submit.bo_handle_count++] = i;',
       '''#ifdef __APPLE__
      const unsigned known = PAN_BO_ACCESS_RW | PAN_BO_ACCESS_SHARED |
         PAN_BO_ACCESS_VERTEX_TILER | PAN_BO_ACCESS_FRAGMENT |
         PAN_BO_ACCESS_VERTEX_INPUT | PAN_BO_ACCESS_FRAGMENT_PIXELS;
      if ((flags[i] & PAN_BO_ACCESS_RW) == PAN_BO_ACCESS_READ &&
          !(flags[i] & ~known) && !contains_job &&
          !lancelot_structural_bo(batch, dev, bo))
         bo_readonly[submit.bo_handle_count] = 1;
#endif
      bo_handles[submit.bo_handle_count++] = i;''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
       '      ret = drmIoctl(panfrost_device_fd(dev), DRM_IOCTL_PANFROST_SUBMIT, &submit);',
       '''#ifdef __APPLE__
      ret = lancelot_drm_submit_access(panfrost_device_fd(dev), &submit, bo_readonly);
#else
      ret = drmIoctl(panfrost_device_fd(dev), DRM_IOCTL_PANFROST_SUBMIT, &submit);
#endif''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
       '   uint32_t *bo_handles = NULL;', '   uint32_t *bo_handles = NULL, *bo_readonly = NULL;')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
       '''      if (capacity > SIZE_MAX / sizeof(*bo_handles)) return ENOMEM;
      bo_handles = malloc(capacity * sizeof(*bo_handles));
      if (!bo_handles) return ENOMEM;''',
       '''      if (capacity > SIZE_MAX / (2 * sizeof(*bo_handles))) return ENOMEM;
      bo_handles = malloc(capacity * 2 * sizeof(*bo_handles));
      if (!bo_handles) return ENOMEM;
      bo_readonly = bo_handles + capacity;''')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
       'has_frag ? 0 : out_sync, bo_handles);', 'has_frag ? 0 : out_sync, bo_handles, bo_readonly);')
change(Path('src/gallium/drivers/panfrost/pan_jm.c'),
       'out_sync, bo_handles);', 'out_sync, bo_handles, bo_readonly);')
# A full clear of a private single-level image discards the old pixels. If
# only already-recorded readers remain, keep their retained BO and give the
# resource new backing BEFORE get_batch_for_fbo registers a writer. This is
# the GPU-only counterpart of the existing whole-discard transfer shadowing;
# no pixels are copied and a pending writer always keeps the original path.
change(Path('src/gallium/drivers/panfrost/pan_context.h'),
       '   bool lancelot_report_batch;',
       '''   bool lancelot_report_batch;
   struct {
      uint64_t pending_bytes, attempts, renamed, bytes, oom, last_ns;
       uint64_t rejected[8];
       uint64_t resource_rejected[8], sample_va, sample_flags, sample_bind, sample_mask;
   } lancelot_clear_rename;''')
change(Path('src/gallium/drivers/panfrost/pan_context.c'),
       '''static void
panfrost_clear(''',
       '''#ifdef __APPLE__
#include "../../../lancelot-drm/lancelot_diag.h"

enum lancelot_clear_reject_reason {
   LM_CLEAR_FRAME, LM_CLEAR_RESOURCE, LM_CLEAR_MODIFIER, LM_CLEAR_WRITER,
   LM_CLEAR_ALIAS, LM_CLEAR_NO_READERS, LM_CLEAR_BUDGET, LM_CLEAR_OOM
};

static uint64_t
lancelot_clear_reject(struct panfrost_context *ctx, unsigned reason)
{
   if (pan_device(ctx->base.screen)->debug & PAN_DBG_PERF)
      ctx->lancelot_clear_rename.rejected[reason]++;
   return 0;
}

struct lancelot_clear_backup {
   struct panfrost_resource *resource;
   struct panfrost_bo *old;
   uint64_t base, bytes;
   bool crc;
   BITSET_DECLARE(data, PAN_MAX_MIP_LEVELS);
};

/* Independent subcondition counts (may overlap), sampled scalars only. This
 * must not turn an imported IOSurface into a private renameable allocation. */
static unsigned
lancelot_clear_resource_mask(const struct pipe_framebuffer_state *fb,
                             const struct pipe_surface *surf,
                             const struct panfrost_resource *r)
{
   const struct pipe_resource *base = surf->texture;
   const struct panfrost_bo *old = r->bo;
   unsigned mask = 0;
   if (!old || base->target != PIPE_TEXTURE_2D || base->last_level ||
       base->depth0 != 1 || base->array_size != 1 || base->nr_samples > 1 ||
       base->nr_storage_samples > 1) mask |= 1u << 0;
   if (base->next || (base->bind & (PAN_BIND_SHARED_MASK | PIPE_BIND_DEPTH_STENCIL)) ||
       r->scanout || r->separate_stencil || r->shadow_image ||
       (old && (old->flags & PAN_BO_SHARED))) mask |= 1u << 1;
   if (base->flags) mask |= 1u << 2;
   if (surf->format != base->format || surf->u.tex.level ||
       surf->u.tex.first_layer || surf->u.tex.last_layer ||
       fb->width != base->width0 || fb->height != base->height0) mask |= 1u << 3;
   if (r->damage.tile_map.enable || r->damage.extent.minx || r->damage.extent.miny ||
       r->damage.extent.maxx != base->width0 ||
       r->damage.extent.maxy != base->height0) mask |= 1u << 4;
   if (old && old->ptr.cpu) mask |= 1u << 5;
   if (old && (old->flags & ~(PAN_BO_DELAY_MMAP | PAN_BO_INVISIBLE | PAN_BO_SHARED)))
       mask |= 1u << 6;
   if (old && r->image.data.base != old->ptr.gpu) mask |= 1u << 7;
   return mask;
}

static uint64_t
lancelot_rename_clear_target(struct panfrost_context *ctx, unsigned buffers,
                             const struct pipe_scissor_state *scissor,
                             struct lancelot_clear_backup *backup)
{
   const struct pipe_framebuffer_state *fb = &ctx->pipe_framebuffer;
   struct panfrost_device *dev = pan_device(ctx->base.screen);
   if (dev->debug & PAN_DBG_PERF) ctx->lancelot_clear_rename.attempts++;
   if (panfrost_device_gpu_id(dev) != 0x7402 || ctx->batch || scissor ||
       buffers != PIPE_CLEAR_COLOR0 || fb->nr_cbufs != 1 || !fb->cbufs[0] ||
       fb->zsbuf || fb->samples > 1 || fb->layers > 1 || ctx->is_noop ||
       (dev->debug & (PAN_DBG_TRACE | PAN_DBG_DUMP | PAN_DBG_SYNC)))
      return lancelot_clear_reject(ctx, LM_CLEAR_FRAME);

   const struct pipe_surface *surf = fb->cbufs[0];
   struct pipe_resource *base = surf->texture;
   struct panfrost_resource *r = pan_resource(base);
   struct panfrost_bo *old = r->bo;
   unsigned resource_mask = lancelot_clear_resource_mask(fb, surf, r);
   if (resource_mask) {
      if (dev->debug & PAN_DBG_PERF) {
         for (unsigned i = 0; i < 8; i++)
            ctx->lancelot_clear_rename.resource_rejected[i] += (resource_mask >> i) & 1;
         ctx->lancelot_clear_rename.sample_va = old ? old->ptr.gpu : 0;
         ctx->lancelot_clear_rename.sample_flags = old ? old->flags : 0;
         ctx->lancelot_clear_rename.sample_bind = base->bind;
         ctx->lancelot_clear_rename.sample_mask = resource_mask;
      }
       return lancelot_clear_reject(ctx, LM_CLEAR_RESOURCE);
   }

   /* resource_setup marks ordinary private LINEAR resources modifier_constant
    * too. That forbids changing layout, not renaming equally sized backing.
    * Retain its exact modifier/stride/CRC layout and all sharing/alias guards.
    * Fixed non-linear modifiers remain excluded in this narrow path. */
   if ((r->modifier_constant && r->image.layout.modifier != DRM_FORMAT_MOD_LINEAR) ||
       (r->image.layout.modifier != DRM_FORMAT_MOD_LINEAR &&
        r->image.layout.modifier != DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED))
      return lancelot_clear_reject(ctx, LM_CLEAR_MODIFIER);

   /* Pending framebuffer writers construct their FBD at submit time from the
    * resource. They must never see renamed backing. Recorded sampler readers
    * already contain their old GPU addresses and hold their own BO references. */
   if (panfrost_any_batch_writes_rsrc(ctx, r))
      return lancelot_clear_reject(ctx, LM_CLEAR_WRITER);

   /* The resource-keyed writer map is insufficient for an alias/writable
    * use through another resource. Check every queued BO access as well. */
   unsigned handle = panfrost_bo_handle(old);
   bool readers = false;
   for (unsigned i = 0; i < PAN_MAX_BATCHES; i++) {
      struct panfrost_batch *batch = &ctx->batches.slots[i];
      if (!batch->seqnum || handle >= util_dynarray_num_elements(&batch->bos, pan_bo_access))
         continue;
      pan_bo_access access = *util_dynarray_element(&batch->bos, pan_bo_access, handle);
      const unsigned allowed = PAN_BO_ACCESS_READ | PAN_BO_ACCESS_SHARED |
         PAN_BO_ACCESS_VERTEX_TILER | PAN_BO_ACCESS_FRAGMENT |
         PAN_BO_ACCESS_VERTEX_INPUT | PAN_BO_ACCESS_FRAGMENT_PIXELS;
      if (access & ~allowed) return lancelot_clear_reject(ctx, LM_CLEAR_ALIAS);
      readers |= (access & PAN_BO_ACCESS_READ) != 0;
   }
   if (!readers) return lancelot_clear_reject(ctx, LM_CLEAR_NO_READERS);

   uint64_t bytes = panfrost_bo_size(old);
   const uint64_t budget = 32 * 1024 * 1024;
   if (!bytes || bytes > budget || ctx->lancelot_clear_rename.pending_bytes > budget - bytes)
      return lancelot_clear_reject(ctx, LM_CLEAR_BUDGET);
   struct panfrost_bo *next = panfrost_bo_create(dev, bytes,
      old->flags | PAN_BO_DELAY_MMAP, old->label);
   if (!next) {
      ctx->lancelot_clear_rename.oom++;
      return lancelot_clear_reject(ctx, LM_CLEAR_OOM);
   }

   backup->resource = r;
   backup->old = old;
   backup->base = r->image.data.base;
   backup->bytes = bytes;
   backup->crc = r->valid.crc;
   memcpy(backup->data, r->valid.data, sizeof(backup->data));
   r->bo = next;
   r->image.data.base = next->ptr.gpu;
   memset(&r->valid, 0, sizeof(r->valid));
   panfrost_dirty_state_all(ctx);
   /* Hold the resource's old reference until batch creation has succeeded. */
   ctx->lancelot_clear_rename.pending_bytes += bytes;
   return bytes;
}

static void
lancelot_finish_clear_rename(struct panfrost_context *ctx,
                             struct lancelot_clear_backup *backup, bool commit)
{
   if (!backup->old) return;
   if (commit) {
      panfrost_bo_unreference(backup->old);
   } else {
      struct panfrost_resource *r = backup->resource;
      struct panfrost_bo *next = r->bo;
      r->bo = backup->old;
      r->image.data.base = backup->base;
      r->valid.crc = backup->crc;
      memcpy(r->valid.data, backup->data, sizeof(backup->data));
      ctx->lancelot_clear_rename.pending_bytes -= backup->bytes;
      panfrost_dirty_state_all(ctx);
      panfrost_bo_unreference(next);
   }
   backup->old = NULL;
}

static void
lancelot_clear_rename_record(struct panfrost_context *ctx, uint64_t bytes)
{
   if (!(pan_device(ctx->base.screen)->debug & PAN_DBG_PERF)) return;
   ctx->lancelot_clear_rename.renamed += bytes != 0;
   ctx->lancelot_clear_rename.bytes += bytes;
   uint64_t now = lancelot_diag_ns();
   if (now - ctx->lancelot_clear_rename.last_ns < UINT64_C(1000000000)) return;
   ctx->lancelot_clear_rename.last_ns = now;
   fprintf(stderr, "lancelot-clear-rename: ctx=%llx at=%llu attempts=%llu renamed=%llu bytes=%llu pending=%llu oom=%llu cumulative=1 v=2 reject_frame=%llu reject_resource=%llu reject_modifier=%llu reject_writer=%llu reject_alias=%llu reject_no_readers=%llu reject_budget=%llu reject_oom=%llu\\n",
      (unsigned long long)(uintptr_t)ctx, (unsigned long long)(now / 1000),
      (unsigned long long)ctx->lancelot_clear_rename.attempts,
      (unsigned long long)ctx->lancelot_clear_rename.renamed,
      (unsigned long long)ctx->lancelot_clear_rename.bytes,
      (unsigned long long)ctx->lancelot_clear_rename.pending_bytes,
      (unsigned long long)ctx->lancelot_clear_rename.oom,
      (unsigned long long)ctx->lancelot_clear_rename.rejected[LM_CLEAR_FRAME],
      (unsigned long long)ctx->lancelot_clear_rename.rejected[LM_CLEAR_RESOURCE],
      (unsigned long long)ctx->lancelot_clear_rename.rejected[LM_CLEAR_MODIFIER],
      (unsigned long long)ctx->lancelot_clear_rename.rejected[LM_CLEAR_WRITER],
      (unsigned long long)ctx->lancelot_clear_rename.rejected[LM_CLEAR_ALIAS],
      (unsigned long long)ctx->lancelot_clear_rename.rejected[LM_CLEAR_NO_READERS],
      (unsigned long long)ctx->lancelot_clear_rename.rejected[LM_CLEAR_BUDGET],
       (unsigned long long)ctx->lancelot_clear_rename.rejected[LM_CLEAR_OOM]);
   fprintf(stderr, "lancelot-clear-resource: v=1 ctx=%llx at=%llu shape=%llu shared=%llu resource_flags=%llu surface=%llu damage=%llu mapped=%llu bo_flags=%llu offset=%llu sample_va=%llx sample_flags=%llx sample_bind=%llx sample_mask=%llx counts=overlapping_cumulative identity=sample_not_generation\\n",
       (unsigned long long)(uintptr_t)ctx, (unsigned long long)(now / 1000),
       (unsigned long long)ctx->lancelot_clear_rename.resource_rejected[0],
       (unsigned long long)ctx->lancelot_clear_rename.resource_rejected[1],
       (unsigned long long)ctx->lancelot_clear_rename.resource_rejected[2],
       (unsigned long long)ctx->lancelot_clear_rename.resource_rejected[3],
       (unsigned long long)ctx->lancelot_clear_rename.resource_rejected[4],
       (unsigned long long)ctx->lancelot_clear_rename.resource_rejected[5],
       (unsigned long long)ctx->lancelot_clear_rename.resource_rejected[6],
       (unsigned long long)ctx->lancelot_clear_rename.resource_rejected[7],
       (unsigned long long)ctx->lancelot_clear_rename.sample_va,
       (unsigned long long)ctx->lancelot_clear_rename.sample_flags,
       (unsigned long long)ctx->lancelot_clear_rename.sample_bind,
       (unsigned long long)ctx->lancelot_clear_rename.sample_mask);
}
#endif

static void
panfrost_clear(''')
change(Path('src/gallium/drivers/panfrost/pan_context.c'),
       '''   struct panfrost_context *ctx = pan_context(pipe);
   struct panfrost_batch *batch = panfrost_get_batch_for_fbo(ctx);''',
       '''   struct panfrost_context *ctx = pan_context(pipe);
#ifdef __APPLE__
   struct lancelot_clear_backup backup = {0};
   uint64_t renamed = lancelot_rename_clear_target(ctx, buffers, scissor_state, &backup);
#endif
   struct panfrost_batch *batch = panfrost_get_batch_for_fbo(ctx);''')
change(Path('src/gallium/drivers/panfrost/pan_context.c'),
       '''   /* At the start of the batch, we can clear for free */''',
       '''#ifdef __APPLE__
   lancelot_finish_clear_rename(ctx, &backup, true);
   lancelot_clear_rename_record(ctx, renamed);
#endif
   /* At the start of the batch, we can clear for free */''')
change(Path('src/gallium/drivers/panfrost/pan_context.c'),
       '''   struct panfrost_batch *batch = panfrost_get_batch_for_fbo(ctx);
   if (!batch)
      return;

#ifdef __APPLE__
   lancelot_finish_clear_rename''',
       '''   struct panfrost_batch *batch = panfrost_get_batch_for_fbo(ctx);
#ifdef __APPLE__
   if (!batch && backup.old) {
      /* The extra BO may have exhausted descriptor-pool space. Roll back
       * before retrying the original flush/readers/clear allocation path. */
      lancelot_finish_clear_rename(ctx, &backup, false);
      renamed = 0;
      ctx->lancelot_clear_rename.oom++;
      lancelot_clear_reject(ctx, LM_CLEAR_OOM);
      batch = panfrost_get_batch_for_fbo(ctx);
   }
#endif
   if (!batch) return;

#ifdef __APPLE__
   lancelot_finish_clear_rename''')
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
       '''   lancelot_flush_lookup(ctx, missing, idle);
#endif
}''',
       '''   lancelot_flush_lookup(ctx, missing, idle);
   /* All active batches above were drained, including old-version readers. */
   ctx->lancelot_clear_rename.pending_bytes = 0;
#endif
}''')
# 2026-11-02: sampler payload/backing identity is a transaction. Build into a
# temporary view, retain its actual unowned pool, then publish/release old state.
# Owned synthetic image payloads already belong to the batch; no CSO retain.
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       'static void panfrost_update_sampler_view(', 'static bool panfrost_update_sampler_view(')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       'static void\npanfrost_create_sampler_view_bo(', 'static bool\nlancelot_build_sampler_view(')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '''      mesa_loge("panfrost_create_sampler_view_bo failed");
      return;''', '''      mesa_loge("panfrost_create_sampler_view_bo failed");
      return false;''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '   so->state = panfrost_pool_take_ref(&ctx->descs, payload.gpu);',
       '''   so->state = pool->owned ? (struct panfrost_pool_ref){.gpu = payload.gpu} :
               panfrost_pool_take_ref(pool, payload.gpu);''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '''   GENX(panfrost_new_texture)(&iview, tex, &payload);
}

static void
panfrost_update_sampler_view''',
       '''   GENX(panfrost_new_texture)(&iview, tex, &payload);
   return true;
}

static bool
panfrost_create_sampler_view_bo(struct panfrost_sampler_view *view,
                               struct pipe_context *pctx,
                               struct pipe_resource *texture)
{
   /* Copy immutable sampling parameters only; do not even read the concurrently
    * retained base.reference while preparing a descriptor transaction. */
   struct panfrost_sampler_view next = {
      .base = {.texture=view->base.texture, .format=view->base.format,
         .target=view->base.target, .u=view->base.u,
         .swizzle_r=view->base.swizzle_r, .swizzle_g=view->base.swizzle_g,
         .swizzle_b=view->base.swizzle_b, .swizzle_a=view->base.swizzle_a,
         .astc_decode_format=view->base.astc_decode_format},
      .pool=view->pool,
   };
   if (!lancelot_build_sampler_view(&next, pctx, texture)) return false;
   struct panfrost_bo *old = view->state.bo;
   /* base.reference may be retained by another context while allocation ran.
    * Publish only the mutable descriptor state, never overwrite that refcount. */
   view->state = next.state;
   view->texture_bo = next.texture_bo;
   view->texture_size = next.texture_size;
   view->modifier = next.modifier;
   view->bifrost_descriptor = next.bifrost_descriptor;
   panfrost_bo_unreference(old);
   return true;
}

static bool
panfrost_update_sampler_view''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '''   if (view->texture_bo != rsrc->image.data.base ||
       view->texture_size != rsrc->image.layout.data_size ||
       view->modifier != rsrc->image.layout.modifier) {
      panfrost_bo_unreference(view->state.bo);
      panfrost_create_sampler_view_bo(view, pctx, &rsrc->base);
   }
}''',
       '''   if (!view->state.gpu || view->texture_bo != rsrc->image.data.base ||
       view->texture_size != rsrc->image.layout.data_size ||
       view->modifier != rsrc->image.layout.modifier)
      return panfrost_create_sampler_view_bo(view, pctx, &rsrc->base);
   return true;
}''')
p=src/'src/gallium/drivers/panfrost/pan_cmdstream.c';data=p.read_text()
needle='      panfrost_update_sampler_view(view, &ctx->base);'
assert data.count(needle)==2
data=data.replace(needle,'      if (!panfrost_update_sampler_view(view, &ctx->base)) return 0;')
p.write_text(data)
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '      panfrost_update_sampler_view(&view, &ctx->base);',
       '      if (!panfrost_update_sampler_view(&view, &ctx->base)) return 0;')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '''   struct mali_texture_packed *out = (struct mali_texture_packed *)T.cpu;

   for (int i = 0; i < last_bit; ++i)''',
       '''   if (!T.cpu) return 0;
   struct mali_texture_packed *out = (struct mali_texture_packed *)T.cpu;

   for (int i = 0; i < last_bit; ++i)''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '   panfrost_create_sampler_view_bo(so, pctx, texture);',
       '''   if (!panfrost_create_sampler_view_bo(so, pctx, texture)) {
      pipe_resource_reference(&so->base.texture, NULL);
      ralloc_free(so);
      return NULL;
   }''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '''   struct panfrost_resource *ptexture = pan_resource(texture);

   pan_legalize_format''',
       '''   struct panfrost_resource *ptexture = pan_resource(texture);
   if (!so) return NULL;

   pan_legalize_format''')
# Failed descriptor emission must not launch a draw with a stale/zero table.
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       'static inline void\npanfrost_update_shader_state(', 'static inline bool\npanfrost_update_shader_state(')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '''      batch->textures[st] = panfrost_emit_texture_descriptors(batch, st);
   }''',
       '''      batch->textures[st] = panfrost_emit_texture_descriptors(batch, st);
      if (ss->info.texture_count && !batch->textures[st]) return false;
   }''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '''         ctx->image_mask[st] ? panfrost_emit_images(batch, st) : 0;
   }''',
       '''         ctx->image_mask[st] ? panfrost_emit_images(batch, st) : 0;
      if (ctx->image_mask[st] && !batch->images[st]) return false;
   }''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '''         panfrost_emit_image_attribs(batch, &batch->attrib_bufs[st], st);
   }
#endif
}''',
       '''         panfrost_emit_image_attribs(batch, &batch->attrib_bufs[st], st);
   }
#endif
   return true;
}''')
p=src/'src/gallium/drivers/panfrost/pan_cmdstream.c';data=p.read_text()
needle='''   panfrost_update_shader_state(batch, PIPE_SHADER_VERTEX);
   panfrost_update_shader_state(batch, PIPE_SHADER_FRAGMENT);'''
assert data.count(needle)==2
data=data.replace(needle,'''   if (!panfrost_update_shader_state(batch, PIPE_SHADER_VERTEX) ||
       !panfrost_update_shader_state(batch, PIPE_SHADER_FRAGMENT)) return;''')
data=data.replace('   panfrost_update_shader_state(batch, PIPE_SHADER_COMPUTE);',
                  '   if (!panfrost_update_shader_state(batch, PIPE_SHADER_COMPUTE)) return;')
p.write_text(data)

# texture_count is an upper extent, NOT proof every lower slot is fetched.
# Preserve the FINAL compiler's usage mask, including indirect array accesses.
# Keep the exact descriptor extent/indices; only proven-unused slots become null.
change(Path('src/panfrost/util/pan_ir.h'), '   unsigned texture_count;',
       '''   unsigned texture_count;
#ifdef __APPLE__
   uint64_t lancelot_texture_mask;
   bool lancelot_texture_mask_valid;
#endif''')
change(Path('src/panfrost/lib/pan_shader.c'),
       '''      BITSET_LAST_BIT(s->info.textures_used);''',
       '''      BITSET_LAST_BIT(s->info.textures_used);
#ifdef __APPLE__
   info->lancelot_texture_mask = 0;
   info->lancelot_texture_mask_valid = info->texture_count <= 64;
   for (unsigned i=0;i<MIN2(info->texture_count,64);i++)
      if (BITSET_TEST(s->info.textures_used,i)) info->lancelot_texture_mask |= UINT64_C(1)<<i;
#endif''')
# A new cache-key schema prevents reading the former shader_info blob layout.
change(Path('src/gallium/drivers/panfrost/pan_disk_cache.c'),
       '   uint8_t data[sizeof(uncompiled->nir_sha1) + sizeof(*shader_key)];',
       '''   uint8_t data[sizeof(uncompiled->nir_sha1) + sizeof(*shader_key)
#ifdef __APPLE__
      + sizeof(uint32_t)
#endif
   ];''')
change(Path('src/gallium/drivers/panfrost/pan_disk_cache.c'),
       '''   disk_cache_compute_key(cache, data, sizeof(data), cache_key);''',
       '''#ifdef __APPLE__
   const uint32_t schema = 0x674d534bu; /* Lancelot final texture-mask info v1 */
   memcpy(data+sizeof(data)-sizeof(schema),&schema,sizeof(schema));
#endif
   disk_cache_compute_key(cache, data, sizeof(data), cache_key);''')
change(Path('src/gallium/drivers/panfrost/pan_context.h'),
       '   bool lancelot_report_batch;',
       '   bool lancelot_report_batch;\n   uint64_t lancelot_texture_holes, lancelot_blend_calls, lancelot_blend_last;')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '''static uint64_t
panfrost_emit_texture_descriptors(''',
       '''static bool
lancelot_texture_slot_unused(struct panfrost_context *ctx, enum pipe_shader_type st, unsigned slot)
{
#ifdef __APPLE__
   const struct pan_shader_info *info = &ctx->prog[st]->info;
   bool unused = info->lancelot_texture_mask_valid && slot < 64 &&
                 !(info->lancelot_texture_mask & (UINT64_C(1)<<slot));
   if (unused && ctx->sampler_views[st][slot]) ctx->lancelot_texture_holes++;
   return unused;
#else
   return false;
#endif
}

static uint64_t
panfrost_emit_texture_descriptors(''')
p=src/'src/gallium/drivers/panfrost/pan_cmdstream.c';data=p.read_text()
needle='''      if (!view) {
         panfrost_emit_null_texture(&out[i]);'''
assert data.count(needle)==1
data=data.replace(needle,'''      if (!view || lancelot_texture_slot_unused(ctx, stage, i)) {
         panfrost_emit_null_texture(&out[i]);''')
needle='''      if (!view) {
         trampolines[i] = 0;'''
assert data.count(needle)==1
data=data.replace(needle,'''      if (!view || lancelot_texture_slot_unused(ctx, stage, i)) {
         trampolines[i] = 0;''')
data=data.replace('slots=%llu views=%llu bytes=%llu\\n",', 'slots=%llu views=%llu bytes=%llu holes_cumulative=%llu\\n",')
data=data.replace('''              (unsigned long long)p->omitted_bytes);''',
                  '''              (unsigned long long)p->omitted_bytes,
              (unsigned long long)ctx->lancelot_texture_holes);''')
p.write_text(data)
# Retain blend results, replace per-use textual warnings by an aggregate.
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '''   if (shader_bo)
      perf_debug(batch->ctx, "Blend shader use");''',
       '''   if (shader_bo) {
#ifdef __APPLE__
      struct panfrost_context *ctx=batch->ctx;
      if (pan_device(ctx->base.screen)->debug & PAN_DBG_PERF) {
         ctx->lancelot_blend_calls++;
         uint64_t now=lancelot_diag_ns();
         if (now-ctx->lancelot_blend_last>=UINT64_C(1000000000)) {
            ctx->lancelot_blend_last=now;
            fprintf(stderr,"lancelot-blend-use: ctx=%llx at=%llu calls=%llu cumulative=1\\n",
               (unsigned long long)(uintptr_t)ctx,(unsigned long long)(now/1000),
               (unsigned long long)ctx->lancelot_blend_calls);
         }
      }
#else
      perf_debug(batch->ctx, "Blend shader use");
#endif
   }''')
# G52 blend helpers are immutable once a draw references them. Pack successive
# uploads into batch-owned EXECUTE slabs, not one native 16 KiB BO per RSD.
# This is storage coalescing, NOT job/dependency coalescing: old slabs stay in
# batch->bos until the existing completion/cleanup path releases them. Never
# read WC shader backing to deduplicate, nor cache mutable compiler variants.
change(Path('src/gallium/drivers/panfrost/pan_job.h'),
       '   uint64_t lancelot_vtq, lancelot_fsq;',
       '''   uint64_t lancelot_vtq, lancelot_fsq;
   struct panfrost_bo *lancelot_blend_bo; /* borrowed from batch->bos */
   unsigned lancelot_blend_offset, lancelot_blend_uploads, lancelot_blend_slabs;
   uint64_t lancelot_blend_bytes;''')
change(Path('src/gallium/drivers/panfrost/pan_context.c'),
       '/* Create a final blend given the context */',
       '''#ifdef __APPLE__
static bool
lancelot_upload_blend(struct panfrost_batch *batch,
                       const struct pan_blend_shader_variant *shader,
                       struct panfrost_bo **bo, uint64_t *address)
{
   /* The compiler-cache mutex protects shader->binary until memcpy finishes.
    * Allocation never enters that cache. Only the active slab cursor changes;
    * no descriptor or instruction referenced by an earlier draw is patched. */
   const unsigned slab_size = 16384;
   size_t size = shader->binary.size;
   if (!size || size > 4096 || !shader->binary.data)
      return false;

   struct panfrost_bo *slab = batch->lancelot_blend_bo;
   unsigned offset = ALIGN_POT(batch->lancelot_blend_offset, 64);
   if (!slab || offset > slab_size || size > slab_size - offset) {
      slab = panfrost_batch_create_bo(batch, slab_size, PAN_BO_EXECUTE,
                                     PIPE_SHADER_FRAGMENT, "Blend shader slab");
      if (!slab || !slab->ptr.cpu || panfrost_bo_size(slab) < slab_size)
         return false;
      offset = 0;
      batch->lancelot_blend_bo = slab;
      batch->lancelot_blend_slabs++;
   }

   memcpy(slab->ptr.cpu + offset, shader->binary.data, size);
   batch->lancelot_blend_offset = offset + size;
   batch->lancelot_blend_uploads++;
   batch->lancelot_blend_bytes += size;
   *bo = slab;
   *address = (slab->ptr.gpu + offset) | shader->first_tag;
   return true;
}
#endif

/* Create a final blend given the context */''')
# A zero address means legitimate fixed-function blend, never allocation
# failure. Make that distinction explicit and propagate failure to both draw
# paths before a new job is launched. Public GL/LMAL ABI is unchanged.
change(Path('src/gallium/drivers/panfrost/pan_blend_cso.h'),
       '''uint64_t panfrost_get_blend(struct panfrost_batch *batch, unsigned rt,
                            struct panfrost_bo **bo, unsigned *shader_offset);''',
       '''bool panfrost_get_blend(struct panfrost_batch *batch, unsigned rt,
                         struct panfrost_bo **bo, unsigned *shader_offset,
                         uint64_t *address);''')
p=src/'src/gallium/drivers/panfrost/pan_context.c';data=p.read_text()
start=data.index('uint64_t\npanfrost_get_blend(')
end=data.index('\nstatic void\npanfrost_bind_rasterizer_state',start)
body=data[start:end]
assert body.count('return 0;')==4
body=body.replace('uint64_t\npanfrost_get_blend(', 'bool\npanfrost_get_blend(',1)
body=body.replace('struct panfrost_bo **bo, unsigned *shader_offset)',
                  'struct panfrost_bo **bo, unsigned *shader_offset, uint64_t *address)',1)
body=body.replace('   /* Use fixed-function if the equation permits,',
                  '''   *address = 0;
   bool packed = false;
#ifdef __APPLE__
   packed = dev->arch == 7 && panfrost_device_gpu_id(dev) == 0x7402;
#endif

   /* Use fixed-function if the equation permits,''',1)
body=body.replace('return 0;','return true;',3)
body=body.replace('   if (!(*bo)) {','   if (!packed && !(*bo)) {',1)
body=body.replace('         return 0;','         return false;',1)
old='''   /* Size check and upload */
   unsigned offset = *shader_offset;
   assert((offset + shader->binary.size) < 4096);
   memcpy((*bo)->ptr.cpu + offset, shader->binary.data, shader->binary.size);
   *shader_offset += shader->binary.size;
   pthread_mutex_unlock(&dev->blend_shaders.lock);

   return ((*bo)->ptr.gpu + offset) | shader->first_tag;'''
new='''   bool ok = false;
   if (!shader || !shader->binary.size || !shader->binary.data)
      goto out;
#ifdef __APPLE__
   if (packed) {
      ok = lancelot_upload_blend(batch, shader, bo, address);
      goto out;
   }
#endif
   /* Keep the legacy layout, but enforce its bound in release builds too. */
   unsigned offset = *shader_offset;
   if (offset >= 4096 || shader->binary.size >= 4096 - offset || !(*bo)->ptr.cpu)
      goto out;
   memcpy((*bo)->ptr.cpu + offset, shader->binary.data, shader->binary.size);
   *shader_offset += shader->binary.size;
   *address = ((*bo)->ptr.gpu + offset) | shader->first_tag;
   ok = true;
out:
   pthread_mutex_unlock(&dev->blend_shaders.lock);
   if (!ok) mesa_loge("failed to upload blend-shader");
   return ok;'''
assert body.count(old)==1;body=body.replace(old,new,1)
data=data[:start]+body+data[end:];p.write_text(data)
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       'static void\npanfrost_get_blend_shaders(', 'static bool\npanfrost_get_blend_shaders(')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '''         blend_shaders[c] =
            panfrost_get_blend(batch, c, &shader_bo, &shader_offset);''',
       '''         if (!panfrost_get_blend(batch, c, &shader_bo, &shader_offset,
                                 &blend_shaders[c]))
            return false;''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '''      perf_debug(batch->ctx, "Blend shader use");
#endif
   }
}''',
       '''      perf_debug(batch->ctx, "Blend shader use");
#endif
   }
   return true;
}''')
p=src/'src/gallium/drivers/panfrost/pan_cmdstream.c';data=p.read_text()
needle='   panfrost_get_blend_shaders(batch, blend_shaders);'
assert data.count(needle)==2
data=data.replace(needle,'   if (!panfrost_get_blend_shaders(batch, blend_shaders)) return 0;')
p.write_text(data)
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '      batch->rsd[st] = panfrost_emit_frag_shader_meta(batch);',
       '''      batch->rsd[st] = panfrost_emit_frag_shader_meta(batch);
      if (!batch->rsd[st]) return false;''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       'static inline void\npanfrost_update_state_3d(', 'static inline bool\npanfrost_update_state_3d(')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '''   if (dirty & PAN_DIRTY_BLEND)
      batch->blend = panfrost_emit_blend_valhall(batch);''',
       '''   if (dirty & PAN_DIRTY_BLEND) {
      batch->blend = panfrost_emit_blend_valhall(batch);
      if (!batch->blend) return false;
   }''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '''#endif
}

static void
panfrost_launch_xfb(''',
       '''#endif
   return true;
}

static void
panfrost_launch_xfb(''')
p=src/'src/gallium/drivers/panfrost/pan_cmdstream.c';data=p.read_text()
needle='   panfrost_update_state_3d(batch);'
assert data.count(needle)==2
data=data.replace(needle,'   if (!panfrost_update_state_3d(batch)) return;')
p.write_text(data)
# Pair the existing sampled shader/job record with the exact render area and
# preloads. These are configuration/upper-bound facts, not hardware pixel or
# shader timings. Do not introduce a per-draw log or any CPU pixel readback.
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
       '#include <stdio.h>\n#include <stdlib.h>',
       '#include <stdio.h>\n#include <stdlib.h>\n#include <unistd.h>')
change(Path('src/gallium/drivers/panfrost/pan_job.c'),
       '''   ret = screen->vtbl.submit_batch(batch, &fb);
   if (ret)''',
       '''   ret = screen->vtbl.submit_batch(batch, &fb);
#ifdef __APPLE__
   if (batch->ctx->lancelot_report_batch) {
      unsigned preloads = 0;
      for (unsigned i = 0; i < fb.rt_count; i++)
         if (fb.rts[i].preload) preloads |= 1u << i;
      fprintf(stderr, "lancelot-render: v=1 pid=%d ctx=%llx seq=%u vtq=%llu fsq=%llu w=%u h=%u x0=%u y0=%u x1=%u y1=%u samples=%u force_samples=%u rts=%u clear=%x draw=%x read=%x resolve=%x preloads=%x preload_z=%u preload_s=%u blend_uploads=%u blend_slabs=%u blend_bytes=%llu scope=config_not_HW\\n",
              getpid(), (unsigned long long)(uintptr_t)ctx, batch->seqnum,
              (unsigned long long)batch->lancelot_vtq,
              (unsigned long long)batch->lancelot_fsq, fb.width, fb.height,
              fb.extent.minx, fb.extent.miny, fb.extent.maxx, fb.extent.maxy,
              fb.nr_samples, fb.force_samples, fb.rt_count,
              batch->clear, batch->draws, batch->read, batch->resolve, preloads,
              fb.zs.preload.z, fb.zs.preload.s, batch->lancelot_blend_uploads,
              batch->lancelot_blend_slabs,
              (unsigned long long)batch->lancelot_blend_bytes);
   }
#endif
   if (ret)''')
# Preload is real fragment work on G52, not a counted Gallium draw. Also repair
# its concrete OOM contract: the RSD cache formerly returned with its mutex
# held, and callers could publish a null preload state. Record failure in the
# transient framebuffer description and stop BEFORE either GPU chain submits.
# No preload is removed and no kernel/wire API is added.
change(Path('src/panfrost/lib/pan_desc.h'), 'struct pan_fb_info {',
       '''struct pan_fb_info {
#ifdef __APPLE__
   bool lancelot_preload_failed;
#endif''')
change(Path('src/gallium/drivers/panfrost/pan_fb_preload.c'),
       '''   rsd = rzalloc(cache->rsds.rsds, struct pan_preload_rsd_data);
   rsd->key = rsd_key;''',
       '''   rsd = rzalloc(cache->rsds.rsds, struct pan_preload_rsd_data);
   if (!rsd) goto out;
   rsd->key = rsd_key;''')
change(Path('src/gallium/drivers/panfrost/pan_fb_preload.c'),
       '''   if (!rsd_ptr.cpu)
      return 0;''',
       '''   if (!rsd_ptr.cpu) {
      ralloc_free(rsd);
      rsd = NULL;
      goto out;
   }''')
change(Path('src/gallium/drivers/panfrost/pan_fb_preload.c'),
       '''   return rsd->address;''', '   return rsd ? rsd->address : 0;')
change(Path('src/gallium/drivers/panfrost/pan_fb_preload.c'),
       '''   shader = rzalloc(cache->shaders.preload, struct pan_preload_shader_data);

   nir_shader_gather_info''',
       '''   shader = rzalloc(cache->shaders.preload, struct pan_preload_shader_data);
   if (!shader) {
      util_dynarray_fini(&binary);
      ralloc_free(b.shader);
      goto out;
   }

   nir_shader_gather_info''')
change(Path('src/gallium/drivers/panfrost/pan_fb_preload.c'),
       '''   ralloc_free(b.shader);

#if PAN_ARCH >= 6''',
       '''   ralloc_free(b.shader);
   if (!shader->address) {
      ralloc_free(shader);
      shader = NULL;
      goto out;
   }

#if PAN_ARCH >= 6''')
change(Path('src/gallium/drivers/panfrost/pan_fb_preload.c'),
       '''   const struct pan_preload_shader_data *preload_shader =
      pan_preload_get_shader(cache, &preload_key);

#if PAN_ARCH <= 5''',
       '''   const struct pan_preload_shader_data *preload_shader =
      pan_preload_get_shader(cache, &preload_key);
   if (!preload_shader) {
      ralloc_free(rsd);
      rsd = NULL;
      goto out;
   }

#if PAN_ARCH <= 5''')
change(Path('src/gallium/drivers/panfrost/pan_fb_preload.c'),
       '''   /* Tiles updated by preload shaders are still considered clean''',
       '''#ifdef __APPLE__
   if (!textures || !samplers || !varyings || !varying_buffers) {
      fb->lancelot_preload_failed = true;
      return;
   }
#endif

   /* Tiles updated by preload shaders are still considered clean''')
change(Path('src/gallium/drivers/panfrost/pan_fb_preload.c'),
       '''   pan_pack(out, DRAW, cfg) {
      uint16_t minx = 0, miny = 0, maxx, maxy;''',
       '''   uint64_t state = pan_preload_get_rsd(cache, &views);
#ifdef __APPLE__
   if (!state) {
      fb->lancelot_preload_failed = true;
      return;
   }
#endif
   pan_pack(out, DRAW, cfg) {
      uint16_t minx = 0, miny = 0, maxx, maxy;''')
change(Path('src/gallium/drivers/panfrost/pan_fb_preload.c'),
       '      cfg.state = pan_preload_get_rsd(cache, &views);', '      cfg.state = state;')
change(Path('src/gallium/drivers/panfrost/pan_fb_preload.c'),
       '''      cfg.viewport = pan_preload_emit_viewport(pool, minx, miny, maxx, maxy);

      cfg.varyings''',
       '''      cfg.viewport = pan_preload_emit_viewport(pool, minx, miny, maxx, maxy);
#ifdef __APPLE__
      if (!cfg.viewport) {
         fb->lancelot_preload_failed = true;
         return;
      }
#endif

      cfg.varyings''')
change(Path('src/gallium/drivers/panfrost/pan_fb_preload.c'),
       '''      mesa_loge("pan_preload_fb_alloc_pre_post_dcds failed");
      return;''',
       '''      mesa_loge("pan_preload_fb_alloc_pre_post_dcds failed");
#ifdef __APPLE__
      fb->lancelot_preload_failed = true;
#endif
      return;''')
change(Path('src/gallium/drivers/panfrost/pan_fb_preload.c'),
       '''                         always_write);
   if (zs)''',
       '''                         always_write);
#ifdef __APPLE__
   if (fb->lancelot_preload_failed) return;
#endif
   if (zs)''')
change(Path('src/gallium/drivers/panfrost/pan_fb_preload.c'),
       '''   coords = pan_pool_upload_aligned(pool, rect, sizeof(rect), 64);

   unsigned njobs''',
       '''   coords = pan_pool_upload_aligned(pool, rect, sizeof(rect), 64);
#ifdef __APPLE__
   if (!coords) {
      fb->lancelot_preload_failed = true;
      return 0;
   }
#endif

   unsigned njobs''')
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.c'),
       '''   JOBX(preload_fb)(batch, fb);
   init_polygon_list(batch);''',
       '''   JOBX(preload_fb)(batch, fb);
#ifdef __APPLE__
   if (fb->lancelot_preload_failed) {
      mesa_loge("framebuffer preload failed; refusing incomplete GPU batch");
      return ENOMEM;
   }
#endif
   init_polygon_list(batch);''')
# The alpha overdraw predicate must inspect enabled colour writes. The pinned
# upstream predicate was inverted, blessing alpha kill/store even when an
# active blend equation did not establish it. Preserve blending semantics;
# never use these flags as an excuse to drop destination reads or preloads.
change(Path('src/gallium/drivers/panfrost/pan_cmdstream.h'),
       '      bool enabled = ctx->pipe_framebuffer.cbufs[i] && !info.enabled;',
       '      bool enabled = ctx->pipe_framebuffer.cbufs[i] && info.enabled;')
# CPU lease prerequisites: never return a CPU pointer/query after a failed
# completion wait. Keep every real wait and the original shadow/copy semantics.
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
       '''   *out_transfer = NULL;
   return NULL;
}
#endif''',
       '''   *out_transfer = NULL;
   return NULL;
}
static bool
lancelot_transfer_wait(struct panfrost_bo *bo, int64_t timeout, bool readers,
                       struct panfrost_transfer *transfer,
                       struct pipe_transfer **out_transfer)
{
   if (panfrost_bo_wait(bo, timeout, readers)) return true;
   lancelot_transfer_fail(transfer, out_transfer);
   return false;
}
#endif''')
p=src/'src/gallium/drivers/panfrost/pan_resource.c';data=p.read_text()
a=data.index('static void *\npanfrost_ptr_map(');b=data.index('{',a)+1;depth=1
while depth:
    depth+=(data[b]=='{')-(data[b]=='}');b+=1
body=data[a:b]
waits=re.findall(r'(?m)^( +)panfrost_bo_wait\(([^;\n]+)\);$',body)
assert len(waits)==5, ('transfer wait count',len(waits))
body=re.sub(r'(?m)^( +)panfrost_bo_wait\(([^;\n]+)\);$',
    lambda m:m[1]+'''#ifdef __APPLE__
'''+m[1]+'''if (!lancelot_transfer_wait('''+m[2]+''', transfer, out_transfer))
'''+m[1]+'''   return NULL;
'''+m[1]+'''#else
'''+m[0]+'''
'''+m[1]+'''#endif''',body)
data=data[:a]+body+data[b:];p.write_text(data)
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
       '   panfrost_bo_wait(rsrc->bo, INT64_MAX, false);',
       '   if (!panfrost_bo_wait(rsrc->bo, INT64_MAX, false)) return;')
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
       '   panfrost_bo_wait(linear->bo, INT64_MAX, false);',
       '''   if (!panfrost_bo_wait(linear->bo, INT64_MAX, false)) {
      if (plinear) pipe_resource_reference(&plinear, NULL);
      return;
   }''')
change(Path('src/gallium/drivers/panfrost/pan_resource.c'),
       '   panfrost_bo_wait(metadata_bo, INT64_MAX, false);',
       '''   if (!panfrost_bo_wait(metadata_bo, INT64_MAX, false)) {
      panfrost_bo_unreference(metadata_bo);
      return;
   }''')
p=src/'src/gallium/drivers/panfrost/pan_context.c';data=p.read_text()
a=data.index('static bool\npanfrost_get_query_result(');b=data.index('\nstatic void',a)
body=data[a:b];needle='      panfrost_bo_wait(rsrc->bo, INT64_MAX, false);'
assert body.count(needle)==3
body=body.replace(needle,'      if (!panfrost_bo_wait(rsrc->bo, INT64_MAX, false)) return false;')
p.write_text(data[:a]+body+data[b:])
# Persistent unowned CSO/shader/preload pools must not append into a pending
# BO-granule CPU lease. Rotate backing on a nonblocking busy result. The old
# backing still has CSO/batch/kernel refs; allocation failure propagates NULL.
change(Path('src/gallium/drivers/panfrost/pan_mempool.c'),
       '''   unsigned offset = ALIGN_POT(pool->transient_offset, alignment);

#ifdef PAN_DBG_OVERFLOW''',
       '''   unsigned offset = ALIGN_POT(pool->transient_offset, alignment);
#ifdef __APPLE__
   if (!pool->owned && bo && !panfrost_bo_wait(bo, 0, true)) {
      bo = panfrost_pool_alloc_backing(pool,
         ALIGN_POT(MAX2(pool->base.slab_size, sz), 16384));
      if (!bo) return (struct panfrost_ptr){0};
      offset = 0;
   }
#endif

#ifdef PAN_DBG_OVERFLOW''')
# Optional synchronous pair, not the unvalidated asynchronous queue. All legacy
# formats/functions stay available. Negotiation happens before either JC runs;
# a failed pair is never retried as two legacy submissions.
change(Path('src/lancelot-drm/xf86drm.h'),
       'int lancelot_drm_submit_access(int, const struct drm_panfrost_submit *, const uint32_t *);',
       '''int lancelot_drm_submit_access(int, const struct drm_panfrost_submit *, const uint32_t *);
struct lancelot_sync_pair_result {
   uint64_t sequence[2];
   int32_t error[2];
   uint32_t attempted_mask, completed_mask;
};
int lancelot_drm_queued_transport(int);
int lancelot_drm_has_sync_pair(int);
int lancelot_drm_sync_pair_eligible(int, const struct drm_panfrost_submit *, const uint32_t *,
                                  const struct drm_panfrost_submit *, const uint32_t *);
int lancelot_drm_submit_pair_sync(int, const struct drm_panfrost_submit *, const uint32_t *,
                                const struct drm_panfrost_submit *, const uint32_t *,
                                struct lancelot_sync_pair_result *);''')
p=src/'src/lancelot-drm/lancelot_drm.c'
with p.open('a') as f:f.write('''
/* The connection ref spans negotiation, call and reply validation. */
int lancelot_drm_queued_transport(int fd) {
   struct device *d=get(fd);if(!d)return 0;
   int queued=(d->caps&LMAL_CAP_QUEUED_DRM)!=0;put(d);return queued;
}
int lancelot_drm_has_sync_pair(int fd) {
   struct device *d=get(fd); if(!d) return 0;
   const unsigned required=LMAL_CAP_SUBMIT_PAIR_SYNC|LMAL_CAP_CHECK_PAIR_SYNC;
   int supported=(d->caps&required)==required;put(d);return supported;
}
static int lancelot_pair_request(const struct drm_panfrost_submit *vt,const uint32_t *vt_ro,
                                const struct drm_panfrost_submit *fs,const uint32_t *fs_ro,
                                struct lmal_submit_pair_sync *q) {
   if(!vt||!fs||!vt_ro||!fs_ro||vt->requirements!=0||fs->requirements!=PANFROST_JD_REQ_FS||
      vt->out_sync||fs->in_sync_count||fs->in_syncs||
      !vt->jc||!fs->jc||(vt->jc&63u)||(fs->jc&63u)||
      !vt->bo_handle_count||vt->bo_handle_count>LMAL_SYNC_PAIR_STAGE_BOS||!vt->bo_handles||
      !fs->bo_handle_count||fs->bo_handle_count>LMAL_SYNC_PAIR_STAGE_BOS||!fs->bo_handles||
      vt->in_sync_count>LMAL_MAX_SUBMIT_SYNCS|| (!!vt->in_sync_count!=!!vt->in_syncs)) {
      errno=EINVAL;return -1;
   }
   *q=(struct lmal_submit_pair_sync){
      .stage={{.jc=vt->jc,.bo_count=vt->bo_handle_count,.bo_handles=vt->bo_handles,.bo_readonly=(uintptr_t)vt_ro},
              {.jc=fs->jc,.bo_count=fs->bo_handle_count,.bo_handles=fs->bo_handles,.bo_readonly=(uintptr_t)fs_ro}},
      .timeout_ns=LMAL_MAX_WAIT_NS,.in_sync_count=vt->in_sync_count,.in_syncs=vt->in_syncs,.out_sync=fs->out_sync};
   return 0;
}
/* 1: trusted snapshot eligible, 0: specifically missing lease, -1: error.
 * No token is cached. Never turn a malformed/failed query into fallback. */
int lancelot_drm_sync_pair_eligible(int fd,const struct drm_panfrost_submit *vt,const uint32_t *vt_ro,
                                  const struct drm_panfrost_submit *fs,const uint32_t *fs_ro) {
   struct lmal_check_pair_sync q={0};
   if(lancelot_pair_request(vt,vt_ro,fs,fs_ro,&q.submit))return -1;
   struct device *d=get(fd);if(!d)return -1;
   const unsigned required=LMAL_CAP_SUBMIT_PAIR_SYNC|LMAL_CAP_CHECK_PAIR_SYNC;
   if((d->caps&required)!=required){put(d);errno=ENOTSUP;return -1;}
   int ret=call(d,LMAL_SELECTOR_CHECK_PAIR_SYNC,&q,sizeof(q));
   put(d);if(ret)return -1;
   const struct lmal_pair_admission *a=&q.admission;
   bool untouched=!q.submit.attempted_mask&&!q.submit.completed_mask&&!q.submit.sequence[0]&&!q.submit.sequence[1]&&
      !q.submit.job_status[0]&&!q.submit.job_status[1]&&!q.submit.stage_error[0]&&!q.submit.stage_error[1];
   if(untouched&&!a->reserved&&a->eligible==1&&a->reason==LMAL_PAIR_REASON_NONE&&
      a->stage_index==LMAL_PAIR_STAGE_NONE&&!a->bo_handle&&!a->surface_id&&!a->bo_flags&&!a->bo_state)return 1;
   const unsigned valid=LMAL_PAIR_BO_MEMORY|LMAL_PAIR_BO_MAPPED|LMAL_PAIR_BO_PREPARED;
   if(untouched&&!a->reserved&&!a->eligible&&a->reason==LMAL_PAIR_REASON_MISSING_LEASE&&
      a->stage_index<2&&a->bo_handle&&a->bo_state==valid)return 0;
   errno=EPROTO;return -1;
}
int lancelot_drm_submit_pair_sync(int fd,const struct drm_panfrost_submit *vt,
                                const uint32_t *vt_ro,const struct drm_panfrost_submit *fs,
                                const uint32_t *fs_ro,struct lancelot_sync_pair_result *out) {
   last_submit_sequence=0;
   if(!out){errno=EINVAL;return submit_result(-1,0);}memset(out,0,sizeof(*out));
   struct lmal_submit_pair_sync q={0};
   if(lancelot_pair_request(vt,vt_ro,fs,fs_ro,&q))return submit_result(-1,0);
   struct device *d=get(fd);if(!d)return submit_result(-1,0);
   if(!(d->caps&LMAL_CAP_SUBMIT_PAIR_SYNC)){put(d);errno=ENOTSUP;return submit_result(-1,0);}
   int ret=call(d,LMAL_SELECTOR_SUBMIT_PAIR_SYNC,&q,sizeof(q));
   out->sequence[0]=q.sequence[0];out->sequence[1]=q.sequence[1];
   out->error[0]=q.stage_error[0];out->error[1]=q.stage_error[1];
   out->attempted_mask=q.attempted_mask;out->completed_mask=q.completed_mask;
   if((q.attempted_mask&~3u)||(q.completed_mask&~q.attempted_mask)||q.attempted_mask==2u||q.completed_mask==2u||
      (!ret&&(q.attempted_mask!=3||q.completed_mask!=3||!q.sequence[0]||!q.sequence[1]||q.stage_error[0]||q.stage_error[1]))) {
      errno=EPROTO;ret=-1;
   }
   int saved_errno=errno;put(d);errno=saved_errno;
   return submit_result(ret,q.sequence[1]);
}
''')

# Derive the prepare-only helper from this generator's complete original JC
# list construction: the same trim/RO/unknown roles, pool BOs, tiler heap and
# sample-position backing. No hand-edited generated Mesa file is authoritative.
p=src/'src/gallium/drivers/panfrost/pan_jm.c';data=p.read_text()
needle='   const bool trim = PAN_ARCH == 7 &&'
assert data.count(needle)==1
data=data.replace(needle,'''   const bool trim =
      (!lancelot_drm_queued_transport(panfrost_device_fd(dev)) ||
       debug_get_bool_option("LANCELOT_STAGE_TRIM", false)) && PAN_ARCH == 7 &&''',1)
first=data.index('static int\njm_submit_jc(')
body=data.index('{',first)+1
finish=data.index('   if (ctx->is_noop)',body)
prepare=data[body:finish]
assert prepare.count('   uint32_t in_syncs[1];')==1 and prepare.count('   int ret;')==1
prepare=prepare.replace('   uint32_t in_syncs[1];\n','').replace('   int ret;\n','')
begin=prepare.index('   if (ctx->in_sync_fd >= 0) {')
end=prepare.index('   assert(bo_handles && bo_readonly);',begin)
prepare=prepare[:begin]+'''   if (ctx->in_sync_fd >= 0 || ctx->is_noop ||
       (dev->debug & (PAN_DBG_TRACE | PAN_DBG_SYNC | PAN_DBG_DUMP))) return EINVAL;
'''+prepare[end:]
prepare=prepare.replace('      struct panfrost_bo *bo = pan_lookup_bo(dev, i);',
                        '      struct panfrost_bo *bo = pan_lookup_bo(dev, i);\n      if (!bo) return EINVAL;')
prepare+='''   out->submit = submit;
   out->original=original_refs;out->kept=kept_refs;out->omitted=omitted_refs;out->bytes=omitted_bytes;
   return 0;
'''
helper='''#ifdef __APPLE__
struct lancelot_jm_prepared {
   struct drm_panfrost_submit submit;
   unsigned original,kept,omitted;
   uint64_t bytes;
};
static int
lancelot_prepare_jc(struct panfrost_batch *batch,uint64_t first_job_desc,uint32_t reqs,
                    uint32_t out_sync,uint32_t *bo_handles,uint32_t *bo_readonly,
                    struct lancelot_jm_prepared *out)
{'''+prepare+''' }
#endif

'''
data=data[:first]+helper+data[first:]
old='''      if (capacity > SIZE_MAX / (2 * sizeof(*bo_handles))) return ENOMEM;
      bo_handles = malloc(capacity * 2 * sizeof(*bo_handles));'''
assert data.count(old)==1
data=data.replace(old,'''      if (capacity > SIZE_MAX / (4 * sizeof(*bo_handles))) return ENOMEM;
      bo_handles = malloc(capacity * 4 * sizeof(*bo_handles));''',1)
anchor='''   if (has_draws) {
      ret = jm_submit_jc'''
assert data.count(anchor)==1
pair='''#ifdef __APPLE__
   size_t pair_capacity=(size_t)panfrost_pool_num_bos(&batch->pool)+
      panfrost_pool_num_bos(&batch->invisible_pool)+batch->num_bos+2;
   bool sync_pair=PAN_ARCH==7 && panfrost_device_gpu_id(dev)==0x7402 &&
      has_draws && has_frag && !reqs && !batch->ctx->is_noop && batch->ctx->in_sync_fd<0 &&
      !(dev->debug&(PAN_DBG_TRACE|PAN_DBG_SYNC|PAN_DBG_DUMP)) && !getenv("LANCELOT_READ_DUMP") &&
      pair_capacity<=LMAL_SYNC_PAIR_STAGE_BOS &&
      lancelot_drm_has_sync_pair(panfrost_device_fd(dev));
   if(sync_pair) {
      struct lancelot_jm_prepared prepared[2]={0};
      uint32_t *fs_handles=bo_handles+pair_capacity*2;
      uint32_t *fs_readonly=fs_handles+pair_capacity;
      ret=lancelot_prepare_jc(batch,batch->jm.jobs.vtc_jc.first_job,0,0,bo_handles,bo_readonly,&prepared[0]);
      if(!ret)ret=lancelot_prepare_jc(batch,batch->jm.jobs.frag,PANFROST_JD_REQ_FS,out_sync,
                                     fs_handles,fs_readonly,&prepared[1]);
      if(ret)goto done; /* before either chain: no fallback/retry needed */
      int eligible=lancelot_drm_sync_pair_eligible(panfrost_device_fd(dev),&prepared[0].submit,bo_readonly,
                                                 &prepared[1].submit,fs_readonly);
      if(eligible<0){ret=errno;goto done;}
      if(eligible) {
      struct lancelot_sync_pair_result result={0};
      int called=lancelot_drm_submit_pair_sync(panfrost_device_fd(dev),&prepared[0].submit,bo_readonly,
                                             &prepared[1].submit,fs_readonly,&result);
      ret=called?errno:0;
      batch->lancelot_vtq=result.sequence[0];batch->lancelot_fsq=result.sequence[1];
      for(unsigned i=0;i<2;i++)if(result.attempted_mask&(1u<<i))
         lancelot_stage_record(batch->ctx,i,prepared[i].original,prepared[i].kept,
                               prepared[i].omitted,prepared[i].bytes,result.error[i]);
      goto done; /* true complete/error reply, never retry possibly-run work */
      }
      /* Only a successful read-only check reporting missing lease selects the
       * unchanged legacy path below, BEFORE either GPU stage has executed. */
   }
#endif

'''
data=data.replace(anchor,pair+anchor,1)
# The pair's role/pool capacities are internal; the copied LMAL header supplies
# the bounded wire constants alongside the existing Darwin backend helpers.
data='#ifdef __APPLE__\n#include "../../../lancelot-drm/lancelot_mali_uapi.h"\n#endif\n'+data
p.write_text(data)
# Heavy UI diagnostics: cover the long tasks that the periodic sample can miss.
# Metadata only; no shader execution, image contents or rendering policy changes.
p=src/'src/gallium/drivers/panfrost/pan_context.h';data=p.read_text()
needle='   bool lancelot_report_batch;'
assert data.count(needle)==1
data=data.replace(needle,needle+'\n   uint64_t lancelot_heavy_report_ns;\n   unsigned lancelot_heavy_report_count;')
p.write_text(data)
p=src/'src/gallium/drivers/panfrost/pan_jm.c';data=p.read_text()
needle='   free(bo_handles);\n#ifdef __APPLE__\n   if (batch->ctx->lancelot_report_batch) {'
assert data.count(needle)==1
data=data.replace(needle,'''#ifdef __APPLE__
   if (dev->debug & PAN_DBG_PERF) {
      uint64_t now=lancelot_diag_ns();
      if(now>=lancelot_began && now-lancelot_began>=UINT64_C(20000000)) {
         if(now<batch->ctx->lancelot_heavy_report_ns ||
            now-batch->ctx->lancelot_heavy_report_ns>=UINT64_C(1000000000)) {
            batch->ctx->lancelot_heavy_report_ns=now;
            batch->ctx->lancelot_heavy_report_count=0;
         }
         if(batch->ctx->lancelot_heavy_report_count<32) {
            ++batch->ctx->lancelot_heavy_report_count;
            batch->ctx->lancelot_report_batch=true;
         }
      }
   }
#endif
'''+needle,1)
p.write_text(data)
p=src/'src/gallium/drivers/panfrost/pan_job.c';data=p.read_text()
needle='   if (batch->ctx->lancelot_report_batch) {\n      unsigned preloads = 0;'
assert data.count(needle)==1
data=data.replace(needle,'''   if (batch->ctx->lancelot_report_batch) {
      for(unsigned i=0;i<batch->key.nr_cbufs;i++) {
         const struct pipe_surface *surf=batch->key.cbufs[i];
         if(!surf || !surf->texture)continue;
         const struct panfrost_resource *r=pan_resource(surf->texture);
         fprintf(stderr,"lancelot-render-resource: pid=%d ctx=%llx seq=%u rt=%u format=%u flags=%x bind=%x modifier=%llx bo=%llx bo_flags=%x cpu_mapped=%u afbc_available=%u\\n",
             getpid(),(unsigned long long)(uintptr_t)ctx,batch->seqnum,i,
             (unsigned)surf->texture->format,surf->texture->flags,surf->texture->bind,
             (unsigned long long)r->image.layout.modifier,
             (unsigned long long)(r->bo?r->bo->ptr.gpu:0),r->bo?r->bo->flags:0,
             r->bo && r->bo->ptr.cpu,pan_device(ctx->base.screen)->has_afbc);
      }
      unsigned preloads = 0;''',1)
p.write_text(data)

# Record the actual clipped viewport emitted for each draw. This is only an
# upper bound on fragment coverage; no vertices or rendered pixels are read.
p=src/'src/gallium/drivers/panfrost/pan_job.h';data=p.read_text()
needle='   uint64_t lancelot_vtq, lancelot_fsq;'
assert data.count(needle)==1
data=data.replace(needle,needle+'\n   uint64_t lancelot_scissor_pixels;\n   bool lancelot_scissor_full;')
p.write_text(data)
p=src/'src/gallium/drivers/panfrost/pan_cmdstream.c';data=p.read_text()
needle='   panfrost_batch_union_scissor(batch, minx, miny, maxx, maxy);'
assert data.count(needle)==1
data=data.replace(needle,needle+'''
#ifdef __APPLE__
   batch->lancelot_scissor_pixels = maxx > minx && maxy > miny ?
      (uint64_t)(maxx-minx)*(uint64_t)(maxy-miny) : 0;
   batch->lancelot_scissor_full = minx == 0 && miny == 0 &&
      maxx == batch->key.width && maxy == batch->key.height;
#endif''')
needle='''         fsid ? &ctx->prog[PIPE_SHADER_FRAGMENT]->lancelot_meta : NULL);'''
assert data.count(needle)==2
data=data.replace(needle,needle+'''
      struct panfrost_compiled_shader *work_fs=ctx->prog[PIPE_SHADER_FRAGMENT];
      lm_shader_mix_record_work(&batch->lancelot_shader_mix,vsid,fsid,
         batch->lancelot_scissor_pixels,batch->lancelot_scissor_full,
         ctx->blend->load_dest_mask!=0,fsid && work_fs->info.fs.outputs_read!=0,
         fsid && pan_allow_forward_pixel_to_kill(ctx,work_fs),
         fsid && work_fs->info.fs.sidefx);''')
p.write_text(data)
p=src/'src/gallium/drivers/panfrost/pan_jm.c';data=p.read_text()
needle='      fprintf(stderr,"lancelot-shader-batch:'
assert data.count(needle)==1
data=data.replace(needle,'''      for(unsigned i=0;i<LM_SHADER_PAIRS;i++) {
         if(!m->pair[i].draws)continue;
         const struct lm_shader_work *w=&m->work[i];
         fprintf(stderr,"lancelot-draw-work: pid=%d ctx=%llx seq=%u pair=%u n=%u area=%llu full=%u blend=%u fetch=%u fpk=%u sidefx=%u scope=scissor_upper_bound\\n",
            getpid(),(unsigned long long)(uintptr_t)batch->ctx,batch->seqnum,i,m->pair[i].draws,
            (unsigned long long)w->scissor_pixels,w->full_framebuffer,w->blend_destination,
            w->framebuffer_fetch,w->forward_kill,w->side_effects);
      }
'''+needle,1)
p.write_text(data)
# P11: select slow metadata by completed native JM time. The snapshot owns no
# BO, shader or context pointers, and polling is nonblocking at the FIFO head.
p=src/'src/gallium/drivers/panfrost/pan_job.c';data=p.read_text()
needle='   ret = screen->vtbl.submit_batch(batch, &fb);'
assert data.count(needle)==1
data=data.replace(needle,needle+'''
#ifdef __APPLE__
   if (measured && (batch->lancelot_vtq || batch->lancelot_fsq)) {
      struct lm_completed_batch completed = {
         .ctx=(uintptr_t)ctx,.seq=batch->seqnum,.vtq=batch->lancelot_vtq,.fsq=batch->lancelot_fsq,
         .at_ns=begin_ns,.submit_end_ns=lancelot_diag_ns(),.draws=original_draws,.reason=reason,
         .w=fb.width,.h=fb.height,.x0=fb.extent.minx,.y0=fb.extent.miny,
         .x1=fb.extent.maxx,.y1=fb.extent.maxy,.samples=fb.nr_samples,.rts=fb.rt_count,
         .vtjc=batch->jm.jobs.vtc_jc.first_job,.fsjc=batch->jm.jobs.frag,
         .submit_error=ret,.mix=batch->lancelot_shader_mix
      };
      for(unsigned i=0;i<fb.rt_count&&i<8;i++) {
         if(fb.rts[i].preload)completed.preloads|=1u<<i;
         const struct pipe_surface *surface=batch->key.cbufs[i];
         if(surface&&surface->texture) {
            completed.format[i]=surface->texture->format;
            completed.modifier[i]=pan_resource(surface->texture)->image.layout.modifier;
         }
      }
      lancelot_drm_profile_batch(panfrost_device_fd(pan_device(pscreen)),&completed);
   }
#endif
''',1)
p.write_text('#ifdef __APPLE__\n#include "../../../lancelot-drm/lancelot_completion_profile.h"\n#endif\n'+data)
print('Lancelot Mesa overlay applied:',src)
