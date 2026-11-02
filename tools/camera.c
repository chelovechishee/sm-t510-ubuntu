// SPDX-License-Identifier: GPL-2.0
/*
 * t510-camera: a light camera for the SM-T510.
 *
 * The preview comes from the binned sensor modes (about 25 frames a second)
 * and is turned upright on the GPU. A photo restarts the camera at its full
 * size for one frame; a video records the preview stream with the built-in
 * microphone. libcamera (its software ISP) and GStreamer do all the work on
 * the pictures; this program only connects them and draws the buttons.
 *
 * Build: cc -O2 -I/usr/include/onnxruntime -o t510-camera camera.c camera-enhance.c $(pkg-config --cflags --libs gtk+-3.0 gstreamer-1.0 gstreamer-video-1.0) -lm
 * (scripts/make-debs.sh builds it into t510-support as /usr/bin/t510-camera)
 */

#include <errno.h>
#include <fcntl.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gtk/gtk.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <math.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "camera-enhance.h"

#define PREVIEW_W	1280
#define PREVIEW_H	960
/*
 * Frames to let go by before a photo: with the exposure of the preview
 * only the white balance has to settle, without it the auto exposure too.
 */
#define STILL_SKIP_KNOWN	5
#define STILL_SKIP_AUTO		12
#define STILL_TIMEOUT_S	20

/*
 * A tap on the preview picks the point of interest: the exposure is
 * metered there (libcamera's software ISP takes the AfWindows control for
 * it) and the rear lens is focused on it by contrast. Once the exposure
 * has settled on the new point, the lens sweeps its range from far to near
 * a step each frame (stopping well past a clear peak), then back over the
 * peak in finer steps. Frames reach us a few frames after they were
 * taken: each counts for where the lens was while it was exposed, from
 * its timestamp (the start of its readout) and the exposure time.
 */
#define POI_SIZE	8	/* the window is 1/8 of the frame wide */
/*
 * Focus windows around the point, 1/8, 1/4 and 1/2 of the frame wide: the
 * smallest with a clear peak counts, a larger one where the point itself
 * is a plain surface.
 */
static const int af_windows[] = { 8, 4, 2 };
#define AF_WINDOWS	G_N_ELEMENTS(af_windows)
#define MARKER_SIZE	96
#define AF_SWEEP_STEP	40	/* lens codes a frame */
#define AF_FINE_SPAN	60	/* the fine sweep: peak +- this */
#define AF_FINE_STEP	10
#define AF_LENS_LAG_NS	(5 * 1000000LL)		/* a step settles */
#define AF_JUMP_NS	(40 * 1000000LL)	/* a jump to a sweep's start */
#define AF_HOLD_NS	(300 * 1000000LL)	/* and a frame after it, at most */
#define AF_AE_FRAMES	8	/* exposure x gain the same for that long */
#define AF_AE_SAME	0.02
#define AF_AE_WAIT_NS	(1500 * 1000000LL)	/* or that long at most */
#define AF_PERIOD_DEFAULT_NS	(40 * 1000000LL)
#define AF_PERIOD_MIN_NS	(15 * 1000000LL)
#define AF_PERIOD_MAX_NS	(200 * 1000000LL)
#define AF_MIN_GAIN	1.2	/* sharpest/softest below that: no focus */
#define AF_FINE_GAIN	1.02
#define AF_PAST_PEAK	0.6	/* two samples below that share of the best */
#define AF_MAX_MOVES	64
#define AF_MAX_SAMPLES	64

/* t510-rotate keeps the desktop's rotation here */
#define ROTATION_STATE	"/var/lib/t510/rotation"

/*
 * The eight ways videoflip can turn a picture, as matrices on (x, y) with
 * y pointing down: new = m * old.
 */
enum flip { FLIP_NONE, FLIP_90R, FLIP_180, FLIP_90L,
	    FLIP_HORIZ, FLIP_VERT, FLIP_UL_LR, FLIP_UR_LL };

static const struct {
	const char *name;
	int m[4];
} flips[] = {
	[FLIP_NONE]	= { "identity",	{  1,  0,  0,  1 } },
	[FLIP_90R]	= { "90r",	{  0, -1,  1,  0 } },
	[FLIP_180]	= { "180",	{ -1,  0,  0, -1 } },
	[FLIP_90L]	= { "90l",	{  0,  1, -1,  0 } },
	[FLIP_HORIZ]	= { "horiz",	{ -1,  0,  0,  1 } },
	[FLIP_VERT]	= { "vert",	{  1,  0,  0, -1 } },
	[FLIP_UL_LR]	= { "ul-lr",	{  0,  1,  1,  0 } },
	[FLIP_UR_LL]	= { "ur-ll",	{  0, -1, -1,  0 } },
};

/* first a, then b */
static enum flip flip_then(enum flip a, enum flip b)
{
	const int *x = flips[b].m, *y = flips[a].m;
	int m[4] = {
		x[0] * y[0] + x[1] * y[2], x[0] * y[1] + x[1] * y[3],
		x[2] * y[0] + x[3] * y[2], x[2] * y[1] + x[3] * y[3],
	};

	for (unsigned int i = 0; i < G_N_ELEMENTS(flips); i++)
		if (!memcmp(m, flips[i].m, sizeof(m)))
			return i;
	return FLIP_NONE;
}

/*
 * glvideoflip has the diagonals the other way round than videoflip (GL's
 * y axis points up); the turns are the same.
 */
static const char *gl_flip_name(enum flip f)
{
	if (f == FLIP_UL_LR)
		return flips[FLIP_UR_LL].name;
	if (f == FLIP_UR_LL)
		return flips[FLIP_UL_LR].name;
	return flips[f].name;
}

struct cam {
	const char *id;		/* libcamera's name of the camera */
	const char *sensor;	/* its V4L2 subdev name starts so */
	const char *lens;	/* the focus motor's subdev, if there is one */
	const char *name;
	int sensor_w, sensor_h;	/* the pixel array, AfWindows coordinates */
	int still_w, still_h;	/* the largest the software ISP gives */
	/*
	 * Both sensors are mounted turned: on the desktop rotated upside
	 * down (portrait-inverted, the USB port up) the pictures need a
	 * quarter turn counterclockwise (DT rotation = 90). The front
	 * preview is mirrored, as people expect from a selfie camera; its
	 * files are not. Other desktop rotations add their own turn
	 * (desktop_turn()).
	 */
	enum flip preview_turn;
	enum flip file_turn;
};

static const struct cam cams[] = {
	{ "/base/soc@0/i2c@138b0000/camera@2d", "s5k4ha", "dw9807", "rear",
	  3264, 2448, 3248, 2436, FLIP_90L, FLIP_90L },
	{ "/base/soc@0/i2c@138a0000/camera@2d", "s5k5e9", NULL, "front",
	  2592, 1944, 2576, 1932, FLIP_UR_LL, FLIP_90L },
};

/*
 * The turn to add for the way the desktop is rotated, against the
 * portrait-inverted one the turns above are for. The pictures follow the
 * desktop, not the accelerometer: what the screen shows upright is upright
 * in the files too. X turns the desktop on the panel ("left" is a quarter
 * turn counterclockwise), so the picture turns the other way.
 */
