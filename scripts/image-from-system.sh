#!/bin/bash
# Make the SM-T510 system image from a working tablet: its root file system
# as it is set up and tested, cleaned of everything that belongs to that one
# tablet and the people using it, with our packages on top. The result is the
# same as from build-image.sh and installs with install.sh: no user account,
# no keys or passwords, no Samsung firmware, English, UTC.
#
# The copy, made on the tablet (as root, the system running):
#   tar --one-file-system --numeric-owner --xattrs --xattrs-include='*' --acls \
#       -C / -cpf - . | zstd -T0 > rootfs.tar.zst
#
# What goes: every user account and home, /root, SSH host keys, the machine
# id, Wi-Fi networks (NetworkManager, netplan, wpa_supplicant), VPNs, Bluetooth
# pairings, printers, the Ubuntu Pro attachment, logs, caches, crash reports,
# backups in /var/backups, the packages of scripts/image-remove.txt (games,
# development tools, GNOME, Chrome, Russian), Samsung firmware in /lib/firmware
# (the audio DSP files become links to the tablet's own VENDOR partition).
# What stays: the installed system and its settings, our packages (out/debs/,
# scripts/make-debs.sh) with the services and the Xfce defaults for a tablet,
# Google's apt source for Chrome.
#
# Needs root (chroot) and, on an x86 PC, qemu-user with binfmt for arm64;
# the network (apt).
#
# Usage: sudo scripts/image-from-system.sh ROOTFS.tar.zst
# Result: out/sm-t510-ubuntu.img (ext4), .img.zst, .manifest, .sha256
set -euo pipefail

T=$(cd "$(dirname "$0")/.." && pwd)
R=$T/out/rootfs
IMG=$T/out/sm-t510-ubuntu.img

