# Changes

The versions before publication were put together afterwards from the stages
of development. Every kernel build is listed under its stage, the ones that
did not boot or hung included, so the way there shows; the build number is
the kernel build counter (`uname -v`, `#N`). It counts every compile: a few
builds left no record of what they changed and are marked so.

Versions are x.y.z: x a very large update (a new Ubuntu base, a reinstall),
y a large feature update, z security and bug fixes
([docs/maintenance.md](docs/maintenance.md)).

## 0.8.1 — a dot under the finger instead of the pointer (build #67)

- The pointer is hidden: Chrome hid it on every touch and showed it again
  on the motion that followed, so it blinked. `longpress` hides it for the
  whole screen (XFixes) while the session runs.
- A small round dot marks the finger instead: a solid rim, inside it dark
  in the middle and light at the rim. It shows on the touch, follows the
  finger and goes a moment after the lift; touches pass through it.

## 0.8.0 — sleep (build #67)

- The tablet sleeps: a short press of the power key locks it, turns the
  screen off and suspends it (s2idle: the processes and the drivers sleep,
  the cores wait in their deepest idle state); the power key wakes it and the
  screen comes back on with the lock screen. Sound, Wi-Fi, touch and USB
  come back as they were.
- Kernel: `CONFIG_SUSPEND` and the suspend debugging switches
  (`/sys/power/pm_test`). The firmware's PSCI 0.1 has no system suspend, so
  `mem` is s2idle too; a deeper sleep (the cluster and the SoC powered down
  through the PMU) is still to come.
- polkit lets the tablet's own sessions suspend while locked (light-locker
  has then switched to the greeter, and the session is no longer active).
- The first lock after logging in: the lock screen no longer comes up black
  (started as the lock screen, the greeter turned the X screensaver on until
  a touch, `tools/noblank.c`), and the screen stays dark while it starts
  (its X server turned the backlight on: a lit black screen with a cursor).

- No new image: everything in this version is in the packages, and a new
  install starts from 0.7.4's image and takes 0.8.0 the first time it is
  online (the new kernel after the next restart).

Builds: #67.

## 0.7.5 — no sleep attempt when idle (build #66)

- After 14 minutes without a touch the power manager tried to suspend, which
  the kernel cannot do yet: while the tablet was locked it waited for the
  password, and after unlocking it showed "Sleep verb 'suspend' is not
  configured or not supported by kernel". Our setting meant "never" to an
  older Xfce; 4.20 reads it as 14 minutes. The screen still turns off after
  5 minutes on battery, 10 on mains.

## 0.7.4 — t510-update waits for the network (build #66)

- Without a network `apt-get update` only warns and succeeds: `t510-update`
  then upgraded nothing, set its mark all the same and never ran again, so
  a new install offline at its first boot stayed on the image's packages.
  It now fails until every source answers and tries again.
