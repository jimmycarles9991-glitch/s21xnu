/*
 * s21_sweet_drm: libdrm + Panfrost emulation over the S21 SweetMali user client.
 *
 * The kernel ABI is documented in s21_sweet_drm.h. In short: the client picks
 * the GPU VA and calls MAP_BO, SUBMIT is synchronous, and a single kernel BO
 * is capped at 15 MiB. This shim keeps its own DRM handle table so a large
 * Mesa allocation (notably the 128 MiB tiler heap) can be split into several
 * adjacent kernel BOs. No PixOS selectors or hand-written SweetMetal code are
 * involved.
 *
 * Build with the fake libdrm/darwin_drm headers from
 * userland/patches/mesa-26.2.5 (patches 0002-0004), or compile this file into
 * the final embedding dylib together with Mesa's panfrost libraries.
 */
#ifdef S21_DRM_HOST_TEST
#define _GNU_SOURCE
#include "fake_iokit.h"
#else
#include <IOKit/IOKitLib.h>
/* kIOMainPortDefault is available from iOS 16 / macOS 12, which this shim targets.
 * Do not alias it to kIOMasterPortDefault: that symbol is unavailable on iOS. */
#ifndef kIOMapAnywhere
#define kIOMapAnywhere 1u
#endif
#ifndef kIOMapWriteCombineCache
#define kIOMapWriteCombineCache 0x400u
#endif
#include <mach/mach.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "xf86drm.h"
#include "drm-uapi/panfrost_drm.h"
#include "darwin_drm.h"
#include "s21_sweet_drm.h"

/* --- kernel ABI --------------------------------------------------------- */

enum {
	S21_GET_INFO = 0,
	S21_CREATE_BO = 1,
	S21_MAP_BO = 2,
	S21_SUBMIT = 3,
	S21_DESTROY_BO = 4,
	S21_LOG = 5,
	S21_MAPFB = 6,
	S21_IMPORT_USER = 8,
	S21_KEEP = 9,
};

#define S21_BO_MAX_BYTES (15ULL << 20) /* SM_MAX_BO_BYTES in 0112 */
#define S21_VA_BASE      0x30000000ULL
#define S21_VA_LIMIT     0x70000000ULL /* keep every VA below 2 GiB */
#define S21_MAX_CHUNKS   64
#define NOFENCE          UINT64_MAX

/* GET_INFO only carries six words. The remaining feature values below are
 * from the measured panparams dump on this phone (Linux Panfrost, 2026-10-16,
 * recorded in /home/tom/s21/S21_Exynos2100.md section 11.21) or conservative
 * Mesa drm-shim values where that dump had nothing. They are constants, not
 * claims about a kernel uAPI the S21 user client does not expose. */
#define G78_TEXTURE0 0xf7fe007eULL
#define G78_TEXTURE1 0xc3fff7ffULL
#define G78_TEXTURE2 0xbfe1ff9fULL
#define G78_TEXTURE3 0x000010c6ULL
#define G78_MMU      0x2830ULL
#define G78_THREAD   0x48000ULL
#define G78_MAX_THR  0x400ULL
#define G78_WG       0x400ULL
#define G78_BARRIER  0x400ULL
#define G78_TILER    0x809ULL  /* 512-byte bins, 8 levels (Mesa default) */
#define G78_MEM      0x301ULL  /* 4 L2 slices */
#define G78_L2       0x80000000ULL /* 256-byte bus width; conservative */
#define G78_AFBC     0ULL      /* no AFBC until verified on this driver */

struct s21_info {
	uint32_t gpu_id, shader_present, tiler_present, l2_present, as_present, js_present;
};

struct bo {
	bool used;
	uint32_t k[S21_MAX_CHUNKS]; /* kernel BO handles */
	uint32_t nk;
	uint64_t va, size, map;
	bool heap, imported, noexec;
	uint64_t seq;               /* last submit that listed this handle */
};

struct sobj {
	bool used;
	uint64_t seq;
};

enum { FD_NONE, FD_SYNC, FD_IMPORT };
struct fdrec {
	uint8_t kind;
	uint64_t seq, uva, len;
};

struct va_seg {
	uint64_t lo, hi;
	bool used;
};

static struct {
	pthread_mutex_t lock;
	bool up;
	bool cfg_init_done;
	io_connect_t conn;
	int dev_fd;
	unsigned create_calls;
	uint32_t last_seq, done_seq;
	struct s21_info info;
	struct bo *bos;
	uint32_t nbos;
	struct sobj *sobjs;
	uint32_t nsobjs;
	struct fdrec *fds;
	uint32_t nfds;
	struct va_seg va[64];
	uint32_t nva;
	uint32_t slot, timeout_ms;
} C = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.dev_fd = -1,
	.slot = 0,
	.timeout_ms = 5000,
};

/* --- small helpers ------------------------------------------------------ */

static int
fail(int e)
{
	errno = e;
	return -1;
}

static int
res(int neg_errno)
{
	return neg_errno ? fail(-neg_errno) : 0;
}

