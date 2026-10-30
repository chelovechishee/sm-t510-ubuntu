#!/bin/bash
# Build the SM-T510 kernel: Linux 7.3 + mainline/patches, config from
# mainline/configs/t510.fragment. No closed firmware goes into the kernel: the
# drivers load it from /lib/firmware at run time (t510-firmware).
# Result: Image and DTB in the kernel tree, ready for mainline/make-boot.sh.
# Usage: mainline/build-kernel.sh            (tree in src/linux-7.3, made on first run)
#        KERNEL_SRC=/path mainline/build-kernel.sh
#        EXTRA_FRAGMENTS=mainline/configs/debug.fragment mainline/build-kernel.sh
set -euo pipefail

M=$(cd "$(dirname "$0")" && pwd)
T=$(dirname "$M")
K=${KERNEL_SRC:-$T/src/linux-7.3}
VER=7.3
TARBALL=$T/dl/linux-$VER.tar.xz
export ARCH=arm64 CROSS_COMPILE=${CROSS_COMPILE:-aarch64-linux-gnu-}
# uname -v shows user@host of the build: keep the builder's machine out of it
export KBUILD_BUILD_USER=${KBUILD_BUILD_USER:-build} KBUILD_BUILD_HOST=${KBUILD_BUILD_HOST:-sm-t510-ubuntu}

if [ ! -d "$K" ]; then
	mkdir -p "$T/dl" "$(dirname "$K")"
	[ -f "$TARBALL" ] || curl -fL "https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-$VER.tar.xz" -o "$TARBALL"
	tar -xJf "$TARBALL" -C "$(dirname "$K")"
	[ "$(dirname "$K")/linux-$VER" = "$K" ] || mv "$(dirname "$K")/linux-$VER" "$K"
	git -C "$K" init -q
	git -C "$K" add -A
	git -C "$K" -c user.name=build -c user.email=build@localhost commit -qm "Linux $VER"
	git -C "$K" -c user.name=build -c user.email=build@localhost am -q "$M"/patches/0*.patch
fi

CFG=$(mktemp)
trap 'rm -f "$CFG"' EXIT
cat "$M/configs/t510.fragment" ${EXTRA_FRAGMENTS:-} > "$CFG"

make -C "$K" -s allnoconfig KCONFIG_ALLCONFIG="$CFG"
make -C "$K" -j"$(nproc)" Image exynos/exynos7904-gta3xlwifi.dtb
ls -la "$K/arch/arm64/boot/Image" "$K/arch/arm64/boot/dts/exynos/exynos7904-gta3xlwifi.dtb"
echo KERNEL_DONE
