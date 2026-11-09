# Workarounds in userspace (package `t510-support`: `overlay/` and `tools/`)

| Problem | Solution |
|---|---|
| desktop on the GPU | `t510-xorg` (LightDM `xserver-command`): modesetting + glamor on the Mali-G71 (Panfrost, `/etc/X11/t510/kms.conf`), fbdev without KMS (`fbdev.conf`); `/etc/X11/t510/force-fbdev` forces fbdev |
| journald loops on charger uevents | `ReadKMsg=no` |
| SCSC Wi-Fi looks for its firmware only when `/system` exists | an empty `/system`; the firmware is `/vendor/etc/wifi` on the Android VENDOR partition (p26), mounted read-only |
| the PC must know the tablet's USB network again on every boot | RNDIS with fixed MAC addresses (`usb-gadget.sh`); `install.sh` and the rescue mode use the same |
| a PC without a system needs an installer, and there is no flash drive | `t510-usb-disk`: the tablet shows an ISO or a disk image to the PC as a USB disk (a second gadget in place of the network one); `t510-powerkey` does not suspend meanwhile |
| mousetweaks is broken | our `longpress` (right click by holding a finger) |
| a double tap on a desktop icon does nothing: GTK3 makes no double click out of touches (XI2 touch), and two taps fit in 0.15 s and 26 px | icons open with one tap (`xfdesktop` single-click); `longpress` sends the right click when the hold time is up, while the finger is still down (it used to come on release, after the left click, and the icon had already opened); xsettings drag threshold `/Net/DndDragThreshold` 32 |
| Onboard crashes in its XInput handler (Python 3.12) | `input-event-source='GTK'` |
| screen rotation | `t510-rotate` writes `kms.conf` (RandR `Rotate`) and `fbdev.conf`, the touch matrix and the order of the audio channels, and restarts LightDM; `t510-rotate refresh` rewrites them without a restart |
| power key | `t510-powerkey` (lock, screen off, suspend once the greeter is up; the press that wakes it only turns the screen on), logind ignores it; polkit lets the locked seat session suspend (`50-t510-suspend.rules`) |
| ABOX and ACPM ask for their firmware before the root file system is mounted, and it may not be built into the kernel (Samsung's code) | the drivers defer their probe; `/lib/firmware/calliope_*.bin` are links to the stock files in `/vendor/firmware`; after `/vendor` is mounted `t510-firmware.service` re-probes the drivers; the ACPM plugin comes from the installer or from `t510-firmware-fetch.service` |
| WirePlumber tries every ABOX PCM → the DSP firmware crashes | no automatic card setup (`wireplumber.conf.d/51-t510.conf`), PipeWire gets a fixed sink and source |
| audio routing (speakers/headphones/microphones) | `t510-audio.service`: setup from `mixer_paths.xml` + switching on the jack |
| the keyring password window (gcr-prompter) grabs input → Onboard cannot be pressed | `/usr/libexec/t510/gcr-prompter` runs it with `LD_PRELOAD=nograb.so` (`tools/nograb.c`); its D-Bus services replace gcr's (`dpkg-divert`) |
| Ubuntu's login message is about a server (help links, "minimized", ESM) and warns about the Android partitions' fsck; sshd adds its own "Last login" | `t510-greeting` from `/etc/profile.d` (xfce4-terminal starts login shells: `xfce4-terminal.xml`); Ubuntu's `/etc/update-motd.d` scripts are diverted to `*.distrib` (run-parts skips them), `PrintLastLog no` |
| the greeter started as the lock screen forces the X screensaver on → the lock screen comes up black until a touch | `t510-gtk-greeter.desktop` runs it with `LD_PRELOAD=noblank.so` (`tools/noblank.c`), which drops only that forced activation |
| 2 GB of RAM | 2 GB of zstd swap in zram (`zram-manual.service`; before kernel #59 zstd was not built for zram and lzo-rle was used silently), MGLRU, `vm.swappiness=100`, `page-cluster=0` (`60-t510-zram.conf`) |
| when memory ran out the OOM killer took dbus/pipewire, not the game: user services have `oom_score_adj` 200, applications 0, and GPU buffers hardly show in RSS | `user@.service` and dbus/pipewire/wireplumber at −100 (`user@.service.d/50-t510-oom.conf`, `/usr/lib/systemd/user/*.service.d/50-t510-oom.conf`), Xorg at −100 (`t510-xorg`) |
| desktop Ubuntu services of no use here (modem, snapd, apport, fwupd…) | `scripts/trim-services.sh` turns them off (the packages stay), journal ≤ 100 MB; printing and scanning (CUPS, cups-browsed, avahi) stay: a network printer is found and added by itself (driverless IPP), scanners through sane-airscan (eSCL) |
| the first boot (machine-id `uninitialized`) applies systemd's presets, and Ubuntu's enable everything | the image builds write `/etc/systemd/system-preset/80-t510-image.preset`: every unit off in the image stays off |
| fixes come from the apt repository, an image only with large versions: a new install would start with the image's packages | `t510-update.service`: the first time online, the newest `t510-support` and `t510-kernel` from the repository, once (`/var/lib/t510/updated`) |
| `resize2fs` on the mounted root: "Device or resource busy" (the kernel has `CONFIG_BLK_DEV_WRITE_MOUNTED` off) | `t510-resize-rootfs` grows `/` with ext4's resize ioctl (`resize-rootfs.service`, first boot) |
| panel launchers given as bare names of `NoDisplay` entries are dropped (xfce4-panel looks them up among visible menu items) | `default.xml` names our launchers by full path |
| left/right speakers depend on the rotation | `t510-rotate` rewrites the channel order in `t510-audio.conf` |
| Android partitions show up on the desktop | `80-t510-udisks.rules`: `UDISKS_IGNORE` for `mmcblk0*` (the microSD card is shown) |

## Sound

Working routes (tested):

- speakers: `RDMA0 (hw:0,0) → SPUS OUT0 → SIFS1 → UAIF3 → TFA9896 ×2`;
- headphones: `RDMA0 → SPUS OUT0 → SIFS0 → UAIF0 → COD3035 HP` (on the jack sensor, which the mainline codec driver does not have yet; untested, no headphones);
- microphone: `MIC1 → COD3035 ADC → UAIF0 → NSRC0 → WDMA1 (hw:0,1)`, MIC1 boost 2 (20 dB) + ADC 150 (+12 dB, its top): speech peaks around −10 dBFS. The PipeWire source `t510-input` starts at volume 1.26 (×2, set once per user by `migrate-user`) and has `priority.session` 1001: WirePlumber counts the speakers' monitor as a source too and took it as the default on a tie.

Android's tables (`/vendor/etc/mixer_paths.xml`) are turned into `amixer`
commands by `tools/mixer_paths2amixer.py`. RDMA7 (as in Android's
`route-spkamp-playback` path) does not move — play through RDMA0.

Keyboard layout `us` (`00-keyboard.conf`); more layouts in Settings › Keyboard.

The top panel shows the CPU and GPU temperatures: the Generic Monitor plugin
(`xfce4-genmon-plugin`, `plugin-40` before the tray, command
`/usr/bin/t510-temps`, every 3 s; orange from 75 °C, red from 90 °C).
The ibus layout icon is hidden from the tray (`90_t510.gschema.override`);
xkb switches the layouts.
