/*
 * nograb: LD_PRELOAD shim that turns gdk_seat_grab() into a no-op.
 *
 * gcr-prompter (the GNOME keyring password dialog) grabs every input device
 * while it is shown. On a tablet that also swallows the touches meant for the
 * Onboard on-screen keyboard, so the password can never be typed and the
 * screen looks frozen. Without the grab the dialog still gets keyboard focus,
 * and Onboard's synthesized key presses reach it.
 *
 * Build: scripts/make-debs.sh (into t510-support as /usr/libexec/t510/nograb.so)
 */

typedef struct _GdkSeat GdkSeat;
typedef struct _GdkWindow GdkWindow;
typedef struct _GdkCursor GdkCursor;
typedef union _GdkEvent GdkEvent;

int
gdk_seat_grab(GdkSeat *seat, GdkWindow *window, int capabilities, int owner_events,
              GdkCursor *cursor, const GdkEvent *event, void *prepare_func, void *prepare_data)
{
	(void)seat; (void)window; (void)capabilities; (void)owner_events;
	(void)cursor; (void)event; (void)prepare_func; (void)prepare_data;
	return 0; /* GDK_GRAB_SUCCESS */
}

void
gdk_seat_ungrab(GdkSeat *seat)
{
	(void)seat;
}
