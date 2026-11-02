// SPDX-License-Identifier: GPL-2.0
/*
 * Photo enhancement for t510-camera: LUTwithBGrid (Kim and Cho, ECCV 2024,
 * Apache-2.0), trained on MIT-Adobe FiveK to retouch like its expert C.
 *
 * A small network looks at the photo shrunk to about 540x400 and predicts
 * a 3D LUT and six 1D curves (camera/enhance/ has the export). The
 * authors' slicing kernels place every pixel but the last row and column
 * at grid position 0 (an integer division), so the model they trained is
 * a global colour transform: per channel two curves, a 1x1 mix of the
 * curves and the colour into three channels, then the LUT (tetrahedral).
 * That transform is baked into one 65^3 LUT here and laid over the photo
 * at full size.
 *
 * ONNX Runtime is loaded when first needed, so the camera works without
 * it (then there is no enhancement).
 */

#include <dlfcn.h>
#include <math.h>
#include <onnxruntime_c_api.h>
#include <stdlib.h>
#include <string.h>

#include "camera-enhance.h"

#define NV		17	/* vertices of the model's LUT and curves */
#define BAKE		65	/* of the LUT laid over the photo */
#define SMALL		540	/* the long side, about what the model looks at */

static const OrtApi *ort;
static OrtEnv *env;
static OrtSession *session;
static float mix_w[3 * 9], mix_b[3];

static gboolean ort_ok(OrtStatus *st, const char *what)
{
	if (!st)
		return TRUE;
	g_printerr("t510-camera: enhance: %s: %s\n", what,
		   ort->GetErrorMessage(st));
	ort->ReleaseStatus(st);
	return FALSE;
}

/* The mix of curves and colour, as metadata: "w0,w1,..." */
static gboolean read_floats(OrtModelMetadata *md, OrtAllocator *al,
			    const char *key, float *out, int n)
{
	char *val = NULL, *p, *end;
	int i = 0;

	if (!ort_ok(ort->ModelMetadataLookupCustomMetadataMap(md, al, key, &val),
		    key) || !val)
		return FALSE;
	for (p = val; i < n; p = end + 1) {
		out[i++] = strtof(p, &end);
		if (end == p || (*end != ',' && *end))
			break;
		if (!*end)
			break;
	}
	ort_ok(ort->AllocatorFree(al, val), "free");
	return i == n;
}

/* T510_CAMERA_MODEL=file tries another model */
static gboolean load(void)
{
	const char *model = g_getenv("T510_CAMERA_MODEL");
	const OrtApiBase *(*get_base)(void);
	OrtSessionOptions *opts = NULL;
	OrtModelMetadata *md = NULL;
	OrtAllocator *al = NULL;
	gboolean ok;
	void *lib;

	if (!model)
		model = ENHANCE_MODEL;
	if (!g_file_test(model, G_FILE_TEST_EXISTS))
		return FALSE;
	lib = dlopen("libonnxruntime.so.1.23", RTLD_NOW | RTLD_LOCAL);
	if (!lib)
		lib = dlopen("libonnxruntime.so", RTLD_NOW | RTLD_LOCAL);
	if (!lib) {
		g_printerr("t510-camera: enhance: no ONNX Runtime: %s\n", dlerror());
		return FALSE;
	}
	get_base = (const OrtApiBase *(*)(void))dlsym(lib, "OrtGetApiBase");
	ort = get_base ? get_base()->GetApi(ORT_API_VERSION) : NULL;
	if (!ort)
		return FALSE;

	ok = ort_ok(ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "t510-camera", &env),
		    "env") &&
	     ort_ok(ort->CreateSessionOptions(&opts), "options") &&
	     ort_ok(ort->SetIntraOpNumThreads(opts, 4), "threads") &&
	     ort_ok(ort->CreateSession(env, model, opts, &session), model) &&
	     ort_ok(ort->GetAllocatorWithDefaultOptions(&al), "allocator") &&
	     ort_ok(ort->SessionGetModelMetadata(session, &md), "metadata") &&
	     read_floats(md, al, "mix_weight", mix_w, 3 * 9) &&
	     read_floats(md, al, "mix_bias", mix_b, 3);
	if (md)
		ort->ReleaseModelMetadata(md);
	if (opts)
		ort->ReleaseSessionOptions(opts);
	if (!ok && session) {
		ort->ReleaseSession(session);
		session = NULL;
	}
	return ok;
}

gboolean enhance_available(void)
{
	static gsize once;
	static gboolean ok;

	if (g_once_init_enter(&once)) {
		ok = load();
		g_once_init_leave(&once, 1);
	}
	return ok;
}

