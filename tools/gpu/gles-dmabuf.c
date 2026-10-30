/*
 * gles-dmabuf: GPU access to a KMS scanout buffer, the way X/glamor uses its
 * screen pixmap. A GBM buffer on the KMS device (Mesa kmsro: a dumb buffer
 * imported into the GPU as a dma-buf) is wrapped in an EGLImage and
 *   1. rendered to (pattern texture drawn into it), read back with glReadPixels
 *   2. read by the CPU through gbm_bo_map
 *   3. sampled as a texture into an ordinary FBO
 *   4. blitted from into an ordinary FBO
 *   5. updated with glTexSubImage2D, sampled again
 *
 * Build (on the tablet): gcc -O2 -o gles-dmabuf gles-dmabuf.c -lEGL -lGLESv2 -lgbm
 * Run (nothing else may be DRM master on card0):
 *   LD_PRELOAD=/opt/mesa-kbase/lib/libpanfrost_kbase_drm_shim.so ./gles-dmabuf
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define S 256

static const char *vs_src =
   "#version 300 es\n"
   "in vec2 pos;\n"
   "out vec2 uv;\n"
   "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fs_src =
   "#version 300 es\n"
   "precision mediump float;\n"
   "uniform sampler2D tex;\n"
   "in vec2 uv;\n"
   "out vec4 color;\n"
   "void main() { color = texture(tex, uv); }\n";

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

static void
pattern(unsigned char *p, int x, int y, int salt)
{
   p[0] = x;
   p[1] = y;
   p[2] = ((x < S / 2) ^ (y < S / 2)) ? 200 : 40 + salt;
   p[3] = 255;
}

static GLuint
fbo_for(GLuint tex)
{
   GLuint f;

   glGenFramebuffers(1, &f);
   glBindFramebuffer(GL_FRAMEBUFFER, f);
   glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "FBO incomplete\n");
      exit(1);
   }
   return f;
}

static void
draw(GLuint fbo, GLuint tex)
{
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glViewport(0, 0, S, S);
   glBindTexture(GL_TEXTURE_2D, tex);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static int
report(const char *what, const unsigned char *got, const unsigned char *expect, int bgrx)
{
   int bad = 0, first = -1;

   for (int i = 0; i < S * S; i++) {
      const unsigned char *g = got + i * 4, *e = expect + i * 4;
      int diff = bgrx ? (g[0] != e[2] || g[1] != e[1] || g[2] != e[0])
                      : (g[0] != e[0] || g[1] != e[1] || g[2] != e[2]);
      if (diff) {
         if (first < 0)
            first = i;
         bad++;
      }
   }
   printf("%-36s %s", what, bad ? "WRONG" : "ok");
   if (bad) {
      const unsigned char *g = got + first * 4, *e = expect + first * 4;
      printf(" (%d of %d; first at (%d,%d): got %u %u %u, want %u %u %u%s)", bad, S * S,
             first % S, first / S, g[0], g[1], g[2], e[0], e[1], e[2], bgrx ? " as BGRX" : "");
   }
   printf("\n");
   return !bad;
}

static int
check_gl(const char *what, GLuint fbo, const unsigned char *expect)
{
   static unsigned char got[S * S * 4];

   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   memset(got, 0x55, sizeof(got));
   glReadPixels(0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, got);
   return report(what, got, expect, 0);
}

int
main(int argc, char **argv)
{
   const char *dev = argc > 1 ? argv[1] : "/dev/dri/card0";
   int fd = open(dev, O_RDWR | O_CLOEXEC);
   struct gbm_device *gbm;
   struct gbm_bo *bo;
   EGLDisplay dpy;
   EGLConfig cfg;
   EGLint n;
   EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
   EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
   EGLImageKHR img;
   PFNEGLCREATEIMAGEKHRPROC create_image;
   PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
   static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
   static unsigned char a[S * S * 4], half[S * S * 4], zero[S * S * 4], cpu[S * S * 4];
   GLuint prog, vbo, tex_a, tex_scan, tex_r, fbo_scan, fbo_r;
   int ok = 1;

   if (fd < 0 || !(gbm = gbm_create_device(fd))) {
      perror(dev);
      return 1;
   }
   dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
   if (!eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API) ||
       !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || !n) {
      fprintf(stderr, "EGL setup failed\n");
      return 1;
   }
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                  eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr));
   printf("renderer: %s\n", glGetString(GL_RENDERER));
   create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
   image_target = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");

   for (int y = 0; y < S; y++)
      for (int x = 0; x < S; x++) {
         pattern(a + (y * S + x) * 4, x, y, 0);
         pattern(half + (y * S + x) * 4, x, y, (x < S / 2 && y < S / 2) ? 7 : 0);
      }

   prog = glCreateProgram();
   glAttachShader(prog, shader(GL_VERTEX_SHADER, vs_src));
   glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_src));
   glBindAttribLocation(prog, 0, "pos");
   glLinkProgram(prog);
   glUseProgram(prog);
   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
   glEnableVertexAttribArray(0);

   glGenTextures(1, &tex_a);
   glBindTexture(GL_TEXTURE_2D, tex_a);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, a);

   glGenTextures(1, &tex_r);
   glBindTexture(GL_TEXTURE_2D, tex_r);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
   fbo_r = fbo_for(tex_r);

   /* the scanout buffer, as glamor creates its screen pixmap */
   bo = gbm_bo_create(gbm, S, S, GBM_FORMAT_ARGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
   if (!bo) {
      fprintf(stderr, "gbm_bo_create failed\n");
      return 1;
   }
   printf("gbm bo: stride %u, modifier 0x%llx\n", gbm_bo_get_stride(bo),
          (unsigned long long)gbm_bo_get_modifier(bo));
   img = create_image(dpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, bo, NULL);
   if (img == EGL_NO_IMAGE_KHR) {
      fprintf(stderr, "eglCreateImageKHR failed: 0x%x\n", eglGetError());
      return 1;
   }
   glGenTextures(1, &tex_scan);
   glBindTexture(GL_TEXTURE_2D, tex_scan);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   image_target(GL_TEXTURE_2D, img);
   fbo_scan = fbo_for(tex_scan);

   /* 1: render into the scanout buffer */
   draw(fbo_scan, tex_a);
   ok &= check_gl("1 render into dma-buf, glReadPixels", fbo_scan, a);

   /* 2: CPU view of the same memory (what decon scans out) */
   {
      uint32_t stride;
      void *map_data = NULL;
      unsigned char *p;

      glFinish();
      p = gbm_bo_map(bo, 0, 0, S, S, GBM_BO_TRANSFER_READ, &stride, &map_data);
      if (!p) {
         printf("2 CPU read via gbm_bo_map            map failed\n");
         ok = 0;
      } else {
         for (int y = 0; y < S; y++)
            memcpy(cpu + y * S * 4, p + y * stride, S * 4);
         gbm_bo_unmap(bo, map_data);
         /* ARGB8888 in memory is B, G, R, A */
         ok &= report("2 CPU read via gbm_bo_map", cpu, a, 1);
      }
   }

   /* 3: sample the scanout buffer */
   draw(fbo_r, tex_scan);
   ok &= check_gl("3 sample dma-buf into FBO", fbo_r, a);

   /* 4: blit from it */
   glBindFramebuffer(GL_FRAMEBUFFER, fbo_r);
   glClearColor(0, 0, 0, 0);
   glClear(GL_COLOR_BUFFER_BIT);
   glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_scan);
   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_r);
   glBlitFramebuffer(0, 0, S, S, 0, 0, S, S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
   ok &= check_gl("4 blit dma-buf -> FBO", fbo_r, a);

   /* 5: blit an ordinary FBO into part of it (like CopyArea to the screen) */
   {
      unsigned char sub[(S / 2) * (S / 2) * 4];

      for (int y = 0; y < S / 2; y++)
         for (int x = 0; x < S / 2; x++)
            pattern(sub + (y * (S / 2) + x) * 4, x, y, 7);
      glBindTexture(GL_TEXTURE_2D, tex_r);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, S / 2, S / 2, GL_RGBA, GL_UNSIGNED_BYTE, sub);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_r);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_scan);
      glBlitFramebuffer(0, 0, S / 2, S / 2, 0, 0, S / 2, S / 2, GL_COLOR_BUFFER_BIT, GL_NEAREST);
      ok &= check_gl("5 blit FBO -> part of dma-buf", fbo_scan, half);
   }

   printf("glGetError: 0x%x\n%s\n", glGetError(), ok ? "DMABUF OK" : "DMABUF WRONG");
   return ok ? 0 : 1;
}
