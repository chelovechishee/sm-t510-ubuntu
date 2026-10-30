#!/bin/bash
# Build the SM-T510 system image: Ubuntu 26.04 arm64 from a dated snapshot of
# the Ubuntu archive (snapshot.ubuntu.com), the packages of
# scripts/image-packages.txt, our overlay and Xfce defaults.
# The image holds no user account, no keys and no Samsung firmware: the person
# installing adds an account through cloud-init (scripts/seed-image.sh), the
# tablet takes its firmware from its own VENDOR partition (t510-firmware), and
# Google Chrome comes from Google on the first boot online (t510-chrome).
# Needs root (chroot) and, on an x86 PC, qemu-user with binfmt for arm64.
#
# Usage: sudo scripts/build-image.sh [--snapshot 20261029T000000Z] [--with-firmware]
#   --with-firmware  put firmware/ (scripts/get-firmware.sh) into /lib/firmware:
#                    for your own tablet only, never for an image you publish
# Result: out/sm-t510-ubuntu.img (ext4), .img.zst, .manifest, .sha256
set -euo pipefail

T=$(cd "$(dirname "$0")/.." && pwd)
SNAPSHOT=20261029T000000Z
WITH_FIRMWARE=0
while [ $# -gt 0 ]; do
	case $1 in
	--snapshot) SNAPSHOT=$2; shift 2 ;;
	--with-firmware) WITH_FIRMWARE=1; shift ;;
	*) sed -n '2,/^set /p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 1 ;;
	esac
done

BASE=ubuntu-base-26.04.1-base-arm64.tar.gz
BASE_URL=https://cdimage.ubuntu.com/ubuntu-base/releases/26.04.1/release/$BASE
BASE_SHA256=5a1906794ced63a71a8119c3f211ef5f0bbe0a243001b4bbd41fdf80c5b219fd
MIRROR=https://snapshot.ubuntu.com/ubuntu/$SNAPSHOT
R=$T/out/rootfs
IMG=$T/out/sm-t510-ubuntu.img

[ "$(id -u)" = 0 ] || { echo "run as root: sudo $0"; exit 1; }

# no terminal inside: apt otherwise gives dpkg a pty of its own, and under
# qemu-user behind sudo's pty apt-get ended up stopped (state T) for good
in_root() { chroot "$R" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin \
	LANG=C.UTF-8 DEBIAN_FRONTEND=noninteractive "$@" < /dev/null; }

cleanup() {
	# daemons a maintainer script left running inside the tree
	for p in /proc/[0-9]*; do
		[ "$(readlink "$p/root" 2>/dev/null)" = "$R" ] && kill "${p#/proc/}" 2>/dev/null
	done
	for m in dev/pts dev proc sys tmp/src; do
		mountpoint -q "$R/$m" && umount -l "$R/$m"
	done
	return 0
}
trap cleanup EXIT

mkdir -p "$T/dl" "$T/out"
[ -f "$T/dl/$BASE" ] || curl -fL "$BASE_URL" -o "$T/dl/$BASE"
echo "$BASE_SHA256  $T/dl/$BASE" | sha256sum -c --quiet

# a run that died may have left /dev, /proc... bound under $R: never let
# rm reach the host through them
findmnt -rn -o TARGET | { grep "^$R/" || true; } | sort -r | while read -r m; do umount -l "$m"; done
if findmnt -rn -o TARGET | grep -q "^$R/"; then
	echo "still mounted under $R, not removing it"; exit 1
fi
rm -rf --one-file-system "$R"
mkdir -p "$R"
tar -xpzf "$T/dl/$BASE" -C "$R"
mount --bind /dev "$R/dev"
mount --bind /dev/pts "$R/dev/pts"
mount -t proc proc "$R/proc"
mount -t sysfs sys "$R/sys"
rm -f "$R/etc/resolv.conf"
cp -L /etc/resolv.conf "$R/etc/resolv.conf"
# no services start inside the build
printf '#!/bin/sh\nexit 101\n' > "$R/usr/sbin/policy-rc.d"
chmod 755 "$R/usr/sbin/policy-rc.d"

