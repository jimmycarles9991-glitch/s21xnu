/* SPDX-License-Identifier: MIT
 * Galaxy S21 port: the OpenGL ES library behind HakiOS's src/eagl.c (hakios.gl.h),
 * in place of gl_hero.c (OSMesa over HeroGPU).  We already ship a working
 * surfaceless EGL/GLES3 stack for the G78 (tools/linux-mesa-ios), so this
 * backend makes small ES3 pbuffer contexts with it, one per calling thread in
 * one share group.
 *
 * V22: simple 4-byte 'BGRA' IOSurfaces can also be imported as Linux dma-bufs
 * (s21_drm_import_user() + EGL_LINUX_DMA_BUF_EXT) and bound directly to a GL
 * texture.  That lets backboardd's frame blit land in the display surface
 * instead of being read back and copied by the CPU on every frame.  Anything
 * else still falls back to eagl.c's CPU upload/copy-out paths.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <drm-uapi/drm_fourcc.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

#include "hakios.gl.h"
#include "s21_drm.h"
#include "s21_iokit.h"

#define S21_WIN 16
#define S21_IMPORT_MAX (15ULL << 20)    /* the kernel's SM_MAX_BO_BYTES */
#define S21_IMPORT_CACHE 16             /* bounded: each entry pins a GPU BO */

struct hakios_gl {
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLSurface surf;                /* the share group's pbuffer; never made current */
	EGLContext ctx;                 /* the share group's root context; never made current */
	int version;
};

static void
say(const char *line)
{
	write(2, line, (size_t)strlen(line));
	s21_iokit_log(line);
}

static void
sayf(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof(line) - 1)
		n = (int)sizeof(line) - 1;
	line[n] = '\0';
	say(line);
}

static EGLDisplay g_last_dpy = EGL_NO_DISPLAY;

/*
 * iOS 16's render server creates its EAGLContext on one thread and draws with it on another.
 * An EGLContext may be current to only one thread, so one shared EGL context cannot serve both:
 * eglMakeCurrent() fails with EGL_BAD_ACCESS on the drawing thread, every gl* call then has no
 * context, glCreateShader()/glCreateProgram() return 0, and CoreAnimation crashes when it tries
 * to use a shader or an IOSurface attachment.  Give each thread its own EGL context and pbuffer
 * in the same share group instead: GL objects are shared, while current-context state is not
 * (HakiOS's S7 bridge has the same per-thread requirement in eagl.m).
 */
static __thread struct hakios_gl *thread_gl;
static __thread EGLDisplay thread_dpy = EGL_NO_DISPLAY;
static __thread EGLContext thread_ctx = EGL_NO_CONTEXT;
static __thread EGLSurface thread_surf = EGL_NO_SURFACE;
static unsigned thread_contexts_made;

static const char *
egl_error_name(EGLint error)
{
	switch (error) {
	case EGL_SUCCESS:               return "EGL_SUCCESS";
	case EGL_NOT_INITIALIZED:       return "EGL_NOT_INITIALIZED";
	case EGL_BAD_ACCESS:            return "EGL_BAD_ACCESS";
	case EGL_BAD_ALLOC:             return "EGL_BAD_ALLOC";
	case EGL_BAD_ATTRIBUTE:         return "EGL_BAD_ATTRIBUTE";
	case EGL_BAD_CONFIG:            return "EGL_BAD_CONFIG";
	case EGL_BAD_CONTEXT:           return "EGL_BAD_CONTEXT";
	case EGL_BAD_CURRENT_SURFACE:   return "EGL_BAD_CURRENT_SURFACE";
	case EGL_BAD_DISPLAY:           return "EGL_BAD_DISPLAY";
	case EGL_BAD_MATCH:             return "EGL_BAD_MATCH";
	case EGL_BAD_NATIVE_PIXMAP:     return "EGL_BAD_NATIVE_PIXMAP";
	case EGL_BAD_NATIVE_WINDOW:     return "EGL_BAD_NATIVE_WINDOW";
	case EGL_BAD_PARAMETER:         return "EGL_BAD_PARAMETER";
	case EGL_BAD_SURFACE:           return "EGL_BAD_SURFACE";
	case EGL_CONTEXT_LOST:          return "EGL_CONTEXT_LOST";
	default:                        return "unknown EGL error";
	}
}