static enum flip desktop_turn(void)
{
	static const struct {
		const char *name;
		enum flip turn;
	} turns[] = {
		{ "portrait", FLIP_180 },		/* RandR normal */
		{ "landscape-ccw", FLIP_90L },		/* RandR left */
		{ "portrait-inverted", FLIP_NONE },	/* RandR inverted */
		{ "landscape-cw", FLIP_90R },		/* RandR right */
	};
	enum flip turn = FLIP_180;	/* no state: t510-rotate says portrait */
	char *state = NULL;

	if (g_file_get_contents(ROTATION_STATE, &state, NULL, NULL)) {
		g_strstrip(state);
		for (unsigned int i = 0; i < G_N_ELEMENTS(turns); i++)
			if (!strcmp(state, turns[i].name))
				turn = turns[i].turn;
	}
	g_free(state);
	return turn;
}

enum state { IDLE, PREVIEW, STILL, RECORDING, STOPPING };

enum af_phase { AF_OFF, AF_WAIT_AE, AF_SWEEP, AF_FINE };

/* The streaming thread focuses: everything here is under the lock */
struct af {
	GMutex lock;
	enum af_phase phase;
	int fd;			/* the lens subdev */
	GstElement *cam;	/* libcamerasrc: the exposure */
	int min, max;		/* the lens's range */
	int start;		/* where it was: kept when nothing is sharp */
	int cur, target, step;	/* this sweep */
	double peak;		/* of the coarse sweep */
	/* the moves of this sweep: from then on (+ lag) the lens was there */
	gint64 move_t[AF_MAX_MOVES];
	int move_pos[AF_MAX_MOVES];
	unsigned int moves;
	gint64 settled;		/* frames exposed from then on count */
	double pos[AF_MAX_SAMPLES];
	double score[AF_MAX_SAMPLES][AF_WINDOWS];
	unsigned int n;
	int win;		/* the window the fine sweep goes by */
	gint64 last_start, period;	/* of the frames */
	gint64 ae_since;	/* waiting for the exposure */
	double ae_last;
	unsigned int ae_same;
	gboolean debug;
};

struct app {
	GtkWidget *window;
	GtkWidget *view_box;	/* holds the video widget of the pipeline */
	GtkWidget *status;
	GtkWidget *shutter;
	GtkWidget *mode_photo;
	GtkWidget *enhance;	/* photos get the learned retouch */
	GtkWidget *switch_cam;
	GtkWidget *marker;	/* the square around the point of interest */
	GstElement *pipeline;
	guint bus_watch;
	enum state state;
	unsigned int cam;
	enum flip desktop;	/* desktop_turn(), read at the start */
	/*
	 * The point of interest in the camera's frame, 0..1 (poi_x < 0: none).
	 * The streaming thread focuses there while measure is set;
	 * measure_gen tells its reports from those of an earlier focusing.
	 */
	double poi_x, poi_y;
	gint measure;
	gint measure_gen;
	struct af af;
	unsigned int still_count;
	unsigned int still_skip;
	gint64 rec_start;
	guint rec_timer;
	guint still_timeout;
	char *last_file;
};

static void start_preview(struct app *a);

static void set_status(struct app *a, const char *text)
{
	gtk_label_set_text(GTK_LABEL(a->status), text ? text : "");
	gtk_widget_set_visible(a->status, text && *text);
}

static void af_stop(struct app *a);

static char *media_dir(void)
{
	const char *pics = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
	char *dir = g_build_filename(pics ? pics : g_get_home_dir(), "Camera",
				     NULL);

	g_mkdir_with_parents(dir, 0755);
	return dir;
}

static char *new_file_name(const char *prefix, const char *ext)
{
	char stamp[32], *dir, *name, *path;
	time_t now = time(NULL);

	strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", localtime(&now));
	dir = media_dir();
	name = g_strdup_printf("%s_%s.%s", prefix, stamp, ext);
	path = g_build_filename(dir, name, NULL);
	g_free(dir);
	g_free(name);
	return path;
}

/* The video widget dies with its sink: never leave one without the other */
static void clear_view(struct app *a)
{
	GList *old = gtk_container_get_children(GTK_CONTAINER(a->view_box));

	for (GList *l = old; l; l = l->next)
		gtk_widget_destroy(l->data);
	g_list_free(old);
}

static void stop_pipeline(struct app *a)
{
	af_stop(a);
	if (!a->pipeline)
		return;

	gst_element_set_state(a->pipeline, GST_STATE_NULL);
	if (a->view_box)
		clear_view(a);
	if (a->bus_watch)
		g_source_remove(a->bus_watch);
	a->bus_watch = 0;
	gst_clear_object(&a->pipeline);
	a->state = IDLE;
}

static void update_buttons(struct app *a)
{
	gboolean busy = a->state == STILL || a->state == STOPPING;
	GtkStyleContext *sc = gtk_widget_get_style_context(a->shutter);

	gtk_widget_set_sensitive(a->shutter, !busy);
	gtk_widget_set_sensitive(a->switch_cam,
				 a->state == PREVIEW || a->state == IDLE);
	gtk_widget_set_sensitive(a->mode_photo, a->state != RECORDING && !busy);

	if (a->state == RECORDING)
		gtk_style_context_add_class(sc, "recording");
	else
		gtk_style_context_remove_class(sc, "recording");
}

static gboolean on_bus(GstBus *bus, GstMessage *msg, gpointer data)
{
	struct app *a = data;
	GError *err = NULL;
	char *dbg = NULL, *text;

	switch (GST_MESSAGE_TYPE(msg)) {
	case GST_MESSAGE_ERROR:
		if (a->still_timeout) {
			g_source_remove(a->still_timeout);
			a->still_timeout = 0;
		}
		gst_message_parse_error(msg, &err, &dbg);
		g_printerr("t510-camera: %s\n%s\n", err->message, dbg ? dbg : "");
		text = g_strdup_printf("Camera error: %s", err->message);
		stop_pipeline(a);
		set_status(a, text);
		g_free(text);
		g_clear_error(&err);
		g_free(dbg);
		if (a->rec_timer) {
			g_source_remove(a->rec_timer);
			a->rec_timer = 0;
		}
		update_buttons(a);
		break;
	case GST_MESSAGE_EOS: {
		/* A photo or a video is complete: back to the preview */
		gboolean saved = a->state == STILL || a->state == STOPPING;

		if (a->still_timeout) {
			g_source_remove(a->still_timeout);
			a->still_timeout = 0;
		}
		stop_pipeline(a);
		text = saved ? g_strdup_printf("Saved %s", a->last_file) :
			       g_strdup("The camera stopped");
		start_preview(a);
		set_status(a, text);
		g_free(text);
		break;
	}
	default:
		break;
	}

	return G_SOURCE_CONTINUE;
}

/* The pipeline brings its own video widget; it replaces the old one */
static gboolean show_sink(struct app *a, GstElement *sink)
{
	GtkWidget *widget = NULL;

	clear_view(a);
	g_object_get(sink, "widget", &widget, NULL);
	if (!widget)
		return FALSE;

	gtk_widget_set_hexpand(widget, TRUE);
	gtk_widget_set_vexpand(widget, TRUE);
	gtk_container_add(GTK_CONTAINER(a->view_box), widget);
	gtk_widget_show(widget);
	g_object_unref(widget);
	return TRUE;
}