# the archive as it was at $SNAPSHOT: the same packages on every build
sources() {
	cat > "$R/etc/apt/sources.list.d/ubuntu.sources" <<-EOF
	Types: deb
	URIs: $1
	Suites: resolute resolute-updates resolute-backports
	Components: main restricted universe multiverse
	Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

	Types: deb
	URIs: $2
	Suites: resolute-security
	Components: main restricted universe multiverse
	Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
	EOF
}
rm -f "$R/etc/apt/sources.list"
sources "$MIRROR" "$MIRROR"
install -D -m 644 -t "$R/etc/apt/preferences.d" "$T"/overlay/etc/apt/preferences.d/*
echo 'Dpkg::Use-Pty "0";' > "$R/etc/apt/apt.conf.d/99build-nopty"
# snapshot.ubuntu.com is https-only and ubuntu-base has no CA certificates
# yet: apt uses the build machine's bundle until the image has its own
install -m 644 /etc/ssl/certs/ca-certificates.crt "$R/tmp/build-ca.crt"
echo 'Acquire::https::CaInfo "/tmp/build-ca.crt";' > "$R/etc/apt/apt.conf.d/99build-ca"

PACKAGES=$(sed 's/#.*//' "$T/scripts/image-packages.txt" | xargs)
in_root apt-get update --error-on=any
in_root apt-get -y upgrade
# shellcheck disable=SC2086
in_root apt-get -y install $PACKAGES

# our small programs, built natively in the tree; the compilers go again
BUILD_DEPS="gcc libc6-dev libx11-dev libxi-dev libxtst-dev"
# shellcheck disable=SC2086
in_root apt-get -y install --no-install-recommends $BUILD_DEPS
mkdir -p "$R/tmp/src" "$R/usr/local/bin" "$R/usr/local/lib"
mount --bind "$T/tools" "$R/tmp/src"
in_root gcc -O2 -o /usr/local/bin/fbtool /tmp/src/fbtool.c
in_root gcc -O2 -o /usr/local/bin/longpress /tmp/src/longpress.c -lX11 -lXi -lXtst
in_root gcc -O2 -shared -fPIC -o /usr/local/lib/t510-nograb.so /tmp/src/nograb.c
umount "$R/tmp/src"
# shellcheck disable=SC2086
in_root apt-get -y purge $BUILD_DEPS
in_root apt-get -y autoremove --purge

# overlay: root-owned, existing directories left as they are
tar -C "$T/overlay" --owner=0 --group=0 -cf - . | tar -x -C "$R" --no-overwrite-dir
(cd "$T/overlay" && find . -mindepth 1 -type d) | while read -r d; do chmod go-w "$R/$d"; done
in_root glib-compile-schemas /usr/share/glib-2.0/schemas
in_root systemctl enable usb-gadget resize-rootfs boot-ok t510-display \
	t510-firmware t510-firmware-fetch t510-audio t510-no-lmk zram-manual \
	t510-chrome systemd-networkd
in_root systemctl mask systemd-networkd-wait-online.service systemd-firstboot.service
# the tablet starts LightDM with the Xfce session: a recommends chain that
# brings in another display manager makes a different desktop, stop here
DM=$(cat "$R/etc/X11/default-display-manager" 2>/dev/null || true)
[ "$DM" = /usr/sbin/lightdm ] || { echo "display manager is '$DM', not LightDM"; exit 1; }
install -m 755 "$T/scripts/trim-services.sh" "$R/tmp/trim-services.sh"
in_root sh /tmp/trim-services.sh
# systemd applies the presets on the first boot (machine-id "uninitialized"),
# and Ubuntu's enable everything: what is off in the image would be on again
# (it was: apport, ModemManager, plocate, Ubuntu Pro timers...). A preset
# keeps every unit off that is off here
mkdir -p "$R/etc/systemd/system-preset"
{
	echo "# image build: units off in the image stay off on the first boot"
	systemctl --root="$R" list-unit-files --type=service,socket,timer,path \
		--state=disabled --no-legend | awk '{ print "disable " $1 }'
} > "$R/etc/systemd/system-preset/80-t510-image.preset"