static void
say_egl_failure(const char *what, EGLint error)
{
	sayf("S21EAGL: %s failed: %s (0x%x)\n", what, egl_error_name(error), (unsigned)error);
}

static void
drop_thread_context(void)
{
	if (thread_dpy == EGL_NO_DISPLAY)
		return;
	if (thread_ctx != EGL_NO_CONTEXT) {
		(void)eglMakeCurrent(thread_dpy, EGL_NO_SURFACE,
		    EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroyContext(thread_dpy, thread_ctx);
	}
	if (thread_surf != EGL_NO_SURFACE)
		eglDestroySurface(thread_dpy, thread_surf);
	thread_gl = NULL;
	thread_dpy = EGL_NO_DISPLAY;
	thread_ctx = EGL_NO_CONTEXT;
	thread_surf = EGL_NO_SURFACE;
}

struct hakios_gl *
hakios_gl_create(int version)
{
	typedef EGLDisplay (*get_platform_display_fn)(EGLenum, EGLNativeDisplayType,
	    const EGLint *);

	get_platform_display_fn get_platform_display =
	    (get_platform_display_fn)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = EGL_NO_DISPLAY;
	if (get_platform_display != NULL) {
		dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
		    EGL_DEFAULT_DISPLAY, NULL);
	}
	if (dpy == EGL_NO_DISPLAY)
		dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (dpy == EGL_NO_DISPLAY)
		return NULL;
	if (!eglInitialize(dpy, NULL, NULL)) {
		say_egl_failure("eglInitialize", eglGetError());
		return NULL;
	}
	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		say_egl_failure("eglBindAPI", eglGetError());
		eglTerminate(dpy);
		return NULL;
	}

	const EGLint cfg_attr[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RENDERABLE_TYPE, version >= 3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLConfig cfg;
	EGLint n = 0;
	if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
		say_egl_failure("eglChooseConfig", eglGetError());
		eglTerminate(dpy);
		return NULL;
	}
	const EGLint pbuf_attr[] = {
		EGL_WIDTH, S21_WIN, EGL_HEIGHT, S21_WIN, EGL_NONE
	};
	EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbuf_attr);
	const EGLint ctx_attr[] = {
		EGL_CONTEXT_CLIENT_VERSION, version >= 3 ? 3 : 2, EGL_NONE
	};
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) {
		say_egl_failure(surf == EGL_NO_SURFACE ? "eglCreatePbufferSurface" :
		    "eglCreateContext", eglGetError());
		if (surf != EGL_NO_SURFACE)
			eglDestroySurface(dpy, surf);
		if (ctx != EGL_NO_CONTEXT)
			eglDestroyContext(dpy, ctx);
		eglTerminate(dpy);
		return NULL;
	}
	struct hakios_gl *gl = calloc(1, sizeof(*gl));
	if (gl == NULL) {
		eglDestroySurface(dpy, surf);
		eglDestroyContext(dpy, ctx);
		eglTerminate(dpy);
		return NULL;
	}
	gl->dpy = dpy;
	gl->cfg = cfg;
	gl->surf = surf;
	gl->ctx = ctx;
	gl->version = version;
	g_last_dpy = dpy;
	sayf("S21EAGL: ES%d context created via Mesa Panfrost\n", version);
	return gl;
}