static int
kr_errno(kern_return_t kr)
{
	switch (kr) {
	case kIOReturnSuccess: return 0;
	case kIOReturnBadArgument: return EINVAL;
	case kIOReturnNoMemory:
	case kIOReturnNoResources: return ENOMEM;
	case kIOReturnNotFound: return ENOENT;
	case kIOReturnBusy: return EBUSY;
	case kIOReturnTimeout: return ETIMEDOUT;
	case kIOReturnUnsupported: return ENOSYS;
	default: return EIO;
	}
}

static uint64_t
align_page(uint64_t v)
{
	return (v + 4095) & ~4095ULL;
}

static void
cfg_init(void)
{
	const char *e;

	e = getenv("S21_DRM_SLOT");
	if (e && e[0]) {
		unsigned long v = strtoul(e, NULL, 0);
		if (v <= 2) {
			C.slot = (uint32_t)v;
		} else {
			fprintf(stderr, "S21DRM: bad S21_DRM_SLOT=%s, using 0\n", e);
		}
	}
	e = getenv("S21_DRM_TIMEOUT_MS");
	if (e && e[0]) {
		unsigned long v = strtoul(e, NULL, 0);
		if (v >= 1 && v <= 5000) {
			C.timeout_ms = (uint32_t)v;
		} else {
			fprintf(stderr, "S21DRM: bad S21_DRM_TIMEOUT_MS=%s, using 5000\n", e);
		}
	}
}

/* --- GPU VA allocator --------------------------------------------------- */

static int
va_init(void)
{
	if (C.nva != 0) {
		return 0;
	}
	C.va[0] = (struct va_seg){ .lo = S21_VA_BASE, .hi = S21_VA_LIMIT };
	C.nva = 1;
	return 0;
}

/* Lock held. First fit, page-aligned (all allocations are page multiples). */
static int
va_alloc(uint64_t size, uint64_t *out)
{
	va_init();
	if (size == 0 || (size & 4095)) {
		return -EINVAL;
	}
	for (uint32_t i = 0; i < C.nva; i++) {
		struct va_seg *s = &C.va[i];
		if (s->used || s->hi - s->lo < size) {
			continue;
		}
		uint64_t start = s->lo;
		if (s->hi - s->lo == size) {
			s->used = true;
		} else {
			s->lo += size;
			if (C.nva >= sizeof(C.va) / sizeof(C.va[0])) {
				return -ENOMEM;
			}
			C.va[C.nva++] = (struct va_seg){ .lo = start, .hi = start + size, .used = true };
		}
		*out = start;
		return 0;
	}
	return -ENOMEM;
}

static void
va_free(uint64_t start, uint64_t size)
{
	uint64_t end = start + size;
	for (uint32_t i = 0; i < C.nva; i++) {
		if (C.va[i].used && C.va[i].lo == start && C.va[i].hi == end) {
			C.va[i].used = false;
			break;
		}
	}
	/* merge adjacent free segments */
	for (bool again = true; again;) {
		again = false;
		for (uint32_t i = 0; i + 1 < C.nva; i++) {
			if (!C.va[i].used && !C.va[i + 1].used && C.va[i].hi == C.va[i + 1].lo) {
				C.va[i].hi = C.va[i + 1].hi;
				memmove(&C.va[i + 1], &C.va[i + 2],
				    (C.nva - i - 2) * sizeof(C.va[0]));
				C.nva--;
				again = true;
				break;
			}
			/* A freed segment can sit before the free remainder: [lo,hi]
			 * was carved out of the front of [hi, end]. */
			if (!C.va[i].used && !C.va[i + 1].used && C.va[i].lo == C.va[i + 1].hi) {
				C.va[i + 1].hi = C.va[i].hi;
				memmove(&C.va[i], &C.va[i + 1],
				    (C.nva - i - 1) * sizeof(C.va[0]));
				C.nva--;
				again = true;
				break;
			}
		}
	}
}

/* --- table helpers ------------------------------------------------------ */

static int
grow(void **tab, uint32_t *n, size_t esz, uint32_t idx)
{
	if (idx < *n) {
		return 0;
	}
	uint32_t m = *n ? *n : 16;
	while (m <= idx) {
		m *= 2;
	}
	void *p = realloc(*tab, (size_t)m * esz);
	if (!p) {
		return -ENOMEM;
	}
	memset((char *)p + (size_t)*n * esz, 0, (size_t)(m - *n) * esz);
	*tab = p;
	*n = m;
	return 0;
}

static struct bo *
bo_get(uint32_t h)
{
	return h >= 1 && h <= C.nbos && C.bos[h - 1].used ? &C.bos[h - 1] : NULL;
}

static int
bo_add(struct bo *b, uint32_t *h)
{
	for (uint32_t i = 0; i < C.nbos; i++) {
		if (!C.bos[i].used) {
			C.bos[i] = *b;
			*h = i + 1;
			return 0;
		}
	}
	uint32_t idx = C.nbos;
	int e = grow((void **)&C.bos, &C.nbos, sizeof(*C.bos), idx);
	if (e) {
		return e;
	}
	C.bos[idx] = *b;
	C.nbos = idx + 1;
	*h = idx + 1;
	return 0;
}

