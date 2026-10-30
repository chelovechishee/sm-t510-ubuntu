/*
 * noblank: LD_PRELOAD shim that keeps lightdm-gtk-greeter from blanking the
 * screen when it starts as the lock screen.
 *
 * Started by light-locker (the lock hint), the greeter forces the X screen
 * saver on before it shows its window, to behave like a screensaver: the
 * screen stays black until a touch or a key reaches its X server. On the
 * tablet the power key turns the screen off and on itself (t510-powerkey),
 * and the press that wakes the tablet from suspend can come before the
 * greeter has started, so the lock screen came up black. Only the forced
 * activation is dropped: the greeter's idle timeout still blanks it.
 *
 * Build: scripts/make-debs.sh (into t510-support as /usr/libexec/t510/noblank.so)
 */
#define _GNU_SOURCE
#include <dlfcn.h>

typedef struct _XDisplay Display;

#define ScreenSaverActive 1

int
XForceScreenSaver(Display *display, int mode)
{
	static int (*next)(Display *, int);

	if (mode == ScreenSaverActive)
		return 1;
	if (!next)
		next = (int (*)(Display *, int))dlsym(RTLD_NEXT, "XForceScreenSaver");
	return next ? next(display, mode) : 1;
}