/*
 * Meter at the point of interest, if there is one: libcamera takes the
 * first AfWindows rectangle, in the sensor's pixel array, for it. A new
 * pipeline starts metering the whole frame.
 */
static void meter_poi(struct app *a)
{
	const struct cam *c = &cams[a->cam];
	int w = c->sensor_w / POI_SIZE, h = w;
	int x = CLAMP(a->poi_x * c->sensor_w - w / 2, 0, c->sensor_w - w);
	int y = CLAMP(a->poi_y * c->sensor_h - h / 2, 0, c->sensor_h - h);
	GstElement *cam;
	char *win;

	if (a->poi_x < 0 || !a->pipeline)
		return;

	cam = gst_bin_get_by_name(GST_BIN(a->pipeline), "cam");
	win = g_strdup_printf("<<%d,%d,%d,%d>>", x, y, w, h);
	gst_util_set_object_arg(G_OBJECT(cam), "af-windows", win);
	g_free(win);
	gst_object_unref(cam);
}

/*
 * One pipeline at a time: the preview alone, or the preview and the
 * recorder. A photo uses a pipeline of its own at the full size.
 */
static gboolean run_pipeline(struct app *a, const char *desc, gboolean view)
{
	GError *err = NULL;
	GstBus *bus;

	a->pipeline = gst_parse_launch(desc, &err);
	if (!a->pipeline) {
		g_printerr("t510-camera: %s\n%s\n", err->message, desc);
		set_status(a, err->message);
		g_clear_error(&err);
		return FALSE;
	}

	if (view) {
		GstElement *bin = gst_bin_get_by_name(GST_BIN(a->pipeline), "out");
		GstElement *sink = gst_element_factory_make("gtkglsink", NULL);

		if (!bin || !sink || !show_sink(a, sink)) {
			set_status(a, "No video output (gtkglsink)");
			gst_clear_object(&bin);
			stop_pipeline(a);
			return FALSE;
		}
		g_object_set(bin, "sink", sink, NULL);
		gst_object_unref(bin);
	}

	meter_poi(a);
	bus = gst_element_get_bus(a->pipeline);
	a->bus_watch = gst_bus_add_watch(bus, on_bus, a);
	gst_object_unref(bus);

	if (gst_element_set_state(a->pipeline, GST_STATE_PLAYING) ==
	    GST_STATE_CHANGE_FAILURE) {
		stop_pipeline(a);
		set_status(a, "The camera does not start");
		return FALSE;
	}

	return TRUE;
}

static char *source_desc(struct app *a, int w, int h)
{
	return g_strdup_printf("libcamerasrc name=cam camera-name=\"%s\" ! "
			       "video/x-raw,width=%d,height=%d",
			       cams[a->cam].id, w, h);
}

static enum flip preview_turn(struct app *a)
{
	return flip_then(cams[a->cam].preview_turn, a->desktop);
}

static const char *file_turn(struct app *a)
{
	return flips[flip_then(cams[a->cam].file_turn, a->desktop)].name;
}

static char *view_desc(struct app *a)
{
	/*
	 * libcamerasrc has only a few buffers: every branch keeps at most one
	 * of them, or the camera stalls.
	 */
	return g_strdup_printf("queue leaky=downstream max-size-buffers=1 ! "
			       "glupload ! glcolorconvert ! "
			       "glvideoflip video-direction=%s ! "
			       "glsinkbin name=out",
			       gl_flip_name(preview_turn(a)));
}

/* The V4L2 subdev whose name starts with prefix, opened, or -1 */
static int open_subdev(const char *prefix, int flags)
{
	const char *node;
	GDir *dir;
	int fd = -1;

	dir = g_dir_open("/sys/class/video4linux", 0, NULL);
	while (dir && fd < 0 && (node = g_dir_read_name(dir))) {
		char *path, *name = NULL;

		if (!g_str_has_prefix(node, "v4l-subdev"))
			continue;
		path = g_strdup_printf("/sys/class/video4linux/%s/name", node);
		if (g_file_get_contents(path, &name, NULL, NULL) &&
		    g_str_has_prefix(name, prefix)) {
			char *dev = g_strdup_printf("/dev/%s", node);

			fd = open(dev, flags);
			g_free(dev);
		}
		g_free(name);
		g_free(path);
	}
	if (dir)
		g_dir_close(dir);
	return fd;
}

/*
 * How sharp the frame is around the point of interest: the squared
 * differences between neighbouring 2x2 blocks (green, or luma; blocks so
 * that the noise counts less) against the squared brightness, so that the
 * exposure does not count.
 */
static double sharpness(GstVideoFrame *f, double fx, double fy, int div)
{
	const GstVideoFormatInfo *fi = f->info.finfo;
	unsigned int comp = GST_VIDEO_FORMAT_INFO_IS_YUV(fi) ? 0 : 1;
	const guint8 *data = GST_VIDEO_FRAME_COMP_DATA(f, comp);
	int stride = GST_VIDEO_FRAME_COMP_STRIDE(f, comp);
	int ps = GST_VIDEO_FRAME_COMP_PSTRIDE(f, comp);
	int w = GST_VIDEO_FRAME_COMP_WIDTH(f, comp);
	int h = GST_VIDEO_FRAME_COMP_HEIGHT(f, comp);
	int bs = MIN(w / div, h) / 2;	/* blocks per side */
	int x0 = CLAMP(fx * w - bs, 0, w - 2 * bs);
	int y0 = CLAMP(fy * h - bs, 0, h - 2 * bs);
	guint64 grad = 0, sum = 0;
	double n, mean;
	int *blk;

	if (GST_VIDEO_FORMAT_INFO_DEPTH(fi, comp) != 8 || bs < 8)
		return 0;
	blk = g_new(int, bs * bs);

	for (int by = 0; by < bs; by++) {
		const guint8 *r0 = data + (y0 + 2 * by) * stride + x0 * ps;
		const guint8 *r1 = r0 + stride;

		for (int bx = 0; bx < bs; bx++) {
			int x = 2 * bx * ps;
			int v = r0[x] + r0[x + ps] + r1[x] + r1[x + ps];

			blk[by * bs + bx] = v;
			sum += v;
		}
	}
	for (int by = 0; by < bs - 1; by++) {
		for (int bx = 0; bx < bs - 1; bx++) {
			int v = blk[by * bs + bx];
			int dx = blk[by * bs + bx + 1] - v;
			int dy = blk[(by + 1) * bs + bx] - v;

			grad += dx * dx + dy * dy;
		}
	}

	g_free(blk);

	n = (double)bs * bs;
	mean = sum / n;
	return mean > 0 ? grad / ((bs - 1) * (bs - 1.0)) / (mean * mean) : 0;
}

/*
 * The monotonic time a frame's readout began: libcamerasrc made the
 * sensor's timestamp a running time with the same clock reading
 */
static gint64 frame_start(GstPad *pad, GstBuffer *buf)
{
	GstElement *el = gst_pad_get_parent_element(pad);
	GstClock *clock = el ? gst_element_get_clock(el) : NULL;
	gint64 start = -1;

	if (clock && GST_BUFFER_PTS_IS_VALID(buf)) {
		GstClockTime running = gst_clock_get_time(clock) -
				       gst_element_get_base_time(el);

		start = g_get_monotonic_time() * 1000 -
			((gint64)running - (gint64)GST_BUFFER_PTS(buf));
	}
	if (clock)
		gst_object_unref(clock);
	if (el)
		gst_object_unref(el);
	return start;
}

