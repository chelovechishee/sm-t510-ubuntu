/*
 * longpress: right click on touchscreen long-press for X11 (libinput touch).
 *
 * Watches XI2 raw touch events on the root window. If a single touch stays
 * within THRESHOLD pixels for HOLD_MS, a secondary click (button 3) is sent
 * through XTest right then, while the finger is still down: sent on lift it
 * came after the tap's own click, and a single-click desktop had already
 * opened the icon.
 *
 * The menu that click opens is then closed by the finger's own lift: the
 * touch's emulated button 1 release reaches the menu's grab well after GTK's
 * 500 ms "first release" grace (our own button 3 release used it up). So if
 * a popup menu mapped after the click is unmapped right after a lift that did
 * not move, the click is sent again and the menu reopens where it was.
 * A finger that slides onto a menu item and lifts there still picks it.
 *
 * The pointer is hidden for good (XFixes, for as long as we run): Chrome
 * hides it on a touch and shows it on the emulated motion that follows, so
 * it blinked. A dot marks the finger instead, from the touch until a moment
 * after the lift; it has no input region, so touches go through it.
 *
 * Build: cc -O2 -o longpress longpress.c -lX11 -lXi -lXtst -lXfixes -lXext -lm
 * Usage: longpress [hold_ms] [threshold_px]
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <time.h>

#define REOPEN_MS	400	/* menu closed this soon after the lift: reopen */
#define REOPEN_DELAY_MS	30	/* let the menu's client drop its grab first */
#define DOT_D		36	/* dot diameter, px (about 4 mm) */
#define DOT_RIM_W	3	/* solid rim, px */
#define DOT_RIM		0x46607a	/* the rim: dark grey-blue, seen on light and dark */
#define DOT_CENTER	0x5a7896	/* dark in the middle... */
#define DOT_INNER	0xc8d8e8	/* ...light just inside the rim: a hollow */
#define DOT_LINGER_MS	300	/* the dot stays this long after the lift */

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void pointer_pos(Display *dpy, Window root, int *x, int *y)
{
	Window r, c;
	int wx, wy;
	unsigned int mask;

	XQueryPointer(dpy, root, &r, &c, x, y, &wx, &wy, &mask);
}

static void right_click(Display *dpy)
{
	XTestFakeButtonEvent(dpy, 3, True, CurrentTime);
	XTestFakeButtonEvent(dpy, 3, False, CurrentTime);
	XFlush(dpy);
}

/* the window may be gone already: ignore BadWindow and friends */
static int ignore_errors(Display *dpy, XErrorEvent *e)
{
	(void)dpy;
	(void)e;
	return 0;
}

/* one channel of the dot's colour, d of the way from the centre to the rim */
static unsigned long mix(unsigned long in, unsigned long out, double d, int shift)
{
	double a = in >> shift & 0xff, b = out >> shift & 0xff;

	return (unsigned long)(a + (b - a) * d + 0.5) << shift;
}