int
hakios_gl_make_current(struct hakios_gl *gl)
{
	EGLint error;

	if (gl == NULL) {
		EGLDisplay dpy = thread_dpy != EGL_NO_DISPLAY ? thread_dpy : g_last_dpy;

		if (dpy == EGL_NO_DISPLAY)
			return 1;
		if (eglMakeCurrent(dpy, EGL_NO_SURFACE,
		    EGL_NO_SURFACE, EGL_NO_CONTEXT) != EGL_TRUE) {
			say_egl_failure("eglMakeCurrent(NULL)", eglGetError());
			return 0;
		}
		thread_gl = NULL;
		return 1;
	}

	if (thread_gl == gl && thread_ctx != EGL_NO_CONTEXT) {
		if (eglMakeCurrent(gl->dpy, thread_surf, thread_surf,
		    thread_ctx) == EGL_TRUE)
			return 1;
		error = eglGetError();
		say_egl_failure("eglMakeCurrent(existing thread context)", error);
		drop_thread_context();
	} else if (thread_dpy != EGL_NO_DISPLAY) {
		drop_thread_context();
	}

	const EGLint pbuf_attr[] = {
		EGL_WIDTH, S21_WIN, EGL_HEIGHT, S21_WIN, EGL_NONE
	};
	const EGLint ctx_attr[] = {
		EGL_CONTEXT_CLIENT_VERSION, gl->version >= 3 ? 3 : 2, EGL_NONE
	};
	EGLSurface surf = eglCreatePbufferSurface(gl->dpy, gl->cfg, pbuf_attr);
	EGLContext ctx = eglCreateContext(gl->dpy, gl->cfg, gl->ctx, ctx_attr);

	if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT) {
		error = eglGetError();
		say_egl_failure(surf == EGL_NO_SURFACE ? "eglCreatePbufferSurface(thread)" :
		    "eglCreateContext(thread)", error);
		if (ctx != EGL_NO_CONTEXT)
			eglDestroyContext(gl->dpy, ctx);
		if (surf != EGL_NO_SURFACE)
			eglDestroySurface(gl->dpy, surf);
		return 0;
	}
	if (eglMakeCurrent(gl->dpy, surf, surf, ctx) != EGL_TRUE) {
		error = eglGetError();
		say_egl_failure("eglMakeCurrent(new thread context)", error);
		eglDestroyContext(gl->dpy, ctx);
		eglDestroySurface(gl->dpy, surf);
		return 0;
	}

	thread_gl = gl;
	thread_dpy = gl->dpy;
	thread_ctx = ctx;
	thread_surf = surf;
	unsigned made = ++thread_contexts_made;
	if (made <= 8) {
		sayf("S21EAGL: per-thread ES%d context %u made current (share group)\n",
		    gl->version, made);
	}
	return 1;
}

