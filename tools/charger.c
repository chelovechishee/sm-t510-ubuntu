/*
 * t510-charger: the charging screen of the initramfs.
 *
 * S-Boot starts the kernel in its charging mode when a charger turns the
 * tablet on (or after "reboot charge", a shutdown on a charger); the boot
 * wrapper turns that into t510.boot=charge on the command line, and the
 * initramfs runs this instead of starting the system. It shows the battery
 * with its charge in per cent, turns the screen off after a while and on
 * again with a press of the power key.
 *
 *   power key held for HOLD_MS   returns 0: the initramfs boots the system
 *   charger unplugged            powers the tablet off
 *
 * Static, for the initramfs (busybox has no drawing): mainline/make-boot.sh
 * builds it with aarch64-linux-gnu-gcc -static -Os -s.
 */
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>

#ifndef BATTERY
#define BATTERY "/sys/class/power_supply/s2mu005-fuel-gauge/"
#define CHARGER "/sys/class/power_supply/s2mu005-charger/online"
#endif

#define SCREEN_MS	15000	/* the screen stays on this long */
#define REFRESH_MS	5000	/* new charge on the screen */
#define CHECK_MS	2000	/* the charger, with the screen off */
#define HOLD_MS		1500	/* power key held: boot */
#define UNPLUGGED_MS	3000	/* no charger this long: power off */
#define BRIGHTNESS	"80"

static unsigned int *fb, *buf;
static unsigned int width, height, stride;
static struct fb_var_screeninfo var;
static int keys = -1;
static char bl_power[300], bl_brightness[300];

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static int read_file(const char *path, char *s, int len)
{
	int fd = open(path, O_RDONLY), n;

	if (fd < 0)
		return -1;
	n = read(fd, s, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == ' '))
		n--;
	s[n] = 0;
	return n;
}

static void write_file(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);

	if (fd >= 0) {
		if (write(fd, s, strlen(s)) < 0)
			;	/* nothing to do about it */
		close(fd);
	}
}

/* --- drawing ------------------------------------------------------------- */

static unsigned int rgb(unsigned int c)
{
	return ((c >> 16 & 0xff) << var.red.offset) |
	       ((c >> 8 & 0xff) << var.green.offset) |
	       ((c & 0xff) << var.blue.offset) |
	       (var.transp.length ? 0xffu << var.transp.offset : 0);
}

/* a rectangle with corners of radius r */
static void rect(int x, int y, int w, int h, int r, unsigned int c)
{
	unsigned int px = rgb(c);

	for (int j = y < 0 ? 0 : y; j < y + h && j < (int)height; j++)
		for (int i = x < 0 ? 0 : x; i < x + w && i < (int)width; i++) {
			int dx = i < x + r ? x + r - i : i >= x + w - r ? i - (x + w - r - 1) : 0;
			int dy = j < y + r ? y + r - j : j >= y + h - r ? j - (y + h - r - 1) : 0;

			if (dx && dy && dx * dx + dy * dy > r * r)
				continue;
			buf[j * stride + i] = px;
		}
}

/* a polygon of n points in a box (x, y, w, h), points in thousandths */
static void polygon(int x, int y, int w, int h, const int (*p)[2], int n, unsigned int c)
{
	unsigned int px = rgb(c);

	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++) {
			/* even-odd rule at the pixel's centre */
			double fx = (i + 0.5) * 1000 / w, fy = (j + 0.5) * 1000 / h;
			int in = 0;

			for (int a = 0, b = n - 1; a < n; b = a++)
				if ((p[a][1] > fy) != (p[b][1] > fy) &&
				    fx < (double)(p[b][0] - p[a][0]) * (fy - p[a][1]) /
					 (p[b][1] - p[a][1]) + p[a][0])
					in = !in;
			if (in && x + i >= 0 && x + i < (int)width && y + j >= 0 && y + j < (int)height)
				buf[(y + j) * stride + x + i] = px;
		}
}

