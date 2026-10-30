/*
 * gles-texture: the texture paths glamor (X 2D acceleration) depends on.
 * EGL surfaceless + GLES 3, every step read back and checked:
 *   1. glTexImage2D upload, sampled onto a quad
 *   2. glTexSubImage2D partial update of that texture
 *   3. render to a texture (FBO), then sample it in a second pass
 *   4. glBlitFramebuffer between two FBO textures
 *   5. glCopyTexSubImage2D from the framebuffer
 *
 * Build (on the tablet): gcc -O2 -o gles-texture gles-texture.c -lEGL -lGLESv2
 * Run:   LD_PRELOAD=/opt/mesa-kbase/lib/libpanfrost_kbase_drm_shim.so ./gles-texture
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S 64

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

/* texel (x, y) of the test pattern: red ramp in x, green ramp in y, blue by quadrant */
static void
pattern(unsigned char *p, int x, int y, int salt)
{
   p[0] = x * 4;
   p[1] = y * 4;
   p[2] = ((x < S / 2) ^ (y < S / 2)) ? 200 : 40 + salt;
   p[3] = 255;
}

static GLuint
make_tex(const unsigned char *data)
{
   GLuint t;

   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_2D, t);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
   return t;
}

static GLuint
make_fbo(GLuint tex)
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
draw_tex(GLuint fbo, GLuint tex)
{
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glViewport(0, 0, S, S);
   glBindTexture(GL_TEXTURE_2D, tex);
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* compare the framebuffer bound for reading with expect[] (S x S RGBA) */
static int
check(const char *what, GLuint fbo, const unsigned char *expect)
{
   unsigned char got[S * S * 4];
   int bad = 0, first = -1;

   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   memset(got, 0x55, sizeof(got));
   glReadPixels(0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, got);
   for (int i = 0; i < S * S; i++)
      if (memcmp(got + i * 4, expect + i * 4, 4)) {
         if (first < 0)
            first = i;
         bad++;
      }
   printf("%-34s %s", what, bad ? "WRONG" : "ok");
   if (bad)
      printf(" (%d of %d texels; first at (%d,%d): got %u %u %u %u, want %u %u %u %u)",
             bad, S * S, first % S, first / S, got[first * 4], got[first * 4 + 1],
             got[first * 4 + 2], got[first * 4 + 3], expect[first * 4], expect[first * 4 + 1],
             expect[first * 4 + 2], expect[first * 4 + 3]);
   printf("\n");
   return bad == 0;
}

int
main(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
   EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
   static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
   static unsigned char a[S * S * 4], b[S * S * 4], half[S * S * 4], zero[S * S * 4];
   EGLConfig cfg;
   EGLint n;
   GLuint prog, vbo, tex_a, tex_r1, tex_r2, fbo_r1, fbo_r2;
   int ok = 1;

   if (!eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API) ||
       !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || !n)
      return 1;
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                  eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr));
   printf("renderer: %s\n", glGetString(GL_RENDERER));

   for (int y = 0; y < S; y++)
      for (int x = 0; x < S; x++) {
         pattern(a + (y * S + x) * 4, x, y, 0);
         pattern(b + (y * S + x) * 4, x, y, 7);
         /* a with the lower-left quarter replaced by b */
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

   tex_a = make_tex(a);
   tex_r1 = make_tex(zero);
   tex_r2 = make_tex(zero);
   fbo_r1 = make_fbo(tex_r1);
   fbo_r2 = make_fbo(tex_r2);

   /* 1: uploaded texture sampled onto the render target */
   draw_tex(fbo_r1, tex_a);
   ok &= check("1 upload + sample", fbo_r1, a);

   /* 2: partial update, then sample again */
   {
      unsigned char sub[(S / 2) * (S / 2) * 4];

      for (int y = 0; y < S / 2; y++)
         memcpy(sub + y * (S / 2) * 4, b + y * S * 4, (S / 2) * 4);
      glBindTexture(GL_TEXTURE_2D, tex_a);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, S / 2, S / 2, GL_RGBA, GL_UNSIGNED_BYTE, sub);
   }
   draw_tex(fbo_r1, tex_a);
   ok &= check("2 sub-image update + sample", fbo_r1, half);

   /* 3: the render target (tex_r1, now = half) used as a texture */
   draw_tex(fbo_r2, tex_r1);
   ok &= check("3 render to texture, then sample", fbo_r2, half);

   /* 4: blit r1 -> r2 after clearing r2 */
   glBindFramebuffer(GL_FRAMEBUFFER, fbo_r2);
   glClearColor(0, 0, 0, 0);
   glClear(GL_COLOR_BUFFER_BIT);
   glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_r1);
   glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_r2);
   glBlitFramebuffer(0, 0, S, S, 0, 0, S, S, GL_COLOR_BUFFER_BIT, GL_NEAREST);
   ok &= check("4 glBlitFramebuffer", fbo_r2, half);

   /* 5: copy from the framebuffer into a texture, then sample it */
   glBindFramebuffer(GL_FRAMEBUFFER, fbo_r1);
   glBindTexture(GL_TEXTURE_2D, tex_a);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
   glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, S, S);
   draw_tex(fbo_r2, tex_a);
   ok &= check("5 glCopyTexSubImage2D + sample", fbo_r2, half);

   printf("glGetError: 0x%x\n%s\n", glGetError(), ok ? "TEXTURES OK" : "TEXTURES WRONG");
   return ok ? 0 : 1;
}