/* the finger's mark: a round window without an input region */
static Window dot_create(Display *dpy, Window root)
{
	int scr = DefaultScreen(dpy), x, y;
	XSetWindowAttributes a = { 0 };
	XImage *img;
	Window w;
	Pixmap pix, mask;
	GC gc;

	a.override_redirect = True;
	w = XCreateWindow(dpy, root, 0, 0, DOT_D, DOT_D, 0, CopyFromParent,
			  InputOutput, CopyFromParent, CWOverrideRedirect, &a);

	/*
	 * Solid rim, inside it a radial gradient from dark in the middle to
	 * light at the rim, so the dot looks pressed in. The X screen is 24-bit
	 * TrueColor: pixel = 0xRRGGBB.
	 */
	img = XCreateImage(dpy, DefaultVisual(dpy, scr), DefaultDepth(dpy, scr),
			   ZPixmap, 0, malloc(DOT_D * DOT_D * 4), DOT_D, DOT_D, 32, 0);
	for (y = 0; y < DOT_D; y++)
		for (x = 0; x < DOT_D; x++) {
			double dx = x + 0.5 - DOT_D / 2.0, dy = y + 0.5 - DOT_D / 2.0;
			double d = sqrt(dx * dx + dy * dy) / (DOT_D / 2.0 - DOT_RIM_W);

			XPutPixel(img, x, y, d >= 1 ? DOT_RIM :
					     mix(DOT_CENTER, DOT_INNER, d, 16) |
					     mix(DOT_CENTER, DOT_INNER, d, 8) |
					     mix(DOT_CENTER, DOT_INNER, d, 0));
		}
	pix = XCreatePixmap(dpy, w, DOT_D, DOT_D, DefaultDepth(dpy, scr));
	gc = XCreateGC(dpy, pix, 0, NULL);
	XPutImage(dpy, pix, gc, img, 0, 0, 0, 0, DOT_D, DOT_D);
	XSetWindowBackgroundPixmap(dpy, w, pix);
	XFreeGC(dpy, gc);
	XFreePixmap(dpy, pix);
	XDestroyImage(img);

	/* only the disc is drawn, the corners of the square show what is below */
	mask = XCreatePixmap(dpy, w, DOT_D, DOT_D, 1);
	gc = XCreateGC(dpy, mask, 0, NULL);
	XSetForeground(dpy, gc, 0);
	XFillRectangle(dpy, mask, gc, 0, 0, DOT_D, DOT_D);
	XSetForeground(dpy, gc, 1);
	XFillArc(dpy, mask, gc, 0, 0, DOT_D, DOT_D, 0, 360 * 64);
	XShapeCombineMask(dpy, w, ShapeBounding, 0, 0, mask, ShapeSet);
	XFreeGC(dpy, gc);
	XFreePixmap(dpy, mask);

	/* no input region: the touch and its emulated pointer go to the window below */
	XShapeCombineRectangles(dpy, w, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
	return w;
}

static void dot_show(Display *dpy, Window dot, int x, int y)
{
	XMoveWindow(dpy, dot, x - DOT_D / 2, y - DOT_D / 2);
	XMapRaised(dpy, dot);
	XFlush(dpy);
}

/* override-redirect window typed as a menu (GTK, Qt and Chrome set it) */
static int is_menu(Display *dpy, Window w)
{
	static Atom type, menus[3];
	Atom actual, *types;
	int format, i, j, found = 0;
	unsigned long n, after;
	unsigned char *data = NULL;

	if (!type) {
		type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
		menus[0] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
		menus[1] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
		menus[2] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU", False);
	}
	if (XGetWindowProperty(dpy, w, type, 0, 16, False, XA_ATOM, &actual,
			       &format, &n, &after, &data) != Success || !data)
		return 0;
	types = (Atom *)data;
	for (i = 0; format == 32 && i < (int)n; i++)
		for (j = 0; j < 3; j++)
			if (types[i] == menus[j])
				found = 1;
	XFree(data);
	return found;
}

int main(int argc, char **argv)
{
	long hold_ms = argc > 1 ? atol(argv[1]) : 700;
	int threshold = argc > 2 ? atoi(argv[2]) : 30;
	Display *dpy = XOpenDisplay(NULL);
	int xi_opcode, ev, err, major = 2, minor = 2, nil;
	unsigned char bits[XIMaskLen(XI_LASTEVENT)] = { 0 };
	XIEventMask mask = { XIAllMasterDevices, sizeof(bits), bits };
	int fixes_major = 4, fixes_minor = 0, shape_major, shape_minor;
	Window root, dot;
	int active = 0, touches = 0, moved = 0, sx = 0, sy = 0;
	int clicked = 0;	/* hold click sent during this touch */
	Window menu = 0;	/* menu mapped since that click */
	long start = 0, lift = -1, reopen = -1;
	long dot_off = -1;	/* time to unmap the dot */

	if (!dpy) {
		fprintf(stderr, "longpress: cannot open display\n");
		return 1;
	}
	if (!XQueryExtension(dpy, "XInputExtension", &xi_opcode, &ev, &err) ||
	    XIQueryVersion(dpy, &major, &minor) != Success ||
	    !XTestQueryExtension(dpy, &nil, &nil, &nil, &nil) ||
	    !XFixesQueryExtension(dpy, &nil, &nil) ||
	    !XFixesQueryVersion(dpy, &fixes_major, &fixes_minor) || fixes_major < 4 ||
	    !XShapeQueryExtension(dpy, &nil, &nil) ||
	    !XShapeQueryVersion(dpy, &shape_major, &shape_minor) ||
	    (shape_major == 1 && shape_minor < 1)) {
		fprintf(stderr, "longpress: need XInput 2.2, XTest, XFixes 4 and Shape 1.1\n");
		return 1;
	}

	root = DefaultRootWindow(dpy);
	XFixesHideCursor(dpy, root);
	dot = dot_create(dpy, root);
	XISetMask(bits, XI_RawTouchBegin);
	XISetMask(bits, XI_RawTouchUpdate);
	XISetMask(bits, XI_RawTouchEnd);
	XISelectEvents(dpy, root, &mask, 1);
	/* Map/UnmapNotify of override-redirect popups */
	XSelectInput(dpy, root, SubstructureNotifyMask);
	XSetErrorHandler(ignore_errors);
	XFlush(dpy);

	for (;;) {
		XEvent e;

		while (XPending(dpy)) {
			XNextEvent(dpy, &e);
			if (e.type == MapNotify) {
				if (clicked && e.xmap.override_redirect &&
				    is_menu(dpy, e.xmap.window))
					menu = e.xmap.window;
				continue;
			}
			if (e.type == UnmapNotify) {
				if (lift >= 0 && menu && e.xunmap.window == menu &&
				    now_ms() - lift <= REOPEN_MS)
					reopen = now_ms() + REOPEN_DELAY_MS;
				if (e.xunmap.window == menu) {
					menu = 0;
					lift = -1;
				}
				continue;
			}
			if (e.type != GenericEvent || e.xcookie.extension != xi_opcode ||
			    !XGetEventData(dpy, &e.xcookie))
				continue;

			switch (e.xcookie.evtype) {
			case XI_RawTouchBegin:
				if (touches++ == 0) {
					active = 1;
					moved = 0;
					clicked = 0;
					menu = 0;
					lift = -1;
					reopen = -1;
					start = now_ms();
					/* pointer emulation has already moved the cursor here */
					pointer_pos(dpy, root, &sx, &sy);
					dot_show(dpy, dot, sx, sy);
					dot_off = -1;
				} else {
					active = 0; /* multi-finger: not a long-press */
					moved = 1;
				}
				break;
			case XI_RawTouchUpdate: {
				int x, y;

				pointer_pos(dpy, root, &x, &y);
				/* the dot follows the finger */
				XMoveWindow(dpy, dot, x - DOT_D / 2, y - DOT_D / 2);
				/* after the click too: a slide onto a menu item */
				if ((active || clicked) && !moved &&
				    (abs(x - sx) > threshold || abs(y - sy) > threshold))
					moved = 1;
				break;
			}
			case XI_RawTouchEnd:
				if (touches > 0)
					touches--;
				if (touches == 0) {
					active = 0;
					if (clicked && !moved && menu)
						lift = now_ms();
					clicked = 0;
					dot_off = now_ms() + DOT_LINGER_MS;
				}
				break;
			}
			XFreeEventData(dpy, &e.xcookie);
		}

		/* held long enough without moving: right click now */
		if (active && !moved && now_ms() - start >= hold_ms) {
			active = 0;
			clicked = 1;
			right_click(dpy);
		}

		/* the lift closed the menu: open it again */
		if (reopen >= 0 && now_ms() >= reopen) {
			reopen = -1;
			lift = -1;
			if (touches == 0)
				right_click(dpy);
		}

		/* the lift was a moment ago: take the dot away */
		if (dot_off >= 0 && now_ms() >= dot_off) {
			dot_off = -1;
			XUnmapWindow(dpy, dot);
			XFlush(dpy);
		}

		/* wait for the next X event, the hold time, the reopen or the dot */
		{
			fd_set fds;
			int fd = ConnectionNumber(dpy);
			struct timeval tv, *tvp = NULL;
			long next = -1;

			if (active && !moved)
				next = start + hold_ms;
			if (reopen >= 0 && (next < 0 || reopen < next))
				next = reopen;
			if (dot_off >= 0 && (next < 0 || dot_off < next))
				next = dot_off;
			if (next >= 0) {
				long left = next - now_ms();

				if (left < 0)
					left = 0;
				tv.tv_sec = left / 1000;
				tv.tv_usec = (left % 1000) * 1000;
				tvp = &tv;
			}
			FD_ZERO(&fds);
			FD_SET(fd, &fds);
			select(fd + 1, &fds, NULL, NULL, tvp);
		}
	}
}