/* 5x7 glyphs, one byte per row, the top bit of the five is the left pixel */
static const struct {
	char ch;
	unsigned char rows[7];
} font[] = {
	{ '0', { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e } },
	{ '1', { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e } },
	{ '2', { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f } },
	{ '3', { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e } },
	{ '4', { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 } },
	{ '5', { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e } },
	{ '6', { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e } },
	{ '7', { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
	{ '8', { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e } },
	{ '9', { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c } },
	{ '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } },
	{ '-', { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 } },
};

/* text centred at y, s pixels per font pixel */
static void text(const char *s, int y, int scale, unsigned int c)
{
	int n = strlen(s), w = n * 6 * scale - scale, x = ((int)width - w) / 2;

	for (; *s; s++, x += 6 * scale)
		for (unsigned int g = 0; g < sizeof(font) / sizeof(font[0]); g++) {
			if (font[g].ch != *s)
				continue;
			for (int row = 0; row < 7; row++)
				for (int col = 0; col < 5; col++)
					if (font[g].rows[row] & 0x10 >> col)
						rect(x + col * scale, y + row * scale, scale, scale, 0, c);
		}
}

static const int bolt[][2] = {
	{ 560, 0 }, { 160, 580 }, { 460, 580 }, { 400, 1000 },
	{ 840, 400 }, { 540, 400 }, { 700, 0 },
};

static void draw(void)
{
	char s[32], status[32] = "";
	int cap = -1, charging, full;
	int bw = width * 36 / 100, bh = bw * 18 / 10;
	int bx = (width - bw) / 2, by = height * 20 / 100;
	int t = bw / 20, gap = t, r = bw / 9;

	if (read_file(BATTERY "capacity", s, sizeof(s)) > 0)
		cap = atoi(s);
	if (cap > 100)
		cap = 100;
	read_file(BATTERY "status", status, sizeof(status));
	full = !strcmp(status, "Full") || cap == 100;
	charging = !full && read_file(CHARGER, s, sizeof(s)) > 0 && !strcmp(s, "1");

	memset(buf, 0, stride * height * 4);
	/* the cap, then the case as a ring */
	rect(bx + bw * 32 / 100, by - bh * 5 / 100, bw * 36 / 100, bh * 5 / 100 + r, r / 2, 0xbdbdbd);
	rect(bx, by, bw, bh, r, 0xbdbdbd);
	rect(bx + t, by + t, bw - 2 * t, bh - 2 * t, r - t, 0x000000);
	if (cap > 0) {
		int iw = bw - 2 * (t + gap), ih = bh - 2 * (t + gap);
		int fh = ih * cap / 100;
		unsigned int c = cap < 15 ? 0xe53935 : cap < 30 ? 0xfb8c00 : 0x43a047;

		if (fh < r)
			fh = r;
		rect(bx + t + gap, by + t + gap + ih - fh, iw, fh, r - t - gap, c);
	}
	if (charging)
		polygon(bx + bw * 25 / 100, by + bh * 29 / 100, bw * 50 / 100, bh * 42 / 100,
			bolt, sizeof(bolt) / sizeof(bolt[0]), 0xffffff);

	if (cap >= 0)
		snprintf(s, sizeof(s), "%d%%", cap);
	else
		strcpy(s, "--%");
	text(s, by + bh + height * 6 / 100, width / 60, 0xffffff);

	memcpy(fb, buf, stride * height * 4);
}

/* --- devices ------------------------------------------------------------- */

static int open_fb(void)
{
	struct fb_fix_screeninfo fix;
	int fd = -1;

	for (int i = 0; i < 50 && fd < 0; i++) {
		fd = open("/dev/fb0", O_RDWR);
		if (fd < 0)
			usleep(100000);
	}
	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix) ||
	    var.bits_per_pixel != 32)
		return -1;
	width = var.xres;
	height = var.yres;
	stride = fix.line_length / 4;
	fb = mmap(NULL, fix.line_length * var.yres, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	buf = malloc(fix.line_length * var.yres);
	return fb == MAP_FAILED || !buf ? -1 : 0;
}

static void find_keys(void)
{
	char path[64], name[64];

	for (int tries = 0; tries < 50 && keys < 0; tries++) {
		for (int i = 0; i < 16 && keys < 0; i++) {
			int fd;

			snprintf(path, sizeof(path), "/dev/input/event%d", i);
			fd = open(path, O_RDONLY | O_NONBLOCK);
			if (fd < 0)
				continue;
			if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) > 0 && !strcmp(name, "gpio-keys"))
				keys = fd;
			else
				close(fd);
		}
		if (keys < 0)
			usleep(100000);
	}
}

static void find_backlight(void)
{
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *e;

	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(bl_power, sizeof(bl_power), "/sys/class/backlight/%s/bl_power", e->d_name);
		snprintf(bl_brightness, sizeof(bl_brightness), "/sys/class/backlight/%s/brightness",
			 e->d_name);
		break;
	}
	if (d)
		closedir(d);
}

static void screen(int on)
{
	if (bl_power[0])
		write_file(bl_power, on ? "0" : "4");
}

static int charger_online(void)
{
	char s[8];

	/* no charger driver: never power off for it */
	return read_file(CHARGER, s, sizeof(s)) <= 0 || strcmp(s, "0");
}

int main(void)
{
	int tty = open("/dev/tty0", O_RDWR);
	long long on_until, refresh, held = 0, unplugged = 0;
	int on = 1;

	if (open_fb())
		return 2;
	find_keys();
	find_backlight();
	/* the console stops drawing over the picture */
	if (tty >= 0)
		ioctl(tty, KDSETMODE, KD_GRAPHICS);
	if (bl_brightness[0])
		write_file(bl_brightness, BRIGHTNESS);
	draw();
	screen(1);
	on_until = now_ms() + SCREEN_MS;
	refresh = now_ms() + REFRESH_MS;

	for (;;) {
		long long t = now_ms(), wake = on ? (on_until < refresh ? on_until : refresh) : t + CHECK_MS;
		struct pollfd p = { .fd = keys, .events = POLLIN };
		struct input_event ev;

		if (wake > t + CHECK_MS)
			wake = t + CHECK_MS;
		if (held && wake > held + HOLD_MS)
			wake = held + HOLD_MS;
		poll(&p, keys >= 0, wake > t ? wake - t : 0);

		while (keys >= 0 && read(keys, &ev, sizeof(ev)) == sizeof(ev)) {
			if (ev.type != EV_KEY || ev.code != KEY_POWER)
				continue;
			if (ev.value == 1) {
				held = now_ms();
				if (!on) {
					draw();
					screen(1);
					on = 1;
				}
				on_until = now_ms() + SCREEN_MS;
				refresh = now_ms() + REFRESH_MS;
			} else if (ev.value == 0) {
				held = 0;
			}
		}
		t = now_ms();

		if (held && t - held >= HOLD_MS) {
			/* boot: a black screen until the system draws */
			memset(fb, 0, stride * height * 4);
			screen(1);
			if (tty >= 0)
				ioctl(tty, KDSETMODE, KD_TEXT);
			return 0;
		}

		if (charger_online()) {
			unplugged = 0;
		} else if (!unplugged) {
			unplugged = t;
		} else if (t - unplugged >= UNPLUGGED_MS) {
			sync();
			reboot(RB_POWER_OFF);
		}

		if (on && t >= on_until) {
			screen(0);
			on = 0;
		} else if (on && t >= refresh) {
			draw();
			refresh = t + REFRESH_MS;
		}
	}
}
