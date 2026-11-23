#!/usr/bin/env python3
"""Apply the fresh S21/iOS glue patches to a pristine Mesa 26.2.4 tree.

This is new code for the Linux-Mesa-based port. It does not consume patches
from the deleted Mesa branch. All edits are guarded by S21_IOS, which the
cross-build script defines.

Patches:
  * enable the DRI2 target on Darwin for the Panfrost-only build
  * use s21_drm_open() for the fake DRM fd
  * report "panfrost" for that fd so the pipe loader picks the right driver
  * make surfaceless EGL probe directly instead of scanning /sys/class/drm
  * create the Panfrost screen directly (Darwin has no libdrm pipe loader)
  * route BO mmap/munmap through the cached SweetMali mappings in the shim
"""
import argparse
import pathlib
import sys

MARK = "S21_IOS"


def edit(path, replacements):
    text = path.read_text()
    if MARK in text:
        print(f"  {path.relative_to(MESA)}: already patched")
        return
    for old, new in replacements:
        n = text.count(old)
        if n != 1:
            print(f"ERROR: anchor count {n} in {path}: {old[:80]!r}", file=sys.stderr)
            raise SystemExit(1)
        text = text.replace(old, new)
    path.write_text(text)
    print(f"  patched {path.relative_to(MESA)}")


