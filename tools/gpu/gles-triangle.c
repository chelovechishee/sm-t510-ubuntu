/*
 * gles-triangle: smallest end-to-end GPU rendering check.
 * EGL surfaceless + GLES2 into a 256x256 FBO: clear green, draw a red
 * triangle, read back and check three pixels. Optional PPM dump.
 *
 * Build (on the tablet): gcc -O2 -o gles-triangle gles-triangle.c -lEGL -lGLESv2
 * Run:   LD_PRELOAD=/opt/mesa-kbase/lib/libpanfrost_kbase_drm_shim.so ./gles-triangle [out.ppm]
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 256
#define H 256

static const char *vs_src =
   "attribute vec2 pos;\n"
   "void main() { gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fs_src =
   "precision mediump float;\n"
   "void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n";

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
check(const unsigned char *px, int x, int y, unsigned r, unsigned g, unsigned b)
{
   const unsigned char *p = px + (y * W + x) * 4;
   int ok = p[0] == r && p[1] == g && p[2] == b;

   printf("  pixel (%3d,%3d) = %3u %3u %3u, want %3u %3u %3u: %s\n", x, y, p[0], p[1], p[2],
          r, g, b, ok ? "ok" : "WRONG");
   return ok;
}

int
main(int argc, char **argv)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
   EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   static const GLfloat tri[] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f };
   EGLConfig cfg;
   EGLint n;
   EGLContext ctx;
   GLuint fbo, rb, prog;
   unsigned char *px;
   int ok = 1;

   if (!eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API) ||
       !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || !n) {
      fprintf(stderr, "EGL setup failed\n");
      return 1;
   }
   ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
   printf("renderer: %s\n", glGetString(GL_RENDERER));

   glGenRenderbuffers(1, &rb);
   glBindRenderbuffer(GL_RENDERBUFFER, rb);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, W, H);
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "FBO incomplete\n");
      return 1;
   }

   prog = glCreateProgram();
   glAttachShader(prog, shader(GL_VERTEX_SHADER, vs_src));
   glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_src));
   glBindAttribLocation(prog, 0, "pos");
   glLinkProgram(prog);
   glUseProgram(prog);

   glViewport(0, 0, W, H);
   glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
   glEnableVertexAttribArray(0);
   glDrawArrays(GL_TRIANGLES, 0, 3);

   px = malloc(W * H * 4);
   memset(px, 0x55, W * H * 4);
   glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
   printf("glGetError: 0x%x\n", glGetError());

   ok &= check(px, 5, 5, 0, 255, 0);         /* corner: clear colour */
   ok &= check(px, W / 2, H / 2, 255, 0, 0); /* centre: triangle */
   ok &= check(px, W / 2, H - 5, 0, 255, 0); /* above the apex */

   if (argc > 1) {
      FILE *f = fopen(argv[1], "wb");
      fprintf(f, "P6 %d %d 255\n", W, H);
      for (int y = H - 1; y >= 0; y--)
         for (int x = 0; x < W; x++)
            fwrite(px + (y * W + x) * 4, 1, 3, f);
      fclose(f);
   }
   printf("%s\n", ok ? "RENDERING OK" : "RENDERING WRONG");
   return ok ? 0 : 1;
}
