#!/usr/bin/env python3
"""Finish Darwin screen setup: real DRI defaults and connection ownership."""
from pathlib import Path
import sys
src=Path(sys.argv[1]).resolve()
def change(rel,old,new):
 p=src/rel;s=p.read_text()
 if old not in s: raise SystemExit(f'{rel}: hunk missing')
 p.write_text(s.replace(old,new,1))
change('src/gallium/drivers/panfrost/meson.build','''driver_panfrost = declare_dependency(
  compile_args : compile_args_panfrost,''','''driver_panfrost = declare_dependency(
  dependencies : [idep_xmlconfig, dep_libdrm],
  compile_args : compile_args_panfrost,''')
change('src/gallium/winsys/panfrost/drm/panfrost_drm_winsys.c','#include "xf86drm.h"','''#include "xf86drm.h"
#include "pipe/p_screen.h"
#ifdef __APPLE__
#include "util/driconf.h"
#include "util/xmlconfig.h"
static const driOptionDescription lancelot_panfrost_options[] = {
#include "panfrost/driinfo_panfrost.h"
};
static struct pipe_screen *
lancelot_screen_create(int fd, const struct pipe_screen_config *requested, struct renderonly *ro)
{
   int alias = lancelot_drm_dup(fd);
   if (alias < 0) return NULL;
   driOptionCache info = {0}, cache = {0};
   driParseOptionInfo(&info, lancelot_panfrost_options, ARRAY_SIZE(lancelot_panfrost_options));
   struct pipe_screen_config config = requested ? *requested : (struct pipe_screen_config){0};
   config.options = &cache;
   config.options_info = &info;
   /* panfrost_create_screen parses files and queries these options immediately. */
   struct pipe_screen *screen = panfrost_create_screen(alias, &config, ro);
   driDestroyOptionCache(&cache);
   driDestroyOptionInfo(&info);
   return screen;
}
#endif''')
change('src/gallium/winsys/panfrost/drm/panfrost_drm_winsys.c','''   int alias = lancelot_drm_dup(fd);
   return alias < 0 ? NULL : panfrost_create_screen(alias, config, NULL);''','''   return lancelot_screen_create(fd, config, NULL);''')
change('src/gallium/winsys/panfrost/drm/panfrost_drm_winsys.c','''   int alias = lancelot_drm_dup(fd);
   return alias < 0 ? NULL : panfrost_create_screen(alias, config, ro);''','''   return lancelot_screen_create(fd, config, ro);''')
print('Applied real Panfrost option-cache setup')