- A new image, the first with the apt repository (0.7.0's has none).

## 0.7.3 — first boot: one apt at a time (build #66)

- `t510-update` and `t510-chrome` both start when a new install first goes
  online, and `apt-get update` does not wait for another one's lock: the
  second failed at once and both tried again every 2 minutes. They now take
  turns (`flock /run/lock/t510-apt`).

## 0.7.2 — new installs bring themselves up to date (build #66)

- `t510-update.service`: the first time a new install is online it takes the
  newest `t510-support` and `t510-kernel` from the apt repository, once. An
  image is now made for large versions only, fixes reach new installs too.
- A new image would have come here (0.7.0's has no apt repository yet); the
  test installs found the first-boot problems of 0.7.3 and 0.7.4 first, so
  the image is 0.7.4's.

## 0.7.1 — apt repository (build #66)

- Updates come with apt: the packages `t510-support` (our services, udev
  rules, settings and programs) and `t510-kernel` (the boot image; its
  update writes BOOT and keeps the previous image in `/var/lib/t510`) from
  our repository at chelovechishee.github.io/sm-t510-ubuntu-apt, signed with
  the key in `t510-archive-keyring`. Ubuntu's kernel packages are pinned off.
  Versions and the way an update is made: `docs/maintenance.md`.
- Our files moved out of `/usr/local`: programs in `/usr/bin`, `/usr/sbin` and
  `/usr/libexec/t510`, units, udev rules and system settings in `/usr/lib`;
  `/etc` keeps what is there for the owner to change. On a 0.7.0 tablet the
  first install of `t510-support` removes the old copies (a changed one is
  kept as `*.t510-0.7.0`), and each user's panel (temperature monitor,
  rotation and keyboard buttons) is pointed at the new places at the next
  login (`/usr/libexec/t510/migrate-user`).
- The image builders install these packages instead of copying `overlay/` and
  compiling `tools/` in the image; files that change on the tablet itself
  (rotation, keyboard, hardware name) are in `image/`.

## 0.7.0 — publication (build #66)

- The published image is made from a working tablet's system
  (`scripts/image-from-system.sh`): everything set up there stays, every
  account, key, network, printer and log goes, and the packages of
  `scripts/image-remove.txt` (games, development tools, GNOME, Chrome,
  Russian).
- The audio DSP firmware is no longer copied: `/lib/firmware/calliope_*.bin`
  are links to the stock files on the VENDOR partition.
- Holding the power key takes a screenshot (Pictures/Screenshots and the
  clipboard), holding it 3 s opens the power dialog; a long press reopens the
  menu the lifted finger closed.
- Xfce defaults: the session, settings and desktop channels are complete
  files (a channel in `/etc/xdg/xdg-xfce` hides the stock one: the login ended
  in "Unable to load a failsafe session", the wallpaper was tiled); GTK4
  draws with cairo in the image too.
- The root file system grows to the whole partition again on the first boot:
  resize2fs opens the mounted device for writing, which the kernel refuses
  (`CONFIG_BLK_DEV_WRITE_MOUNTED` off); `t510-resize-rootfs` uses ext4's
  resize ioctl instead. The units the image has off stay off on the first
  boot (a systemd preset), and the panel's keyboard and rotation buttons are
  there from the start.
- The left rotation button turns the screen the way the tablet is held
  (`t510-rotate auto`) instead of always to portrait.
- No closed files in the repository: Wi-Fi firmware is read from the tablet's
  VENDOR partition (`/vendor`, read-only).
- No Samsung firmware in the kernel any more (build #66): ABOX and ACPM defer
  their probe while the files are missing from `/lib/firmware` (ACPM before it
  requests its mailbox channels); `t510-firmware.service` re-probes them once
  VENDOR is mounted, the ACPM plugin comes from the installer or from
  `t510-firmware-fetch.service` (Samsung's GPL kernel tree). The kernel and
  `boot.img` can now be distributed.
- The kernel version string no longer names the build machine
  (`build@sm-t510-ubuntu`); `mainline/export-patches.sh` exports the commits of
  the kernel tree to `patches/`.
- `mainline/build-kernel.sh`: the kernel from scratch out of the Linux 7.3
  tarball and the patches; `make-boot.sh` fetches minimal_sboot_wrapper itself.
- `scripts/build-image.sh` replaces `build-rootfs.sh`: an Ubuntu 26.04 image
  from a snapshot of the archive at one date, Xubuntu minimal, in English, with
  no user, keys or firmware; Xfce defaults for the tablet
  (`/etc/xdg/xdg-xfce`); `fbtool`, `longpress` and `t510-nograb.so` are built
  with the image. `scripts/seed-image.sh` writes the user, Wi-Fi, SSH key and
  time zone into the image for cloud-init. The browser is Google Chrome from
  Google's repository the first time the tablet is online; snaps are blocked.
- The documentation is in English.
- `install.sh`: one script from the PC — your account into the image, one
  short Download-mode session for VBMETA and an installer BOOT, then the
  system and the normal BOOT over the USB network, checked by md5 on the
  tablet. The rescue shell of the initramfs works again (busybox has no
  `mountpoint`), and `scripts/tn.py` returns the remote exit status.
- `install.sh` downloads the system image and the boot images of the release
  from GitHub and checks them against `SHA256SUMS` when there is no build of
  your own (`scripts/make-release.sh` prepares those files).

Planned: Bluetooth, the microphone and the cameras; packages and an apt
repository for updates; a first-boot wizard on the tablet (for an image
flashed without settings).

Builds:
- #65 — the first kernel without any Samsung file built in: the audio DSP
  started from `/lib/firmware`, but ACPM oopsed (a mailbox channel freed on
  deferral came back without its poll timer), so no CPU/GPU frequency
  control, and the reboot hung on the lock the crashed probe still held.
- #66 — ACPM checks for its plugin before it asks for the mailbox channels:
  sound, cpufreq and the GPU all work with the firmware from the tablet.

## 0.6.0 — charging and the desktop (build #64)

- The S2MU005 charger driver in the kernel sets the stock currents for the
  adapter type (BC1.2) instead of a userspace service.
- FUSE and desktop file systems, network printing (CUPS, driverless IPP).
- CPU and GPU temperatures on the panel.
- The long-press right click fires when the hold time is up; desktop icons
  open with one tap.
- Found the shader derivative bug of the Mali-G71; GTK4 draws with cairo.

Builds:
- #63 — the I2C bus of the S2MU005 charger switched on (the charge currents
  were still the bootloader's 500/450 mA, set from userspace for now); FUSE,
  overlay, squashfs and the desktop file systems (FAT, exFAT, NTFS, ISO, UDF,
  CIFS), uinput.
- #64 — the kernel's `s2m-charger` sets the stock currents from the adapter
  type itself; the userspace service is gone.

## 0.5.0 — power saving (build #62)

- cpuidle C2 on all 8 cores: registers around PSCI CPU_SUSPEND, the automatic
  cluster power-down armed by S-Boot switched off, the tick from the MCT.
- The display frame interrupt is masked while idle.
- MGLRU, zram with zstd, dbus/PipeWire/Xorg protected from the OOM killer,
  unneeded services off.

Builds:
- #59 — the display frame interrupt masked while nothing waits for it (60
  interrupts a second on an idle desktop before); vblank still gives 60 fps.
- #60 — first cpuidle C2 on all cores: the tablet panicked when the firmware
  refused a core's sleep (PSCI DENIED) and returned with registers clobbered.
- #61 — x19–x28 saved around the firmware call: still crashed, on cpu1 too.
- #62 — C2 works on all 8 cores: the clusters stay powered (S-Boot had armed
  a cluster power-down that lost L2 and corrupted memory), the tick moves to
  the MCT local timers.

## 0.4.0 — graphics (build #58)

- Own KMS driver `exynos7885-decon` on top of the bootloader's buffer, vblank,
  60 fps.
- Mali-G71 on upstream Panfrost with Ubuntu's Mesa, X on modesetting + glamor.
- GPU DVFS through ACPM (343–845 MHz) and throttling on the G3D sensor.

Builds:
- #56 — own KMS driver on the DECON setup the bootloader leaves running, Mali-G71
  on upstream Panfrost, X on modesetting + glamor; the first try had X fail in
  landscape (the mode was limited to 1200 px wide), fixed in this build.
- #57 — a GPU cooling device for the thermal zone.
- #58 — GPU DVFS over the ACPM G3D clock, 343–845 MHz, throttling from 75 °C.

## 0.3.0 — sound (build #55)

- ABOX ported from Samsung's kernel to ASoC in 7.3, CMU_DISPAUD clocks, SysMMU.
- TFA9896 amplifiers in `tfa989x`, the tablet's sound card, PipeWire, volume
  keys.

Builds:
- #36 — CMU_DISPAUD audio clocks (modelled on the Exynos 850's CMU_AUD) and
  the start of the ABOX port.
- #37 — no record.
- #38 — the ported ABOX driver builds on the 7.3 ASoC API.
- #39 — ABOX switched on for the first time: the tablet hung while the DSP
  started.
- #40 — a start-up race fixed; the DSP core ran its firmware and hung the
  tablet a little later.
- #41 — more diagnostics: the DSP's mapping of its firmware was right, the
  SysMMU was the problem; hung again.
- #42 — the DSP goes through the second of the SysMMU's two page tables:
  programmed it; hung again, the log was cut short.
- #43 — the same with the copies of the firmware done as iomem; not tested
  that night.
- #44 — no record.
- #45 — TUN, WireGuard, policy routing and nftables for VPNs.
- #46 — a debug kernel (netconsole, lockdep) to test ABOX safely.
- #47, #48 — a fix for the next hang found on #47, built into #48.
- #49 — the DSP crashed during the mixer setup: one of the ERAP controls.
- #50 — the DSP's own controls kept in a cache while it is off, restored after.
- #51 — hung while booting: every access restarted the already running DSP.
- #52 — the DSP stays on and is not restarted.
- #53 — sound plays through PipeWire without dropouts, volume keys work.
- #54 — the release (non-debug) kernel; some DSP components did not show up.
- #55 — DSP component registration logged; the release kernel with sound.

## 0.2.0 — Wi-Fi and the session

- Wi-Fi: Samsung SCSC (Maxwell core + `scsc_wlan`) ported to the 7.3 kernel.
- CPU DVFS through the fvp plugin of the ACPM firmware, temperature sensors.
- Panel backlight over DCS, LIS2DS12 accelerometer and screen rotation.
- Password login with the on-screen keyboard, screen lock, VPN.

Builds:
- #16 — no record.
- #17 — panel backlight over DSI commands (the brightness slider works), the
  framebuffer console takes over later, so the boot is quiet.
- #18 — CPU capacities for the scheduler (big and little cores told apart);
  the touchscreen keeps resting fingers.
- #19 — no record.
- #20 — the first CPU frequency request through ACPM crashed the APM
  firmware.
- #21 — cpufreq works: the kernel is loaded at 0xA0000000 so that it no
  longer overwrites the ECT table S-Boot leaves at 0x90000000; read-only
  temperature sensors.
- #22–#24 — the Wi-Fi/BT core (Samsung SCSC Maxwell) added: the kernel hung
  before the console even with the probe off.
- #25 — the cause: 7.3 changed how reserved memory is declared; the Wi-Fi/BT
  firmware starts.
- #26 — no record.
- #27 — the `scsc_wlan` driver: `wlan0` appears, NetworkManager brings it up.
- #28 — no record; starting Wi-Fi hung the whole tablet.
- #29 — a debug kernel with lockdep and netconsole to catch the hang.
- #30 — no record.
- #31 — the hang was NetworkManager waiting on a Wi-Fi block left without
  power: its LDO regulators are switched on now.
- #32, #33 — Wi-Fi connects to the network.
- #34 — the touchscreen releases all contacts when its event queue overflows,
  battery information for the fuel gauge.
- #35 — the LIS2DS12 accelerometer on its I2C bus: rotation follows the
  sensor.

## 0.1.0 — the mainline kernel

- Linux 7.3 boots from S-Boot through minimal_sboot_wrapper: display
  (simplefb), 8 cores, eMMC, keys.
- USB gadget (RNDIS network and console), own touchscreen driver, charge level.
- Own SPEEDY bus driver, S2MPU08 PMIC (RTC, regulators), microSD.
- Ubuntu 26.04.1 LTS.

Builds:
- #1 — the first mainline kernel on the SM-T510: 8 cores, all 2 GB, eMMC in
  HS200 with all 33 partitions, a console on the bootloader's framebuffer.
- #2 — the initramfs reads the Ubuntu partition (read-only).
- #3–#5 — USB device mode: the port stayed in the "default" state, no
  network for the PC.
- #6 — USB works (the 7885 variant of the USB PHY, its 50 MHz reference from
  the USB PLL): Ubuntu 24.04 boots on 7.3, SSH over the cable.
