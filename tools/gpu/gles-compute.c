/*
 * gles-compute: does the GPU execute shader code and write memory?
 * A GLES 3.1 compute shader writes id*3+7 for 1024 invocations into an SSBO.
 *
 * Build (on the tablet): gcc -O2 -o gles-compute gles-compute.c -lEGL -lGLESv2
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <stdio.h>

#define N 1024

static const char *cs_src =
   "#version 310 es\n"
   "layout(local_size_x = 64) in;\n"
   "layout(std430, binding = 0) buffer Out { uint v[]; };\n"
   "void main() { uint i = gl_GlobalInvocationID.x; v[i] = i * 3u + 7u; }\n";

int
main(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
   EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
   EGLint ctx_attr[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
   EGLConfig cfg;
   EGLint n;
   GLuint s, prog, buf;
   GLint ok;
   const GLuint *v;
   int bad = 0;

   if (!eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API) ||
       !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || !n)
      return 1;
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
                  eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr));
   printf("renderer: %s\n", glGetString(GL_RENDERER));

   s = glCreateShader(GL_COMPUTE_SHADER);
   glShaderSource(s, 1, &cs_src, NULL);
   glCompileShader(s);
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[1024];
      glGetShaderInfoLog(s, sizeof(log), NULL, log);
      fprintf(stderr, "compile: %s\n", log);
      return 1;
   }
   prog = glCreateProgram();
   glAttachShader(prog, s);
   glLinkProgram(prog);
   glUseProgram(prog);

   glGenBuffers(1, &buf);
   glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf);
   glBufferData(GL_SHADER_STORAGE_BUFFER, N * 4, NULL, GL_DYNAMIC_READ);
   glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, buf);
   glDispatchCompute(N / 64, 1, 1);
   glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);

   v = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0, N * 4, GL_MAP_READ_BIT);
   if (!v) {
      fprintf(stderr, "map failed: 0x%x\n", glGetError());
      return 1;
   }
   for (int i = 0; i < N; i++)
      if (v[i] != (GLuint)i * 3 + 7) {
         if (bad++ < 5)
            printf("  v[%d] = %u, want %u\n", i, v[i], i * 3 + 7);
      }
   printf("v[0..3] = %u %u %u %u, v[1023] = %u\n", v[0], v[1], v[2], v[3], v[N - 1]);
   printf("%s (%d wrong of %d)\n", bad ? "COMPUTE WRONG" : "COMPUTE OK", bad, N);
   return bad != 0;
}
