/* Linux host test for the S21 drm shim. No Mesa or IOKit is linked here. */
#include "s21_drm.h"
#include "s21_iokit.h"
#include "xf86drm.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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
   errno = 0;
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &bad) == -EINVAL);
   /* Mesa logs errno, not drmIoctl()'s return value. */
   assert(errno == EINVAL);
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

/* V30: hot DRM lines are opt-in. One BO + one submit must not emit the V29
 * per-BO/submit-call flood; a quiet boot keeps at most the first result line. */
static void
test_quiet_logging(int fd)
{
   uint64_t va = 0;
   uint32_t bo = create_bo(fd, 0x4000, 0, &va);
   uint32_t sync = 0;
   uint32_t handles[1] = { bo };
   struct drm_panfrost_submit sub = {
      .jc = va,
      .out_sync = sync,
      .bo_handles = (uint64_t)(uintptr_t)handles,
      .bo_handle_count = 1,
      .requirements = PANFROST_JD_REQ_FS,
   };

   assert(drmSyncobjCreate(fd, 0, &sync) == 0);
   sub.out_sync = sync;
   s21_host_logs_reset();
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &sub) == 0);
   assert(s21_host_logs() <= 1);

   uint32_t first = 99;
   assert(drmSyncobjWait(fd, &sync, 1, 0, 0, &first) == 0);
   assert(first == 0);
   assert(drmSyncobjDestroy(fd, sync) == 0);
   assert(drmCloseBufferHandle(fd, bo) == 0);
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
test_small_bo_16k_alignment(int fd)
{
   /* A 4 KiB request must reserve all 16 KiB that the kernel maps and later
    * unmaps, so consecutive small BOs cannot share GPU page-table entries. */
   struct drm_panfrost_create_bo a = { .size = 0x1000, .flags = 0 };
   struct drm_panfrost_create_bo b = { .size = 0x1000, .flags = 0 };

   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &a) == 0);
   assert(a.size == 0x4000);
   assert((a.offset & 0x3fff) == 0);
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &b) == 0);
   assert(b.size == 0x4000);
   assert(b.offset == a.offset + 0x4000);
   assert(s21_host_live_bos() == 2);

   assert(drmCloseBufferHandle(fd, a.handle) == 0);
   assert(drmCloseBufferHandle(fd, b.handle) == 0);
   assert(s21_host_live_bos() == 0);
}

static void
test_va_table_growth(int fd)
{
   /* V16 logged 135 live VA segments in one backboardd, over the old fixed
    * table of 128. V20 also filled the old 256-BO shim table. */
   enum { N = 300 };
   uint32_t bo[N];
   uint64_t va[N];

   for (unsigned i = 0; i < N; i++) {
      bo[i] = create_bo(fd, 0x4000, 0, &va[i]);
      for (unsigned j = 0; j < i; j++)
         assert(va[i] != va[j]);
   }
   assert(s21_host_live_bos() == N);

   /* Free every other BO: the table must coalesce without losing the rest. */
   for (unsigned i = 0; i < N; i += 2)
      assert(drmCloseBufferHandle(fd, bo[i]) == 0);
   assert(s21_host_live_bos() == N / 2);
   for (unsigned i = 1; i < N; i += 2)
      assert(drmCloseBufferHandle(fd, bo[i]) == 0);
   assert(s21_host_live_bos() == 0);
}

static void
test_va_best_fit(int fd)
{
   uint64_t big_va = 0, small_va = 0, again_va = 0;

   uint32_t big = create_bo(fd, 0x10000, 0, &big_va);
   uint32_t small = create_bo(fd, 0x4000, 0, &small_va);
   assert(drmCloseBufferHandle(fd, big) == 0);
   assert(drmCloseBufferHandle(fd, small) == 0);

   /* The 16 KiB hole is second, so first fit would split the larger hole. */
   uint32_t again = create_bo(fd, 0x4000, 0, &again_va);
   assert(again_va == small_va);
   assert(drmCloseBufferHandle(fd, again) == 0);
   assert(s21_host_live_bos() == 0);
}

