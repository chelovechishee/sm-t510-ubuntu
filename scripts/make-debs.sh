#!/bin/bash
# Build the packages of the apt repository in out/debs/ (docs/maintenance.md):
#   t510-support          overlay/ and the programs of tools/; the version is
#                         the top one of CHANGELOG.md
#   t510-kernel           mainline/out/boot-mainline.img; the version is the
#                         kernel's and its build, 7.3.0+66
#   t510-archive-keyring  the repository's apt source and public key
# Install them on your own tablet to test them: scripts/publish.sh then
# publishes exactly these files.
#
# Needs: dpkg-deb, the arm64 cross compiler and the arm64 X libraries
#   sudo apt install gcc-aarch64-linux-gnu libx11-dev:arm64 libxi-dev:arm64 libxtst-dev:arm64
# and the repository's key in packaging/t510-archive-keyring/ (docs/maintenance.md,
# "The signing key"; T510_KEYRING=file for another one).
#
# Usage: scripts/make-debs.sh           build them
#        scripts/make-debs.sh --check   only check that out/debs/ holds them,
#                                       newer than their sources (the image
#                                       builders ask)
set -euo pipefail

T=$(cd "$(dirname "$0")/.." && pwd)
P=$T/packaging
O=$T/out/debs
K=${KERNEL_SRC:-$T/src/linux-7.3}
BOOT=$T/mainline/out/boot-mainline.img
KEY=${T510_KEYRING:-$P/t510-archive-keyring/t510-archive-keyring.gpg}
CC=aarch64-linux-gnu-gcc

die() { echo "make-debs: $*" >&2; exit 1; }
case ${1:-} in
'') CHECK=0 ;;
--check) CHECK=1 ;;
*) sed -n '2,/^set /p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 1 ;;
esac

VERSION=$(sed -n 's/^## \([0-9][0-9.]*\) .*/\1/p' "$T/CHANGELOG.md" | head -n1)
[ -n "$VERSION" ] || die "no '## x.y.z — title' heading in CHANGELOG.md"
if [ "$CHECK" = 1 ]; then
	for p in t510-support_"$VERSION"_arm64 t510-kernel_*_arm64 t510-archive-keyring_*_all; do
		ls "$O"/$p.deb >/dev/null 2>&1 || die "no $p.deb in $O: run scripts/make-debs.sh"
	done
	n=$(find "$T/overlay" "$T/packaging" "$T/tools/fbtool.c" "$T/tools/longpress.c" \
		"$T/tools/nograb.c" "$T/tools/noblank.c" "$BOOT" -type f -newer "$(ls -tr "$O"/*.deb | head -n1)" -print -quit)
	[ -z "$n" ] || die "the packages in $O are older than ${n#"$T"/}: run scripts/make-debs.sh"
	# built with the repository's key, not a test one (T510_KEYRING)
	[ -f "$P/t510-archive-keyring/t510-archive-keyring.gpg" ] &&
		dpkg-deb --fsys-tarfile "$(ls "$O"/t510-archive-keyring_*_all.deb)" |
		tar -xO ./usr/share/keyrings/t510-archive-keyring.gpg |
		cmp -s - "$P/t510-archive-keyring/t510-archive-keyring.gpg" ||
		die "t510-archive-keyring in $O does not carry packaging/t510-archive-keyring/t510-archive-keyring.gpg"
	exit 0
fi
command -v "$CC" >/dev/null || die "no $CC (sudo apt install gcc-aarch64-linux-gnu)"
[ -f "$KEY" ] || die "no repository key at $KEY (docs/maintenance.md, \"The signing key\")"

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
# the commit's time on every file: the same sources make the same files
SOURCE_DATE_EPOCH=$(git -C "$T" log -1 --format=%ct)
export SOURCE_DATE_EPOCH

# DIR TEMPLATE [sed expressions]: DEBIAN/control and md5sums, then the .deb
# (root-owned, nothing writable but by root)
build() {
	local d=$1 tmpl=$2
	shift 2
	mkdir -p "$d/DEBIAN"
	(cd "$d" && find . -path ./DEBIAN -prune -o -type f -printf '%P\0' |
		sort -z | xargs -0r md5sum) > "$d/DEBIAN/md5sums"
	[ -s "$d/DEBIAN/md5sums" ] || rm "$d/DEBIAN/md5sums"
	sed -e "s/@SIZE@/$(du -sk --exclude=DEBIAN "$d" | cut -f1)/" "$@" "$tmpl" > "$d/DEBIAN/control"
	find "$d" -type d -exec chmod 755 {} +
	chmod -R go-w "$d"
	find "$d" -newermt "@$SOURCE_DATE_EPOCH" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +
	dpkg-deb --root-owner-group -Zxz --build "$d" "$O/" >/dev/null
}

