#!/usr/bin/env python3
"""Apply the Lancelot-only Mesa overlay to a clean archive tree."""
from pathlib import Path
import shutil, sys, re
root=Path(__file__).resolve().parent
src=Path(sys.argv[1]).resolve()
if not (src/'meson.build').exists() or not (src/'src/gallium').is_dir(): raise SystemExit('Mesa source tree expected (meson.build + src/gallium)')
def change(path,old,new):
 p=src/path; data=p.read_text()
 if old not in data: raise SystemExit(f'{path}: expected source hunk missing: {old[:80]!r}')
 p.write_text(data.replace(old,new,1))
change(Path('meson.build'),'if dep_libdrm.found()\n',"if host_machine.system() == 'darwin' and with_gallium_panfrost\n  dep_libdrm = declare_dependency(include_directories : include_directories('src/lancelot-drm'), link_args : ['-framework', 'IOKit', '-framework', 'CoreFoundation'])\n  pre_args += '-DHAVE_LIBDRM'\nelif dep_libdrm.found()\n")
change(Path('src/compiler/meson.build'),"subdir('spirv')","subdir('spirv')\nsubdir('air')")
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
         dev, 16 * 1024 * 1024, PAN_BO_INVISIBLE, "Fixed Lancelot tiler heap");
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
print('Lancelot Mesa overlay applied:',src)
