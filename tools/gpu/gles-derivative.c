/*
 * gles-derivative: dFdx/dFdy/fwidth of a pixel-space varying. Quads of
 * S x S pixels (gap 1, offset O) fill a 64x64 RGBA8 target; the fragment
 * writes |dFdx(p.x)|/4, |dFdy(p.y)|/4, fwidth(p).x/4, so every covered pixel
 * must read 64,64,64. On G71 (Mesa 26.0.8, Bifrost v6 CLPER_OLD) the
 * derivative comes back as the value itself (pixel 1,0 reads 96 = 1.5*64):
 * Firefox WebRender AA ranges blow up and rounded corners turn into blurry
 * squares.
 *
 * Build (on the tablet): gcc -O1 -o gles-derivative gles-derivative.c -lEGL -lGLESv2
 * Run:   ./gles-derivative [S [O]]   (default 64 0)
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
static const char *vs = "#version 300 es\n"
 "in vec2 a; out highp vec2 p; void main(){ p = a; gl_Position = vec4(a/32.0-1.0,0,1); }";
static const char *fs = "#version 300 es\nprecision highp float;\n"
 "in highp vec2 p; out vec4 o; void main(){ o = vec4(abs(dFdx(p.x))/4.0, abs(dFdy(p.y))/4.0, fwidth(p).x/4.0, 1); }";
int main(int argc, char **argv) {
	float S = argc > 1 ? atof(argv[1]) : 64, O = argc > 2 ? atof(argv[2]) : 0;
	EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	eglInitialize(d, 0, 0); eglBindAPI(EGL_OPENGL_ES_API);
	EGLint ca[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
	EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
	eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c);
	printf("%s\n", glGetString(GL_RENDERER));
	GLuint t, f; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 64, 64);
	glGenFramebuffers(1, &f); glBindFramebuffer(GL_FRAMEBUFFER, f);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
	glViewport(0, 0, 64, 64); glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
	GLuint p = glCreateProgram(), s;
	s = glCreateShader(GL_VERTEX_SHADER); glShaderSource(s, 1, &vs, 0); glCompileShader(s); glAttachShader(p, s);
	s = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(s, 1, &fs, 0); glCompileShader(s); glAttachShader(p, s);
	glBindAttribLocation(p, 0, "a"); glLinkProgram(p); glUseProgram(p);
	float v[4096 * 12]; int n = 0;
	for (float y = O; y + S <= 64.01; y += S + 1) for (float x = O; x + S <= 64.01; x += S + 1) {
		float q[12] = { x,y, x+S,y, x,y+S, x+S,y, x+S,y+S, x,y+S };
		for (int i = 0; i < 12; i++) v[n * 12 + i] = q[i]; n++;
	}
	glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, 0, 0, v);
	glDrawArrays(GL_TRIANGLES, 0, n * 6);
	unsigned char px[64 * 64 * 4]; glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px);
	int bad = 0, cov = 0;
	for (int i = 0; i < 64 * 64; i++) {
		if (!px[i*4+3]) continue; cov++;
		for (int k = 0; k < 3; k++) if (abs(px[i*4+k] - 64) > 2) { if (bad < 8) printf("px %d,%d: %d %d %d\n", i%64, i/64, px[i*4], px[i*4+1], px[i*4+2]); bad++; break; }
	}
	printf("S=%g O=%g quads=%d covered=%d bad=%d\n", S, O, n, cov, bad);
	return 0;
}