rm -rf "$O"
mkdir -p "$O"

# --- t510-support
S=$W/t510-support
mkdir -p "$S/usr/libexec/t510"
cp -a "$T/overlay/." "$S/"
find "$S" -name __pycache__ -prune -exec rm -rf {} +
"$CC" -O2 -o "$S/usr/libexec/t510/fbtool" "$T/tools/fbtool.c"
"$CC" -O2 -o "$S/usr/libexec/t510/longpress" "$T/tools/longpress.c" -lX11 -lXi -lXtst ||
	die "longpress does not link: sudo apt install libx11-dev:arm64 libxi-dev:arm64 libxtst-dev:arm64"
"$CC" -O2 -shared -fPIC -o "$S/usr/libexec/t510/nograb.so" "$T/tools/nograb.c"
"$CC" -O2 -shared -fPIC -o "$S/usr/libexec/t510/noblank.so" "$T/tools/noblank.c"
aarch64-linux-gnu-strip "$S"/usr/libexec/t510/{fbtool,longpress,nograb.so,noblank.so}
chmod 644 "$S"/usr/libexec/t510/{nograb,noblank}.so
chmod 440 "$S/etc/sudoers.d/t510-rotate"
mkdir -p "$S/DEBIAN"
# every file in /etc is the owner's to change: dpkg asks before replacing it
(cd "$S" && find etc -type f | sort | sed 's|^|/|') > "$S/DEBIAN/conffiles"
# the files of 0.7.0, marked by whether the package still has them there
grep -v '^#' "$P/t510-support/legacy-0.7.0" | while read -r sum path; do
	if [ -e "$S$path" ]; then echo "same $sum $path"; else echo "moved $sum $path"; fi
done > "$W/legacy"
awk -v list="$W/legacy" '
	$0 == "@LEGACY@" { while ((getline l < list) > 0) print l; next }
	{ print }' "$P/t510-support/preinst" > "$S/DEBIAN/preinst"
install -m 755 "$P/t510-support/postinst" "$P/t510-support/postrm" "$S/DEBIAN/"
chmod 755 "$S/DEBIAN/preinst"
build "$S" "$P/t510-support/control" -e "s/@VERSION@/$VERSION/"

# --- t510-kernel: the boot image of the kernel tree as it is built now
IMAGE=$K/arch/arm64/boot/Image
[ -f "$BOOT" ] && [ -f "$IMAGE" ] || die "no kernel build (README.md, \"Building\")"
n=$(find "$K/.config" "$IMAGE" "$T/initramfs" -newer "$BOOT" -print -quit)
[ -z "$n" ] || die "$BOOT is older than $n: run mainline/make-boot.sh"
# no Samsung firmware built in: the published kernel reads it from the tablet
grep -qx 'CONFIG_EXTRA_FIRMWARE=""' "$K/.config" ||
	die "the kernel builds firmware in: $(grep '^CONFIG_EXTRA_FIRMWARE=' "$K/.config")"
KVER=$(make -s -C "$K" kernelversion)
BUILD=$(LC_ALL=C grep -a -o '#[0-9][0-9]* SMP PREEMPT' "$IMAGE" | head -n1 | tr -dc 0-9)
[ -n "$BUILD" ] || die "no build number (#N) in $IMAGE"
LC_ALL=C grep -aq "#$BUILD SMP PREEMPT" "$BOOT" || die "$BOOT does not hold build #$BUILD"
S=$W/t510-kernel
install -D -m 644 "$BOOT" "$S/usr/lib/t510/boot-mainline.img"
install -D -m 755 "$P/t510-kernel/postinst" "$S/DEBIAN/postinst"
build "$S" "$P/t510-kernel/control" -e "s/@VERSION@/$KVER+$BUILD/" \
	-e "s/@KERNEL@/$KVER/" -e "s/@BUILD@/$BUILD/"

# --- t510-archive-keyring
S=$W/t510-archive-keyring
install -D -m 644 "$KEY" "$S/usr/share/keyrings/t510-archive-keyring.gpg"
install -D -m 644 "$P/t510-archive-keyring/t510.sources" "$S/etc/apt/sources.list.d/t510.sources"
mkdir -p "$S/DEBIAN"
echo /etc/apt/sources.list.d/t510.sources > "$S/DEBIAN/conffiles"
build "$S" "$P/t510-archive-keyring/control"

ls -la "$O"
echo DEBS_DONE
