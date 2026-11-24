/* SPDX-License-Identifier: MIT
 * Galaxy S21 port: the OpenGL ES library behind HakiOS's src/eagl.c (hakios.gl.h),
 * in place of gl_hero.c (OSMesa over HeroGPU).  We already ship a working
 * surfaceless EGL/GLES3 stack for the G78 (tools/linux-mesa-ios), so this
 * backend makes a small ES3 pbuffer context with it and lets eagl.c do its
 * CPU copies between IOSurfaces and ordinary GL textures.  Deliberately no
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
	EGLSurface surf;
	EGLContext ctx;
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
	if (!eglInitialize(dpy, NULL, NULL))
		return NULL;
	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
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
	g_last_dpy = dpy;
	sayf("S21EAGL: ES%d context created via Mesa Panfrost\n", version);
	return gl;
}

int
hakios_gl_make_current(struct hakios_gl *gl)
{
	if (gl == NULL)
		return eglMakeCurrent(g_last_dpy, EGL_NO_SURFACE,
		    EGL_NO_SURFACE, EGL_NO_CONTEXT) == EGL_TRUE;
	return eglMakeCurrent(gl->dpy, gl->surf, gl->surf, gl->ctx) == EGL_TRUE;
}

void
hakios_gl_destroy(struct hakios_gl *gl)
{
	if (gl == NULL)
		return;
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