static struct sobj *
so_get(uint32_t h)
{
	return h >= 1 && h <= C.nsobjs && C.sobjs[h - 1].used ? &C.sobjs[h - 1] : NULL;
}

static int
so_new(uint64_t seq, uint32_t *h)
{
	for (uint32_t i = 0; i < C.nsobjs; i++) {
		if (!C.sobjs[i].used) {
			C.sobjs[i] = (struct sobj){ .used = true, .seq = seq };
			*h = i + 1;
			return 0;
		}
	}
	uint32_t idx = C.nsobjs;
	int e = grow((void **)&C.sobjs, &C.nsobjs, sizeof(*C.sobjs), idx);
	if (e) {
		return e;
	}
	C.sobjs[idx] = (struct sobj){ .used = true, .seq = seq };
	C.nsobjs = idx + 1;
	*h = idx + 1;
	return 0;
}

static struct fdrec *
fd_get(int fd, uint8_t kind)
{
	if (fd < 0 || (uint32_t)fd >= C.nfds) {
		return NULL;
	}
	struct fdrec *r = &C.fds[fd];
	return r->kind == kind ? r : NULL;
}

static int
fd_new(uint8_t kind, uint64_t seq, int *out)
{
	int fd = dup(C.dev_fd);
	if (fd < 0) {
		return -errno;
	}
	if ((uint32_t)fd >= C.nfds) {
		int e = grow((void **)&C.fds, &C.nfds, sizeof(*C.fds), (uint32_t)fd);
		if (e) {
			close(fd);
			return e;
		}
	}
	C.fds[fd] = (struct fdrec){ .kind = kind, .seq = seq };
	*out = fd;
	return 0;
}

/* --- kernel calls ------------------------------------------------------- */

static int
kcall(uint32_t sel, const uint64_t *in, uint32_t nin,
      uint64_t *out, uint32_t *nout)
{
	uint32_t n = nout ? *nout : 0;
	kern_return_t kr = IOConnectCallMethod(C.conn, sel, in, nin,
	    NULL, 0, out, nout ? &n : NULL, NULL, NULL);
	if (kr != kIOReturnSuccess) {
		return -kr_errno(kr);
	}
	if (nout && n != *nout) {
		return -EIO;
	}
	return 0;
}

/* Lock held. */
static int
ensure_ctx(void)
{
	if (C.up) {
		return 0;
	}
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
	    IOServiceMatching("SweetMali"));
	if (!svc) {
		return -ENODEV;
	}
	io_connect_t conn = 0;
	kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
	IOObjectRelease(svc);
	if (kr != kIOReturnSuccess) {
		return kr == kIOReturnBusy ? -EBUSY : -ENODEV;
	}
	struct s21_info info;
	size_t isz = sizeof(info);
	memset(&info, 0, sizeof(info));
	kr = IOConnectCallMethod(conn, S21_GET_INFO, NULL, 0, NULL, 0,
	    NULL, NULL, &info, &isz);
	int fd;
	if (kr != kIOReturnSuccess || isz != sizeof(info) || info.gpu_id == 0) {
		IOServiceClose(conn);
		return kr == kIOReturnSuccess ? -EIO : -kr_errno(kr);
	}
	fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		IOServiceClose(conn);
		return -errno;
	}
	C.conn = conn;
	C.dev_fd = fd;
	C.info = info;
	C.up = true;
	fprintf(stderr, "S21DRM: GPU id 0x%08x shader 0x%08x tiler %u l2 %u as 0x%x js 0x%x\n",
	    info.gpu_id, info.shader_present, info.tiler_present,
	    info.l2_present, info.as_present, info.js_present);
	return 0;
}

/* Returns 0 with the lock held, or -errno with it released. */
static int
enter(void)
{
	pthread_mutex_lock(&C.lock);
	if (!C.cfg_init_done) {
		cfg_init();
		C.cfg_init_done = true;
	}
	return ensure_ctx();
}

static void
leave(void)
{
	pthread_mutex_unlock(&C.lock);
}

/* --- BO creation / destruction ----------------------------------------- */

/* Lock held. Create one kernel BO and map it at `va`. */
static int
create_chunk(uint64_t size, bool exec, uint64_t va, uint32_t *kh)
{
	uint64_t in[2] = { size, exec ? 1 : 0 }, out[2];
	uint32_t n = 2;
	int e = kcall(S21_CREATE_BO, in, 2, out, &n);
	if (e) {
		return e;
	}
	if (out[0] == 0 || out[0] > UINT32_MAX) {
		return -EIO;
	}
	uint32_t h = (uint32_t)out[0];
	C.create_calls++;
	uint64_t mapin[2] = { h, va };
	kern_return_t kr = IOConnectCallMethod(C.conn, S21_MAP_BO, mapin, 2,
	    NULL, 0, NULL, NULL, NULL, NULL);
	if (kr != kIOReturnSuccess) {
		e = -kr_errno(kr);
		uint64_t hin = h;
		(void)kcall(S21_DESTROY_BO, &hin, 1, NULL, NULL);
		return e;
	}
	*kh = h;
	return 0;
}

