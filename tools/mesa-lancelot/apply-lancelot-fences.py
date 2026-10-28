#!/usr/bin/env python3
"""Apply LMAL v1's explicit synchronous-fence policy to Mesa Panfrost."""
from pathlib import Path
root=Path(__file__).resolve().parent
src=Path(__import__('sys').argv[1]).resolve()
def change(rel, old, new):
 p=src/rel;s=p.read_text()
 if old not in s: raise SystemExit(f'{rel}: missing hunk')
 p.write_text(s.replace(old,new,1))
change(Path('src/gallium/drivers/panfrost/pan_fence.c'),'''int
panfrost_fence_get_fd(struct pipe_screen *screen, struct pipe_fence_handle *f)
{
   struct panfrost_device *dev = pan_device(screen);
   int fd = -1;

   drmSyncobjExportSyncFile(panfrost_device_fd(dev), f->syncobj, &fd);
   return fd;
}''','''int
panfrost_fence_get_fd(struct pipe_screen *screen, struct pipe_fence_handle *f)
{
#ifdef __APPLE__
   (void)screen; (void)f;
   /* LMAL v1 intentionally has no sync-file/dma-buf ABI. */
   errno = ENOTSUP;
   return -1;
#else
   struct panfrost_device *dev = pan_device(screen);
   int fd = -1;

   drmSyncobjExportSyncFile(panfrost_device_fd(dev), f->syncobj, &fd);
   return fd;
#endif
}''')
change(Path('src/gallium/drivers/panfrost/pan_fence.c'),'''struct pipe_fence_handle *
panfrost_fence_create(struct panfrost_context *ctx)
{
   struct panfrost_device *dev = pan_device(ctx->base.screen);''','''struct pipe_fence_handle *
panfrost_fence_create(struct panfrost_context *ctx)
{
   struct panfrost_device *dev = pan_device(ctx->base.screen);
#ifdef __APPLE__
   struct pipe_fence_handle *f = calloc(1, sizeof(*f));
   if (!f) return NULL;
   if (drmSyncobjWait(panfrost_device_fd(dev), &ctx->syncobj, 1,
                      INT64_MAX, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL) < 0) {
      free(f);
      return NULL;
   }
   if (drmSyncobjCreate(panfrost_device_fd(dev), DRM_SYNCOBJ_CREATE_SIGNALED,
                        &f->syncobj) < 0) {
      free(f);
      return NULL;
   }
   pipe_reference_init(&f->reference, 1);
   f->signaled = true;
   return f;
#else''')
change(Path('src/gallium/drivers/panfrost/pan_fence.c'),'''   close(fd);

   return f;
}''','''   close(fd);

   return f;
#endif
}''')
change(Path('src/gallium/drivers/panfrost/pan_context.c'),'''   ret = drmSyncobjExportSyncFile(panfrost_device_fd(dev), f->syncobj, &fd);
   assert(!ret);

   sync_accumulate("panfrost", &ctx->in_sync_fd, fd);
   close(fd);''','''#ifdef __APPLE__
   /* v1 has no sync-file object. Wait here; the following submit is ordered by
    * the same client VM, and the wait is the only truthful dependency bridge. */
   ret = drmSyncobjWait(panfrost_device_fd(dev), &f->syncobj, 1, INT64_MAX,
                        DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL);
   assert(!ret);
   (void)ctx;
#else
   ret = drmSyncobjExportSyncFile(panfrost_device_fd(dev), f->syncobj, &fd);
   assert(!ret);

   sync_accumulate("panfrost", &ctx->in_sync_fd, fd);
   close(fd);
#endif''')
change(Path('src/gallium/drivers/panfrost/pan_screen.c'),'   caps->native_fence_fd = true;','   caps->native_fence_fd = false;')
print('Applied LMAL v1 synchronous fence policy')