static void
test_map_failure_rollback(int fd)
{
   /* V19 leaked a shim slot whenever the kernel's GPU page-table pool made
    * MAP_BO fail. The next CREATE_BO then had no slot and Mesa logged err=5. */
   struct drm_panfrost_create_bo c = { .size = 0x4000, .flags = 0 };

   s21_host_fail_map(1);
   errno = 0;
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c) == -ENOMEM);
   assert(errno == ENOMEM);
   assert(s21_host_live_bos() == 0);

   s21_host_fail_map(0);
   uint32_t h = create_bo(fd, 0x4000, 0, NULL);
   assert(s21_host_live_bos() == 1);
   assert(drmCloseBufferHandle(fd, h) == 0);
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
test_syncobj_import_v17(int fd)
{
   uint32_t imported = 0, from_fd = 0;

   /* V17 behaviour: import does not invent a signal, and FDToHandle creates
    * an unsignaled syncobj. V18's signaled stand-in destabilised backboardd. */
   assert(drmSyncobjCreate(fd, 0, &imported) == 0);
   assert(drmSyncobjImportSyncFile(fd, STDOUT_FILENO, imported) == 0);
   assert(drmSyncobjWait(fd, &imported, 1, 0, 0, NULL) == -ETIMEDOUT);

   assert(drmSyncobjFDToHandle(fd, STDOUT_FILENO, &from_fd) == 0);
   assert(from_fd != 0);
   assert(drmSyncobjWait(fd, &from_fd, 1, 0, 0, NULL) == -ETIMEDOUT);

   assert(drmSyncobjDestroy(fd, imported) == 0);
   assert(drmSyncobjDestroy(fd, from_fd) == 0);
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

static void
test_submit_list_and_prime(int fd)
{
   /* V22: a large IOSurface-like import must travel through kSMSubmitList and
    * signal its Mesa out_sync without any sync-file export. */
   void *mem = aligned_alloc(4096, 128 * 1024);
   assert(mem != NULL);
   uint32_t h = 0;
   uint64_t va = 0;
   assert(s21_drm_import_user((uint64_t)(uintptr_t)mem, 128 * 1024,
                              &h, &va) == 0);

   int prime_fd = -1;
   assert(drmPrimeHandleToFD(fd, h, 0, &prime_fd) == 0);
   assert(prime_fd > 0);
   assert(s21_drm_prime_fd_size(prime_fd) == 128 * 1024);
   uint32_t imported = 0;
   assert(drmPrimeFDToHandle(fd, prime_fd, &imported) == 0);
   assert(imported == h);

   uint32_t sync = 0;
   assert(drmSyncobjCreate(fd, 0, &sync) == 0);
   uint32_t handles[1] = { h };
   struct drm_panfrost_submit s = {
      .jc = va,
      .out_sync = sync,
      .bo_handles = (uint64_t)(uintptr_t)handles,
      .bo_handle_count = 1,
      .requirements = PANFROST_JD_REQ_FS,
   };
   unsigned submits = s21_host_submits();
   unsigned flushes = s21_host_flush_calls();
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &s) == 0);
   assert(s21_host_submits() == submits + 1);
   assert(s21_host_flush_calls() == flushes + 1);
   /* One large BO is duplicated: first half before the job, second after. */
   assert(s21_host_last_struct_len() == 2 * sizeof(uint64_t));
   assert(s21_host_last_nread() == 1);

   uint32_t first = 99;
   assert(drmSyncobjWait(fd, &sync, 1, 0, 0, &first) == 0);
   assert(first == 0);

   assert(drmSyncobjDestroy(fd, sync) == 0);
   assert(drmCloseBufferHandle(fd, h) == 0);
   assert(s21_drm_prime_fd_size(prime_fd) == -EINVAL);
   free(mem);
}

static void
test_submit_pair(int fd)
{
   /* Mesa's tiler + fragment pair must become one kSMSubmitRange call. */
   uint64_t tiler_va = 0, frag_va = 0;
   uint32_t tiler_bo = create_bo(fd, 0x4000, 0, &tiler_va);
   uint32_t frag_bo = create_bo(fd, 0x4000, 0, &frag_va);
   uint32_t sync = 0;
   assert(drmSyncobjCreate(fd, 0, &sync) == 0);

   uint32_t tiler_handles[1] = { tiler_bo };
   uint32_t frag_handles[1] = { frag_bo };
   struct drm_panfrost_submit tiler = {
      .jc = tiler_va,
      .bo_handles = (uint64_t)(uintptr_t)tiler_handles,
      .bo_handle_count = 1,
   };
   struct drm_panfrost_submit frag = {
      .jc = frag_va,
      .out_sync = sync,
      .bo_handles = (uint64_t)(uintptr_t)frag_handles,
      .bo_handle_count = 1,
      .requirements = PANFROST_JD_REQ_FS,
   };

   unsigned submits = s21_host_submits();
   unsigned pairs = s21_host_pair_calls();
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &tiler) == 0);
   assert(s21_host_submits() == submits);
   assert(s21_host_pair_calls() == pairs);

   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &frag) == 0);
   assert(s21_host_submits() == submits + 1);
   assert(s21_host_pair_calls() == pairs + 1);
   assert(s21_host_last_slot() == 1);
   assert(s21_host_last_jc() == tiler_va);
   assert(s21_host_last_second_jc() == frag_va);
   assert(s21_host_last_struct_len() == 0);   /* both BOs are small */

   uint32_t first = 99;
   assert(drmSyncobjWait(fd, &sync, 1, 0, 0, &first) == 0);
   assert(first == 0);

   assert(drmSyncobjDestroy(fd, sync) == 0);
   assert(drmCloseBufferHandle(fd, tiler_bo) == 0);
   assert(drmCloseBufferHandle(fd, frag_bo) == 0);
}

