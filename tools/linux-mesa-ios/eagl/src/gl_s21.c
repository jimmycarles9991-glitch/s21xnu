/* SPDX-License-Identifier: MIT
 * Galaxy S21 port: the OpenGL ES library behind HakiOS's src/eagl.c (hakios.gl.h),
 * in place of gl_hero.c (OSMesa over HeroGPU).  We already ship a working
 * surfaceless EGL/GLES3 stack for the G78 (tools/linux-mesa-ios), so this
 * backend makes small ES3 pbuffer contexts with it, one per calling thread in
 * one share group, and lets eagl.c do its CPU copies between IOSurfaces and
 * ordinary GL textures.  Deliberately no
 * direct IOSurface memory sharing (hakios_gl_surface_texture() returns 0):
 * correctness first; direct EGLImage imports can be added after the first
 * backboardd frame is visible.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hakios.gl.h"
#include "s21_iokit.h"

#define S21_WIN 16

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

/* No IOSurface-backed texture yet: let eagl.c's ordinary texture + CPU copy
 * paths do the work.  Once glEGLImageTargetTexture2DOES or EGL dma-buf
 * imports are wired to IOSurface memory this can return 1 for BGRA surfaces. */
int
hakios_gl_surface_texture(struct hakios_gl *gl, uint32_t surface)
{
	static int reported;

	(void)gl;
	if (!reported) {
		reported = 1;
		sayf("S21EAGL: IOSurface %u not shared; using CPU texture copy\n", surface);
	}
	return 0;
}

void
hakios_gl_surface_written(struct hakios_gl *gl, uint32_t surface)
{
	(void)gl;
	(void)surface;
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