/* Lock held. */
static int
create_bo(struct drm_panfrost_create_bo *r)
{
	const uint32_t known = PANFROST_BO_NOEXEC | PANFROST_BO_HEAP | PANFROST_BO_WB_MMAP;
	if (r->size == 0 || r->pad || (r->flags & ~known) ||
	    ((r->flags & PANFROST_BO_HEAP) && (r->flags & PANFROST_BO_WB_MMAP))) {
		return -EINVAL;
	}
	uint64_t size = align_page(r->size);
	uint32_t nk = (uint32_t)((size + S21_BO_MAX_BYTES - 1) / S21_BO_MAX_BYTES);
	if (nk == 0 || nk > S21_MAX_CHUNKS) {
		return -ENOMEM;
	}
	struct bo b = { .used = true, .size = size, .nk = nk,
	                .heap = (r->flags & PANFROST_BO_HEAP) != 0,
	                .noexec = (r->flags & PANFROST_BO_NOEXEC) != 0 };
	int e = va_alloc(size, &b.va);
	if (e) {
		return e;
	}
	for (uint32_t i = 0; i < nk; i++) {
		uint64_t off = (uint64_t)i * S21_BO_MAX_BYTES;
		uint64_t chunk = size - off;
		if (chunk > S21_BO_MAX_BYTES) {
			chunk = S21_BO_MAX_BYTES;
		}
		e = create_chunk(chunk, !b.noexec, b.va + off, &b.k[i]);
		if (e) {
			for (uint32_t j = 0; j < i; j++) {
				uint64_t h = b.k[j];
				(void)kcall(S21_DESTROY_BO, &h, 1, NULL, NULL);
			}
			va_free(b.va, b.size);
			return e;
		}
	}
	uint32_t h;
	e = bo_add(&b, &h);
	if (e) {
		for (uint32_t i = 0; i < nk; i++) {
			uint64_t kh = b.k[i];
			(void)kcall(S21_DESTROY_BO, &kh, 1, NULL, NULL);
		}
		va_free(b.va, b.size);
		return e;
	}
	r->handle = h;
	r->size = (uint32_t)size;
	r->offset = b.va;
	return 0;
}

/* Lock held. */
static int
close_bo(uint32_t h)
{
	struct bo *b = bo_get(h);
	if (!b) {
		return -EINVAL;
	}
	int ret = 0;
	if (b->map) {
		kern_return_t kr = IOConnectUnmapMemory64(C.conn, b->k[0],
		    mach_task_self(), b->map);
		if (kr == kIOReturnSuccess) {
			b->map = 0;
		} else {
			ret = -kr_errno(kr);
		}
	}
	for (uint32_t i = 0; i < b->nk; i++) {
		uint64_t kh = b->k[i];
		int e = kcall(S21_DESTROY_BO, &kh, 1, NULL, NULL);
		if (e && !ret) {
			ret = e;
		}
	}
	if (!ret) {
		va_free(b->va, b->size);
		b->used = false;
	}
	return ret;
}

/* Lock held. */
static int
submit(struct drm_panfrost_submit *r)
{
	const uint32_t known = PANFROST_JD_REQ_FS | PANFROST_JD_REQ_CYCLE_COUNT;
	if (r->jc == 0 || r->pad || (r->requirements & ~known)) {
		return -EINVAL;
	}
	if (r->out_sync && !so_get(r->out_sync)) {
		return -ENOENT;
	}
	uint64_t in[3] = { C.slot, r->jc, C.timeout_ms }, out[1] = { 0 };
	uint32_t n = 1;
	int e = kcall(S21_SUBMIT, in, 3, out, &n);
	if (e) {
		return e;
	}
	if (out[0] != 0) {
		fprintf(stderr, "S21DRM: submit status 0x%llx jc 0x%llx\n",
		    (unsigned long long)out[0], (unsigned long long)r->jc);
		return -EIO;
	}
	C.last_seq++;
	C.done_seq = C.last_seq;
	if (r->bo_handles) {
		const uint32_t *hs = (const uint32_t *)(uintptr_t)r->bo_handles;
		uint32_t nbo = r->bo_handle_count;
		if (nbo > 4096) {
			nbo = 4096;
		}
		for (uint32_t i = 0; i < nbo; i++) {
			struct bo *b = bo_get(hs[i]);
			if (b) {
				b->seq = C.last_seq;
			}
		}
	}
	if (r->out_sync) {
		so_get(r->out_sync)->seq = C.last_seq;
	}
	return 0;
}

/* --- GET_PARAM ---------------------------------------------------------- */