- #7 — no record.
- #8 — Xorg on the framebuffer.
- #9 — the desktop starts and stays up.
- #10 — own touchscreen driver (STM FTS1BA90A), battery level; no failed
  services left.
- #11–#15 — own SPEEDY bus driver to the S2MPU08 PMIC, its clock and
  regulators, the microSD slot; power-off through PS_HOLD. After these the
  system was upgraded to Ubuntu 26.04.1 (systemd 259 needs a kernel newer
  than 4.4).

## 0.0.0 — Samsung's 4.4 kernel

Before mainline the tablet ran Ubuntu 24.04 on Samsung's own 4.4 kernel
(its build counter starts again; #1 is the stock kernel). None of it is in
this repository: this kernel could not run Ubuntu 26.04 and is not
maintained.

Builds:
- #2 — the first own build: the initramfs boots Ubuntu from the data
  partition instead of Android.
- #3 — no record.
- #4, #5 — a colour framebuffer console, 100×87 characters.
- #6 — the screen comes back after it was blanked.
- #7, #8 — the colours fixed (the framebuffer is XRGB, not ABGR).
- #9, #10 — no record.
- #11 — DRM in the kernel.
- #12 — own KMS driver on top of Samsung's display driver; the desktop then
  ran on the Mali-G71 through a shim between Mesa and Samsung's GPU driver.
