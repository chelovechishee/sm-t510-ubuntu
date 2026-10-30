/*
 * fbtool: drive the Samsung exynos decon framebuffer on SM-T510.
 * The decon driver ignores write(2) on /dev/fb0; pixels go through mmap and
 * reach the panel only after FBIOBLANK(UNBLANK) + FBIOPAN_DISPLAY.
 *
 *   fbtool unblank          wake the panel and show the current buffer
 *   fbtool show <file.raw>  copy a raw 1200x1920 32bpp image, then show it
 *   fbtool info             print screen geometry
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int pan(int fd, struct fb_var_screeninfo *var)
{
	var->xoffset = 0;
	var->yoffset = 0;
	if (ioctl(fd, FBIOPAN_DISPLAY, var) < 0) {
		perror("FBIOPAN_DISPLAY");
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = getenv("FB") ? getenv("FB") : "/dev/fb0";
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int fd;

	if (argc < 2) {
		fprintf(stderr, "usage: %s unblank | show <file.raw> | info\n", argv[0]);
		return 2;
	}

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror(dev);
		return 1;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		perror("FBIOGET_*SCREENINFO");
		return 1;
	}

	if (!strcmp(argv[1], "info")) {
		printf("%ux%u virt %ux%u bpp %u line %u smem %u\n", var.xres, var.yres,
		       var.xres_virtual, var.yres_virtual, var.bits_per_pixel,
		       fix.line_length, fix.smem_len);
		return 0;
	}

	if (ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0)
		perror("FBIOBLANK (already on?)");

	if (!strcmp(argv[1], "show") && argc > 2) {
		size_t len = (size_t)fix.line_length * var.yres;
		unsigned char *fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE,
					 MAP_SHARED, fd, 0);
		FILE *f;

		if (fb == MAP_FAILED) {
			perror("mmap");
			return 1;
		}
		f = fopen(argv[2], "rb");
		if (!f) {
			perror(argv[2]);
			return 1;
		}
		if (fread(fb, 1, len, f) != len)
			fprintf(stderr, "short image, showing anyway\n");
		fclose(f);
		msync(fb, len, MS_SYNC);
	}

	return pan(fd, &var);
}