void
hakios_gl_destroy(struct hakios_gl *gl)
{
	if (gl == NULL)
		return;
	drop_thread_context();
	(void)eglMakeCurrent(gl->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(gl->dpy, gl->ctx);
	eglDestroySurface(gl->dpy, gl->surf);
	eglTerminate(gl->dpy);
	free(gl);
}

const char *
hakios_gl_library(void)
{
	return "Mesa 26.2.4 Panfrost G78 (surfaceless EGL/GLES3, S21)";
}

/* V22: an EGLImage made from an IOSurface's own memory.  The IOSurface is
 * retained while cached so its base address cannot be recycled behind us; the
 * cache is small because every entry also pins a SweetMali BO. */
struct imported_surface {
	EGLImageKHR image;
	IOSurfaceRef surface;
	uint32_t id;
	uintptr_t base;
	size_t row, height;
	uint64_t used;
};

static pthread_mutex_t import_lock = PTHREAD_MUTEX_INITIALIZER;
static struct imported_surface imported[S21_IMPORT_CACHE];
static uint64_t import_clock;
static unsigned imports_made, imports_failed;

typedef void (*target_texture_fn)(GLenum, GLeglImageOES);
static target_texture_fn target_texture;

static void
import_refused(const char *why, IOSurfaceRef surface, size_t row,
    size_t height, size_t needed, size_t alloc)
{
	static unsigned told;

	if (told++ < 16) {
		sayf("S21EAGL: IOSurface %u not importable (%s; %zux%zu row=%zu need=%zu alloc=%zu)\n",
		    IOSurfaceGetID(surface), why, (size_t)IOSurfaceGetWidth(surface), height, row, needed,
		    alloc);
	}
}

static bool
importable(IOSurfaceRef surface, size_t *row_out, size_t *height_out,
    size_t *size_out)
{
	size_t width = IOSurfaceGetWidth(surface);
	size_t height = IOSurfaceGetHeight(surface);
	size_t row = IOSurfaceGetBytesPerRow(surface);
	size_t needed;
	size_t alloc;
	size_t size;

	if (IOSurfaceGetPixelFormat(surface) != 'BGRA') {
		import_refused("pixel format", surface, row, height, 0, 0);
		return false;
	}
	if (IOSurfaceGetPlaneCount(surface) != 0 || IOSurfaceGetBytesPerElement(surface) != 4 ||
	    width == 0 || height == 0 || row < width * 4 || IOSurfaceGetBaseAddress(surface) == NULL) {
		import_refused("shape", surface, row, height, 0, 0);
		return false;
	}
	/* V31: Panfrost checks the WSI row pitch against the modifier's buffer
	 * alignment before it can import a dma-buf. A 120x120 BGRA picture has a
	 * 480-byte row and a 5152-byte row was seen too; neither is 64-byte
	 * aligned, so both were doomed to EGL_BAD_ALLOC after Mesa logged
	 * "WSI pitch not properly aligned". Refuse them here and use the CPU
	 * copy without the failed EGLImage round trip. The 4608-byte display
	 * rows remain importable. */
	if ((row & 63) != 0) {
		import_refused("row pitch/64", surface, row, height, 0, 0);
		return false;
	}
	needed = row * height;
	alloc = IOSurfaceGetAllocSize(surface);
	/* kSMImportUser maps whole 4 KiB pages and requires a page-multiple length.
	 * IOSurfaceGetAllocSize() is not necessarily page-aligned: the 1125x2436
	 * display surface is 11,225,088 bytes, which V22/V23 rejected here. Round
	 * up instead of requiring the allocation itself to be aligned. */
	size = alloc >= needed ? alloc : needed;
	size = (size + 0xfff) & ~(size_t)0xfff;
	if (size == 0 || size > S21_IMPORT_MAX) {
		import_refused("size", surface, row, height, needed, alloc);
		return false;
	}
	*row_out = row;
	*height_out = height;
	*size_out = size;
	return true;
}

static EGLImageKHR
import_image(EGLDisplay dpy, IOSurfaceRef surface, size_t row, size_t height,
    size_t size)
{
	static PFNEGLCREATEIMAGEKHRPROC create_image;
	static int no_proc;
	static unsigned extensions_logged;
	uintptr_t base = (uintptr_t)IOSurfaceGetBaseAddress(surface);
	uint32_t handle = 0;
	uint64_t gpu_va = 0;
	int prime_fd = -1;
	EGLImageKHR image;

	if (create_image == NULL && !no_proc) {
		create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
		if (create_image == NULL) {
			no_proc = 1;
			say("S21EAGL: eglCreateImageKHR unavailable; using CPU copies\n");
		}
	}
	if (create_image == NULL)
		return EGL_NO_IMAGE_KHR;
	if (!extensions_logged) {
		const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);

		extensions_logged = 1;
		sayf("S21EAGL: EGL dma-buf import modifiers are %s\n",
		    exts != NULL && strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers") != NULL ?
		    "advertised" : "absent");
	}

	if (s21_drm_import_user((uint64_t)base, size, &handle, &gpu_va) != 0) {
		if (imports_failed++ < 8) {
			sayf("S21EAGL: IOSurface %u import_user failed (%zu x %zu, %llu bytes)\n",
			    IOSurfaceGetID(surface), row, height,
			    (unsigned long long)size);
		}
		return EGL_NO_IMAGE_KHR;
	}
	if (drmPrimeHandleToFD(S21_DRM_FD, handle, 0, &prime_fd) != 0) {
		drmCloseBufferHandle(S21_DRM_FD, handle);
		return EGL_NO_IMAGE_KHR;
	}

	/* V25: use the implicit modifier. Mesa only accepts the explicit
	 * MODIFIER_LO/HI attributes when the display advertises
	 * EGL_EXT_image_dma_buf_import_modifiers; V24's display surfaces were
	 * rejected with EGL_BAD_PARAMETER before the import ever reached the
	 * Panfrost driver. DRM_FORMAT_MOD_INVALID means linear here, so omitting
	 * the attributes keeps these simple IOSurface buffers linear. */
	const EGLint attr[] = {
		EGL_WIDTH, (EGLint)IOSurfaceGetWidth(surface),
		EGL_HEIGHT, (EGLint)height,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888, /* memory: B,G,R,A */
		EGL_DMA_BUF_PLANE0_FD_EXT, prime_fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)row,
		EGL_NONE
	};
	image = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	if (image == EGL_NO_IMAGE_KHR) {
		/* Panfrost closes the GEM handle if its import failed; a second
		 * close is rejected harmlessly by the shim. */
		drmCloseBufferHandle(S21_DRM_FD, handle);
		if (imports_failed++ < 8) {
			EGLint error = eglGetError();

			sayf("S21EAGL: eglCreateImageKHR failed for IOSurface %u: %s (0x%x)\n",
			    IOSurfaceGetID(surface), egl_error_name(error),
			    (unsigned)error);
		}
		return EGL_NO_IMAGE_KHR;
	}
	if (imports_made++ < 8) {
		sayf("S21EAGL: shared IOSurface %u (BGRA %zu x %zu, %zu bytes a row)\n",
		    IOSurfaceGetID(surface), (size_t)IOSurfaceGetWidth(surface), height, row);
	}
	return image;
}