/* The photo shrunk by area averaging, as the model's planar 0..1 input */
static float *shrink(const guint8 *data, int w, int h, int stride,
		     int *sw, int *sh)
{
	int f = MAX(1, MAX(w, h) / SMALL);
	int ow = w / f, oh = h / f;
	float *out = g_new(float, 3 * ow * oh);
	float norm = 1.0f / (255.0f * f * f);

	for (int y = 0; y < oh; y++) {
		for (int x = 0; x < ow; x++) {
			unsigned int s[3] = { 0, 0, 0 };

			for (int dy = 0; dy < f; dy++) {
				const guint8 *p = data + (y * f + dy) * stride +
						  x * f * 3;

				for (int dx = 0; dx < f; dx++, p += 3) {
					s[0] += p[0];
					s[1] += p[1];
					s[2] += p[2];
				}
			}
			for (int c = 0; c < 3; c++)
				out[(c * oh + y) * ow + x] = s[c] * norm;
		}
	}
	*sw = ow;
	*sh = oh;
	return out;
}

static gboolean predict(const guint8 *data, int w, int h, int stride,
			float *lut, float *curves)
{
	const char *in_names[] = { "image" };
	const char *out_names[] = { "lut", "curves" };
	OrtMemoryInfo *mi = NULL;
	OrtValue *in = NULL, *out[2] = { NULL, NULL };
	int64_t shape[4];
	float *small, *p;
	int sw, sh;
	gboolean ok;

	small = shrink(data, w, h, stride, &sw, &sh);
	shape[0] = 1;
	shape[1] = 3;
	shape[2] = sh;
	shape[3] = sw;
	ok = ort_ok(ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
					     &mi), "memory") &&
	     ort_ok(ort->CreateTensorWithDataAsOrtValue(mi, small,
			sizeof(float) * 3 * sw * sh, shape, 4,
			ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in), "input") &&
	     ort_ok(ort->Run(session, NULL, in_names, (const OrtValue *const *)&in,
			     1, out_names, 2, out), "run");
	if (ok && ort_ok(ort->GetTensorMutableData(out[0], (void **)&p), "lut"))
		memcpy(lut, p, sizeof(float) * 3 * NV * NV * NV);
	else
		ok = FALSE;
	if (ok && ort_ok(ort->GetTensorMutableData(out[1], (void **)&p), "curves"))
		memcpy(curves, p, sizeof(float) * 6 * NV);
	else
		ok = FALSE;

	for (int i = 0; i < 2; i++)
		if (out[i])
			ort->ReleaseValue(out[i]);
	if (in)
		ort->ReleaseValue(in);
	if (mi)
		ort->ReleaseMemoryInfo(mi);
	g_free(small);
	return ok;
}

/* Where v falls among n vertices: the lower one and the way to the next */
static int cell(float v, int n, float *d)
{
	int i = (int)floorf(v * (n - 1));

	i = CLAMP(i, 0, n - 2);
	*d = v * (n - 1) - i;
	return i;
}

static float curve(const float *c, float v)
{
	float d;
	int i = cell(v, NV, &d);

	return c[i] * (1 - d) + c[i + 1] * d;
}

/* The authors' tetrahedral LUT transform; lut[c][b][g][r] */
static void tetra(const float *lut, const float *in, float *out)
{
	const int n = NV, sh = NV * NV * NV;
	float rd, gd, bd, w0, w1, w2, w3;
	int r = cell(in[0], n, &rd);
	int g = cell(in[1], n, &gd);
	int b = cell(in[2], n, &bd);
	int i000 = r + g * n + b * n * n;
	int i111 = i000 + 1 + n + n * n;
	int i1, i2;

	if (rd > gd) {
		if (gd > bd) {
			i1 = i000 + 1;			i2 = i000 + 1 + n;
			w0 = 1 - rd; w1 = rd - gd; w2 = gd - bd; w3 = bd;
		} else if (rd > bd) {
			i1 = i000 + 1;			i2 = i000 + 1 + n * n;
			w0 = 1 - rd; w1 = rd - bd; w2 = bd - gd; w3 = gd;
		} else {
			i1 = i000 + n * n;		i2 = i000 + 1 + n * n;
			w0 = 1 - bd; w1 = bd - rd; w2 = rd - gd; w3 = gd;
		}
	} else {
		if (bd > gd) {
			i1 = i000 + n * n;		i2 = i000 + n + n * n;
			w0 = 1 - bd; w1 = bd - gd; w2 = gd - rd; w3 = rd;
		} else if (bd > rd) {
			i1 = i000 + n;			i2 = i000 + n + n * n;
			w0 = 1 - gd; w1 = gd - bd; w2 = bd - rd; w3 = rd;
		} else {
			i1 = i000 + n;			i2 = i000 + 1 + n;
			w0 = 1 - gd; w1 = gd - rd; w2 = rd - bd; w3 = bd;
		}
	}
	for (int c = 0; c < 3; c++) {
		const float *l = lut + c * sh;

		out[c] = w0 * l[i000] + w1 * l[i1] + w2 * l[i2] + w3 * l[i111];
	}
}