static void set_marker(struct app *a, const char *state)
{
	GtkStyleContext *sc = gtk_widget_get_style_context(a->marker);

	gtk_style_context_remove_class(sc, "focused");
	gtk_style_context_remove_class(sc, "failed");
	if (state)
		gtk_style_context_add_class(sc, state);
}

struct report {
	struct app *a;
	gint gen;
	const char *state;
};

static gboolean af_report(gpointer data)
{
	struct report *r = data;

	if (r->gen == g_atomic_int_get(&r->a->measure_gen))
		set_marker(r->a, r->state);
	g_free(r);
	return G_SOURCE_REMOVE;
}

/* Called with the lock held; restore: put the lens back where it was */
static void af_close(struct af *af, gboolean restore)
{
	if (af->fd >= 0) {
		if (restore) {
			struct v4l2_control ctrl = {
				.id = V4L2_CID_FOCUS_ABSOLUTE,
				.value = af->start,
			};

			ioctl(af->fd, VIDIOC_S_CTRL, &ctrl);
		}
		close(af->fd);
	}
	af->fd = -1;
	af->phase = AF_OFF;
	gst_clear_object(&af->cam);
}

/* The end, from the streaming thread: the marker tells how it went */
static void af_finish(struct app *a, const char *state)
{
	struct report *r = g_new(struct report, 1);

	g_atomic_int_set(&a->measure, 0);
	af_close(&a->af, FALSE);
	r->a = a;
	r->gen = g_atomic_int_get(&a->measure_gen);
	r->state = state;
	g_idle_add(af_report, r);
}

/* The lens stays where it is, or goes back if it was still searching */
static void af_stop(struct app *a)
{
	g_atomic_int_set(&a->measure, 0);
	g_atomic_int_inc(&a->measure_gen);
	g_mutex_lock(&a->af.lock);
	af_close(&a->af, a->af.phase != AF_OFF);
	g_mutex_unlock(&a->af.lock);
}

static void lens_move(struct af *af, int pos, gint64 now)
{
	struct v4l2_control ctrl = {
		.id = V4L2_CID_FOCUS_ABSOLUTE,
		.value = pos,
	};

	if (ioctl(af->fd, VIDIOC_S_CTRL, &ctrl))
		g_printerr("t510-camera: lens: %s\n", g_strerror(errno));
	if (af->moves < AF_MAX_MOVES) {
		af->move_t[af->moves] = now;
		af->move_pos[af->moves++] = pos;
	}
	af->cur = pos;
}

/* A sweep of the lens from one end to the other, a step each frame */
static void af_sweep(struct af *af, enum af_phase phase, int from, int to,
		     int step, gint64 now)
{
	af->phase = phase;
	af->moves = 0;
	af->n = 0;
	af->target = to;
	af->step = from < to ? step : -step;
	lens_move(af, from, now);
	/* a jump to the start rings for longer than a step */
	af->settled = now + AF_JUMP_NS;
}

/*
 * Where the lens was, on average, while the point of interest was exposed
 * between b and e; -1 if it had not got to the sweep yet
 */
static double af_lens_at(struct af *af, gint64 b, gint64 e)
{
	double sum = 0;

	for (int k = 0; k < 8; k++) {
		gint64 t = b + (e - b) * k / 7;
		int pos = -1;

		for (unsigned int i = 0; i < af->moves; i++)
			if (af->move_t[i] + AF_LENS_LAG_NS <= t)
				pos = af->move_pos[i];
		if (pos < 0)
			return -1;
		sum += pos;
	}
	return sum / 8;
}

/* The sharpest sample in window k, and the softest score */
static unsigned int af_best(struct af *af, int k, double *lo)
{
	unsigned int best = 0;

	*lo = af->score[0][k];
	for (unsigned int i = 1; i < af->n; i++) {
		if (af->score[i][k] > af->score[best][k])
			best = i;
		*lo = MIN(*lo, af->score[i][k]);
	}
	return best;
}

/* The smallest window with a clear peak so far, or -1 */
static int af_window(struct af *af)
{
	for (unsigned int k = 0; k < AF_WINDOWS; k++) {
		double lo;
		unsigned int best = af_best(af, k, &lo);

		if (af->score[best][k] >= AF_MIN_GAIN * lo)
			return k;
	}
	return -1;
}

/* The top of a parabola through the best sample and its neighbours */
static double af_peak(struct af *af, int k, unsigned int best)
{
	double x0, x1, x2, y0, y1, y2, d, top;

	if (best == 0 || best + 1 >= af->n)
		return af->pos[best];
	x0 = af->pos[best - 1]; y0 = af->score[best - 1][k];
	x1 = af->pos[best];     y1 = af->score[best][k];
	x2 = af->pos[best + 1]; y2 = af->score[best + 1][k];
	d = (x0 - x1) * (x0 - x2) * (x1 - x2);
	if (fabs(d) < 1e-9)
		return x1;
	/* y = A x^2 + B x + C */
	double A = (x2 * (y1 - y0) + x1 * (y0 - y2) + x0 * (y2 - y1)) / d;
	double B = (x2 * x2 * (y0 - y1) + x1 * x1 * (y2 - y0) +
		    x0 * x0 * (y1 - y2)) / d;

	if (A >= 0)
		return x1;
	top = -B / (2 * A);
	return CLAMP(top, MIN(x0, x2), MAX(x0, x2));
}

/* The coarse sweep goes from far to near: well past a clear peak, stop */
static gboolean af_past_peak(struct af *af)
{
	double lo, hi;
	unsigned int best;
	int k;

	if (af->n < 4 || (k = af_window(af)) < 0)
		return FALSE;
	best = af_best(af, k, &lo);
	hi = af->score[best][k];
	return best + 2 < af->n &&
	       af->score[af->n - 1][k] < AF_PAST_PEAK * hi &&
	       af->score[af->n - 2][k] < AF_PAST_PEAK * hi;
}

/* A sweep is over: on to the fine one, or to the focus */
static void af_sweep_done(struct app *a, gint64 now)
{
	struct af *af = &a->af;
	unsigned int best;
	double lo, hi, peak;
	int k;

	/* The coarse sweep picks the window, the fine one goes by it */
	k = af->phase == AF_SWEEP ? af_window(af) : af->win;
	/* Nothing stands out even in the largest: too plain, or too dark */
	if (!af->n || k < 0) {
		if (af->debug)
			g_printerr("af: nothing sharp\n");
		af_close(af, TRUE);
		af_finish(a, "failed");
		return;
	}
	best = af_best(af, k, &lo);
	hi = af->score[best][k];
	peak = af_peak(af, k, best);
	if (af->debug)
		g_printerr("af: %s sweep, window 1/%d: best %.0f (%.5f, softest %.5f), peak %.0f\n",
			   af->phase == AF_SWEEP ? "coarse" : "fine",
			   af_windows[k], af->pos[best], hi, lo, peak);

	if (af->phase == AF_SWEEP) {
		af->win = k;
		af->peak = peak;
		/* Back over the peak, finer: the other way round */
		af_sweep(af, AF_FINE,
			 CLAMP(peak + AF_FINE_SPAN, af->min, af->max),
			 CLAMP(peak - AF_FINE_SPAN, af->min, af->max),
			 AF_FINE_STEP, now);
		return;
	}

	/* A flat fine sweep (the scene moved?): keep the coarse peak */
	if (hi < AF_FINE_GAIN * lo)
		peak = af->peak;
	lens_move(af, CLAMP(lrint(peak), af->min, af->max), now);
	if (af->debug)
		g_printerr("af: focus at %ld\n", lrint(peak));
	af_finish(a, "focused");
}