static void
forget_entry(struct imported_surface *entry)
{
	static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
	EGLDisplay dpy = g_last_dpy;

	if (entry->image != EGL_NO_IMAGE_KHR) {
		if (destroy_image == NULL) {
			destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
		}
		if (destroy_image != NULL && dpy != EGL_NO_DISPLAY)
			destroy_image(dpy, entry->image);
		entry->image = EGL_NO_IMAGE_KHR;
	}
	if (entry->surface != NULL) {
		CFRelease(entry->surface);
		entry->surface = NULL;
	}
	entry->id = 0;
	entry->base = 0;
	entry->row = entry->height = 0;
	entry->used = 0;
}

int
hakios_gl_surface_texture(struct hakios_gl *gl, uint32_t surface_id)
{
	IOSurfaceRef surface;
	struct imported_surface *entry = NULL;
	size_t row, height, size;
	uintptr_t base;
	EGLImageKHR image;

	if (gl == NULL || gl->dpy == EGL_NO_DISPLAY)
		return 0;
	if (target_texture == NULL) {
		target_texture = (target_texture_fn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
		if (target_texture == NULL) {
			static int told;
			if (!told) {
				told = 1;
				say("S21EAGL: glEGLImageTargetTexture2DOES unavailable; using CPU copies\n");
			}
			return 0;
		}
	}

	surface = IOSurfaceLookup(surface_id);
	if (surface == NULL)
		return 0;
	if (!importable(surface, &row, &height, &size)) {
		CFRelease(surface);
		return 0;
	}
	base = (uintptr_t)IOSurfaceGetBaseAddress(surface);

	pthread_mutex_lock(&import_lock);
	for (unsigned i = 0; i < S21_IMPORT_CACHE; i++) {
		struct imported_surface *c = &imported[i];

		if (c->image == EGL_NO_IMAGE_KHR)
			continue;
		if (c->id == surface_id && c->base == base && c->row == row && c->height == height) {
			entry = c;
			break;
		}
		if (c->id == surface_id) {
			forget_entry(c);       /* the surface was recreated at another address */
		}
	}
	if (entry == NULL) {
		uint64_t oldest = UINT64_MAX;

		for (unsigned i = 0; i < S21_IMPORT_CACHE; i++) {
			if (imported[i].used < oldest) {
				oldest = imported[i].used;
				entry = &imported[i];
			}
		}
		forget_entry(entry);

		image = import_image(gl->dpy, surface, row, height, size);
		if (image != EGL_NO_IMAGE_KHR) {
			entry->surface = (IOSurfaceRef)CFRetain(surface);
			entry->image = image;
			entry->id = surface_id;
			entry->base = base;
			entry->row = row;
			entry->height = height;
		}
	}
	if (entry != NULL && entry->image != EGL_NO_IMAGE_KHR) {
		entry->used = ++import_clock;
		target_texture(GL_TEXTURE_2D, (GLeglImageOES)entry->image);
	}
	pthread_mutex_unlock(&import_lock);

	CFRelease(surface);
	return entry != NULL && entry->image != EGL_NO_IMAGE_KHR;
}

void
hakios_gl_surface_written(struct hakios_gl *gl, uint32_t surface)
{
	(void)gl;
	(void)surface;

	/* Submits are synchronous in the shim, but CoreAnimation publishes the
	 * surface as soon as we return; make the GPU blit visible before then. */
	glFinish();
}

/* ES1 entry in iOS's macro table that Mesa's ES3 library does not export.
 * CoreAnimation's GLES3 renderer does not use alpha test; keep the slot
 * harmless instead of logging an unmapped call. */
void
glAlphaFunc(GLenum func, GLclampf ref)
{
	(void)func;
	(void)ref;
}

__attribute__((constructor)) static void
s21_backend_loaded(void)
{
	(void)s21_iokit_open();
	say("S21EAGL: backend loaded (surfaceless EGL/GLES3 over Mesa Panfrost)\n");
}
