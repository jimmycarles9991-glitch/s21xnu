/* Single-process GLES proof for the fresh S21 Mesa port:
 *  - surfaceless GLES3 clear + exact readback
 *  - textured quad + alpha-blended texel, both read back
 *  - visible full-screen PASS/FAIL painted on the real scanout
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "s21_iokit.h"

#define TEST_SIZE 64

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
   uint32_t height = (uint32_t)(out[3] & 0xffff);
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

static int
near(byte got, int want)
{
   return abs((int)got - want) <= 24;
}

static GLuint
compile_shader(GLenum type, const char *src)
{
   GLuint sh = glCreateShader(type);
   if (sh == 0)
      return 0;
   glShaderSource(sh, 1, &src, NULL);
   glCompileShader(sh);
   GLint ok = 0;
   glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[200] = { 0 };
      glGetShaderInfoLog(sh, sizeof(log) - 1, NULL, log);
      char line[256];
      snprintf(line, sizeof(line), "shader compile FAIL: %.160s", log);
      say(line);
      glDeleteShader(sh);
      return 0;
   }
   return sh;
}

static GLuint
build_program(void)
{
   static const char vs[] =
      "attribute vec2 a_pos;\n"
      "attribute vec2 a_uv;\n"
      "varying vec2 v_uv;\n"
      "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
   static const char fs[] =
      "precision mediump float;\n"
      "varying vec2 v_uv;\n"
      "uniform sampler2D u_tex;\n"
      "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

   GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
   GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);
   if (v == 0 || f == 0)
      return 0;
   GLuint p = glCreateProgram();
   if (p == 0)
      return 0;
   glAttachShader(p, v);
   glAttachShader(p, f);
   glLinkProgram(p);
   GLint ok = 0;
   glGetProgramiv(p, GL_LINK_STATUS, &ok);
   if (!ok) {
      char log[200] = { 0 };
      glGetProgramInfoLog(p, sizeof(log) - 1, NULL, log);
      char line[256];
      snprintf(line, sizeof(line), "shader link FAIL: %.160s", log);
      say(line);
      glDeleteProgram(p);
      p = 0;
   }
   glDeleteShader(v);
   glDeleteShader(f);
   return p;
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

   const EGLint pbuf_attr[] = {
      EGL_WIDTH, TEST_SIZE, EGL_HEIGHT, TEST_SIZE, EGL_NONE
   };
   EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbuf_attr);
   const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
   if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
       !eglMakeCurrent(dpy, surf, surf, ctx))
      return finish("context", 0, 0, 0, 0, 0);
   say("context ok");

   glViewport(0, 0, TEST_SIZE, TEST_SIZE);
   glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   say("clear submitted");
   glFinish();
   say("glFinish ok");

   unsigned char px[4] = { 0, 0, 0, 0 };
   glReadPixels(TEST_SIZE / 2, TEST_SIZE / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   int clear_ok = px[0] < 16 && px[1] > 240 && px[2] < 16 && px[3] > 240;
   report("clear", clear_ok, px[0], px[1], px[2], px[3]);
   if (!clear_ok)
      return finish("clear+quad", 0, px[0], px[1], px[2], px[3]);

   GLuint prog = build_program();
   if (prog == 0)
      return finish("quad-shader", 0, 0, 0, 0, 0);
   say("shaders ok");

   /* 2x2 nearest texture: opaque red/green on the bottom row, translucent
    * white and opaque blue on the top row. */
   static const unsigned char texel[16] = {
      255, 0, 0, 255,      0, 255, 0, 255,
      255, 255, 255, 128,  0, 0, 255, 255,
   };
   GLuint tex = 0;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, texel);
   say("texture uploaded");

   static const GLfloat quad[] = {
      -1.0f, -1.0f, 0.0f, 0.0f,
       1.0f, -1.0f, 1.0f, 0.0f,
      -1.0f,  1.0f, 0.0f, 1.0f,
       1.0f,  1.0f, 1.0f, 1.0f,
   };
   GLuint vbo = 0;
   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

   glUseProgram(prog);
   GLint pos = glGetAttribLocation(prog, "a_pos");
   GLint uv = glGetAttribLocation(prog, "a_uv");
   GLint sampler = glGetUniformLocation(prog, "u_tex");
   if (pos < 0 || uv < 0 || sampler < 0)
      return finish("quad-locations", 0, 0, 0, 0, 0);
   glUniform1i(sampler, 0);
   glEnableVertexAttribArray(pos);
   glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                         (const void *)0);
   glEnableVertexAttribArray(uv);
   glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                         (const void *)(2 * sizeof(GLfloat)));

   /* Blue background makes the translucent white texel visibly blended. */
   glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glEnable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   say("quad submitted");
   glFinish();
   say("quad glFinish ok");

   unsigned char tex_px[4] = { 0, 0, 0, 0 };
   glReadPixels(TEST_SIZE / 4, TEST_SIZE / 4, 1, 1, GL_RGBA,
                GL_UNSIGNED_BYTE, tex_px);
   int tex_ok = near(tex_px[0], 255) && near(tex_px[1], 0) &&
                near(tex_px[2], 0) && near(tex_px[3], 255);
   report("textured quad", tex_ok, tex_px[0], tex_px[1], tex_px[2], tex_px[3]);

   unsigned char blend_px[4] = { 0, 0, 0, 0 };
   glReadPixels(TEST_SIZE / 4, 3 * TEST_SIZE / 4, 1, 1, GL_RGBA,
                GL_UNSIGNED_BYTE, blend_px);
   int blend_ok = near(blend_px[0], 128) && near(blend_px[1], 128) &&
                  near(blend_px[2], 255) && near(blend_px[3], 191);
   report("blended quad", blend_ok, blend_px[0], blend_px[1], blend_px[2],
          blend_px[3]);

   int ok = tex_ok && blend_ok;
   return finish("clear+quad", ok, blend_px[0], blend_px[1], blend_px[2],
                 blend_px[3]);
}
