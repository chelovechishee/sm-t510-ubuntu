// SPDX-License-Identifier: GPL-2.0
/*
 * Dump the USB PHY, DWC3, PMU and USB clock registers of the SM-T510 to the
 * kernel log, to compare the working downstream setup with mainline.
 * insmod prints and fails on purpose, so it never stays loaded.
 */
#include <linux/io.h>
#include <linux/module.h>

struct span {
	const char *name;
	phys_addr_t base;
	const u32 *offs;
	int n;
};

static const u32 usbcon[] = {
	0x00, 0x04, 0x08, 0x20, 0x24, 0x28, 0x30, 0x34, 0x38, 0x48,
	0x50, 0x54, 0x58, 0x5c, 0x60, 0x100,
};
static const u32 dwc3[] = {
	0xc100, 0xc104, 0xc108, 0xc10c, 0xc110, 0xc120, 0xc12c, 0xc140,
	0xc144, 0xc148, 0xc14c, 0xc150, 0xc154, 0xc158, 0xc200, 0xc2c0,
	0xc630, 0xc700, 0xc704, 0xc708, 0xc70c,
};
static const u32 pmu[] = { 0x66c, 0x674, 0x808, 0x80c };
static const u32 cmu_fsys[] = {
	0x0000, 0x0180, 0x01a0, 0x01a4, 0x01a8, 0x200c, 0x2068, 0x2074, 0x2078,
};
static const u32 cmu_top[] = { 0x1038, 0x1854, 0x2054 };

static const struct span spans[] = {
	{ "usbcon", 0x135d0000, usbcon, ARRAY_SIZE(usbcon) },
	{ "dwc3", 0x13600000, dwc3, ARRAY_SIZE(dwc3) },
	{ "pmu", 0x11c80000, pmu, ARRAY_SIZE(pmu) },
	{ "cmu_fsys", 0x13400000, cmu_fsys, ARRAY_SIZE(cmu_fsys) },
	{ "cmu_top", 0x12060000, cmu_top, ARRAY_SIZE(cmu_top) },
};

static int __init usbregs_init(void)
{
	int i, j;

	for (i = 0; i < ARRAY_SIZE(spans); i++) {
		const struct span *s = &spans[i];
		void __iomem *base = ioremap(s->base, 0x10000);

		if (!base)
			continue;
		for (j = 0; j < s->n; j++)
			pr_info("usbregs %s+0x%04x = 0x%08x\n", s->name,
				s->offs[j], readl(base + s->offs[j]));
		iounmap(base);
	}
	return -EAGAIN;
}
module_init(usbregs_init);
MODULE_LICENSE("GPL");