/* Lock held. */
static int
get_param(struct drm_panfrost_get_param *p)
{
	const struct s21_info *i = &C.info;
	uint64_t v;

	if (p->pad) {
		return -EINVAL;
	}
	switch (p->param) {
	case DRM_PANFROST_PARAM_GPU_PROD_ID: v = i->gpu_id >> 16; break;
	case DRM_PANFROST_PARAM_GPU_REVISION: v = i->gpu_id & 0xffff; break;
	case DRM_PANFROST_PARAM_SHADER_PRESENT: v = i->shader_present; break;
	case DRM_PANFROST_PARAM_TILER_PRESENT: v = i->tiler_present; break;
	case DRM_PANFROST_PARAM_L2_PRESENT: v = i->l2_present; break;
	case DRM_PANFROST_PARAM_AS_PRESENT: v = i->as_present; break;
	case DRM_PANFROST_PARAM_JS_PRESENT: v = i->js_present; break;
	case DRM_PANFROST_PARAM_L2_FEATURES: v = G78_L2; break;
	case DRM_PANFROST_PARAM_TILER_FEATURES: v = G78_TILER; break;
	case DRM_PANFROST_PARAM_MEM_FEATURES: v = G78_MEM; break;
	case DRM_PANFROST_PARAM_MMU_FEATURES: v = G78_MMU; break;
	case DRM_PANFROST_PARAM_THREAD_FEATURES: v = G78_THREAD; break;
	case DRM_PANFROST_PARAM_MAX_THREADS: v = G78_MAX_THR; break;
	case DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ: v = G78_WG; break;
	case DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ: v = G78_BARRIER; break;
	case DRM_PANFROST_PARAM_TEXTURE_FEATURES0: v = G78_TEXTURE0; break;
	case DRM_PANFROST_PARAM_TEXTURE_FEATURES1: v = G78_TEXTURE1; break;
	case DRM_PANFROST_PARAM_TEXTURE_FEATURES2: v = G78_TEXTURE2; break;
	case DRM_PANFROST_PARAM_TEXTURE_FEATURES3: v = G78_TEXTURE3; break;
	case DRM_PANFROST_PARAM_AFBC_FEATURES: v = G78_AFBC; break;
	case DRM_PANFROST_PARAM_ALLOWED_JM_CTX_PRIORITIES:
		v = 1ULL << PANFROST_JM_CTX_PRIORITY_MEDIUM;
		break;
	case DRM_PANFROST_PARAM_SELECTED_COHERENCY:
		v = DRM_PANFROST_GPU_COHERENCY_NONE;
		break;
	default:
		return -EINVAL;
	}
	p->value = v;
	return 0;
}

/* --- libdrm entry points ------------------------------------------------ */

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	if (!arg) {
		return fail(EFAULT);
	}
	int e = enter();
	if (e) {
		return fail(-e);
	}
	if (request == DRM_IOCTL_GEM_CLOSE) {
		e = close_bo(((struct drm_gem_close *)arg)->handle);
		leave();
		return res(e);
	}
	if (((request >> 8) & 0xff) != DRM_IOCTL_BASE ||
	    (request & 0xff) < DRM_COMMAND_BASE ||
	    (request & 0xff) >= DRM_COMMAND_END) {
		leave();
		return fail(EINVAL);
	}
	switch ((request & 0xff) - DRM_COMMAND_BASE) {
	case DRM_PANFROST_GET_PARAM:
		e = get_param(arg);
		break;
	case DRM_PANFROST_CREATE_BO:
		e = create_bo(arg);
		break;
	case DRM_PANFROST_GET_BO_OFFSET: {
		struct drm_panfrost_get_bo_offset *r = arg;
		struct bo *b = bo_get(r->handle);
		e = r->pad ? -EINVAL : !b ? -ENOENT : 0;
		if (!e) {
			r->offset = b->va;
		}
		break;
	}
	case DRM_PANFROST_MMAP_BO: {
		struct drm_panfrost_mmap_bo *r = arg;
		struct bo *b = bo_get(r->handle);
		e = r->flags ? -EINVAL : !b ? -ENOENT : b->heap ? -EINVAL : 0;
		if (!e) {
			r->offset = (uint64_t)r->handle << 32;
		}
		break;
	}
	case DRM_PANFROST_WAIT_BO: {
		struct drm_panfrost_wait_bo *r = arg;
		e = r->pad ? -EINVAL : bo_get(r->handle) ? 0 : -ENOENT;
		/* SUBMIT is synchronous; every submitted seqno is already done. */
		break;
	}
	case DRM_PANFROST_SUBMIT:
		e = submit(arg);
		break;
	case DRM_PANFROST_MADVISE: {
		struct drm_panfrost_madvise *r = arg;
		e = r->madv > PANFROST_MADV_DONTNEED ? -EINVAL :
		    bo_get(r->handle) ? 0 : -ENOENT;
		if (!e) {
			r->retained = 1;
		}
		break;
	}
	case DRM_PANFROST_SYNC_BO:
	case DRM_PANFROST_QUERY_BO_INFO:
	case DRM_PANFROST_SET_LABEL_BO:
	case DRM_PANFROST_JM_CTX_CREATE:
	case DRM_PANFROST_JM_CTX_DESTROY:
	case DRM_PANFROST_PERFCNT_ENABLE:
	case DRM_PANFROST_PERFCNT_DUMP:
		e = -ENOSYS;
		break;
	default:
		e = -EINVAL;
		break;
	}
	leave();
	return res(e);
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close c = { .handle = handle };
	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
}

