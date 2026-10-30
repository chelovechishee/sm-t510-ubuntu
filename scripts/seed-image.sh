#!/bin/bash
# Make a built image yours before flashing it: asks for a user name and
# password, Wi-Fi, an SSH key and the time zone, and writes them into the
# ext4 image as a cloud-init NoCloud seed, applied once on the first boot
# (then the seed is deleted and cloud-init switched off). Also puts the ACPM
# DVFS plugin into /usr/lib/firmware, so the GPU is there before the login screen.
# Works on the image file with debugfs: no mounting, no root.
#
# Usage: scripts/seed-image.sh [out/sm-t510-ubuntu.img]
set -euo pipefail

T=$(cd "$(dirname "$0")/.." && pwd)
IMG=${1:-$T/out/sm-t510-ubuntu.img}
[ -f "$IMG" ] || { echo "no image $IMG (scripts/build-image.sh makes it)"; exit 1; }

FVP=exynos7885_acpm_fvp.fw
FVP_URL=https://raw.githubusercontent.com/gta3xlwifi-dev/android_kernel_gta3xlwifi/00e4b9481434f9b0b644925a9bb2feaf3940219d/firmware/$FVP
FVP_SHA256=3793478b6df52fe760a89cbcfb258a9c489365ee2d13d9dae8df2f161abadb61

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

ask() { local v; read -r -p "$1${2:+ [$2]}: " v; echo "${v:-${2:-}}"; }

while :; do
	USERNAME=$(ask "User name")
	[[ $USERNAME =~ ^[a-z_][a-z0-9_-]*$ ]] && break
	echo "lower case letters, digits, - and _ only"
done
FULLNAME=$(ask "Full name" "$USERNAME")
while :; do
	read -r -s -p "Password: " PW1; echo
	read -r -s -p "Password again: " PW2; echo
	[ -n "$PW1" ] && [ "$PW1" = "$PW2" ] && break
	echo "the passwords differ or are empty"
done
HASH=$(printf '%s' "$PW1" | openssl passwd -6 -stdin)
unset PW1 PW2
HOSTNAME=$(ask "Host name" t510)
TIMEZONE=$(ask "Time zone" "$(cat /etc/timezone 2>/dev/null || echo Etc/UTC)")
# the usual key, else the newest public key in ~/.ssh
KEYFILE=
for k in "$HOME"/.ssh/id_ed25519.pub "$HOME"/.ssh/id_rsa.pub \
	$(ls -t "$HOME"/.ssh/*.pub 2>/dev/null); do
	[ -f "$k" ] && { KEYFILE=$k; break; }
done
while :; do
	KEYFILE=$(ask "SSH public key file (empty: no SSH login)" "$KEYFILE")
	[ -z "$KEYFILE" ] || [ -f "$KEYFILE" ] && break
	echo "  no such file: $KEYFILE"
	KEYFILE=
done
SSID=$(ask "Wi-Fi network (empty: set it up on the tablet)")
PSK=
if [ -n "$SSID" ]; then
	read -r -s -p "Wi-Fi password: " PSK; echo
fi

# YAML double-quoted scalars
q() { local s=${1//\\/\\\\}; s=${s//\"/\\\"}; printf '"%s"' "$s"; }

{
	echo "#cloud-config"
	echo "hostname: $(q "$HOSTNAME")"
	echo "timezone: $(q "$TIMEZONE")"
	echo "users:"
	echo "  - name: $USERNAME"
	echo "    gecos: $(q "$FULLNAME")"
	echo "    groups: [adm, sudo, audio, video, plugdev, netdev, dialout, lpadmin]"
	echo "    shell: /bin/bash"
	echo "    lock_passwd: false"
	echo "    hashed_passwd: $(q "$HASH")"
	if [ -n "$KEYFILE" ]; then
		echo "    ssh_authorized_keys:"
		echo "      - $(q "$(head -n1 "$KEYFILE")")"
	fi
	echo "ssh_pwauth: false"
	if [ -n "$SSID" ]; then
		echo "write_files:"
		echo "  - path: /etc/NetworkManager/system-connections/wifi.nmconnection"
		echo "    permissions: '0600'"
		echo "    content: |"
		echo "      [connection]"
		echo "      id=$SSID"
		echo "      type=wifi"
		echo "      [wifi]"
		echo "      mode=infrastructure"
		echo "      ssid=$SSID"
		if [ -n "$PSK" ]; then
			echo "      [wifi-security]"
			echo "      key-mgmt=wpa-psk"
			echo "      psk=$PSK"
		fi
		echo "      [ipv4]"
		echo "      method=auto"
		echo "      [ipv6]"
		echo "      method=auto"
	fi
	echo "runcmd:"
	[ -n "$SSID" ] && echo "  - [nmcli, connection, reload]"
	# the seed holds the password hash and the Wi-Fi key: gone after use
	echo "  - [rm, -rf, /var/lib/cloud/seed/nocloud]"
	echo "  - [touch, /etc/cloud/cloud-init.disabled]"
} > "$W/user-data"
printf 'instance-id: t510-%s\nlocal-hostname: %s\n' "$(od -An -N8 -tx8 /dev/urandom | tr -d ' ')" "$HOSTNAME" > "$W/meta-data"

echo "fetching $FVP from Samsung's kernel tree"
curl -fsSL "$FVP_URL" -o "$W/$FVP"
echo "$FVP_SHA256  $W/$FVP" | sha256sum -c --quiet

S=/var/lib/cloud/seed/nocloud
{
	# debugfs mkdir over an existing name leaves an orphaned inode behind:
	# create only what is missing
	for d in /var/lib/cloud /var/lib/cloud/seed $S; do
		if ! debugfs -R "stat $d" "$IMG" 2>/dev/null | grep -q '^Inode:'; then
			echo "mkdir $d"
			echo "sif $d uid 0"
			echo "sif $d gid 0"
		fi
	done
	echo "sif $S mode 040700"
	for f in user-data meta-data; do
		echo "rm $S/$f"
		echo "write $W/$f $S/$f"
		echo "sif $S/$f uid 0"
		echo "sif $S/$f gid 0"
		echo "sif $S/$f mode 0100600"
	done
	echo "rm /usr/lib/firmware/$FVP"
	echo "write $W/$FVP /usr/lib/firmware/$FVP"
	echo "sif /usr/lib/firmware/$FVP uid 0"
	echo "sif /usr/lib/firmware/$FVP gid 0"
	echo "sif /usr/lib/firmware/$FVP mode 0100644"
} > "$W/cmds"
# debugfs reports "already exists" / "not found" for mkdir and rm: expected
debugfs -w -f "$W/cmds" "$IMG" >/dev/null 2>&1
e2fsck -fn "$IMG" >/dev/null 2>&1 || { echo "the image fails e2fsck"; exit 1; }
debugfs -R "ls -l $S" "$IMG" 2>/dev/null | grep -E 'user-data|meta-data'
echo "seeded $IMG for $USERNAME"
