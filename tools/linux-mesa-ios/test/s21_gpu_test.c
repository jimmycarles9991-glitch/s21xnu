/* Single-process GLES clear/readback test for the fresh S21 Mesa port. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
report(const char *stage, int ok, unsigned char r, unsigned char g,
       unsigned char b, unsigned char a)
{
   char line[256];
   snprintf(line, sizeof(line),
            "S21GPU: %s %s rgba=%02x%02x%02x%02x\n", stage,
            ok ? "PASS" : "FAIL", r, g, b, a);
   fputs(line, stdout);
   FILE *f = fopen("/var/mobile/s21-gpu-test.txt", "a");
   if (f != NULL) {
      fputs(line, f);
      fclose(f);
   }
}

int
main(void)
{
   typedef EGLDisplay (*get_platform_display_fn)(EGLenum, EGLNativeDisplayType,
                                                 const EGLint *);
   get_platform_display_fn get_platform_display =
      (get_platform_display_fn)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = EGL_NO_DISPLAY;
   if (get_platform_display != NULL)
      dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                 EGL_DEFAULT_DISPLAY, NULL);
   if (dpy == EGL_NO_DISPLAY)
      dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (dpy == EGL_NO_DISPLAY) {
      report("display", 0, 0, 0, 0, 0);
      return 1;
   }
   if (!eglInitialize(dpy, NULL, NULL)) {
      report("initialize", 0, 0, 0, 0, 0);
      return 1;
   }
   if (!eglBindAPI(EGL_OPENGL_ES_API)) {
      report("bind", 0, 0, 0, 0, 0);
      return 1;
   }

   const EGLint cfg_attr[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_NONE,
   };
   EGLConfig cfg;
   EGLint n = 0;
   if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
      report("config", 0, 0, 0, 0, 0);
      return 1;
   }

   const EGLint pbuf_attr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
   EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbuf_attr);
   const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
   if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
       !eglMakeCurrent(dpy, surf, surf, ctx)) {
      report("context", 0, 0, 0, 0, 0);
      return 1;
   }

   glViewport(0, 0, 16, 16);
   glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glFinish();

   unsigned char px[4] = { 0, 0, 0, 0 };
   glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   int ok = px[0] < 16 && px[1] > 240 && px[2] < 16 && px[3] > 240;
   report("clear", ok, px[0], px[1], px[2], px[3]);

   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(dpy, ctx);
   eglDestroySurface(dpy, surf);
   eglTerminate(dpy);
   return ok ? 0 : 1;
}