drmVersionPtr
drmGetVersion(int fd)
{
	(void)fd;
	int e = enter();
	if (e) {
		errno = -e;
		return NULL;
	}
	leave();
	static const char name[] = "panfrost", desc[] = "S21 SweetMali / Panfrost";
	drmVersionPtr v = calloc(1, sizeof(*v) + sizeof(name) + sizeof(desc));
	if (!v) {
		return NULL;
	}
	char *p = (char *)(v + 1);
	v->version_major = 1;
	v->version_minor = 2; /* no coherency/timestamp queries on this client */
	v->version_patchlevel = 0;
	v->name = p;
	v->name_len = (int)(sizeof(name) - 1);
	memcpy(p, name, sizeof(name));
	p += sizeof(name);
	v->desc = p;
	v->desc_len = (int)(sizeof(desc) - 1);
	memcpy(p, desc, sizeof(desc));
	return v;
}

void
drmFreeVersion(drmVersionPtr v)
{
	free(v);
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	if (value) {
		*value = 0;
	}
	return fail(EINVAL);
}

/* --- BO CPU map hooks (Mesa patch 0004) -------------------------------- */

void *
darwin_drm_bo_map(int dev_fd, size_t size, int prot, int flags, uint64_t offset)
{
	(void)dev_fd;
	(void)prot;
	(void)flags;
	uint32_t h = (uint32_t)(offset >> 32);
	if ((offset & 0xffffffffULL) != 0 || enter()) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	struct bo *b = bo_get(h);
	void *ret = MAP_FAILED;
	if (!b || b->heap || b->nk != 1 || size > b->size) {
		errno = EINVAL;
	} else if (b->map) {
		ret = (void *)(uintptr_t)b->map;
	} else {
		mach_vm_address_t addr = 0;
		mach_vm_size_t sz = 0;
		kern_return_t kr = IOConnectMapMemory64(C.conn, b->k[0],
		    mach_task_self(), &addr, &sz,
		    kIOMapAnywhere | kIOMapWriteCombineCache);
		if (kr != kIOReturnSuccess) {
			errno = kr_errno(kr);
		} else if (sz < size) {
			(void)IOConnectUnmapMemory64(C.conn, b->k[0],
			    mach_task_self(), addr);
			errno = EINVAL;
		} else {
			b->map = addr;
			ret = (void *)(uintptr_t)addr;
		}
	}
	leave();
	return ret;
}

int
darwin_drm_bo_unmap(void *addr, size_t size)
{
	(void)size;
	if (!addr || enter()) {
		return fail(EINVAL);
	}
	int ret = fail(EINVAL);
	for (uint32_t i = 0; i < C.nbos; i++) {
		struct bo *b = &C.bos[i];
		if (b->used && b->map == (uint64_t)(uintptr_t)addr) {
			kern_return_t kr = IOConnectUnmapMemory64(C.conn,
			    b->k[0], mach_task_self(), b->map);
			ret = kr == kIOReturnSuccess ? 0 : fail(kr_errno(kr));
			if (!ret) {
				b->map = 0;
			}
			break;
		}
	}
	leave();
	return ret;
}

/* --- user-memory import ------------------------------------------------- */

/* Lock held. */
static int
import_user_locked(uint64_t uva, uint64_t len, uint32_t *handle, uint64_t *gpu_va)
{
	if (uva == 0 || len == 0 || (uva & 4095) || (len & 4095) ||
	    len > S21_BO_MAX_BYTES) {
		return -EINVAL;
	}
	uint64_t va;
	int e = va_alloc(len, &va);
	if (e) {
		return e;
	}
	uint64_t in[3] = { uva, len, va }, out[1] = { 0 };
	uint32_t n = 1;
	e = kcall(S21_IMPORT_USER, in, 3, out, &n);
	if (e) {
		va_free(va, len);
		return e;
	}
	if (out[0] == 0 || out[0] > UINT32_MAX) {
		va_free(va, len);
		return -EIO;
	}
	struct bo b = {
		.used = true, .nk = 1, .va = va, .size = len,
		.imported = true,
	};
	b.k[0] = (uint32_t)out[0];
	e = bo_add(&b, handle);
	if (e) {
		uint64_t h = b.k[0];
		(void)kcall(S21_DESTROY_BO, &h, 1, NULL, NULL);
		va_free(va, len);
		return e;
	}
	if (gpu_va) {
		*gpu_va = va;
	}
	return 0;
}

int
s21_sweet_drm_import_user(uint64_t uva, uint64_t len,
                          uint32_t *handle, uint64_t *gpu_va)
{
	int e = enter();
	if (e) {
		return e;
	}
	e = import_user_locked(uva, len, handle, gpu_va);
	leave();
	return e;
}

