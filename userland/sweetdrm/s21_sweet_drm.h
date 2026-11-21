/*
 * s21_sweet_drm: libdrm/Panfrost emulation over this fork's SweetMali IOKit
 * user client (xnu/patches/0112-s21-mali-iokit.sh).
 *
 * This is not PixOS's sweet_drm_mali.c. The S21 kernel ABI is:
 *   selector 0 GET_INFO   structure-out: u32 gpu_id, shader_present,
 *                         tiler_present, l2_present, as_present, js_present
 *   selector 1 CREATE_BO  scalar-in: size             scalar-out: handle, pa
 *   selector 2 MAP_BO     scalar-in: handle, gpu_va   (no outputs)
 *   selector 3 SUBMIT     scalar-in: slot, jc_va, timeout_ms
 *                                                     scalar-out: status (0 = done)
 *   selector 4 DESTROY_BO scalar-in: handle
 *   selector 5 LOG        structure-in: text (kernel console)
 *   selector 6 MAPFB      scalar-out: handle, pa, rowbytes, (width<<16)|height
 *   selector 8 IMPORT_USER scalar-in: uva, len, gpu_va; scalar-out: handle
 *   selector 9 KEEP       structure-in: text replayed into pmsg on recovery
 *   IOConnectMapMemory64(type = kernel handle) maps a BO into this task.
 *
 * The client chooses the GPU VA, must call MAP_BO after CREATE_BO, and SUBMIT
 * is synchronous. S21 limits BOs to 15 MiB each, so a Linux-sized 128 MiB
 * tiler heap is split into several kernel BOs mapped at adjacent GPU VAs.
 */
#ifndef S21_SWEET_DRM_H
#define S21_SWEET_DRM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open the SweetMali user client lazily and return a real fd (/dev/null) to
 * give to panfrost_create_screen(). Returns -1 with errno set:
 * ENODEV (service absent), EBUSY (another client), ENOMEM/ENODEV on a bad
 * GET_INFO reply. Retrying is allowed while the GPU comes up after boot. */
int s21_sweet_drm_open(void);

/* Register an IOSurface backing store (already mapped into this task) as the
 * source of a future drmPrimeFDToHandle() call. `len` must be page aligned and
 * <= the kernel's single-BO limit. Returns 0 or -errno. */
int s21_sweet_drm_register_import(int fd, uint64_t uva, uint64_t len);
void s21_sweet_drm_unregister_import(int fd);

/* Import a user range directly: calls selector 8 and returns the shim DRM
 * handle plus the GPU VA assigned to it. 0 or -errno. */
int s21_sweet_drm_import_user(uint64_t uva, uint64_t len,
                              uint32_t *handle, uint64_t *gpu_va);

/* Paint the real panel framebuffer a solid colour via selector 6 (MAPFB).
 * `color` is in the framebuffer's in-memory byte order: 0xFF00FF00 = green,
 * 0xFF0000FF = red. With hold != 0 this never returns; it repaints every
 * 50 ms so the GUI cannot overwrite the visible result. Returns 0 for a
 * successful one-shot paint, -1 with errno set on failure. */
int s21_sweet_fb_paint(uint32_t color, int hold);

/* Send a short line to the kernel's SweetMali console log (selector 5) and
 * keep buffer (selector 9). Best-effort; no return value. */
void s21_sweet_log_line(const char *line);

/* Diagnostic: number of CREATE_BO calls the shim has made (host test). */
unsigned s21_sweet_drm_create_calls(void);

#ifdef __cplusplus
}
#endif
#endif