/*
 * One frame, in the streaming thread with the lock held: first wait for
 * the exposure to settle on the new point, then sweep.
 */
static void af_frame(struct app *a, const double *score, gint64 start)
{
	struct af *af = &a->af;
	gint64 now = g_get_monotonic_time() * 1000;
	gint64 exposure, b, e;
	float gain = 0;
	int us = 0;

	if (start < 0)
		start = now - 3 * af->period;	/* about how late frames come */
	else if (af->last_start > 0 && start > af->last_start)
		af->period = CLAMP(start - af->last_start, AF_PERIOD_MIN_NS,
				   AF_PERIOD_MAX_NS);
	af->last_start = start;

	g_object_get(af->cam, "exposure-time", &us, "analogue-gain", &gain,
		     NULL);
	exposure = us > 0 ? us * 1000LL : af->period;

	if (af->phase == AF_WAIT_AE) {
		double v = us * (double)gain;

		if (v > 0 && fabs(v - af->ae_last) <= AF_AE_SAME * af->ae_last)
			af->ae_same++;
		else
			af->ae_same = 0;
		af->ae_last = v;
		if (af->ae_same >= AF_AE_FRAMES || now - af->ae_since > AF_AE_WAIT_NS) {
			if (af->debug)
				g_printerr("af: exposure %d us x %.2f after %lld ms\n",
					   us, gain,
					   (long long)(now - af->ae_since) / 1000000);
			af_sweep(af, AF_SWEEP, af->min, af->max,
				 AF_SWEEP_STEP, now);
		}
		return;
	}

	/* The rows of the point of interest are read out that much later */
	e = start + a->poi_y * af->period;
	b = e - exposure;
	if (b >= af->settled && af->n < AF_MAX_SAMPLES) {
		double pos = af_lens_at(af, b, e);

		if (pos >= 0) {
			af->pos[af->n] = pos;
			memcpy(af->score[af->n++], score, sizeof(af->score[0]));
			if (af->debug)
				g_printerr("af: %s lens %.0f score %.5f %.5f %.5f\n",
					   af->phase == AF_SWEEP ? "coarse" : "fine",
					   pos, score[0], score[1], score[2]);
		}
	}

	if (af->phase == AF_SWEEP && af_past_peak(af)) {
		af_sweep_done(a, now);
		return;
	}
	/* The sweep starts with a jump: hold there until it is measured */
	if (!af->n && now < af->settled + AF_HOLD_NS)
		return;
	if (af->cur != af->target) {
		int next = af->cur + af->step;

		if ((af->step > 0 && next > af->target) ||
		    (af->step < 0 && next < af->target))
			next = af->target;
		lens_move(af, next, now);
		return;
	}
	/* At the end: done once a frame exposed after the last step came */
	if (b >= af->move_t[af->moves - 1] + AF_LENS_LAG_NS ||
	    af->n >= AF_MAX_SAMPLES || af->moves >= AF_MAX_MOVES)
		af_sweep_done(a, now);
}

/* In the streaming thread: measure the frame and focus on */
static GstPadProbeReturn look_probe(GstPad *pad, GstPadProbeInfo *info,
				    gpointer data)
{
	struct app *a = data;
	GstBuffer *buf = gst_pad_probe_info_get_buffer(info);
	GstVideoFrame frame;
	GstVideoInfo vi;
	GstCaps *caps;
	double score[AF_WINDOWS];
	gboolean ok;
	gint64 start;

	if (!g_atomic_int_get(&a->measure))
		return GST_PAD_PROBE_OK;

	caps = gst_pad_get_current_caps(pad);
	if (!caps)
		return GST_PAD_PROBE_OK;
	ok = gst_video_info_from_caps(&vi, caps);
	gst_caps_unref(caps);
	if (!ok || !gst_video_frame_map(&frame, &vi, buf, GST_MAP_READ))
		return GST_PAD_PROBE_OK;
	for (unsigned int k = 0; k < AF_WINDOWS; k++)
		score[k] = sharpness(&frame, a->poi_x, a->poi_y, af_windows[k]);
	gst_video_frame_unmap(&frame);
	start = frame_start(pad, buf);

	g_mutex_lock(&a->af.lock);
	if (a->af.phase != AF_OFF)
		af_frame(a, score, start);
	g_mutex_unlock(&a->af.lock);

	return GST_PAD_PROBE_OK;
}

static void watch_frames(struct app *a)
{
	GstElement *look = gst_bin_get_by_name(GST_BIN(a->pipeline), "look");
	GstPad *pad = gst_element_get_static_pad(look, "src");

	gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, look_probe, a, NULL);
	gst_object_unref(pad);
	gst_object_unref(look);
}

/* Focus on the point of interest, if the camera has a lens to move */
static void af_start(struct app *a)
{
	const struct cam *c = &cams[a->cam];
	struct af *af = &a->af;
	struct v4l2_queryctrl q = { .id = V4L2_CID_FOCUS_ABSOLUTE };
	struct v4l2_control cur = { .id = V4L2_CID_FOCUS_ABSOLUTE };
	int fd;

	set_marker(a, c->lens ? NULL : "focused");
	if (!c->lens)
		return;

	fd = open_subdev(c->lens, O_RDWR);
	if (fd < 0 || ioctl(fd, VIDIOC_QUERYCTRL, &q) ||
	    ioctl(fd, VIDIOC_G_CTRL, &cur)) {
		g_printerr("t510-camera: no lens to focus with\n");
		if (fd >= 0)
			close(fd);
		set_marker(a, "failed");
		return;
	}

	g_mutex_lock(&af->lock);
	af->fd = fd;
	af->cam = gst_bin_get_by_name(GST_BIN(a->pipeline), "cam");
	af->debug = g_getenv("T510_CAMERA_DEBUG") != NULL;
	af->min = q.minimum;
	af->max = q.maximum;
	af->start = af->cur = cur.value;
	af->period = AF_PERIOD_DEFAULT_NS;
	af->last_start = 0;
	af->ae_since = g_get_monotonic_time() * 1000;
	af->ae_last = 0;
	af->ae_same = 0;
	af->phase = AF_WAIT_AE;
	g_mutex_unlock(&af->lock);

	g_atomic_int_set(&a->measure, 1);
}

/*
 * A tap on the preview: from the widget back into the camera's frame
 * through the letterbox and the turn (the inverse of a turn or a flip is
 * its transpose), then meter and focus there.
 */
