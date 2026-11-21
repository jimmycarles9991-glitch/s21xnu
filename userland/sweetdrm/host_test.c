/* Linux host test for the S21 SweetMali/Panfrost shim (fake IOKit). */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "fake_iokit.h"
#include "s21_sweet_drm.h"
#include "xf86drm.h"
#include "drm-uapi/panfrost_drm.h"
#include "darwin_drm.h"

#define CHECK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	failures++; \
} } while (0)

static unsigned failures;

int
main(void)
{
	fake_reset();
	int fd = s21_sweet_drm_open();
	CHECK(fd >= 0);
	CHECK(fake.opens == 1);
	CHECK(fake_count(0) == 1);

	drmVersionPtr v = drmGetVersion(fd);
	CHECK(v != NULL);
	if (v) {
		CHECK(strcmp(v->name, "panfrost") == 0);
		CHECK(v->version_major == 1 && v->version_minor == 2);
		drmFreeVersion(v);
	}

	struct drm_panfrost_get_param gp = { .param = DRM_PANFROST_PARAM_GPU_PROD_ID };
	CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp) == 0);
	CHECK(gp.value == 0x9202);
	gp.param = DRM_PANFROST_PARAM_MMU_FEATURES;
	CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp) == 0);
	CHECK(gp.value == 0x2830);
	gp.param = DRM_PANFROST_PARAM_ALLOWED_JM_CTX_PRIORITIES;
	CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_PARAM, &gp) == 0);
	CHECK(gp.value == 2);

	struct drm_panfrost_create_bo c = { .size = 32768 };
	CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &c) == 0);
	CHECK(c.handle == 1);
	CHECK(c.size == 32768);
	CHECK((c.offset & 4095) == 0);
	CHECK(fake_count(1) == 1);
	CHECK(fake_count(2) == 1);
	CHECK(fake_last(1)->in[0] == 32768);
	CHECK(fake_last(2)->in[0] == 1);
	CHECK(fake_last(2)->in[1] == c.offset);

	struct drm_panfrost_get_bo_offset off = { .handle = c.handle };
	CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_GET_BO_OFFSET, &off) == 0);
	CHECK(off.offset == c.offset);

	struct drm_panfrost_mmap_bo mb = { .handle = c.handle };
	CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_MMAP_BO, &mb) == 0);
	void *cpu = darwin_drm_bo_map(fd, c.size, PROT_READ | PROT_WRITE,
	                              MAP_SHARED, mb.offset);
	CHECK(cpu != MAP_FAILED);
	CHECK(darwin_drm_bo_unmap(cpu, c.size) == 0);

	struct drm_panfrost_submit s = { .jc = c.offset, .requirements = PANFROST_JD_REQ_FS };
	CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &s) == 0);
	CHECK(fake.submits == 1);
	CHECK(fake.last_slot == 0);
	CHECK(fake.last_jc == c.offset);
	CHECK(fake.last_timeout == 5000);

	struct drm_panfrost_wait_bo w = { .handle = c.handle, .timeout_ns = 1000000 };
	CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_WAIT_BO, &w) == 0);
	CHECK(drmCloseBufferHandle(fd, c.handle) == 0);
	CHECK(fake.destroys == 1);

	/* A Linux-sized 128 MiB tiler heap must become nine 15 MiB-or-smaller
	 * kernel BOs at adjacent GPU VAs; S21's per-BO cap is 15 MiB. */
	unsigned before_create = fake_count(1), before_map = fake_count(2);
	struct drm_panfrost_create_bo heap = {
		.size = 128u << 20,
		.flags = PANFROST_BO_HEAP | PANFROST_BO_NOEXEC,
	};
	CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &heap) == 0);
	CHECK(fake_count(1) - before_create == 9);
	CHECK(fake_count(2) - before_map == 9);
	CHECK(heap.size == 128u << 20);
	CHECK((heap.offset & 4095) == 0);
	CHECK(fake_last(1)->in[0] == (8u << 20)); /* last chunk: 128 - 8*15 */
	/* Mesa never CPU-maps the heap; the shim must refuse it. */
	mb.handle = heap.handle;
	CHECK(drmIoctl(fd, DRM_IOCTL_PANFROST_MMAP_BO, &mb) != 0);
	errno = 0;
	CHECK(darwin_drm_bo_map(fd, 4096, PROT_READ, MAP_SHARED,
	                        (uint64_t)heap.handle << 32) == MAP_FAILED);
	CHECK(drmCloseBufferHandle(fd, heap.handle) == 0);
	CHECK(fake.destroys == 10);

	/* IOSurface-backed import: selector 8 takes the user VA, length and the
	 * GPU VA chosen by the shim. */
	const uint64_t uva = 0x40000000ULL, ilen = 4096;
	int impfd = dup(fd);
	CHECK(s21_sweet_drm_register_import(impfd, uva, ilen) == 0);
	uint32_t ih = 0;
	CHECK(drmPrimeFDToHandle(fd, impfd, &ih) == 0);
	CHECK(fake.imports == 1);
	CHECK(fake.last_import_uva == uva);
	CHECK(fake.last_import_len == ilen);
	CHECK(fake.last_import_va >= 0x30000000ULL);
	CHECK(drmCloseBufferHandle(fd, ih) == 0);
	s21_sweet_drm_unregister_import(impfd);
	close(impfd);

	/* Submits are synchronous; a signalled syncobj is ready immediately. */
	uint32_t so = 0, so2 = 0;
	CHECK(drmSyncobjCreate(fd, DRM_SYNCOBJ_CREATE_SIGNALED, &so) == 0);
	CHECK(drmSyncobjWait(fd, &so, 1, 0, 0, NULL) == 0);
	int sofd = -1;
	CHECK(drmSyncobjExportSyncFile(fd, so, &sofd) == 0);
	CHECK(drmSyncobjFDToHandle(fd, sofd, &so2) == 0);
	CHECK(drmSyncobjWait(fd, &so2, 1, 0, 0, NULL) == 0);
	CHECK(drmSyncobjTransfer(fd, so2, 0, so, 0, 0) == 0);
	CHECK(drmSyncobjDestroy(fd, so2) == 0);
	CHECK(drmSyncobjDestroy(fd, so) == 0);
	close(sofd);

	CHECK(s21_sweet_drm_create_calls() == 10);
	if (failures == 0) {
		printf("PASS: S21 SweetMali/Panfrost shim host test\n");
		return 0;
	}
	printf("FAIL: %u checks failed\n", failures);
	return 1;
}