int
s21_sweet_drm_register_import(int fd, uint64_t uva, uint64_t len)
{
	int e = enter();
	if (e) {
		return e;
	}
	if ((uint32_t)fd >= C.nfds) {
		e = grow((void **)&C.fds, &C.nfds, sizeof(*C.fds), (uint32_t)fd);
	}
	if (!e) {
		C.fds[fd] = (struct fdrec){ .kind = FD_IMPORT, .uva = uva, .len = len };
	}
	leave();
	return e;
}

void
s21_sweet_drm_unregister_import(int fd)
{
	pthread_mutex_lock(&C.lock);
	struct fdrec *r = fd_get(fd, FD_IMPORT);
	if (r) {
		memset(r, 0, sizeof(*r));
	}
	pthread_mutex_unlock(&C.lock);
}

/* --- boot framebuffer solid-colour painter ------------------------------ */

/*
 * Selector 6 (kSMMapFB in 0112-s21-mali-iokit.sh) wraps the physical scanout
 * buffer as a normal kernel BO and returns its handle. Mapping that handle
 * with IOConnectMapMemory64 gives the CPU the very pixels the panel reads, so
 * a result can be painted without the flaky GPU-present path. `color` uses
 * the framebuffer's in-memory byte order: 0xFF00FF00 is green, 0xFF0000FF is
 * red. With hold != 0 this repaints forever and never returns, so a GUI
 * redraw cannot cover the result.
 */
int
s21_sweet_fb_paint(uint32_t color, int hold)
{
	int e = enter();
	if (e) {
		errno = -e;
		return -1;
	}

	uint64_t out[4] = { 0, 0, 0, 0 };
	uint32_t nout = 4;
	int r = kcall(S21_MAPFB, NULL, 0, out, &nout);
	if (r) {
		leave();
		errno = -r;
		return -1;
	}
	if (nout != 4 || out[0] == 0 || out[1] == 0 || out[2] == 0) {
		leave();
		errno = EIO;
		return -1;
	}

	const uint32_t handle = (uint32_t)out[0];
	const uint32_t rowbytes = (uint32_t)out[2];
	const uint32_t width = (uint32_t)(out[3] >> 16);
	const uint32_t height = (uint32_t)(out[3] & 0xffffu);
	const uint64_t bytes = rowbytes * (uint64_t)height;
	if (width == 0 || height == 0 || rowbytes < width * 4u) {
		leave();
		errno = EINVAL;
		return -1;
	}

	mach_vm_address_t addr = 0;
	mach_vm_size_t sz = 0;
	kern_return_t kr = IOConnectMapMemory64(C.conn, handle, mach_task_self(),
	    &addr, &sz, kIOMapAnywhere | kIOMapWriteCombineCache);
	if (kr != kIOReturnSuccess) {
		leave();
		errno = kr_errno(kr);
		return -1;
	}
	if (sz < bytes) {
		(void)IOConnectUnmapMemory64(C.conn, handle, mach_task_self(), addr);
		leave();
		errno = EIO;
		return -1;
	}

	volatile uint32_t *px = (volatile uint32_t *)(uintptr_t)addr;
	const uint32_t words_per_row = rowbytes / 4u;
	for (;;) {
		for (uint32_t y = 0; y < height; y++) {
			volatile uint32_t *row = px + (size_t)y * words_per_row;
			for (uint32_t x = 0; x < width; x++)
				row[x] = color;
		}
		if (!hold)
			break;
		usleep(50000);
	}

	(void)IOConnectUnmapMemory64(C.conn, handle, mach_task_self(), addr);
	leave();
	return 0;
}

/*
 * Mirror a short result line into the kernel's SweetMali log (selector 5)
 * and its keep buffer (selector 9, replayed into pmsg before a recovery
 * reboot). Unlike launchd stdout this does not depend on /dev/console
 * routing, so the PASS/FAIL text survives even when stdout is discarded.
 */
void
s21_sweet_log_line(const char *line)
{
	if (!line || !*line)
		return;
	int e = enter();
	if (e)
		return;
	size_t n = strlen(line);
	if (n > 150)
		n = 150;
	(void)IOConnectCallMethod(C.conn, S21_LOG, NULL, 0, line, n,
	    NULL, NULL, NULL, NULL);
	(void)IOConnectCallMethod(C.conn, S21_KEEP, NULL, 0, line, n,
	    NULL, NULL, NULL, NULL);
	leave();
}

int
s21_sweet_drm_open(void)
{
	int e = enter();
	if (e) {
		leave();
		errno = -e;
		return -1;
	}
	int fd = dup(C.dev_fd);
	leave();
	if (fd < 0) {
		errno = EIO;
	}
	return fd;
}

unsigned
s21_sweet_drm_create_calls(void)
{
	return C.create_calls;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	(void)fd;
	int e = enter();
	if (e) {
		return e;
	}
	struct fdrec *r = fd_get(prime_fd, FD_IMPORT);
	if (!r) {
		leave();
		return -EINVAL;
	}
	e = import_user_locked(r->uva, r->len, handle, NULL);
	leave();
	return e;
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd; (void)handle; (void)flags; (void)prime_fd;
	return -ENOSYS;
}

