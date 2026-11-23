/* Linux host test for the S21 drm shim. No Mesa or IOKit is linked here. */
#include "s21_drm.h"
#include "s21_iokit.h"
#include "xf86drm.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "drm-uapi/panfrost_drm.h"

static uint64_t
get_param(int fd, enum drm_panfrost_param param)
{
   struct drm_panfrost_get_param p = { .param = param };
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &p) == 0);
   return p.value;
}

static void
test_params(int fd)
{
   assert(get_param(fd, DRM_PANFROST_PARAM_GPU_PROD_ID) == 0x9202);
   assert(get_param(fd, DRM_PANFROST_PARAM_GPU_REVISION) == 0x0010);
   assert(get_param(fd, DRM_PANFROST_PARAM_SHADER_PRESENT) == 0x00ee0ceeULL);
   assert(get_param(fd, DRM_PANFROST_PARAM_AS_PRESENT) == 0xff);
   assert(get_param(fd, DRM_PANFROST_PARAM_JS_PRESENT) == 0x7);
   assert(get_param(fd, DRM_PANFROST_PARAM_L2_FEATURES) == 0x07120206ULL);
   assert(get_param(fd, DRM_PANFROST_PARAM_MEM_FEATURES) == 0x301);
   assert(get_param(fd, DRM_PANFROST_PARAM_MMU_FEATURES) == 0x2830);
   assert(get_param(fd, DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ) == 0x200);
   assert(get_param(fd, DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ) == 0x200);
   assert(get_param(fd, DRM_PANFROST_PARAM_TEXTURE_FEATURES0) == 0xf7fe007eULL);
   assert(get_param(fd, DRM_PANFROST_PARAM_TEXTURE_FEATURES3) == 0x000010c6ULL);

   struct drm_panfrost_get_param bad = { .param = 99999 };
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &bad) == -EINVAL);
}

static uint32_t
create_bo(int fd, uint32_t size, uint32_t flags, uint64_t *gpu_va)
{
   struct drm_panfrost_create_bo c = { .size = size, .flags = flags };
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c) == 0);
   assert(c.handle != 0);
   assert((c.offset & 0xfff) == 0);

   struct drm_panfrost_get_bo_offset g = { .handle = c.handle };
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_BO_OFFSET, &g) == 0);
   assert(g.offset == c.offset);
   if (gpu_va != NULL)
      *gpu_va = g.offset;
   return c.handle;
}

static void
test_small_bo_submit_and_map(int fd)
{
   uint64_t va = 0;
   uint32_t bo = create_bo(fd, 0x4000, 0, &va);
   assert(s21_host_live_bos() == 1);

   struct drm_panfrost_mmap_bo m = { .handle = bo };
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_MMAP_BO, &m) == 0);
   void *cpu = s21_drm_bo_map(fd, 0x4000, 0, 0, m.offset);
   assert(cpu != MAP_FAILED);
   memset(cpu, 0xa5, 256);
   assert(s21_drm_bo_unmap(cpu, 0x4000) == 0);

   uint32_t handles[1] = { bo };
   struct drm_panfrost_submit s = {
      .jc = va,
      .bo_handles = (uint64_t)(uintptr_t)handles,
      .bo_handle_count = 1,
      .requirements = PANFROST_JD_REQ_FS,
   };
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &s) == 0);
   assert(s21_host_submits() == 1);
   assert(s21_host_last_jc() == va);
   assert(s21_host_last_slot() == 0);

   assert(drmCloseBufferHandle(fd, bo) == 0);
   assert(s21_host_live_bos() == 0);
}

static void
test_heap_split(int fd)
{
   uint64_t va = 0;
   /* 128 MiB tiler heap: kernel limit is 15 MiB per BO, so this must become 9. */
   uint32_t heap = create_bo(fd, 128ULL << 20, PANFROST_BO_HEAP, &va);
   assert(s21_host_live_bos() == 9);
   assert(va >= 0x30000000ULL && va < 0x70000000ULL);

   assert(drmCloseBufferHandle(fd, heap) == 0);
   assert(s21_host_live_bos() == 0);
}

static void
test_syncobjs(int fd)
{
   uint32_t h = 0;
   assert(drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &h) == 0);
   assert(h != 0);

   uint32_t first = 99;
   assert(drmSyncobjWait(fd, &h, 1, 0, 0, &first) == 0);
   assert(first == 0);

   assert(drmSyncobjReset(fd, &h, 1) == 0);
   assert(drmSyncobjWait(fd, &h, 1, 0, 0, NULL) == -ETIMEDOUT);

   assert(drmSyncobjSignal(fd, &h, 1) == 0);
   uint64_t point = 1, first_point = 99;
   assert(drmSyncobjTimelineWait(fd, &h, &point, 1, 0, 0, &first_point) == 0);
   assert(first_point == 0);

   assert(drmSyncobjDestroy(fd, h) == 0);
   assert(drmSyncobjWait(fd, &h, 1, 0, 0, NULL) == -ETIMEDOUT);
}

static void
test_import_user(int fd)
{
   void *mem = aligned_alloc(4096, 4096);
   assert(mem != NULL);
   uint32_t h = 0;
   uint64_t va = 0;
   int ie = s21_drm_import_user((uint64_t)(uintptr_t)mem, 4096, &h, &va);
   if (ie != 0)
      fprintf(stderr, "import_user failed: %d (%s)\n", ie, strerror(-ie));
   assert(ie == 0);
   assert(h != 0 && va != 0);
   assert(s21_host_live_bos() == 1);
   assert(drmCloseBufferHandle(fd, h) == 0);
   assert(s21_host_live_bos() == 0);
   free(mem);
}

int
main(void)
{
   s21_host_reset();
   int fd = s21_drm_open();
   assert(fd > 0);
   assert(s21_host_opens() == 1);

   test_params(fd);
   test_small_bo_submit_and_map(fd);
   test_heap_split(fd);
   test_syncobjs(fd);
   test_import_user(fd);

   drmVersionPtr v = drmGetVersion(fd);
   assert(v != NULL && v->name != NULL && strcmp(v->name, "panfrost") == 0);
   drmFreeVersion(v);

   drmClose(fd);
   s21_iokit_close();
   puts("s21-drm-host-test: PASS");
   return 0;
}