/* The model's transform of one colour, 0..1 */
static void transform(const float *lut, const float *curves,
		      const float *rgb, float *out)
{
	float feat[9], mid[3];

	for (int k = 0; k < 6; k++)
		feat[k] = curve(curves + k * NV, rgb[k / 2]);
	feat[6] = rgb[0];
	feat[7] = rgb[1];
	feat[8] = rgb[2];
	for (int c = 0; c < 3; c++) {
		float v = mix_b[c];

		for (int k = 0; k < 9; k++)
			v += mix_w[c * 9 + k] * feat[k];
		mid[c] = CLAMP(v, 0.0f, 1.0f);
	}
	tetra(lut, mid, out);
	for (int c = 0; c < 3; c++)
		out[c] = CLAMP(out[c], 0.0f, 1.0f) * 255.0f;
}

/* The baked LUT, [b][g][r][c] in 0..255 */
static float *bake(const float *lut, const float *curves)
{
	float *baked = g_new(float, BAKE * BAKE * BAKE * 3);
	float *p = baked;

	for (int b = 0; b < BAKE; b++)
		for (int g = 0; g < BAKE; g++)
			for (int r = 0; r < BAKE; r++, p += 3) {
				float rgb[3] = {
					(float)r / (BAKE - 1),
					(float)g / (BAKE - 1),
					(float)b / (BAKE - 1),
				};

				transform(lut, curves, rgb, p);
			}
	return baked;
}

struct lay {
	const float *baked;
	guint8 *data;
	int width, stride;
	int y0, y1;
};

/* Trilinear over the baked LUT, a band of rows */
static gpointer lay_rows(gpointer data)
{
	const struct lay *l = data;
	const int n = BAKE, sg = n * 3, sb = n * n * 3;
	float pos[256];
	int idx[256];

	for (int v = 0; v < 256; v++) {
		float f = v * (n - 1) / 255.0f;

		idx[v] = MIN((int)f, n - 2);
		pos[v] = f - idx[v];
	}

	for (int y = l->y0; y < l->y1; y++) {
		guint8 *px = l->data + y * l->stride;

		for (int x = 0; x < l->width; x++, px += 3) {
			const float *c = l->baked + idx[px[0]] * 3 +
					 idx[px[1]] * sg + idx[px[2]] * sb;
			float fr = pos[px[0]], fg = pos[px[1]], fb = pos[px[2]];

			for (int k = 0; k < 3; k++) {
				const float *q = c + k;
				float c00 = q[0] + (q[3] - q[0]) * fr;
				float c10 = q[sg] + (q[sg + 3] - q[sg]) * fr;
				float c01 = q[sb] + (q[sb + 3] - q[sb]) * fr;
				float c11 = q[sg + sb] +
					    (q[sg + sb + 3] - q[sg + sb]) * fr;
				float c0 = c00 + (c10 - c00) * fg;
				float c1 = c01 + (c11 - c01) * fg;

				px[k] = (guint8)(c0 + (c1 - c0) * fb + 0.5f);
			}
		}
	}
	return NULL;
}

gboolean enhance_rgb(guint8 *data, int width, int height, int stride)
{
	float lut[3 * NV * NV * NV], curves[6 * NV], *baked;
	struct lay bands[4];
	GThread *threads[4];
	int n = G_N_ELEMENTS(bands);

	if (!enhance_available() ||
	    !predict(data, width, height, stride, lut, curves))
		return FALSE;

	baked = bake(lut, curves);
	for (int i = 0; i < n; i++) {
		bands[i] = (struct lay) {
			.baked = baked, .data = data,
			.width = width, .stride = stride,
			.y0 = height * i / n, .y1 = height * (i + 1) / n,
		};
		threads[i] = g_thread_new("enhance", lay_rows, &bands[i]);
	}
	for (int i = 0; i < n; i++)
		g_thread_join(threads[i]);
	g_free(baked);
	return TRUE;
}
