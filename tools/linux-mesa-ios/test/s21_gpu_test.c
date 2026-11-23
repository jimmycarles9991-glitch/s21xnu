/* Single-process GLES clear/readback test for the fresh S21 Mesa port. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "s21_iokit.h"

static void
say(const char *stage)
{
   char line[256];
   snprintf(line, sizeof(line), "S21GPU: %s", stage);
   fputs(line, stdout);
   fputc('\n', stdout);
   fflush(stdout);
   s21_iokit_log(line);
}

static void
report(const char *stage, int ok, unsigned char r, unsigned char g,
       unsigned char b, unsigned char a)
{
   char line[256];
   snprintf(line, sizeof(line),
            "S21GPU: %s %s rgba=%02x%02x%02x%02x\n", stage,
            ok ? "PASS" : "FAIL", r, g, b, a);
   fputs(line, stdout);
   fflush(stdout);
   s21_iokit_log(line);
   FILE *f = fopen("/var/mobile/s21-gpu-test.txt", "a");
   if (f != NULL) {
      fputs(line, f);
      fclose(f);
   }
}

/* SweetMali selector 6 maps the real scanout buffer. Hold the result on the
 * panel so the boot is readable without a shell; AppleCLCD can overwrite the
 * buffer on later presents, so repaint forever like the old SWEETGL test. */
static void
paint_fb(uint32_t colour)
{
   uint64_t out[4] = { 0, 0, 0, 0 };
   uint32_t n = 4;
   if (s21_iokit_scalar(S21_MAP_FB, NULL, 0, out, &n) != 0 || n < 4) {
      say("framebuffer map FAIL");
      return;
   }
   void *va = s21_iokit_map((uint32_t)out[0], 0);
   if (va == NULL) {
      say("framebuffer mmap FAIL");
      return;
   }
   uint32_t stride = (uint32_t)out[2];
   uint32_t width = (uint32_t)(out[3] >> 16);
   uint32_t height = (uint32_t)out[3];
   char line[256];
   snprintf(line, sizeof(line),
            "framebuffer %ux%u stride %u handle %u colour %08x",
            width, height, stride, (uint32_t)out[0], colour);
   say(line);

   for (;;) {
      for (uint32_t y = 0; y < height; y++) {
         volatile uint32_t *row =
            (volatile uint32_t *)((unsigned char *)va + (size_t)y * stride);
         for (uint32_t x = 0; x < width; x++)
            row[x] = colour;
      }
      usleep(50000);
   }
}

static int
finish(const char *stage, int ok, unsigned char r, unsigned char g,
       unsigned char b, unsigned char a)
{
   report(stage, ok, r, g, b, a);
   paint_fb(ok ? 0xFF00FF00U : 0xFF0000FFU);
   return ok ? 0 : 1;
}

int
main(void)
{
   typedef EGLDisplay (*get_platform_display_fn)(EGLenum, EGLNativeDisplayType,
                                                  const EGLint *);

   if (s21_iokit_open() != 0)
      say("SweetMali open FAIL");
   else
      say("start");

   get_platform_display_fn get_platform_display =
      (get_platform_display_fn)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = EGL_NO_DISPLAY;
   if (get_platform_display != NULL)
      dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA,
                                 EGL_DEFAULT_DISPLAY, NULL);
   if (dpy == EGL_NO_DISPLAY)
      dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (dpy == EGL_NO_DISPLAY)
      return finish("display", 0, 0, 0, 0, 0);
   say("display ok");

   if (!eglInitialize(dpy, NULL, NULL))
      return finish("initialize", 0, 0, 0, 0, 0);
   say("initialize ok");

   if (!eglBindAPI(EGL_OPENGL_ES_API))
      return finish("bind", 0, 0, 0, 0, 0);

   const EGLint cfg_attr[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_NONE,
   };
   EGLConfig cfg;
   EGLint n = 0;
   if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1)
      return finish("config", 0, 0, 0, 0, 0);
   say("config ok");

   const EGLint pbuf_attr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
   EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbuf_attr);
   const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
   if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
       !eglMakeCurrent(dpy, surf, surf, ctx))
      return finish("context", 0, 0, 0, 0, 0);
   say("context ok");

   glViewport(0, 0, 16, 16);
   glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   say("clear submitted");
   glFinish();
   say("glFinish ok");

   unsigned char px[4] = { 0, 0, 0, 0 };
   glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   int ok = px[0] < 16 && px[1] > 240 && px[2] < 16 && px[3] > 240;
   return finish("clear", ok, px[0], px[1], px[2], px[3]);
}
