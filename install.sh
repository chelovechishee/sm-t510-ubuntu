#!/bin/bash
# Install Ubuntu on a Samsung Galaxy Tab A 10.1 (2019, SM-T510) from a Linux PC.
# Erases the tablet's USERDATA (everything on it) and writes BOOT and VBMETA;
# VENDOR, RECOVERY and the rest of Android stay, so the stock firmware can
# always be flashed back.
#
# How: one short Download-mode session writes VBMETA and an installer BOOT
# (Samsung's bootloader does not finish a multi-gigabyte USERDATA reliably:
# heimdall gives up waiting while it still writes, and the partition is left
# without a file system). The installer kernel then brings up a USB network,
# this script sends the system and the normal BOOT over it, checks both, and
# restarts the tablet.
#
# The system image and the boot images: your own build when there is one
# (out/, mainline/out/), otherwise the files of this version's GitHub release,
# downloaded to dl/release/ and checked against its SHA256SUMS.
#
# Needs: an unlocked bootloader (stock T510XXU5CWA1), a USB cable, and on the
# PC heimdall, zstd, curl, openssl, debugfs (e2fsprogs) and python3.
#
# Usage: ./install.sh          install (erases the tablet's data)
#        ./install.sh --logs   copy the logs of the installed system to the PC
#                              (out/logs/), changing nothing on the tablet
set -euo pipefail

T=$(cd "$(dirname "$0")" && pwd)
IMAGE=$T/out/sm-t510-ubuntu.img.zst
BOOT=$T/mainline/out/boot-mainline.img
INSTALLER=$T/mainline/out/boot-install.img
VBMETA=$T/vbmeta.img
W=$T/out/install
# the release these sources belong to (scripts/make-release.sh checks it
# against CHANGELOG.md)
REPO=chelovechishee/sm-t510-ubuntu
RELEASE=v0.7.4
REL=$T/dl/release
TABLET=172.16.42.1

usage() { sed -n '2,/^set /p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 1; }
[ $# -le 1 ] || usage
case ${1:-} in
'') MODE=install ;;
--logs) MODE=logs ;;
*) usage ;;
esac
# what to run when something fails: the same mode again (never a plain
# ./install.sh after --logs, that one erases the tablet)
AGAIN="Run ./install.sh${1:+ $1} again."

say() { printf '\n\033[1m%s\033[0m\n' "$*"; }
die() { printf '\n\033[1;31m%s\033[0m\n' "$*" >&2; exit 1; }
tn() { python3 "$T/scripts/tn.py" "$1" "${2:-30}"; }

# send FILE over USB to the tablet's partition DEV, then check: the md5 of what
# the tablet received (computed while writing), the exit status of its dd,
# and what was written (up to 256 MB) read back from the eMMC
send() {
	local file=$1 dev=$2 port=$3 sum n head got size
	n=$(stat -c %s "$file")
	size=$(tn "blockdev --getsize64 $dev") ||
		die "The tablet does not answer. $AGAIN"
	[ "$n" -le "$size" ] ||
		die "$(basename "$file") ($n bytes) does not fit into $dev ($size bytes)."
	sum=$(md5sum < "$file" | cut -d' ' -f1)
	[ "$n" -le 268435456 ] || n=268435456
	head=$(head -c "$n" "$file" | md5sum | cut -d' ' -f1)
	# a receiver left over from a run that failed still holds the port: stop
	# its whole session first. setsid: the receiver must outlive the telnet
	# session that starts it
	tn "[ -f /tmp/recv.pid ] && kill -- -\$(cat /tmp/recv.pid) 2>/dev/null; rm -f /tmp/recv.pid /tmp/sent /tmp/recv.md5 /tmp/dd.rc /tmp/fifo; mkfifo /tmp/fifo; setsid sh -c 'echo \$\$ > /tmp/recv.pid; md5sum < /tmp/fifo > /tmp/recv.md5 & nc -l -p $port | tee /tmp/fifo | dd of=$dev bs=4M 2>/dev/null; echo \$? > /tmp/dd.rc; wait; sync; touch /tmp/sent' >/dev/null 2>&1 &" ||
		die "The tablet does not answer. $AGAIN"
	sleep 1
	if ! dd if="$file" bs=4M status=progress 2>&1 > "/dev/tcp/$TABLET/$port" |
		tr '\r' '\n' | grep --line-buffered -o '[0-9]* bytes.*copied.*' |
		while read -r line; do printf '\r  %s   ' "$line"; done; then
		die "Sending to the tablet failed. $AGAIN"
	fi
	echo
	local i
	for i in $(seq 120); do
		tn '[ -e /tmp/sent ]' >/dev/null 2>&1 && break
		[ "$i" = 120 ] && die "The tablet did not finish writing. $AGAIN"
		sleep 1
	done
	printf '  checking... '
	[ "$(tn 'cat /tmp/dd.rc')" = 0 ] ||
		die "The tablet could not write the data. $AGAIN"
	got=$(tn 'cat /tmp/recv.md5' | cut -d' ' -f1)
	[ "$got" = "$sum" ] ||
		die "The data arrived damaged ($got, expected $sum). $AGAIN"
	# O_DIRECT: from the eMMC itself, not from the page cache dd just filled
	got=$(tn "dd if=$dev bs=4M iflag=direct,count_bytes count=$n 2>/dev/null | md5sum" 120 | cut -d' ' -f1)
	[ "$got" = "$head" ] ||
		die "What the tablet wrote does not read back the same. $AGAIN"
	echo ok
}