static gboolean on_view_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
	struct app *a = data;
	const int *m = flips[preview_turn(a)].m;
	gboolean across = m[0] == 0;	/* a quarter turn: width is height */
	double vw = across ? PREVIEW_H : PREVIEW_W;
	double vh = across ? PREVIEW_W : PREVIEW_H;
	double ww = gtk_widget_get_allocated_width(w);
	double wh = gtk_widget_get_allocated_height(w);
	double scale = MIN(ww / vw, wh / vh);
	double u = (ev->x - (ww - vw * scale) / 2) / (vw * scale) - 0.5;
	double v = (ev->y - (wh - vh * scale) / 2) / (vh * scale) - 0.5;

	if (ev->type != GDK_BUTTON_PRESS || ev->button != 1 || !a->pipeline ||
	    (a->state != PREVIEW && a->state != RECORDING) ||
	    fabs(u) > 0.5 || fabs(v) > 0.5)
		return FALSE;

	af_stop(a);
	a->poi_x = m[0] * u + m[2] * v + 0.5;
	a->poi_y = m[1] * u + m[3] * v + 0.5;

	meter_poi(a);

	gtk_widget_set_margin_start(a->marker,
		CLAMP(ev->x - MARKER_SIZE / 2, 0, ww - MARKER_SIZE));
	gtk_widget_set_margin_top(a->marker,
		CLAMP(ev->y - MARKER_SIZE / 2, 0, wh - MARKER_SIZE));
	gtk_widget_show(a->marker);

	af_start(a);
	return TRUE;
}

/* A new camera: back to metering the whole frame, the lens stays */
static void clear_poi(struct app *a)
{
	af_stop(a);
	a->poi_x = a->poi_y = -1;
	gtk_widget_hide(a->marker);
}

static void start_preview(struct app *a)
{
	char *src = source_desc(a, PREVIEW_W, PREVIEW_H);
	char *view = view_desc(a);
	char *desc = g_strdup_printf("%s ! identity name=look ! %s", src, view);

	set_status(a, NULL);
	if (run_pipeline(a, desc, TRUE)) {
		watch_frames(a);
		a->state = PREVIEW;
	}
	update_buttons(a);

	g_free(src);
	g_free(view);
	g_free(desc);
}

static gboolean send_eos(gpointer data)
{
	struct app *a = data;

	if (a->pipeline)
		gst_element_send_event(a->pipeline, gst_event_new_eos());
	return G_SOURCE_REMOVE;
}

/* Let a few frames go by for the exposure, keep one, end the stream */
static GstPadProbeReturn still_probe(GstPad *pad, GstPadProbeInfo *info,
				     gpointer data)
{
	struct app *a = data;
	unsigned int n = a->still_count++;

	if (n < a->still_skip)
		return GST_PAD_PROBE_DROP;
	if (n == a->still_skip) {
		g_idle_add(send_eos, a);
		return GST_PAD_PROBE_OK;
	}
	return GST_PAD_PROBE_DROP;
}

static gboolean still_timed_out(gpointer data)
{
	struct app *a = data;

	a->still_timeout = 0;
	stop_pipeline(a);
	start_preview(a);
	set_status(a, "Could not take the photo");
	return G_SOURCE_REMOVE;
}

/*
 * The exposure the preview runs with, read from the sensor itself: the
 * line length and the pixel rate are the same in the binned and the full
 * modes, so it carries over to the photo.
 */
static gboolean sensor_exposure(const struct cam *c, int *exp_us, double *gain)
{
	struct v4l2_ext_control ctrl[4] = {
		{ .id = V4L2_CID_EXPOSURE },
		{ .id = V4L2_CID_ANALOGUE_GAIN },
		{ .id = V4L2_CID_HBLANK },
		{ .id = V4L2_CID_PIXEL_RATE },
	};
	struct v4l2_ext_controls ctrls = {
		.which = V4L2_CTRL_WHICH_CUR_VAL,
		.count = G_N_ELEMENTS(ctrl),
		.controls = ctrl,
	};
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	gboolean found;
	int fd = open_subdev(c->sensor, O_RDONLY);

	if (fd < 0)
		return FALSE;

	found = ioctl(fd, VIDIOC_G_EXT_CTRLS, &ctrls) == 0 &&
		ioctl(fd, VIDIOC_SUBDEV_G_FMT, &fmt) == 0 &&
		ctrl[3].value64 > 0;
	close(fd);
	if (!found)
		return FALSE;

	*exp_us = (double)ctrl[0].value * (fmt.format.width + ctrl[2].value) *
		  1e6 / ctrl[3].value64;
	*gain = ctrl[1].value / 32.0;	/* both sensors: code 32 is 1x */
	return *exp_us > 0;
}

/*
 * The retouch of camera-enhance.c, switched on and off with the Enhance
 * box and remembered across starts. Only photos get it.
 */
#define SETTINGS_GROUP	"camera"

static char *settings_file(void)
{
	return g_build_filename(g_get_user_config_dir(), "t510-camera",
				"settings.ini", NULL);
}

static gboolean setting(const char *key)
{
	GKeyFile *kf = g_key_file_new();
	char *path = settings_file();
	gboolean on = g_key_file_load_from_file(kf, path, 0, NULL) &&
		      g_key_file_get_boolean(kf, SETTINGS_GROUP, key, NULL);

	g_key_file_free(kf);
	g_free(path);
	return on;
}

static void set_setting(const char *key, gboolean on)
{
	GKeyFile *kf = g_key_file_new();
	char *path = settings_file();
	char *dir = g_path_get_dirname(path);

	g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
	g_key_file_set_boolean(kf, SETTINGS_GROUP, key, on);
	g_mkdir_with_parents(dir, 0755);
	g_key_file_save_to_file(kf, path, NULL);
	g_key_file_free(kf);
	g_free(dir);
	g_free(path);
}

static gboolean enhancing(struct app *a)
{
	return gtk_widget_get_sensitive(a->enhance) &&
	       gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->enhance));
}

static void on_enhance(GtkToggleButton *b, gpointer data)
{
	set_setting("enhance", gtk_toggle_button_get_active(b));
}

/* In the streaming thread: retouch the photo before it is encoded */
static GstPadProbeReturn enhance_probe(GstPad *pad, GstPadProbeInfo *info,
				       gpointer data)
{
	GstBuffer *buf = gst_buffer_make_writable(gst_pad_probe_info_get_buffer(info));
	GstVideoFrame frame;
	GstVideoInfo vi;
	GstCaps *caps = gst_pad_get_current_caps(pad);
	gboolean ok = caps && gst_video_info_from_caps(&vi, caps);

	GST_PAD_PROBE_INFO_DATA(info) = buf;
	if (caps)
		gst_caps_unref(caps);
	if (!ok || GST_VIDEO_INFO_FORMAT(&vi) != GST_VIDEO_FORMAT_RGB ||
	    !gst_video_frame_map(&frame, &vi, buf, GST_MAP_READWRITE))
		return GST_PAD_PROBE_OK;

	if (!enhance_rgb(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0),
			 GST_VIDEO_FRAME_WIDTH(&frame),
			 GST_VIDEO_FRAME_HEIGHT(&frame),
			 GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0)))
		g_printerr("t510-camera: the photo is saved without enhancement\n");
	gst_video_frame_unmap(&frame);
	return GST_PAD_PROBE_OK;
}

static void enhance_photo(struct app *a)
{
	GstElement *enh = gst_bin_get_by_name(GST_BIN(a->pipeline), "enh");
	GstPad *pad = gst_element_get_static_pad(enh, "sink");

	gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, enhance_probe, a, NULL);
	gst_object_unref(pad);
	gst_object_unref(enh);
}

/* ONNX Runtime and the model load in the background: a second or so */
static struct app *enhance_app;
static gboolean enhance_ok;

