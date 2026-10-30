#!/bin/bash
# Collect Samsung's closed firmware into firmware/ for a local system image
# (scripts/build-image.sh --with-firmware puts it in /lib/firmware). Nothing here is
# redistributed by this project, and the kernel embeds none of it:
#   exynos7885_acpm_fvp.fw  ACPM DVFS plugin — from Samsung's GPL kernel tree
#                           (gta3xlwifi-dev/android_kernel_gta3xlwifi)
#   calliope_{sram,dram}.bin  ABOX audio DSP — from the tablet's own Android
#                           VENDOR partition (p26), which the install keeps
# On the tablet, t510-firmware does the same at boot (VENDOR is mounted at
# /vendor, where the Wi-Fi driver also reads /vendor/etc/wifi).
#
# Usage: scripts/get-firmware.sh --ssh root@172.16.42.1   (tablet runs this Ubuntu)
#        scripts/get-firmware.sh --adb                     (tablet runs stock Android)
#        scripts/get-firmware.sh --vendor-img vendor.img[.lz4]  (from the stock firmware AP tar)
set -euo pipefail

T=$(cd "$(dirname "$0")/.." && pwd)
FW=$T/firmware

FVP_COMMIT=00e4b9481434f9b0b644925a9bb2feaf3940219d
FVP_URL=https://raw.githubusercontent.com/gta3xlwifi-dev/android_kernel_gta3xlwifi/$FVP_COMMIT/firmware/exynos7885_acpm_fvp.fw

# the versions this port was tested with (stock Android 11 vendor)
declare -A SHA256=(
	[exynos7885_acpm_fvp.fw]=3793478b6df52fe760a89cbcfb258a9c489365ee2d13d9dae8df2f161abadb61
	[calliope_sram.bin]=df08279adb9f6758ea89a7fe02321c4048a6b6f02389510e92b3a7a465c3fa8d
	[calliope_dram.bin]=f35ec94146fe476c765f6fc8254be112fc38f0f3d2b821c74c675bb23fed62e5
)
VENDOR_FILES="calliope_sram.bin calliope_dram.bin"

usage() { sed -n '2,/^set /p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 1; }

check() {
	local f=$1 sum
	sum=$(sha256sum "$FW/$f" | cut -d' ' -f1)
	if [ "$sum" = "${SHA256[$f]}" ]; then
		echo "ok       $f"
	else
		echo "WARNING  $f differs from the tested version (another stock firmware?)"
	fi
}

[ $# -ge 1 ] || usage
mkdir -p "$FW"

echo "fetching exynos7885_acpm_fvp.fw from Samsung's kernel tree"
curl -fsSL "$FVP_URL" -o "$FW/exynos7885_acpm_fvp.fw.tmp"
mv "$FW/exynos7885_acpm_fvp.fw.tmp" "$FW/exynos7885_acpm_fvp.fw"

case $1 in
--ssh)
	[ $# -eq 2 ] || usage
	# debugfs reads the ext4 partition without mounting it
	for f in $VENDOR_FILES; do
		${SSH:-ssh} "$2" "debugfs -R 'cat /firmware/$f' /dev/mmcblk0p26 2>/dev/null" > "$FW/$f.tmp"
		mv "$FW/$f.tmp" "$FW/$f"
	done
	;;
--adb)
	for f in $VENDOR_FILES; do
		adb pull "/vendor/firmware/$f" "$FW/$f"
	done
	;;
--vendor-img)
	[ $# -eq 2 ] || usage
	IMG=$2
	TMP=$(mktemp -d)
	trap 'rm -rf "$TMP"' EXIT
	case $IMG in
	*.lz4) lz4 -dc "$IMG" > "$TMP/vendor.img"; IMG=$TMP/vendor.img ;;
	esac
	# Android sparse image → raw ext4
	if [ "$(head -c4 "$IMG" | od -An -tx4 | tr -d ' ')" = ed26ff3a ]; then
		simg2img "$IMG" "$TMP/vendor.raw"
		IMG=$TMP/vendor.raw
	fi
	for f in $VENDOR_FILES; do
		debugfs -R "dump /firmware/$f $FW/$f" "$IMG" 2>/dev/null
		[ -s "$FW/$f" ] || { echo "$f not found in $2"; exit 1; }
	done
	;;
*)
	usage
	;;
esac

for f in exynos7885_acpm_fvp.fw $VENDOR_FILES; do
	[ -s "$FW/$f" ] || { echo "$f is missing or empty"; exit 1; }
	check "$f"
done
echo "firmware in $FW"