# boot the tablet into the installer BOOT: one short Download-mode session
# (VBMETA too when installing). A tablet already in the installer is used as is.
start_installer() {
	if tn '[ -e /install ]' >/dev/null 2>&1; then
		echo "  The installer is already running on the tablet."
		return
	fi
	cat <<-'EOF'
	  Put the tablet into Download mode:
	    1. Turn it off. If it hangs or shows an error screen, hold
	       Power + Volume Down for 10 seconds until the screen goes dark.
	    2. Right away hold BOTH volume keys, with the USB cable in this PC.
	    3. On the warning screen press Volume Up.
	EOF
	printf '\n  Waiting for the tablet'
	until heimdall detect >/dev/null 2>&1; do
		printf '.'
		sleep 2
	done
	printf ' found.\n\n'
	heimdall flash "$@" --BOOT "$INSTALLER" 2>&1 | tee "$W/heimdall.log" || true
	[ "$(grep -c 'upload successful' "$W/heimdall.log")" = $(($# / 2 + 1)) ] ||
		die "Flashing the installer failed. $AGAIN"
	rm -f "$W/heimdall.log"

	printf '\n  The tablet restarts into the installer. Waiting for it on USB'
	local i
	for i in $(seq 120); do
		tn '[ -e /install ]' >/dev/null 2>&1 && break
		[ "$i" = 120 ] && die "The tablet did not show up on USB. If its screen says
\"An error has occurred\", restart it into Download mode. $AGAIN"
		printf '.'
		sleep 2
	done
	printf ' found.\n'
}

# download FILES of the release into $REL (unless already there and intact)
# and check each against the release's SHA256SUMS
fetch_release() {
	# T510_RELEASE_URL: a mirror with the same files (or a local test server)
	local base=${T510_RELEASE_URL:-https://github.com/$REPO/releases/download/$RELEASE} f
	mkdir -p "$REL"
	curl -fsSL --retry 3 "$base/SHA256SUMS" -o "$REL/SHA256SUMS.part" ||
		die "Could not download $base/SHA256SUMS. $AGAIN"
	mv "$REL/SHA256SUMS.part" "$REL/SHA256SUMS"
	for f in "$@"; do
		if [ -f "$REL/$f" ] && (cd "$REL" && grep "  $f\$" SHA256SUMS |
			sha256sum -c --quiet --strict) >/dev/null 2>&1; then
			continue
		fi
		echo "  $f"
		curl -fL --retry 3 --progress-bar "$base/$f" -o "$REL/$f.part" ||
			die "Downloading $f failed. $AGAIN"
		mv "$REL/$f.part" "$REL/$f"
		(cd "$REL" && grep "  $f\$" SHA256SUMS | sha256sum -c --quiet --strict) ||
			die "$f does not match the checksum of release $RELEASE. $AGAIN"
	done
}

restart_tablet() {
	tn "sync; setsid sh -c 'sleep 1; reboot -f' >/dev/null 2>&1 &" >/dev/null || true
}

# an image must be newer than the sources it is built from, or the tablet gets
# what the scripts no longer make (an image older than its overlay booted into
# a cloud-init that never finished, without SSH host keys)
stale() {
	local img=$1 newer
	shift
	newer=$(find "$@" -type f -newer "$img" -print -quit 2>/dev/null)
	[ -z "$newer" ] || die "$img is older than ${newer#"$T"/}.
Build it again (README.md, \"Building\")."
}

missing=
for c in heimdall zstd curl openssl debugfs e2fsck python3 md5sum sha256sum ip; do
	command -v "$c" >/dev/null || missing="$missing $c"
done
[ -z "$missing" ] || die "Missing on this PC:$missing
On Ubuntu: sudo apt install heimdall-flash zstd curl openssl e2fsprogs python3 iproute2"
[ -f "$VBMETA" ] || die "Missing $VBMETA: this copy of the repository is incomplete."
# own builds are used and must be up to date; what is not built here comes
# from the release
download=()
own() { [ -f "$1" ] || { download+=("$(basename "$1")"); return 1; }; }
if own "$BOOT"; then stale "$BOOT" "$T/initramfs"; else BOOT=$REL/boot-mainline.img; fi
if own "$INSTALLER"; then stale "$INSTALLER" "$T/initramfs"; else INSTALLER=$REL/boot-install.img; fi
OWN_IMAGE=1
if [ "$MODE" = install ] && ! own "$IMAGE"; then
	IMAGE=$REL/sm-t510-ubuntu.img.zst
	OWN_IMAGE=0
fi
if [ ${#download[@]} -gt 0 ]; then
	say "Downloading release $RELEASE from github.com/$REPO"
	fetch_release "${download[@]}"
fi
mkdir -p "$W"

if [ "$MODE" = logs ]; then
	say "1/3  Starting the installer on the tablet (your data stays as it is)"
	start_installer
	say "2/3  Copying the logs"
	# read-only and without replaying the journal: nothing on the tablet changes
	tn 'mkdir -p /mnt; mount -o ro,noload /dev/mmcblk0p33 /mnt' ||
		die "The system partition does not mount. Is Ubuntu installed?"
	LOGS=$T/out/logs/$(date +%Y%m%d-%H%M%S)
	mkdir -p "$LOGS"
	pc=$(ip -4 route get "$TABLET" | sed -n 's/.* src \([0-9.]*\).*/\1/p')
	# the PC listens, the tablet sends: busybox nc closes at the end of the data
	python3 - "$LOGS/var-log.tar" 5559 <<-'EOF' &
	import socket, sys
	s = socket.socket()
	s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
	s.bind(("", int(sys.argv[2])))
	s.listen(1)
	s.settimeout(120)
	c, _ = s.accept()
	with open(sys.argv[1], "wb") as f:
	    while d := c.recv(65536):
	        f.write(d)
	EOF
	listener=$!
	sleep 1
	tn "tar -C /mnt -cf - var/log etc/fstab etc/machine-id | nc $pc 5559" 300 ||
		{ kill "$listener" 2>/dev/null; die "Copying the logs failed. $AGAIN"; }
	wait "$listener" ||
		die "The PC got no logs (a firewall on port 5559?). $AGAIN"
	tn 'umount /mnt' >/dev/null || true
	tar -tf "$LOGS/var-log.tar" >/dev/null || die "The copied logs are damaged."
	echo "  $LOGS/var-log.tar ($(du -h "$LOGS/var-log.tar" | cut -f1))"
	say "3/3  Putting the normal boot partition back"
	send "$BOOT" /dev/mmcblk0p15 5556
	restart_tablet
	echo
	echo "  Done. The tablet restarts into Ubuntu."
	exit 0
fi

# the image: what build-image.sh (or the release) says it is, built from
# the sources here, and room to unpack it
# (a downloaded one was checked by fetch_release)
if [ "$OWN_IMAGE" = 1 ]; then
	if [ -f "${IMAGE%.zst}.sha256" ]; then
		(cd "$(dirname "$IMAGE")" &&
			grep "  $(basename "$IMAGE")\$" "$(basename "${IMAGE%.zst}").sha256" |
			sha256sum -c --quiet --strict) ||
			die "$IMAGE does not match its checksum: build it again."
	fi
	stale "$IMAGE" "$T/overlay" "$T/image" "$T/packaging" \
		"$T/scripts/image-packages.txt" "$T/scripts/trim-services.sh" \
		"$T/tools/fbtool.c" "$T/tools/longpress.c" "$T/tools/nograb.c"
fi
size=$(zstd -lv "$IMAGE" 2>/dev/null | sed -n 's/.*Decompressed Size:.*(\([0-9]*\) B).*/\1/p')
free=$(df --output=avail -B1 "$W" | tail -n1)
[ -z "$size" ] || [ "$size" -lt "$free" ] ||
	die "Unpacking the image needs $((size >> 20)) MB in $W, there are $((free >> 20)) MB."

cat <<'EOF'

  Ubuntu for the Samsung Galaxy Tab A 10.1 (SM-T510)

  This ERASES EVERYTHING on the tablet's data partition: apps, files, the
  Android setup. Android's system and vendor partitions stay, so you can go
  back by flashing the stock firmware.
EOF
read -r -p $'\n  Type YES to continue: ' answer
[ "$answer" = YES ] || die "Nothing was changed."

say "1/5  Unpacking the system image"
# holds your password hash once seeded: removed however the script ends
trap 'rm -f "$W/userdata.img"' EXIT
zstd -d -q -f "$IMAGE" -o "$W/userdata.img"
# systemd counts the first start as a first boot only with this machine-id:
# then sshd-keygen makes the tablet's SSH host keys (an image with an empty
# one came up without SSH)
[ "$(debugfs -R 'cat /etc/machine-id' "$W/userdata.img" 2>/dev/null)" = uninitialized ] ||
	die "The image is not ready for a first boot (/etc/machine-id).
Build it again (README.md, \"Building\")."

say "2/5  Your account on the tablet"
"$T/scripts/seed-image.sh" "$W/userdata.img"

say "3/5  Starting the installer on the tablet"
start_installer --VBMETA "$VBMETA"

say "4/5  Writing the system (a few minutes)"
send "$W/userdata.img" /dev/mmcblk0p33 5555
rm -f "$W/userdata.img"
echo "  boot partition:"
send "$BOOT" /dev/mmcblk0p15 5556

say "5/5  Restarting the tablet"
restart_tablet

cat <<'EOF'

  Done. The tablet restarts into Ubuntu.

  The first start takes a little longer: the file system grows to the whole
  partition, your account is created and the audio firmware is read from
  the tablet itself. Log in with the password you chose. If you gave no Wi-Fi
  network, pick one with the network icon on the panel; Google Chrome then
  installs by itself.
EOF