static gboolean enhance_ready(gpointer data)
{
	struct app *a = enhance_app;

	gtk_widget_set_sensitive(a->enhance, enhance_ok);
	if (!enhance_ok)
		gtk_widget_set_tooltip_text(a->enhance,
			"Needs ONNX Runtime (libonnxruntime1.23) and "
			ENHANCE_MODEL);
	return G_SOURCE_REMOVE;
}

static gpointer enhance_load(gpointer data)
{
	enhance_ok = enhance_available();
	g_idle_add(enhance_ready, NULL);
	return NULL;
}

static void take_photo(struct app *a)
{
	const struct cam *c = &cams[a->cam];
	char *src, *desc, *base;
	GstElement *gate;
	GstPad *pad;
	double gain = 1.0;
	int exp_us = 0;

	/* Before the preview stops: it holds the exposure */
	if (sensor_exposure(c, &exp_us, &gain))
		a->still_skip = STILL_SKIP_KNOWN;
	else
		a->still_skip = STILL_SKIP_AUTO;

	stop_pipeline(a);
	g_free(a->last_file);
	a->last_file = new_file_name("IMG", "jpg");

	base = source_desc(a, c->still_w, c->still_h);
	if (a->still_skip == STILL_SKIP_KNOWN) {
		char *at = strstr(base, " ! ");

		/* libcamerasrc camera-name=... ae-enable=false ... ! caps */
		*at = '\0';
		src = g_strdup_printf("%s ae-enable=false exposure-time=%d "
				      "analogue-gain=%.3f ! %s", base, exp_us,
				      gain, at + 3);
	} else {
		src = g_strdup(base);
	}
	g_free(base);
	desc = g_strdup_printf("%s ! identity name=gate ! videoconvert ! "
			       "videoflip video-direction=%s ! %s"
			       "jpegenc quality=92 ! filesink location=\"%s\"",
			       src, file_turn(a),
			       enhancing(a) ? "video/x-raw,format=RGB ! "
					      "identity name=enh ! videoconvert ! " : "",
			       a->last_file);

	a->state = STILL;
	a->still_count = 0;
	set_status(a, "Taking a photo…");
	update_buttons(a);

	if (run_pipeline(a, desc, FALSE)) {
		gate = gst_bin_get_by_name(GST_BIN(a->pipeline), "gate");
		pad = gst_element_get_static_pad(gate, "src");
		gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, still_probe,
				  a, NULL);
		gst_object_unref(pad);
		gst_object_unref(gate);
		if (enhancing(a))
			enhance_photo(a);
		a->still_timeout = g_timeout_add_seconds(STILL_TIMEOUT_S,
							 still_timed_out, a);
	} else {
		start_preview(a);
	}

	g_free(src);
	g_free(desc);
}

static gboolean rec_tick(gpointer data)
{
	struct app *a = data;
	gint64 s = (g_get_monotonic_time() - a->rec_start) / G_USEC_PER_SEC;
	char text[32];

	snprintf(text, sizeof(text), "● %02d:%02d", (int)(s / 60), (int)(s % 60));
	set_status(a, text);
	return G_SOURCE_CONTINUE;
}

/*
 * libcamerasrc stamps frames with the sensor's time, and the first ones
 * come before the pipeline starts: negative running time, which the muxer
 * cannot place against the microphone. Stamp them on arrival instead, as
 * the audio source does. videoconvert has made the buffer our own.
 */
static GstPadProbeReturn restamp(GstPad *pad, GstPadProbeInfo *info,
				 gpointer data)
{
	GstElement *pipeline = data;
	GstClock *clock = gst_element_get_clock(pipeline);
	GstBuffer *buf;

	if (!clock)
		return GST_PAD_PROBE_OK;

	buf = gst_buffer_make_writable(gst_pad_probe_info_get_buffer(info));
	GST_BUFFER_PTS(buf) = gst_clock_get_time(clock) -
			      gst_element_get_base_time(pipeline);
	GST_BUFFER_DTS(buf) = GST_CLOCK_TIME_NONE;
	GST_BUFFER_DURATION(buf) = GST_CLOCK_TIME_NONE;
	GST_PAD_PROBE_INFO_DATA(info) = buf;
	gst_object_unref(clock);

	return GST_PAD_PROBE_OK;
}

static void start_recording(struct app *a)
{
	GstElementFactory *x264 = gst_element_factory_find("x264enc");
	gboolean h264 = x264 != NULL;
	char *src, *view, *desc;
	const char *video, *audio;

	gst_clear_object(&x264);
	stop_pipeline(a);
	g_free(a->last_file);
	a->last_file = new_file_name("VID", h264 ? "mp4" : "webm");

	/* H.264 if gstreamer1.0-plugins-ugly is there, else VP8 */
	if (h264) {
		video = "x264enc speed-preset=ultrafast tune=zerolatency "
			"bitrate=6000";
		audio = "avenc_aac bitrate=128000";
	} else {
		video = "vp8enc deadline=1 cpu-used=16 threads=6 "
			"end-usage=cbr target-bitrate=5000000";
		audio = "opusenc bitrate=96000";
	}

	src = source_desc(a, PREVIEW_W, PREVIEW_H);
	view = view_desc(a);
	desc = g_strdup_printf("%s ! identity name=look ! tee name=t "
			       "t. ! %s "
			       "t. ! queue leaky=downstream max-size-buffers=1 ! "
			       "videoconvert name=rconv ! "
			       "videoflip video-direction=%s ! "
			       "queue max-size-buffers=8 ! "
			       "%s ! queue ! %s name=mux ! "
			       "filesink location=\"%s\" "
			       "pulsesrc ! audioconvert ! audioresample ! %s ! "
			       "queue ! mux.",
			       src, view, file_turn(a), video,
			       h264 ? "mp4mux" : "webmmux", a->last_file,
			       audio);

	if (run_pipeline(a, desc, TRUE)) {
		GstElement *conv = gst_bin_get_by_name(GST_BIN(a->pipeline),
						       "rconv");
		GstPad *pad = gst_element_get_static_pad(conv, "src");

		gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, restamp,
				  a->pipeline, NULL);
		gst_object_unref(pad);
		gst_object_unref(conv);
		watch_frames(a);
		a->state = RECORDING;
		a->rec_start = g_get_monotonic_time();
		rec_tick(a);
		a->rec_timer = g_timeout_add_seconds(1, rec_tick, a);
	} else {
		start_preview(a);
	}
	update_buttons(a);

	g_free(src);
	g_free(view);
	g_free(desc);
}

static void stop_recording(struct app *a)
{
	if (a->rec_timer) {
		g_source_remove(a->rec_timer);
		a->rec_timer = 0;
	}
	a->state = STOPPING;
	set_status(a, "Saving…");
	update_buttons(a);
	/* The muxer writes its index on EOS; on_bus restarts the preview */
	send_eos(a);
}

static void on_shutter(GtkButton *button, gpointer data)
{
	struct app *a = data;

	if (a->state == RECORDING)
		stop_recording(a);
	else if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->mode_photo)))
		take_photo(a);
	else
		start_recording(a);
}

