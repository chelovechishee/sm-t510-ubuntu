/*
 * gles-multidraw: several draws into one render target in one frame, the
 * way glamor composites (background, then a small rectangle on top).
 * Each case draws its rectangles (pixel coordinates, solid colours), reads
 * the target back and compares every pixel with what is expected.
 *
 * Build (on the tablet): gcc -O2 -o gles-multidraw gles-multidraw.c -lEGL -lGLESv2
 * Run:   LD_PRELOAD=/opt/mesa-kbase/lib/libpanfrost_kbase_drm_shim.so ./gles-multidraw
 *        MULTIDRAW_FINISH=1 submits every draw on its own,
 *        MULTIDRAW_CLEAR=1 clears first (no reload of the old contents).
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *vs_src =
   "#version 300 es\n"
   "uniform vec2 size;\n"
   "in vec2 pos;\n"
   "void main() { gl_Position = vec4(pos / size * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *fs_src =
   "#version 300 es\n"
   "precision mediump float;\n"
   "uniform vec4 col;\n"
   "out vec4 color;\n"
   "void main() { color = col; }\n";

struct rect { int x, y, w, h; unsigned char c[4]; };

static GLuint prog;
static GLint u_size, u_col;

static GLuint
shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   GLint ok;

   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[1024];
      glGetShaderInfoLog(s, sizeof(log), NULL, log);
      fprintf(stderr, "shader: %s\n", log);
      exit(1);
   }
   return s;
}

static int
run_case(const char *name, int w, int h, const struct rect *r, int n)
{
   GLuint tex, fbo;
   unsigned char *got = malloc(w * h * 4), *want = calloc(w * h, 4);
   int bad = 0, first = -1;

   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   glViewport(0, 0, w, h);
   glUniform2f(u_size, w, h);
   if (getenv("MULTIDRAW_CLEAR")) { /* start the frame with a clear, not a reload */
      glClearColor(0, 0, 0, 0);
      glClear(GL_COLOR_BUFFER_BIT);
   }

   for (int i = 0; i < n; i++) {
      GLfloat v[8] = { r[i].x, r[i].y, r[i].x + r[i].w, r[i].y,
                       r[i].x, r[i].y + r[i].h, r[i].x + r[i].w, r[i].y + r[i].h };
      glUniform4f(u_col, r[i].c[0] / 255.0f, r[i].c[1] / 255.0f, r[i].c[2] / 255.0f,
                  r[i].c[3] / 255.0f);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
      glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
      if (getenv("MULTIDRAW_FINISH")) /* one job chain per draw */
         glFinish();
      for (int y = r[i].y; y < r[i].y + r[i].h; y++)
         for (int x = r[i].x; x < r[i].x + r[i].w; x++)
            memcpy(want + (y * w + x) * 4, r[i].c, 4);
   }

   memset(got, 0x55, w * h * 4);
   glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, got);
   for (int i = 0; i < w * h; i++)
      if (memcmp(got + i * 4, want + i * 4, 4)) {
         if (first < 0)
            first = i;
         bad++;
      }
   printf("%-40s %s", name, bad ? "WRONG" : "ok");
   if (bad)
      printf(" (%d px; first (%d,%d) got %u %u %u %u want %u %u %u %u)", bad, first % w,
             first / w, got[first * 4], got[first * 4 + 1], got[first * 4 + 2],
             got[first * 4 + 3], want[first * 4], want[first * 4 + 1], want[first * 4 + 2],
             want[first * 4 + 3]);
   printf("\n");
   glDeleteFramebuffers(1, &fbo);
   glDeleteTextures(1, &tex);
   free(got);
   free(want);
   return !bad;
}

#define WHITE { 255, 255, 255, 255 }
#define RED { 255, 0, 0, 255 }
#define GREEN { 0, 255, 0, 255 }
#define BLUE { 0, 0, 255, 255 }

int
main(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
   EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
   EGLConfig cfg;
   EGLint n;
   int ok = 1;

   static const struct rect c1[] = { { 0, 0, 10, 10, WHITE }, { 1, 1, 3, 3, RED } };
   static const struct rect c2[] = { { 0, 0, 64, 64, WHITE }, { 1, 1, 3, 3, RED } };
   static const struct rect c3[] = { { 0, 0, 64, 64, WHITE }, { 20, 20, 30, 30, RED } };
   static const struct rect c4[] = { { 0, 0, 64, 64, WHITE }, { 0, 0, 32, 32, RED },
                                     { 32, 32, 32, 32, GREEN }, { 8, 40, 4, 4, BLUE } };
   static const struct rect c5[] = { { 1, 1, 3, 3, RED } };
   static const struct rect c6[] = { { 0, 0, 300, 200, WHITE }, { 5, 5, 3, 3, RED },
                                     { 150, 100, 3, 3, GREEN }, { 290, 190, 3, 3, BLUE } };

   if (!eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API) ||
       !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || !n)
      return 1;
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                  eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr));
   printf("renderer: %s\n", glGetString(GL_RENDERER));

   prog = glCreateProgram();
   glAttachShader(prog, shader(GL_VERTEX_SHADER, vs_src));
   glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_src));
   glBindAttribLocation(prog, 0, "pos");
   glLinkProgram(prog);
   glUseProgram(prog);
   u_size = glGetUniformLocation(prog, "size");
   u_col = glGetUniformLocation(prog, "col");
   glEnableVertexAttribArray(0);

   ok &= run_case("10x10: background + 3x3 (dst coords)", 10, 10, c1, 2);
   ok &= run_case("64x64: background + 3x3", 64, 64, c2, 2);
   ok &= run_case("64x64: background + 30x30", 64, 64, c3, 2);
   ok &= run_case("64x64: 4 rectangles", 64, 64, c4, 4);
   ok &= run_case("10x10: one 3x3 only", 10, 10, c5, 1);
   ok &= run_case("300x200: background + 3 small", 300, 200, c6, 4);

   /* more draws in one frame than kbase has atom slots (255) */
   {
      static struct rect many[1 + 1000];

      many[0] = (struct rect){ 0, 0, 100, 100, WHITE };
      for (int i = 0; i < 1000; i++)
         many[1 + i] = (struct rect){ (i * 7) % 97, (i * 13) % 97, 3, 3,
                                      { i & 0xff, (i >> 2) & 0xff, 128, 255 } };
      ok &= run_case("100x100: background + 1000 small", 100, 100, many, 1001);
   }

   printf("glGetError: 0x%x\n%s\n", glGetError(), ok ? "MULTIDRAW OK" : "MULTIDRAW WRONG");
   return ok ? 0 : 1;
}
