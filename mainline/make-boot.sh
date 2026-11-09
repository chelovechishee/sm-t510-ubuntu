#!/bin/bash
# Pack the mainline kernel into a Samsung boot.img for SM-T510:
# S-Boot loads minimal_sboot_wrapper as the "kernel"; the wrapper ignores the
# S-Boot DTB and cmdline and starts Image with our DTB, using the ramdisk that
# S-Boot always places at 0x89000000.
# Usage: [SPLASH=1] [FB_MARK=1] [INSTALL=1] mainline/make-boot.sh [initramfs dir]   (default: initramfs-phase0)
# INSTALL=1: boot-install.img, whose initramfs only waits on USB for install.sh
# KERNEL_SRC=/path: another kernel tree than src/linux-7.3
set -euo pipefail

M=$(cd "$(dirname "$0")" && pwd)
T=$(dirname "$M")
K=${KERNEL_SRC:-$T/src/linux-7.3}
O=$M/out
I=$O/initramfs
SRC=${1:-$M/initramfs-phase0}
# default: kernel warnings and systemd messages on screen (see what fails);
# SPLASH=1: S-Boot logo stays (deferred fbcon takeover), then the Plymouth splash;
# FB_MARK=1: the wrapper paints a green bar when it gets control (bring-up)
BASE="clk_ignore_unused pd_ignore_unused printk.devkmsg=on fbcon=font:TER16x32 panic=0"
if [ "${SPLASH:-0}" = 1 ]; then
	CMDLINE="console=tty0 quiet loglevel=3 splash vt.global_cursor_default=0 $BASE"
else
	CMDLINE="console=tty0 loglevel=4 $BASE"
fi
FB_MARK_ARG=
[ "${FB_MARK:-0}" = 1 ] && FB_MARK_ARG="FB_MARK=0xEC000000"

W=$M/minimal_sboot_wrapper
if [ ! -d "$W" ]; then
	git clone -q https://github.com/VDavid003/minimal_sboot_wrapper.git "$W"
	git -C "$W" checkout -q f0fbe9c8c812f15507988153a7ae63fe316c6a66
	git -C "$W" apply "$M/patches/sboot-wrapper.patch"
fi

rm -rf "$I"
mkdir -p "$I/bin" "$I/dev" "$I/proc" "$I/sys"
cp "$T/out/busybox" "$I/bin/busybox"
cp -a "$SRC/." "$I/"
chmod 755 "$I/init"
OUT=$O/boot-mainline.img
if [ "${INSTALL:-0}" = 1 ]; then
	touch "$I/install"
	OUT=$O/boot-install.img
fi
(cd "$I" && find . | cpio -o -H newc -R 0:0 --quiet | gzip -9) > "$O/initramfs.cpio.gz"
RD_SIZE=$(stat -c %s "$O/initramfs.cpio.gz")

make -s -C "$M/minimal_sboot_wrapper" clean >/dev/null 2>&1 || true
make -s -C "$M/minimal_sboot_wrapper" \
	KERNEL_PATH="$K/arch/arm64/boot/Image" \
	DTB_PATH="$K/arch/arm64/boot/dts/exynos/exynos7904-gta3xlwifi.dtb" \
	CMDLINE="$CMDLINE" \
	SEC_REBOOT_REASON_RESET=1 \
	KERNEL_LOAD=0xA0000000 \
	$FB_MARK_ARG \
	INITRD_END=$(printf '0x%x' $((0x89000000 + RD_SIZE)))
cp "$M/minimal_sboot_wrapper/wrapped_kernel" "$O/wrapped_kernel"

mkbootimg \
	--kernel "$O/wrapped_kernel" \
	--ramdisk "$O/initramfs.cpio.gz" \
	--cmdline "$CMDLINE" \
	--base 0x10000000 --pagesize 2048 \
	--kernel_offset 0x00008000 --ramdisk_offset 0x01000000 --tags_offset 0x00000100 \
	--os_version 11.0.0 --os_patch_level 2023-01 \
	--header_version 1 \
	-o "$OUT"

ls -la "$O/wrapped_kernel" "$O/initramfs.cpio.gz" "$OUT"
SIZE=$(stat -c %s "$OUT")
# fits both BOOT (32 MiB) and RECOVERY (45 MiB); the wrapper copies 32 MiB of Image
[ "$SIZE" -le $((32 * 1024 * 1024)) ] || { echo "$OUT too big: $SIZE"; exit 1; }
echo BOOT_DONE