# English, UTC; the installer's cloud-init seed may change both
in_root sed -i 's/^# *\(en_US.UTF-8\)/\1/' /etc/locale.gen
in_root locale-gen
# files below are replaced, not written through: a symlink in the tree must
# never lead a write out to the build machine
rm -f "$R/etc/locale.conf" "$R/etc/timezone" "$R/etc/hostname" "$R/etc/hosts" "$R/etc/fstab"
echo LANG=en_US.UTF-8 > "$R/etc/locale.conf"
ln -sf /usr/share/zoneinfo/Etc/UTC "$R/etc/localtime"
echo Etc/UTC > "$R/etc/timezone"

echo t510 > "$R/etc/hostname"
printf '127.0.0.1\tlocalhost\n127.0.1.1\tt510\n::1\tlocalhost ip6-localhost ip6-loopback\n' > "$R/etc/hosts"
# Android's VENDOR partition stays on the tablet: Wi-Fi firmware is read from
# /vendor/etc/wifi there (ro,noload: never writes, not even a journal replay)
cat > "$R/etc/fstab" <<'EOF'
# / is mounted by the initramfs
/dev/mmcblk0p3	/efs	ext4	ro,nofail	0 0
/dev/mmcblk0p26	/vendor	ext4	ro,noload,nofail	0 0
EOF
# mount point for VENDOR; SCSC looks for firmware only when /system exists
mkdir -p "$R/vendor" "$R/system"
if [ "$WITH_FIRMWARE" = 1 ]; then
	install -D -m 644 -t "$R/lib/firmware" "$T"/firmware/*
fi

# nothing of this machine or of the build: every tablet gets its own
in_root passwd -l root
# "uninitialized" (not empty) makes the next boot a first boot: systemd sets
# ConditionFirstBoot, so sshd-keygen makes this tablet's SSH host keys
echo uninitialized > "$R/etc/machine-id"
rm -f "$R/var/lib/dbus/machine-id" "$R"/etc/ssh/ssh_host_*
ln -s /etc/machine-id "$R/var/lib/dbus/machine-id"
sources http://archive.ubuntu.com/ubuntu http://security.ubuntu.com/ubuntu

in_root dpkg-query -W -f '${Package} ${Version}\n' > "$IMG.manifest"
cp "$R/bin/busybox" "$T/out/busybox"
in_root apt-get clean
rm -f "$R/etc/apt/apt.conf.d/99build-ca" "$R/etc/apt/apt.conf.d/99build-nopty"
rm -rf "$R"/var/lib/apt/lists/* "$R"/var/cache/debconf/*-old "$R"/tmp/* "$R"/var/tmp/* \
	"$R"/root/.bash_history "$R/usr/sbin/policy-rc.d"
find "$R/var/log" -type f -exec truncate -s 0 {} +
ln -sf ../run/systemd/resolve/stub-resolv.conf "$R/etc/resolv.conf"

cleanup
trap - EXIT

rm -f "$IMG" "$IMG.zst"
SIZE_MB=$(( $(du -sm "$R" | cut -f1) * 11 / 10 + 256 ))
mkfs.ext4 -q -F -L ubuntu -d "$R" "$IMG" "${SIZE_MB}M"
e2fsck -fy "$IMG" >/dev/null || true
zstd -q -T0 -19 "$IMG" -o "$IMG.zst"
(cd "$T/out" && sha256sum "$(basename "$IMG")" "$(basename "$IMG").zst" > "$(basename "$IMG").sha256")
[ -n "${SUDO_UID:-}" ] && chown "$SUDO_UID:$SUDO_GID" "$IMG" "$IMG".* "$T/out/busybox"
ls -la "$IMG" "$IMG".*
echo IMAGE_DONE