static void on_switch(GtkButton *button, gpointer data)
{
	struct app *a = data;
	char *text;

	a->cam = (a->cam + 1) % G_N_ELEMENTS(cams);
	clear_poi(a);
	stop_pipeline(a);
	start_preview(a);
	text = g_strdup_printf("%s camera", cams[a->cam].name);
	set_status(a, text);
	g_free(text);
}

static void on_gallery(GtkButton *button, gpointer data)
{
	struct app *a = data;
	char *dir = media_dir();
	char *uri = g_filename_to_uri(dir, NULL, NULL);
	GError *err = NULL;

	if (!g_app_info_launch_default_for_uri(uri, NULL, &err)) {
		set_status(a, err->message);
		g_clear_error(&err);
	}
	g_free(uri);
	g_free(dir);
}

static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
	struct app *a = data;

	if (ev->keyval == GDK_KEY_space || ev->keyval == GDK_KEY_Return) {
		if (gtk_widget_get_sensitive(a->shutter))
			on_shutter(NULL, a);
		return TRUE;
	}
	return FALSE;
}

static void on_destroy(GtkWidget *w, gpointer data)
{
	struct app *a = data;

	/* A video still recording is lost; stop it first to keep it */
	stop_pipeline(a);
}

static const char css[] =
	"#bar { background-color: #1e1e1e; padding: 12px; }"
	"#view { background-color: black; }"
	"#status { background-color: rgba(0,0,0,0.6); color: white;"
	"  padding: 6px 12px; border-radius: 6px; margin: 12px; }"
	"#shutter { min-width: 72px; min-height: 72px; border-radius: 36px;"
	"  background: white; border: 4px solid #888; }"
	"#shutter.recording { background: #d22; border-color: white; }"
	"#marker { border: 2px solid rgba(255,255,255,0.8); border-radius: 4px; }"
	"#marker.focused { border-color: #ffd400; }"
	"#marker.failed { border-color: #d22; }";

static GtkWidget *icon_button(const char *icon, const char *tip)
{
	GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_DND);

	gtk_widget_set_tooltip_text(b, tip);
	return b;
}

static void activate(GtkApplication *gapp, gpointer data)
{
	struct app *a = data;
	GtkCssProvider *prov = gtk_css_provider_new();
	GtkWidget *overlay, *box, *bar, *gallery, *mode_video, *modes, *taps;

	if (a->window) {
		gtk_window_present(GTK_WINDOW(a->window));
		return;
	}

	gtk_css_provider_load_from_data(prov, css, -1, NULL);
	gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
		GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(prov);

	a->window = gtk_application_window_new(gapp);
	gtk_window_set_title(GTK_WINDOW(a->window), "Camera");
	gtk_window_set_icon_name(GTK_WINDOW(a->window), "camera-photo");
	gtk_window_set_default_size(GTK_WINDOW(a->window), 720, 1000);
	gtk_window_maximize(GTK_WINDOW(a->window));

	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	overlay = gtk_overlay_new();
	a->view_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_name(a->view_box, "view");
	gtk_widget_set_vexpand(a->view_box, TRUE);
	/* The video widget has no window of its own: taps come here */
	taps = gtk_event_box_new();
	gtk_event_box_set_visible_window(GTK_EVENT_BOX(taps), FALSE);
	gtk_widget_add_events(taps, GDK_BUTTON_PRESS_MASK);
	g_signal_connect(taps, "button-press-event",
			 G_CALLBACK(on_view_press), a);
	gtk_container_add(GTK_CONTAINER(taps), a->view_box);
	gtk_container_add(GTK_CONTAINER(overlay), taps);

	a->marker = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_name(a->marker, "marker");
	gtk_widget_set_size_request(a->marker, MARKER_SIZE, MARKER_SIZE);
	gtk_widget_set_halign(a->marker, GTK_ALIGN_START);
	gtk_widget_set_valign(a->marker, GTK_ALIGN_START);
	gtk_widget_set_no_show_all(a->marker, TRUE);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), a->marker);
	gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), a->marker,
					     TRUE);

	a->status = gtk_label_new(NULL);
	gtk_widget_set_name(a->status, "status");
	gtk_widget_set_halign(a->status, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(a->status, GTK_ALIGN_START);
	gtk_widget_set_no_show_all(a->status, TRUE);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), a->status);
	gtk_box_pack_start(GTK_BOX(box), overlay, TRUE, TRUE, 0);

	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
	gtk_widget_set_name(bar, "bar");

	gallery = icon_button("folder-pictures", "Photos and videos");
	g_signal_connect(gallery, "clicked", G_CALLBACK(on_gallery), a);

	modes = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	a->mode_photo = gtk_radio_button_new_with_label(NULL, "Photo");
	mode_video = gtk_radio_button_new_with_label_from_widget(
		GTK_RADIO_BUTTON(a->mode_photo), "Video");
	gtk_box_pack_start(GTK_BOX(modes), a->mode_photo, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(modes), mode_video, FALSE, FALSE, 0);

	a->enhance = gtk_check_button_new_with_label("Enhance");
	gtk_widget_set_tooltip_text(a->enhance,
		"Retouch photos: colour and tone learned from photographers' "
		"edits (LUTwithBGrid). Videos are not touched.");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(a->enhance),
				     setting("enhance"));
	gtk_widget_set_sensitive(a->enhance, FALSE);
	g_signal_connect(a->enhance, "toggled", G_CALLBACK(on_enhance), a);
	gtk_box_pack_start(GTK_BOX(modes), a->enhance, FALSE, FALSE, 0);
	gtk_widget_set_valign(modes, GTK_ALIGN_CENTER);

	a->shutter = gtk_button_new();
	gtk_widget_set_name(a->shutter, "shutter");
	gtk_widget_set_tooltip_text(a->shutter, "Take");
	g_signal_connect(a->shutter, "clicked", G_CALLBACK(on_shutter), a);

	a->switch_cam = icon_button("camera-switch", "Switch camera");
	g_signal_connect(a->switch_cam, "clicked", G_CALLBACK(on_switch), a);

	gtk_box_pack_start(GTK_BOX(bar), gallery, TRUE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), modes, TRUE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), a->shutter, TRUE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(bar), a->switch_cam, TRUE, FALSE, 0);
	gtk_widget_set_valign(a->shutter, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(gallery, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(a->switch_cam, GTK_ALIGN_CENTER);
	gtk_box_pack_end(GTK_BOX(box), bar, FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(a->window), box);
	g_signal_connect(a->window, "key-press-event", G_CALLBACK(on_key), a);
	g_signal_connect(a->window, "destroy", G_CALLBACK(on_destroy), a);
	gtk_widget_show_all(a->window);

	/* Rotating the desktop restarts the session, and this program */
	a->desktop = desktop_turn();
	start_preview(a);
	enhance_app = a;
	g_thread_unref(g_thread_new("enhance-load", enhance_load, NULL));
}

int main(int argc, char **argv)
{
	struct app a = { .poi_x = -1, .poi_y = -1, .af.fd = -1 };
	GtkApplication *gapp;
	int ret;

	gst_init(&argc, &argv);
	g_mutex_init(&a.af.lock);
	gapp = gtk_application_new("org.t510.Camera", G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(gapp, "activate", G_CALLBACK(activate), &a);
	ret = g_application_run(G_APPLICATION(gapp), argc, argv);

	stop_pipeline(&a);
	g_object_unref(gapp);
	g_free(a.last_file);
	return ret;
}