/* --- syncobj emulation -------------------------------------------------- */

/* All S21 submits are synchronous, so sequence numbers complete immediately. */
static int
so_ready(uint64_t seq)
{
	if (seq == NOFENCE) {
		return 0;
	}
	return seq <= C.done_seq;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	if (flags & ~DRM_SYNCOBJ_CREATE_SIGNALED) {
		return fail(EINVAL);
	}
	int e = enter();
	if (e) {
		return e;
	}
	e = so_new(flags & DRM_SYNCOBJ_CREATE_SIGNALED ? 0 : NOFENCE, handle);
	leave();
	return e;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	int e = enter();
	if (e) {
		return e;
	}
	struct sobj *s = so_get(handle);
	if (!s) {
		leave();
		return fail(EINVAL);
	}
	s->used = false;
	leave();
	return 0;
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	(void)fd;
	int e = enter();
	if (e) {
		return e;
	}
	struct sobj *s = so_get(handle);
	if (!s) {
		leave();
		return fail(EINVAL);
	}
	e = fd_new(FD_SYNC, s->seq, obj_fd);
	leave();
	return e;
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)fd;
	int e = enter();
	if (e) {
		return e;
	}
	struct fdrec *r = fd_get(obj_fd, FD_SYNC);
	if (!r) {
		leave();
		return fail(EINVAL);
	}
	e = so_new(r->seq, handle);
	leave();
	return e;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd;
	int e = enter();
	if (e) {
		return e;
	}
	struct sobj *s = so_get(handle);
	struct fdrec *r = fd_get(sync_file_fd, FD_SYNC);
	if (!s || !r) {
		leave();
		return fail(EINVAL);
	}
	s->seq = r->seq;
	leave();
	return 0;
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	(void)fd;
	int e = enter();
	if (e) {
		return e;
	}
	struct sobj *s = so_get(handle);
	if (!s || s->seq == NOFENCE) {
		leave();
		return fail(EINVAL);
	}
	e = fd_new(FD_SYNC, s->seq, sync_file_fd);
	leave();
	return e;
}

static int
wait_handles(uint32_t *handles, uint64_t *points, unsigned n,
             int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	(void)timeout_nsec;
	if (n == 0) {
		return fail(EINVAL);
	}
	if (first_signaled) {
		*first_signaled = 0;
	}
	int e = enter();
	if (e) {
		return e;
	}
	int ret = 0;
	for (unsigned i = 0; i < n; i++) {
		struct sobj *s = so_get(handles[i]);
		if (!s) {
			ret = fail(EINVAL);
			break;
		}
		uint64_t seq = points ? points[i] : s->seq;
		if (points && points[i] != 0) {
			ret = fail(EINVAL); /* binary syncobjs only */
			break;
		}
		if (so_ready(seq)) {
			if (first_signaled && (*first_signaled == 0 ||
			    !(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL))) {
				*first_signaled = handles[i];
			}
			if (!(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL)) {
				break;
			}
		} else if (!(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE)) {
			ret = fail(ETIMEDOUT);
			break;
		}
	}
	leave();
	return ret;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
               int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	(void)fd;
	return wait_handles(handles, NULL, num_handles, timeout_nsec, flags, first_signaled);
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points,
                       unsigned num_handles, int64_t timeout_nsec,
                       unsigned flags, uint32_t *first_signaled)
{
	(void)fd;
	return wait_handles(handles, points, num_handles, timeout_nsec, flags, first_signaled);
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
	(void)fd;
	int e = enter();
	if (e) {
		return e;
	}
	int ret = 0;
	for (uint32_t i = 0; i < handle_count; i++) {
		struct sobj *s = so_get(handles[i]);
		if (!s) {
			ret = fail(EINVAL);
			break;
		}
		s->seq = NOFENCE;
	}
	leave();
	return ret;
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
	(void)fd;
	int e = enter();
	if (e) {
		return e;
	}
	int ret = 0;
	for (uint32_t i = 0; i < handle_count; i++) {
		struct sobj *s = so_get(handles[i]);
		if (!s) {
			ret = fail(EINVAL);
			break;
		}
		s->seq = 0;
	}
	leave();
	return ret;
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points,
                         uint32_t handle_count)
{
	for (uint32_t i = 0; i < handle_count; i++) {
		if (points && points[i] != 0) {
			errno = EINVAL;
			return -1;
		}
	}
	return drmSyncobjSignal(fd, handles, handle_count);
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points,
                 uint32_t handle_count, uint32_t flags)
{
	(void)fd; (void)handles; (void)points; (void)handle_count; (void)flags;
	return fail(ENOSYS);
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
                   uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
	(void)fd;
	if (dst_point != 0 || src_point != 0 || flags != 0) {
		return fail(EINVAL);
	}
	int e = enter();
	if (e) {
		return e;
	}
	struct sobj *d = so_get(dst_handle), *s = so_get(src_handle);
	if (!d || !s) {
		leave();
		return fail(EINVAL);
	}
	d->seq = s->seq;
	leave();
	return 0;
}