[ $# = 1 ] && [ -f "$1" ] || { sed -n '2,/^set /p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 1; }
SRC=$(realpath "$1")
[ "$(id -u)" = 0 ] || { echo "run as root: sudo $0 $1"; exit 1; }
"$T/scripts/make-debs.sh" --check

# no terminal inside: apt otherwise gives dpkg a pty of its own, and under
# qemu-user behind sudo's pty apt-get ended up stopped (state T) for good
in_root() { chroot "$R" /usr/bin/env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin \
	LANG=C.UTF-8 DEBIAN_FRONTEND=noninteractive "$@" < /dev/null; }

cleanup() {
	# daemons a maintainer script left running inside the tree
	for p in /proc/[0-9]*; do
		[ "$(readlink "$p/root" 2>/dev/null)" = "$R" ] && kill "${p#/proc/}" 2>/dev/null
	done
	for m in dev/pts dev proc sys; do
		mountpoint -q "$R/$m" && umount -l "$R/$m"
	done
	return 0
}
trap cleanup EXIT

# a run that died may have left /dev, /proc... bound under $R: never let
# rm reach the host through them
mkdir -p "$T/out"
findmnt -rn -o TARGET | { grep "^$R/" || true; } | sort -r | while read -r m; do umount -l "$m"; done
if findmnt -rn -o TARGET | grep -q "^$R/"; then
	echo "still mounted under $R, not removing it"; exit 1
fi
rm -rf --one-file-system "$R"
mkdir -p "$R"
echo "unpacking $SRC"
# numeric: the owners of the tablet, not this PC's names for them; of the
# extended attributes only file capabilities (and ACLs): SELinux labels or
# user.* ones may not be settable here and mean nothing on the tablet
zstd -dc "$SRC" | tar -x --numeric-owner --xattrs --xattrs-include='security.capability' \
	--acls -p -C "$R"
[ -x "$R/usr/bin/apt-get" ] && [ -f "$R/etc/os-release" ] ||
	{ echo "$SRC holds no root file system"; exit 1; }

mount --bind /dev "$R/dev"
mount --bind /dev/pts "$R/dev/pts"
mount -t proc proc "$R/proc"
mount -t sysfs sys "$R/sys"
# files below are replaced, not written through: a symlink in the tree must
# never lead a write out to the build machine
rm -f "$R/etc/resolv.conf"
cp -L /etc/resolv.conf "$R/etc/resolv.conf"
# no services start inside the build
printf '#!/bin/sh\nexit 101\n' > "$R/usr/sbin/policy-rc.d"
chmod 755 "$R/usr/sbin/policy-rc.d"
echo 'Dpkg::Use-Pty "0";' > "$R/etc/apt/apt.conf.d/99build-nopty"

# --- people: accounts, homes, what they left in the system
USERS=$(awk -F: '$3 >= 1000 && $3 < 60000 { print $1 }' "$R/etc/passwd")
for u in $USERS; do
	echo "removing user $u"
	in_root userdel -r -f "$u" 2>/dev/null || in_root userdel -f "$u"
done
rm -rf "$R"/home/* "$R"/var/mail/* "$R"/var/spool/cron/crontabs/*
# shadow's backups of the account files still list the removed users
rm -f "$R"/etc/passwd- "$R"/etc/shadow- "$R"/etc/group- "$R"/etc/gshadow- \
	"$R"/etc/subuid- "$R"/etc/subgid-
find "$R/root" -mindepth 1 -maxdepth 1 ! -name .bashrc ! -name .profile -exec rm -rf {} +
rm -rf "$R"/var/lib/AccountsService/users/* "$R"/var/lib/AccountsService/icons/* \
	"$R"/var/lib/lightdm-data/* "$R"/var/lib/sudo/lectured/*
find "$R/var/lib/lightdm" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
rm -f "$R"/etc/lightdm/lightdm.conf.d/60-t510-rotate-autologin.conf

# networks, keys, pairings, printers, the Ubuntu Pro token
rm -rf "$R"/etc/NetworkManager/system-connections/* "$R"/var/lib/NetworkManager/* \
	"$R"/etc/netplan/90-NM-* "$R"/etc/wpa_supplicant/wpa_supplicant-*.conf \
	"$R"/var/lib/dhcp/* "$R"/var/lib/dhcpcd/* \
	"$R"/etc/openvpn/client/* "$R"/etc/openvpn/server/* \
	"$R"/var/lib/bluetooth/* \
	"$R"/etc/cups/printers.conf* "$R"/etc/cups/classes.conf* \
	"$R"/etc/cups/subscriptions.conf* "$R"/etc/cups/ppd/* "$R"/var/spool/cups/* \
	"$R"/var/cache/cups/* \
	"$R"/etc/apt/auth.conf.d/90ubuntu-advantage "$R"/var/lib/ubuntu-advantage \
	"$R"/etc/ssh/ssh_host_* "$R"/etc/ssl/private/ssl-cert-snakeoil.key \
	"$R"/etc/ssl/certs/ssl-cert-snakeoil.pem \
	"$R"/var/lib/polkit-1/localauthority/50-local.d/*
# the setup's own ssh access (key login for its user)
rm -f "$R"/etc/ssh/sshd_config.d/20-t510-* "$R"/etc/NetworkManager/dispatcher.d/50-t510-ssh-*

# apt: Ubuntu's archive and Google's Chrome repository (overlay), nothing else
# (sources.list and its leftovers .disabled, .distUpgrade...: files only,
# sources.list.d is a directory)
find "$R/etc/apt" -maxdepth 1 -type f -name 'sources.list*' -delete
rm -f "$R"/etc/apt/sources.list.d/* "$R"/etc/apt/preferences.d/mozilla \
	"$R"/etc/apt/keyrings/packages.mozilla.org.asc
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
sources http://archive.ubuntu.com/ubuntu http://security.ubuntu.com/ubuntu
install -D -m 644 -t "$R/etc/apt/preferences.d" "$T"/overlay/etc/apt/preferences.d/*
in_root apt-get update --error-on=any

# --- packages: what the image is made of stays, the rest of the list goes
pkgs() { sed 's/#.*//' "$1" | xargs; }
# the installed packages that match the names and patterns of a list file;
# the patterns go to dpkg as they are, never to the shell's globbing
installed() {
	local -
	set -f
	# shellcheck disable=SC2046
	{ in_root dpkg-query -W -f '${db:Status-Abbrev} ${Package}\n' $(pkgs "$1") 2>/dev/null || true; } |
		awk '$1 == "ii" { print $2 }' | xargs
}
# the system as it was tested: nothing new is installed, what the image is
# made of is only kept (autoremove spares it)
KEEP="xubuntu-desktop xubuntu-desktop-minimal $(pkgs "$T/scripts/image-packages.txt")"
KEEP=$(in_root dpkg-query -W -f '${db:Status-Abbrev} ${Package}\n' $KEEP 2>/dev/null |
	awk '$1 == "ii" { print $2 }' | xargs || true)
# shellcheck disable=SC2086
in_root apt-mark manual $KEEP >/dev/null
REMOVE=$(installed "$T/scripts/image-remove.txt")
if [ -n "$REMOVE" ]; then
	# shellcheck disable=SC2086
	GONE=$(in_root apt-get -s purge $REMOVE | awk '$1 == "Purg" { print $2 }')
	for p in $KEEP; do
		if echo "$GONE" | grep -qx "$p"; then
			echo "purging $REMOVE would remove $p too"; exit 1
		fi
	done
	# what depends on the list goes with it: in the log, to check
	echo "also removed with them:" $(echo "$GONE" | grep -vxF -f <(echo "$REMOVE" | tr ' ' '\n') | xargs)
	# shellcheck disable=SC2086
	in_root apt-get -y purge $REMOVE
fi

in_root apt-get -y autoremove --purge
# configuration files of packages removed before this copy was made
RC=$(in_root dpkg-query -W -f '${db:Status-Abbrev} ${Package}\n' | awk '$1 == "rc" { print $2 }' | xargs)
# shellcheck disable=SC2086
[ -z "$RC" ] || in_root dpkg --purge $RC

# programs outside the packages: none (ours are t510-support now)
rm -rf "$R"/opt/* "$R/mnt/android-vendor" "$R/snap" "$R/var/snap" \
	"$R"/etc/apparmor.d/claude-desktop "$R"/etc/apparmor.d/Discord \
	"$R"/etc/systemd/system/happd.service "$R"/etc/systemd/system/*.wants/happd.service
rm -rf "$R"/usr/local/bin/* "$R"/usr/local/sbin/*
[ -e "$R/usr/bin/firefox" ] || rm -rf "$R/etc/firefox"
# what purged packages left behind ("directory not empty" in the log): gdm3's
# changed hooks, gdb's bytecode of the old Python 3.12
[ -e "$R/usr/sbin/gdm3" ] || rm -rf "$R/etc/gdm3"
[ -e "$R/usr/bin/gdb" ] || rm -rf "$R/usr/share/gdb/python"

# Samsung firmware: none in the image. t510-support links the audio DSP files to
# the tablet's own VENDOR partition, the ACPM plugin comes from the installer
# (scripts/seed-image.sh) or t510-firmware-fetch
rm -f "$R"/usr/lib/firmware/calliope_*.bin "$R"/usr/lib/firmware/exynos7885_acpm_fvp.fw

# the files of the image only (written on the tablet later: rotation,
# keyboard, hardware name), then our packages as the apt repository has them
# (scripts/make-debs.sh). On a 0.7.0 system t510-support's preinst moves its
# old files away; what someone had changed it keeps as *.t510-0.7.0, and an
# image keeps none of that
tar -C "$T/image" --owner=0 --group=0 -cf - . | tar -x -C "$R" --no-overwrite-dir
(cd "$T/image" && find . -mindepth 1 -type d) | while read -r d; do chmod go-w "$R/$d"; done
mkdir -p "$R/tmp/debs"
cp "$T"/out/debs/t510-*.deb "$R/tmp/debs/"
# --force-confnew: the packaged /etc files, not what the tree had there
in_root sh -c 'apt-get -y -o Dpkg::Options::=--force-confnew install /tmp/debs/*.deb'
find "$R/etc" "$R/usr/local" -name '*.t510-0.7.0' -print -delete
in_root glib-compile-schemas /usr/share/glib-2.0/schemas
in_root systemctl enable systemd-networkd
in_root systemctl disable openvpn.service 2>/dev/null || true
in_root systemctl mask systemd-networkd-wait-online.service systemd-firstboot.service
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

# the screen upright (portrait); the rotation files of image/ are its
in_root /usr/sbin/t510-rotate refresh

# English, US keyboard, UTC; the installer's cloud-init seed may change them.
# debconf first: the packages write these files from its answers again on
# every upgrade (the copy had Russian, us,ru and Moscow there)
# (a file: in_root feeds its commands /dev/null)
cat > "$R/tmp/selections" <<-'EOF'
	locales locales/default_environment_locale select en_US.UTF-8
	locales locales/locales_to_be_generated multiselect en_US.UTF-8 UTF-8
	tzdata tzdata/Areas select Etc
	tzdata tzdata/Zones/Etc select UTC
	tzdata tzdata/Zones/Europe select
	keyboard-configuration keyboard-configuration/layoutcode string us
	keyboard-configuration keyboard-configuration/variantcode string
	keyboard-configuration keyboard-configuration/optionscode string
	keyboard-configuration keyboard-configuration/layout select English (US)
	keyboard-configuration keyboard-configuration/variant select English (US)
	keyboard-configuration keyboard-configuration/toggle select No toggling
	console-setup console-setup/codesetcode string guess
	console-setup console-setup/codeset47 select Guess optimal character set
	EOF
in_root debconf-set-selections /tmp/selections
rm "$R/tmp/selections"
sed -i 's/^CODESET=.*/CODESET="guess"/' "$R/etc/default/console-setup"
sed -i 's/^\([^#].*\)/# \1/; s/^# *\(en_US.UTF-8 UTF-8\)/\1/' "$R/etc/locale.gen"
rm -f "$R/usr/lib/locale/locale-archive"
in_root locale-gen
rm -f "$R/etc/default/locale" "$R/etc/locale.conf" "$R/etc/timezone" \
	"$R/etc/localtime" "$R/etc/hostname" "$R/etc/hosts" "$R/etc/fstab"
echo LANG=en_US.UTF-8 > "$R/etc/default/locale"
echo LANG=en_US.UTF-8 > "$R/etc/locale.conf"
sed -i 's/^XKBLAYOUT=.*/XKBLAYOUT="us"/; s/^XKBVARIANT=.*/XKBVARIANT=""/; s/^XKBOPTIONS=.*/XKBOPTIONS=""/' \
	"$R/etc/default/keyboard"
ln -sf /usr/share/zoneinfo/Etc/UTC "$R/etc/localtime"
echo Etc/UTC > "$R/etc/timezone"
echo t510 > "$R/etc/hostname"
printf '127.0.0.1\tlocalhost\n127.0.1.1\tt510\n::1\tlocalhost ip6-localhost ip6-loopback\n' > "$R/etc/hosts"
# Android's VENDOR partition stays on the tablet: Wi-Fi firmware is read from
# /vendor/etc/wifi there, the audio DSP from /vendor/firmware (ro,noload:
# never writes, not even a journal replay)
cat > "$R/etc/fstab" <<'EOF'
# / is mounted by the initramfs
/dev/mmcblk0p3	/efs	ext4	ro,nofail	0 0
/dev/mmcblk0p26	/vendor	ext4	ro,noload,nofail	0 0
EOF
mkdir -p "$R/vendor" "$R/system"

# nothing of this tablet: every tablet gets its own
in_root passwd -l root
# "uninitialized" (not empty) makes the next boot a first boot: systemd sets
# ConditionFirstBoot, so sshd-keygen makes this tablet's SSH host keys
echo uninitialized > "$R/etc/machine-id"
rm -f "$R/var/lib/dbus/machine-id"
ln -s /etc/machine-id "$R/var/lib/dbus/machine-id"
# the snakeoil TLS key comes back per tablet (ssl-cert.service); cloud-init
# must run again for the installer's seed, and resize-rootfs must grow the
# file system to the new tablet's partition (the copy's mark said it was done:
# the image stayed at its own size and the disk was full at once); t510-update
# must bring the new install up to date (its mark is the copy's too)
rm -rf "$R/var/lib/cloud" "$R/var/lib/resize-rootfs.done" "$R/.boot-attempt" \
	"$R/var/lib/t510/updated"

in_root dpkg-query -W -f '${Package} ${Version}\n' > "$IMG.manifest"
cp "$R/bin/busybox" "$T/out/busybox"
in_root apt-get clean
rm -f "$R/etc/apt/apt.conf.d/99build-nopty"
rm -rf "$R"/var/lib/apt/lists/* "$R"/var/cache/debconf/*-old "$R"/tmp/* "$R"/var/tmp/* \
	"$R"/var/crash/* "$R"/var/backups/* "$R"/var/cache/apt/* "$R"/var/cache/man/* \
	"$R"/var/cache/fontconfig/* "$R"/var/lib/systemd/pstore/* \
	"$R"/var/lib/systemd/coredump/* "$R"/var/lib/systemd/random-seed \
	"$R"/var/lib/systemd/timesync/* "$R"/var/lib/plocate/plocate.db \
	"$R"/var/lib/PackageKit/transactions.db "$R"/var/lib/colord/*.db \
	"$R"/var/lib/geoclue/* "$R"/var/lib/upower/* \
	"$R"/etc/.resolv.conf.systemd-resolved.bak "$R/usr/sbin/policy-rc.d" \
	"$R"/var/lib/systemd/network/* "$R"/root/.cache
find "$R/var/log" -type f -exec truncate -s 0 {} +
find "$R/var/log" -type f \( -name '*.gz' -o -name '*.[0-9]' -o -name '*.old' \) -delete
rm -rf "$R"/var/log/journal/*
ln -sf ../run/systemd/resolve/stub-resolv.conf "$R/etc/resolv.conf"

# what still names a removed user: shown, so the next run can clean it too
LEFT=0
for u in $USERS; do
	if grep -rIlw --exclude-dir=dpkg -- "$u" "$R/etc" "$R/var/lib" 2>/dev/null; then
		LEFT=1
	fi
done
[ "$LEFT" = 0 ] || echo "warning: the files above still name a removed user"

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