def main() -> int:
    global MESA
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("mesa_dir", type=pathlib.Path)
    args = ap.parse_args()
    MESA = args.mesa_dir.resolve()

    required = [
        "meson.build",
        "src/gallium/frontends/dri/dri2.c",
        "src/panfrost/lib/kmod/pan_kmod.h",
        "src/gallium/drivers/panfrost/pan_bo.c",
        "src/loader/loader.c",
        "src/egl/drivers/dri2/platform_surfaceless.c",
    ]
    for rel in required:
        if not (MESA / rel).is_file():
            print(f"ERROR: {MESA / rel} not found", file=sys.stderr)
            return 1

    print("Patching Mesa 26.2.4 for S21/iOS:")

    edit(MESA / "meson.build", [
        ('''if with_gallium and (system_has_kms_drm or (with_gallium_zink and (freedreno_kmds.contains('kgsl') or host_machine.system() == 'darwin')))
''',
         '''if with_gallium and (system_has_kms_drm or (with_gallium_zink and (freedreno_kmds.contains('kgsl') or host_machine.system() == 'darwin')) or
                     (host_machine.system() == 'darwin' and with_gallium_panfrost)) # S21_IOS
'''),
    ])

    edit(MESA / "src/gallium/frontends/dri/dri2.c", [
        ('#include "pipe-loader/pipe_loader.h"\n',
         '''#include "pipe-loader/pipe_loader.h"
#ifdef S21_IOS
#include "util/xmlconfig.h"
#include "util/driconf.h"
#include "gallium/winsys/panfrost/drm/panfrost_drm_public.h"
#endif
'''),
        ('''dri2_init_screen(struct dri_screen *screen, bool driver_name_is_inferred)
{
   struct pipe_screen *pscreen = NULL;

   screen->can_share_buffer = true;

#ifdef HAVE_LIBDRM
''',
         '''dri2_init_screen(struct dri_screen *screen, bool driver_name_is_inferred)
{
   struct pipe_screen *pscreen = NULL;

   screen->can_share_buffer = true;

#ifdef S21_IOS
   /* No libdrm pipe loader on iOS. Create the only compiled driver directly. */
   static const driOptionDescription s21_pan_options_desc[] = {
#include "panfrost/driinfo_panfrost.h"
   };
   static struct driOptionCache s21_pan_options_info = { 0 };
   struct driOptionCache s21_pan_options = { 0 };
   driParseOptionInfo(&s21_pan_options_info, s21_pan_options_desc,
                      sizeof(s21_pan_options_desc) /
                         sizeof(s21_pan_options_desc[0]));
   driParseConfigFiles(&s21_pan_options, &s21_pan_options_info,
                       &(driConfigFileParseParams) { .driverName = "panfrost" });
   struct pipe_screen_config s21_config = {
      .driver_name_is_inferred = driver_name_is_inferred,
      .options = &s21_pan_options,
      .options_info = &s21_pan_options_info,
   };
   pscreen = panfrost_drm_screen_create(screen->fd, &s21_config);
#else
#ifdef HAVE_LIBDRM
'''),
        ('''   if (pipe_loader_drm_probe_fd(&screen->dev, screen->fd, false))
      pscreen = pipe_loader_create_screen(screen->dev, driver_name_is_inferred);
#endif

   return pscreen;
}
''',
         '''   if (pipe_loader_drm_probe_fd(&screen->dev, screen->fd, false))
      pscreen = pipe_loader_create_screen(screen->dev, driver_name_is_inferred);
#endif
#endif

   return pscreen;
}
'''),
    ])

    edit(MESA / "src/panfrost/lib/kmod/pan_kmod.h", [
        ('#include <xf86drm.h>\n',
         '#include <xf86drm.h>\n#ifdef S21_IOS\n#include "s21_drm.h"\n#endif\n'),
        ('''   host_addr =
      os_mmap(host_addr, bo->size, prot, flags, bo->dev->fd, mmap_offset);
''',
         '''#ifdef S21_IOS
   host_addr = s21_drm_bo_map(bo->dev->fd, bo->size, prot, flags,
                              (uint64_t)mmap_offset);
#else
   host_addr =
      os_mmap(host_addr, bo->size, prot, flags, bo->dev->fd, mmap_offset);
#endif
'''),
    ])

    edit(MESA / "src/gallium/drivers/panfrost/pan_bo.c", [
        ('#include <xf86drm.h>\n',
         '#include <xf86drm.h>\n#ifdef S21_IOS\n#include "s21_drm.h"\n#endif\n'),
        ('   if (os_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {\n',
         '''#ifdef S21_IOS
   if (s21_drm_bo_unmap((void *)(uintptr_t)bo->ptr.cpu,
                        panfrost_bo_size(bo))) {
#else
   if (os_munmap((void *)(uintptr_t)bo->ptr.cpu, panfrost_bo_size(bo))) {
#endif
'''),
    ])

    edit(MESA / "src/loader/loader.c", [
        ('#include "git_sha1.h"\n',
         '#include "git_sha1.h"\n#ifdef S21_IOS\n#include "s21_drm.h"\n#endif\n'),
        ('''int
loader_open_device(const char *device_name)
{
   int fd;
''',
         '''int
loader_open_device(const char *device_name)
{
#ifdef S21_IOS
   (void)device_name;
   return s21_drm_open();
#else
   int fd;
'''),
        ('''   if (fd == -1 && errno == EACCES) {
      log_(_LOADER_WARNING, "failed to open %s: %s\\n",
           device_name, strerror(errno));
   }
   return fd;
}
''',
         '''   if (fd == -1 && errno == EACCES) {
      log_(_LOADER_WARNING, "failed to open %s: %s\\n",
           device_name, strerror(errno));
   }
   return fd;
#endif
}
'''),
        ('''loader_get_driver_for_fd(int fd)
{
   char *driver;
''',
         '''loader_get_driver_for_fd(int fd)
{
#ifdef S21_IOS
   if (fd == S21_DRM_FD)
      return strdup("panfrost");
#endif
   char *driver;
'''),
    ])

    edit(MESA / "src/egl/drivers/dri2/platform_surfaceless.c", [
        ('#include "dri_screen.h"\n',
         '#include "dri_screen.h"\n#ifdef S21_IOS\n#include "s21_drm.h"\n#endif\n'),
        ('''   _EGLDevice *dev_list = _eglGlobal.DeviceList;
   drmDevicePtr device;

   while (dev_list) {
''',
         '''   _EGLDevice *dev_list = _eglGlobal.DeviceList;
   drmDevicePtr device;

#ifdef S21_IOS
   /* No /dev/dri or sysfs on iOS: one fixed SweetMali render node. */
   (void)node_type;
   (void)dev_list;
   (void)device;
   if (swrast || zink)
      return false;
   dri2_dpy->fd_render_gpu = s21_drm_open();
   if (dri2_dpy->fd_render_gpu < 0)
      return false;
   dri2_dpy->fd_display_gpu = dri2_dpy->fd_render_gpu;
   dri2_dpy->driver_name = strdup("panfrost");
   dri2_dpy->loader_extensions = image_loader_extensions;
   if (!dri2_create_screen(disp)) {
      _eglLog(_EGL_WARNING, "S21: failed to create panfrost screen");
      return false;
   }
   return true;
#endif

   while (dev_list) {
'''),
    ])

    print("done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
