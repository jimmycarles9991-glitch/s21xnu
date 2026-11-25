/* Extra entry points provided by the S21 drm shim, outside libdrm. */
#ifndef S21_DRM_EXTRA_H
#define S21_DRM_EXTRA_H

#include <stddef.h>
#include <stdint.h>

/* The shim hands Mesa a stable fake DRM fd. Mesa's loader patches recognize it. */
#define S21_DRM_FD 3

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the SweetMali user client and returns the fake DRM fd. */
int s21_drm_open(void);

/* BO mmap hooks. Mesa's Darwin panfrost platform code will call these instead
 * of mmap() on the DRM fd. The mmap offset is (handle << 32), returned by
 * DRM_IOCTL_PANFROST_MMAP_BO. */
void *s21_drm_bo_map(int fd, size_t size, int prot, int flags, uint64_t offset);
int s21_drm_bo_unmap(void *addr, size_t size);

/* Wrap caller-owned CPU memory (e.g. an IOSurface backing store) as a GPU BO. */
int s21_drm_import_user(uint64_t uva, uint64_t len,
                        uint32_t *handle, uint64_t *gpu_va);

/* Fake PRIME fds are synthetic numbers, not real descriptors. This returns the
 * wrapped BO's byte size so Mesa's pan_kmod_bo_import() does not lseek() them. */
int64_t s21_drm_prime_fd_size(int fd);

#ifdef __cplusplus
}
#endif

#endif
