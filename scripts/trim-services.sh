#!/bin/sh
# Run on the tablet as root, or in an image build (chroot): turn off the desktop-Ubuntu services a Wi-Fi
# tablet with 2 GB of RAM has no use for. Idempotent; packages stay installed
# (undo with systemctl enable / unmask and by deleting the Hidden= files).
# Bluetooth (bluez, blueman), printing and scanning (CUPS, cups-browsed, avahi
# for network printers) are left alone.
set -u

# no modem, one GPU, no crash reporting, no snaps,
# no Ubuntu Pro / MOTD / update popups, no firmware updates (fwupd has
# nothing for this board), no daily plocate scan of the eMMC
SYSTEM="
ModemManager.service
switcheroo-control.service
apport.service apport-autoreport.path apport-autoreport.timer apport-forward.socket
whoopsie.path
ubuntu-advantage.service ua-reboot-cmds.service ua-timer.timer
motd-news.timer update-notifier-download.timer update-notifier-motd.timer
fwupd-refresh.timer plocate-updatedb.timer
snapd.service snapd.socket snapd.seeded.service snapd.apparmor.service
snapd.autoimport.service snapd.core-fixup.service
snapd.recovery-chooser-trigger.service snapd.system-shutdown.service
snapd.snap-repair.timer
"
# --now only on a running system: in an image build (chroot) systemctl
# refuses the whole command for it, and nothing was disabled
NOW=
[ -d /run/systemd/system ] && NOW=--now
for u in $SYSTEM; do
	systemctl disable $NOW "$u" 2>/dev/null
done
# D-Bus activated, so disabling is not enough
systemctl mask $NOW fwupd.service 2>/dev/null

# user units of every user: the VM agent, and Evolution's calendar/contacts
# daemons with GNOME Online Accounts (D-Bus activated, nothing here uses them)
systemctl --global disable spice-vdagent.service 2>/dev/null
systemctl --global mask evolution-source-registry.service \
	evolution-calendar-factory.service evolution-addressbook-factory.service \
	evolution-user-prompter.service gvfs-goa-volume-monitor.service 2>/dev/null

# XDG autostart entries: hidden per user, like the ayatana indicator
for home in /home/*; do
	user=$(basename "$home")
	id "$user" >/dev/null 2>&1 || continue
	dir=$home/.config/autostart
	install -d -o "$user" -g "$user" "$dir"
	for d in update-notifier org.gnome.Evolution-alarm-notify spice-vdagent \
		geoclue-demo-agent; do
		[ -e "$dir/$d.desktop" ] && continue
		printf '[Desktop Entry]\nType=Application\nName=%s\nHidden=true\n' "$d" > "$dir/$d.desktop"
		chown "$user:$user" "$dir/$d.desktop"
	done
done
