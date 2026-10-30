#!/bin/bash
# Put the files of a GitHub release together in out/release/: the system
# image, the two boot images and SHA256SUMS, plus NOTES.md (this version's
# CHANGELOG section) to paste as the release text. install.sh downloads
# exactly these names from the tag in its RELEASE= line.
#
# Checks first: the version in install.sh is the newest one in CHANGELOG.md,
# the images are newer than what they are built from, the system image holds
# the packages of out/debs/ and BOOT is t510-kernel's image, the system image is
# ready for a first boot and holds no user, the kernel has no firmware built
# in. Upload the files to the release by hand (github.com → Releases).
#
# Usage: scripts/make-release.sh
set -euo pipefail

T=$(cd "$(dirname "$0")/.." && pwd)
O=$T/out/release
IMAGE=$T/out/sm-t510-ubuntu.img.zst
BOOT=$T/mainline/out/boot-mainline.img
INSTALLER=$T/mainline/out/boot-install.img

die() { echo "make-release: $*" >&2; exit 1; }

TAG=$(sed -n 's/^RELEASE=//p' "$T/install.sh")
VERSION=$(sed -n 's/^## \([0-9][0-9.]*\) .*/\1/p' "$T/CHANGELOG.md" | head -n1)
[ "$TAG" = "v$VERSION" ] ||
	die "install.sh downloads $TAG, CHANGELOG.md is at $VERSION"

for f in "$IMAGE" "$BOOT" "$INSTALLER"; do
	[ -f "$f" ] || die "missing $f (README.md, \"Building\")"
done
newer() { find "${@:2}" -type f -newer "$1" -print -quit; }
n=$(newer "$BOOT" "$T/initramfs")
[ -z "$n" ] || die "$BOOT is older than ${n#"$T"/}"
n=$(newer "$INSTALLER" "$T/initramfs")
[ -z "$n" ] || die "$INSTALLER is older than ${n#"$T"/}"
n=$(newer "$IMAGE" "$T/image" "$T/out/debs" "$T/scripts/image-packages.txt" \
	"$T/scripts/image-remove.txt" "$T/scripts/trim-services.sh")
[ -z "$n" ] || die "$IMAGE is older than ${n#"$T"/}"
# the image holds our packages as the apt repository gets them, and the
# BOOT of the release is the kernel of that t510-kernel
"$T/scripts/make-debs.sh" --check
for deb in "$T"/out/debs/t510-*.deb; do
	pv="$(dpkg-deb -f "$deb" Package) $(dpkg-deb -f "$deb" Version)"
	grep -qx "$pv" "${IMAGE%.zst}.manifest" ||
		die "the image does not have $pv (out/debs): build it again"
done
dpkg-deb --fsys-tarfile "$T"/out/debs/t510-kernel_*.deb | tar -xO ./usr/lib/t510/boot-mainline.img |
	cmp -s - "$BOOT" || die "$(basename "$BOOT") is not the boot image of out/debs/t510-kernel"

# the system image (both files of the build): its checksums, first boot,
# nobody's account
RAW=${IMAGE%.zst}
[ -f "$RAW" ] || die "$RAW is missing: the checks below read the unpacked image"
(cd "$T/out" && sha256sum -c --quiet --strict sm-t510-ubuntu.img.sha256) ||
	die "the image does not match its checksums"
d() { debugfs -R "$1" "$RAW" 2>/dev/null; }
[ "$(d 'cat /etc/machine-id')" = uninitialized ] || die "the image is not ready for a first boot"
users=$(d 'cat /etc/passwd' | awk -F: '$3 >= 1000 && $3 < 60000 { print $1 }')
[ -z "$users" ] || die "the image has user accounts: $users"
homes=$(d 'ls /home' | tr -s ' ' '\n' | grep -vE '^$|^\.\.?$|^[0-9]+$|^\([0-9]+\)$' || true)
[ -z "$homes" ] || die "the image has home directories: $homes"

# no Samsung firmware built into the kernel: it is read from the tablet itself
K=${KERNEL_SRC:-$T/src/linux-7.3}
[ -f "$K/.config" ] || die "no kernel tree at $K (KERNEL_SRC=...) to check its config"
grep -qx 'CONFIG_EXTRA_FIRMWARE=""' "$K/.config" ||
	die "the kernel builds firmware in: $(grep '^CONFIG_EXTRA_FIRMWARE=' "$K/.config")"
# the boot images carry the kernel built in that tree
for b in "$BOOT" "$INSTALLER"; do
	n=$(newer "$b" "$K/.config" "$K/arch/arm64/boot/Image")
	[ -z "$n" ] || die "$(basename "$b") is older than ${n#"$K"/} of the kernel tree"
done

rm -rf "$O"
mkdir -p "$O"
cp "$IMAGE" "$BOOT" "$INSTALLER" "$O/"
(cd "$O" && sha256sum sm-t510-ubuntu.img.zst boot-mainline.img boot-install.img > SHA256SUMS)
awk -v v="$VERSION" '
	/^## / { if (on) exit; on = index($0, "## " v " ") == 1 }
	on' "$T/CHANGELOG.md" > "$O/NOTES.md"
ls -la "$O"
echo "Release $TAG: create it on GitHub with the tag $TAG, NOTES.md as its text,"
echo "and upload the four files above."