static void
test_submit_pair_large_flush(int fd)
{
   /* Both halves reference the same large BO: selector 12 must receive one
    * {va, before|after} entry, not two list copies from kSMSubmitList. */
   uint64_t va = 0;
   uint32_t bo = create_bo(fd, 128 * 1024, 0, &va);
   uint32_t sync = 0;
   assert(drmSyncobjCreate(fd, 0, &sync) == 0);
   uint32_t handles[1] = { bo };
   struct drm_panfrost_submit tiler = {
      .jc = va,
      .bo_handles = (uint64_t)(uintptr_t)handles,
      .bo_handle_count = 1,
   };
   struct drm_panfrost_submit frag = {
      .jc = va + 0x1000,
      .out_sync = sync,
      .bo_handles = (uint64_t)(uintptr_t)handles,
      .bo_handle_count = 1,
      .requirements = PANFROST_JD_REQ_FS,
   };
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &tiler) == 0);
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &frag) == 0);
   assert(s21_host_last_struct_len() == 2 * sizeof(uint64_t));
   assert(s21_host_last_nread() == 1);
   assert(s21_host_last_second_jc() == va + 0x1000);
   assert(drmSyncobjDestroy(fd, sync) == 0);
   assert(drmCloseBufferHandle(fd, bo) == 0);
}

static void
test_submit_pair_fallback(int fd)
{
   /* A kernel that rejects selector 12 must get the old two-submit path. */
   uint64_t tiler_va = 0, frag_va = 0;
   uint32_t tiler_bo = create_bo(fd, 0x4000, 0, &tiler_va);
   uint32_t frag_bo = create_bo(fd, 0x4000, 0, &frag_va);
   uint32_t sync = 0;
   assert(drmSyncobjCreate(fd, 0, &sync) == 0);

   uint32_t tiler_handles[1] = { tiler_bo };
   uint32_t frag_handles[1] = { frag_bo };
   struct drm_panfrost_submit tiler = {
      .jc = tiler_va,
      .bo_handles = (uint64_t)(uintptr_t)tiler_handles,
      .bo_handle_count = 1,
   };
   struct drm_panfrost_submit frag = {
      .jc = frag_va,
      .out_sync = sync,
      .bo_handles = (uint64_t)(uintptr_t)frag_handles,
      .bo_handle_count = 1,
      .requirements = PANFROST_JD_REQ_FS,
   };

   s21_host_fail_submit_range(1);
   unsigned submits = s21_host_submits();
   unsigned pairs = s21_host_pair_calls();
   unsigned flushes = s21_host_flush_calls();
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &tiler) == 0);
   assert(drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &frag) == 0);
   assert(s21_host_submits() == submits + 2);
   assert(s21_host_pair_calls() == pairs);
   assert(s21_host_flush_calls() == flushes + 2);
   assert(s21_host_last_slot() == 0);
   assert(s21_host_last_jc() == frag_va);

   uint32_t first = 99;
   assert(drmSyncobjWait(fd, &sync, 1, 0, 0, &first) == 0);
   assert(first == 0);

   assert(drmSyncobjDestroy(fd, sync) == 0);
   assert(drmCloseBufferHandle(fd, tiler_bo) == 0);
   assert(drmCloseBufferHandle(fd, frag_bo) == 0);
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
   test_small_bo_16k_alignment(fd);
   test_va_table_growth(fd);
   test_va_best_fit(fd);
   test_map_failure_rollback(fd);
   test_heap_split(fd);
   test_syncobjs(fd);
   test_syncobj_import_v17(fd);
   test_import_user(fd);
   test_submit_list_and_prime(fd);
   test_submit_pair(fd);
   test_submit_pair_large_flush(fd);
   test_submit_pair_fallback(fd);
   test_quiet_logging(fd);

   drmVersionPtr v = drmGetVersion(fd);
   assert(v != NULL && v->name != NULL && strcmp(v->name, "panfrost") == 0);
   drmFreeVersion(v);

   drmClose(fd);
   s21_iokit_close();
   puts("s21-drm-host-test: PASS");
   return 0;
}
