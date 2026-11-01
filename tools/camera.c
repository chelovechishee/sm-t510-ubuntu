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
 * Build: cc -O2 -o t510-camera camera.c $(pkg-config --cflags --libs gtk+-3.0 gstreamer-1.0)
 * (scripts/make-debs.sh builds it into t510-support as /usr/bin/t510-camera)
 */

#include <fcntl.h>
#include <gst/gst.h>
#include <gtk/gtk.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define PREVIEW_W	1280
#define PREVIEW_H	960
/*
 * Frames to let go by before a photo: with the exposure of the preview
 * only the white balance has to settle, without it the auto exposure too.
 */
#define STILL_SKIP_KNOWN	5
#define STILL_SKIP_AUTO		12
#define STILL_TIMEOUT_S	20

struct cam {
	const char *id;		/* libcamera's name of the camera */
	const char *sensor;	/* its V4L2 subdev name starts so */
	const char *name;
	int still_w, still_h;	/* the largest the software ISP gives */
	/*
	 * Both sensors are mounted turned: the pictures need a quarter turn
	 * counterclockwise (DT rotation = 90). The front preview is mirrored,
	 * as people expect from a selfie camera; its files are not. In
	 * glvideoflip the diagonals are the other way round than in
	 * videoflip (GL's y axis points up): ul-lr is the turn plus mirror.
	 */
	const char *preview_turn;
	const char *file_turn;
};

static const struct cam cams[] = {
	{ "/base/soc@0/i2c@138b0000/camera@2d", "s5k4ha", "rear", 3248, 2436,
	  "90l", "90l" },
	{ "/base/soc@0/i2c@138a0000/camera@2d", "s5k5e9", "front", 2576, 1932,
	  "ul-lr", "90l" },
};

enum state { IDLE, PREVIEW, STILL, RECORDING, STOPPING };

struct app {
	GtkWidget *window;
	GtkWidget *view_box;	/* holds the video widget of the pipeline */
	GtkWidget *status;
	GtkWidget *shutter;
	GtkWidget *mode_photo;
	GtkWidget *switch_cam;
	GstElement *pipeline;
	guint bus_watch;
	enum state state;
	unsigned int cam;
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

static char *source_desc(const struct cam *c, int w, int h)
{
	return g_strdup_printf("libcamerasrc camera-name=\"%s\" ! "
			       "video/x-raw,width=%d,height=%d", c->id, w, h);
}

static char *view_desc(const struct cam *c)
{
	/*
	 * libcamerasrc has only a few buffers: every branch keeps at most one
	 * of them, or the camera stalls.
	 */
	return g_strdup_printf("queue leaky=downstream max-size-buffers=1 ! "
			       "glupload ! glcolorconvert ! "
			       "glvideoflip video-direction=%s ! "
			       "glsinkbin name=out", c->preview_turn);
}

static void start_preview(struct app *a)
{
	const struct cam *c = &cams[a->cam];
	char *src = source_desc(c, PREVIEW_W, PREVIEW_H);
	char *view = view_desc(c);
	char *desc = g_strdup_printf("%s ! %s", src, view);

	set_status(a, NULL);
	if (run_pipeline(a, desc, TRUE))
		a->state = PREVIEW;
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
	gboolean found = FALSE;
	const char *node;
	GDir *dir;
	int fd = -1;

	dir = g_dir_open("/sys/class/video4linux", 0, NULL);
	while (dir && !found && (node = g_dir_read_name(dir))) {
		char *path, *name = NULL;

		if (!g_str_has_prefix(node, "v4l-subdev"))
			continue;
		path = g_strdup_printf("/sys/class/video4linux/%s/name", node);
		if (g_file_get_contents(path, &name, NULL, NULL) &&
		    g_str_has_prefix(name, c->sensor)) {
			char *dev = g_strdup_printf("/dev/%s", node);

			fd = open(dev, O_RDONLY);
			found = fd >= 0;
			g_free(dev);
		}
		g_free(name);
		g_free(path);
	}
	if (dir)
		g_dir_close(dir);
	if (!found)
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

	base = source_desc(c, c->still_w, c->still_h);
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
			       "videoflip video-direction=%s ! "
			       "jpegenc quality=92 ! filesink location=\"%s\"",
			       src, c->file_turn, a->last_file);

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
	const struct cam *c = &cams[a->cam];
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

	src = source_desc(c, PREVIEW_W, PREVIEW_H);
	view = view_desc(c);
	desc = g_strdup_printf("%s ! tee name=t "
			       "t. ! %s "
			       "t. ! queue leaky=downstream max-size-buffers=1 ! "
			       "videoconvert name=rconv ! "
			       "videoflip video-direction=%s ! "
			       "queue max-size-buffers=8 ! "
			       "%s ! queue ! %s name=mux ! "
			       "filesink location=\"%s\" "
			       "pulsesrc ! audioconvert ! audioresample ! %s ! "
			       "queue ! mux.",
			       src, view, c->file_turn, video,
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
	"#shutter.recording { background: #d22; border-color: white; }";

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
	GtkWidget *overlay, *box, *bar, *gallery, *mode_video, *modes;

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
	gtk_container_add(GTK_CONTAINER(overlay), a->view_box);

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

	start_preview(a);
}

int main(int argc, char **argv)
{
	struct app a = { 0 };
	GtkApplication *gapp;
	int ret;

	gst_init(&argc, &argv);
	gapp = gtk_application_new("org.t510.Camera", G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(gapp, "activate", G_CALLBACK(activate), &a);
	ret = g_application_run(G_APPLICATION(gapp), argc, argv);

	stop_pipeline(&a);
	g_object_unref(gapp);
	g_free(a.last_file);
	return ret;
}
