# Ubuntu on the Samsung Galaxy Tab A 10.1 (2019, SM-T510)

Native Ubuntu 26.04.1 LTS (Xfce desktop) instead of Android: no chroot and no
container, but a kernel of its own — mainline Linux 7.3 with drivers for the
Exynos 7904 — and plain Ubuntu arm64 on the `userdata` partition.

Author: [chelovechishee](https://github.com/chelovechishee), co-author: Claude (Anthropic).

> **Warning.** This needs an unlocked bootloader, which wipes all data and
> irreversibly trips the Knox fuse. Everything was tested on one tablet only,
> with stock firmware **T510XXU5CWA1** (Android 11); with other bootloader
> versions something may not work. You do this at your own risk.

## Quick install

On a Linux PC, with the tablet's bootloader unlocked (step 1 of
[Installing](#installing)) and a USB cable:

```bash
sudo apt install git heimdall-flash zstd curl openssl e2fsprogs python3 iproute2
git clone https://github.com/chelovechishee/sm-t510-ubuntu
cd sm-t510-ubuntu
./install.sh
```

`install.sh` downloads the system image and the boot images of the current
release (about 1.1 GB, [Releases](https://github.com/chelovechishee/sm-t510-ubuntu/releases)),
checks them, asks for your account and Wi-Fi and walks you through the rest.
What changed between versions: [CHANGELOG.md](CHANGELOG.md).

## About this system

| | |
|---|---|
| Device model | Samsung Galaxy Tab A 10.1 (2019) Wi-Fi, SM-T510 |
| Processor | Samsung Exynos 7904: 2× Cortex-A73 up to 1.77 GHz + 6× Cortex-A53 up to 1.69 GHz |
| Graphics | ARM Mali-G71 MP2 (Panfrost), up to 845 MHz |
| Memory | 2 GB (+ 2 GB of swap in zram) |
| Disk capacity | 32 GB eMMC + microSD |
| Display | 10.1″, 1200×1920 |
| OS name | Ubuntu 26.04.1 LTS, 64-bit |
| Desktop | Xfce 4.20, X11 |
| Kernel | Linux 7.3 with the patches of this project |

## What works

| | |
|---|---|
| Display | KMS (own `exynos7885-decon` driver), 1200×1920, rotation from the accelerometer, backlight |
| GPU | Mali-G71 on upstream Panfrost + Ubuntu's Mesa, X on modesetting + glamor, DVFS 343–845 MHz |
| Touch | own STM FTS1BA90A driver, on-screen keyboard, right click by long press |
| Wi-Fi | Samsung SCSC (ported Samsung driver), 2.4 and 5 GHz, NetworkManager |
| Sound | speakers: ABOX + 2×TFA9896, PipeWire, volume keys |
| Power | CPU DVFS through ACPM, cpuidle (C2) on all 8 cores, temperatures and throttling |
| Battery | charge level (S2MU005), charging at stock currents by the kernel driver |
| Other | microSD, clock (RTC in the PMIC over the SPEEDY bus), USB network and console to a PC, network printing |

**Not working yet:** Bluetooth, headphones and microphone (COD3035 codec),
cameras, suspend, USB host/OTG. Shader derivatives are broken on the G71, so
GTK4 draws with cairo (`GSK_RENDERER=cairo`).

## Using the tablet

The top panel, left to right:

| | |
|---|---|
| **Applications** | the menu of all programs |
| open windows | tap one to bring it up |
| keyboard icon | shows or hides the on-screen keyboard (Onboard) |
| arrow to the left | turns the screen the way you hold the tablet |
| arrow to the right | turns the screen 90° clockwise |
| speaker | volume |
| battery | charge; tap it for screen brightness and the power settings |
| CPU / GPU | temperatures: orange from 75 ° (the chip starts to slow down), red from 90 ° |
| network | Wi-Fi networks |
| date and time | calendar |
| your name | lock, log out, restart, shut down |

Turning the screen restarts the desktop: save your work first, open windows
close. The dock at the bottom (it steps aside when a window covers it):
show the desktop, terminal, home folder, browser, search for programs,
folders.

The power key:

| | |
|---|---|
| short press | locks the tablet and turns the screen off; with the screen off, turns it on |
| hold 1–3 s, then release | screenshot: saved to Pictures/Screenshots and copied, ready to paste |
| hold 3 s | the power dialog (log out, restart, shut down) |

On the touch screen: hold a finger still for a moment for a right click;
desktop icons open with one tap.

## How it fits together

- **Boot.** S-Boot (Samsung's bootloader) starts not the kernel from the BOOT
  partition but [minimal_sboot_wrapper](https://github.com/VDavid003/minimal_sboot_wrapper),
  which hands over to the 7.3 kernel with its own DTB and command line. The
  initramfs (`initramfs/init`) mounts Ubuntu from `userdata`, or, if the
  previous boot failed, brings up a rescue shell over USB.
- **Kernel.** Linux 7.3 + `mainline/patches/` (the tablet's DT, USB, touch,
  SPEEDY/PMIC, ACPM, Wi-Fi, sound, display, cpuidle, charging). Details and the
  pitfalls found on the way: [mainline/README.md](mainline/README.md).
- **System.** Ubuntu arm64 + our package `t510-support` (`overlay/`: services,
  udev rules, workarounds; the programs of `tools/`); what each one is for:
  [docs/userspace.md](docs/userspace.md). The kernel is the package
  `t510-kernel`; both come from our apt repository.
- **Updates.** What x.y.z mean, how a fix or a kernel security update reaches
  installed tablets, the checks before publishing:
  [docs/maintenance.md](docs/maintenance.md).

## Closed firmware

There is not a single Samsung file in this repository. Everything needed comes
from the tablet itself or from what Samsung publishes:

| File | For | From |
|---|---|---|
| `mx140.bin`, `mx140*_wlan.hcf` | Wi-Fi | the Android VENDOR partition (p26) of the tablet; the install leaves it alone and the system mounts it read-only at `/vendor` |
| `calliope_sram.bin`, `calliope_dram.bin` | audio DSP | VENDOR (p26): `/lib/firmware` holds only links to `/vendor/firmware`, the tablet's own stock files |
| `exynos7885_acpm_fvp.fw` | DVFS (frequencies and voltages) | Samsung's GPL kernel tree [gta3xlwifi-dev/android_kernel_gta3xlwifi](https://github.com/gta3xlwifi-dev/android_kernel_gta3xlwifi): put in place by `scripts/seed-image.sh`, or by `t510-firmware --fetch` the first time the tablet is online |

None of it is built into the kernel: the drivers wait for the files in
`/lib/firmware` and bind as soon as they appear, so the kernel and `boot.img`
we build carry no Samsung code.

## Building

You need: Linux on a PC, `aarch64-linux-gnu-gcc`, `make`, `git`, `curl`,
`mkbootimg`, `heimdall`, `zstd`, `debugfs` (e2fsprogs), `dpkg-deb`, the arm64 X
libraries for `tools/` (`libx11-dev:arm64 libxi-dev:arm64 libxtst-dev:arm64`);
for the system image on x86 also `qemu-user` (arm64 through binfmt) and sudo.

```bash
mainline/build-kernel.sh              # Linux 7.3 + patches → Image and DTB (in src/linux-7.3)
mainline/make-boot.sh initramfs       # → mainline/out/boot-mainline.img
INSTALL=1 mainline/make-boot.sh initramfs   # → boot-install.img for install.sh
scripts/make-debs.sh                  # our packages → out/debs/ (t510-support, t510-kernel, t510-archive-keyring)
sudo scripts/image-from-system.sh rootfs.tar.zst   # system image from a set-up tablet → out/sm-t510-ubuntu.img (+ .zst)
sudo scripts/build-image.sh           # or: the system image from scratch
scripts/make-release.sh               # checks them all → out/release/ for a GitHub release
scripts/publish.sh                    # the packages → the apt repository (docs/maintenance.md)
```

`install.sh` uses these builds when they are there and downloads the release
otherwise; its `RELEASE=` line names the version, and `make-release.sh`
refuses to go on when it does not match the newest one in `CHANGELOG.md`.

The published system image is a working tablet's system, cleaned:
`scripts/image-from-system.sh` takes a copy of its root file system (the
command for it is at the top of the script) and removes every user account
and home, SSH host keys, the machine ID, Wi-Fi networks, VPNs, Bluetooth
pairings, printers, the Ubuntu Pro attachment, logs and caches, the packages
of `scripts/image-remove.txt` (games, development tools, GNOME, Chrome, the
Russian language) and the Samsung firmware; then our packages from
`out/debs/` are installed (`t510-support`: `overlay/` and the programs of
`tools/`), and the files of `image/` go on top. Everything set up and tested
on the tablet stays:
rotation, the power key, long press, the Xfce panel, Xubuntu's programs.

`scripts/build-image.sh` makes the image from scratch instead: Ubuntu 26.04
arm64 from a snapshot of the Ubuntu archive
taken at one date (`--snapshot`, pinned in the script by default): the same
packages on every build, their versions listed in
`out/sm-t510-ubuntu.img.manifest`. Its contents: `scripts/image-packages.txt`
(Xubuntu minimal and what the port needs), then our packages with the Xfce
defaults for the tablet (`overlay/etc/xdg/xdg-xfce`: panel, scaling, touch)
and the files of `image/`.
Language English, time zone UTC.

The image has no user, no keys, no Samsung firmware and no Google Chrome:
- the user, password, Wi-Fi, SSH key and time zone are written by
  `scripts/seed-image.sh` (cloud-init settings, applied on the first boot and
  deleted afterwards);
- SSH host keys and the machine ID are made on the first boot, so each tablet
  has its own;
- Chrome may not be redistributed: the image carries only Google's repository,
  and the browser is installed from dl.google.com the first time the tablet is
  online (`t510-chrome.service`), under Google's terms.

For your own tablet the Samsung firmware can go into the image right away
(`scripts/get-firmware.sh`, then `build-image.sh --with-firmware`); an image you
publish must not contain it.

## Installing

1. Flash stock T510XXU5CWA1, enable "OEM unlocking" in the developer options
   and unlock the bootloader.
2. On the PC, in a copy of this repository (your own builds are used when
   there are any, the release is downloaded otherwise):

   ```bash
   ./install.sh
   ```

   It asks for your user name, password, Wi-Fi and SSH key and tells you how
   to put the tablet into Download mode. One short session writes VBMETA
   (image verification off) and an installer BOOT; the tablet restarts into
   it, and the script sends the system and the normal BOOT over the USB
   network, checks both against their checksums and restarts the tablet.
   (Samsung's bootloader does not reliably finish a multi-gigabyte USERDATA:
   heimdall stops waiting while it is still writing, and the partition is left
   without a file system.) If anything fails, run `./install.sh` again.
   Before touching the tablet it checks the image: a downloaded one against
   the release's `SHA256SUMS`, an own build against its checksum and against
   `overlay/` and the build scripts (it must be newer), and that it is ready
   for a first boot; an image that fails is refused.

   Something wrong after installing? `./install.sh --logs` copies the logs of
   the installed system to `out/logs/` through the same installer, without
   changing anything on the tablet, and puts the normal BOOT back.
3. On the first boot the file system grows to the whole `userdata` partition,
   your account is created, the audio firmware is read from VENDOR and Chrome
   is installed from the network.

Keep the RECOVERY partition stock: with it you can always go back to Android by
flashing the complete stock firmware.

## Updates

Tablets installed from 0.7.1 on update like any Ubuntu: the update notifier
shows our packages next to Ubuntu's, or

```bash
sudo apt update && sudo apt upgrade
```

A new kernel is written to the BOOT partition by the update and starts with
the next restart. A tablet installed with 0.7.0 gets the repository once:

```bash
curl -fLo /tmp/t510-archive-keyring.deb https://chelovechishee.github.io/sm-t510-ubuntu-apt/t510-archive-keyring.deb
sudo apt install /tmp/t510-archive-keyring.deb
sudo apt update && sudo apt install t510-support t510-kernel
```

What the version numbers mean and how updates are made:
[docs/maintenance.md](docs/maintenance.md).

## Access and rescue mode

- USB cable: the tablet brings up an RNDIS network at `172.16.42.1` and gives
  the PC an address over DHCP; SSH with keys only. Console on `/dev/ttyACM0`
  (`scripts/acm.py`); on the PC install `configs/70-t510-console.rules` so that
  ModemManager leaves it alone.
- Wi-Fi: the network icon on the panel, or `wifi-add` on the console.
- If a boot does not reach `multi-user.target` (the `boot-ok` service did not
  remove the `/.boot-attempt` mark), the next one stops in the busybox shell of
  the initramfs: RNDIS + telnet without a password, the Ubuntu root at
  `/newroot`. To force it, create `/rescue` in the root. The kernel can be
  rewritten right from there: `dd` to `/dev/mmcblk0p15`.

## Directories

- `mainline/` — kernel patches, config (`configs/t510.fragment`),
  `build-kernel.sh`, `make-boot.sh`, `export-patches.sh`, the debug initramfs
  of bring-up stages 0/1.
- `initramfs/init` — boots Ubuntu, rescue mode.
- `overlay/` — the files of the package `t510-support` (services, udev rules,
  Xfce defaults); `packaging/` — the packages' control files and install
  scripts; `image/` — files only the system image gets (they change on the
  tablet: rotation, keyboard, hardware name);
  `scripts/` — packages, the apt repository, building and seeding the system
  image, firmware, helpers.
- `tools/` — our programs (`longpress.c`, `fbtool.c`, `nograb.c`) and
  development tools (SPEEDY, ACPM, DSI, ASoC); `tools/gpu/` — GLES tests.
- `gpu/` — the Mesa patch for derivatives on the G71 and a Mesa build to test it.

## Thanks

- Exynos 7885 support in mainline Linux (Galaxy A8 2018, `jackpotlte`) — the base of the kernel.
- [VDavid003/minimal_sboot_wrapper](https://github.com/VDavid003/minimal_sboot_wrapper) — running mainline from S-Boot.
- Samsung and [gta3xlwifi-dev](https://github.com/gta3xlwifi-dev/android_kernel_gta3xlwifi) — the GPL kernel code the Wi-Fi (SCSC) and audio (ABOX) drivers were ported from, and where many registers were looked up.
- Panfrost and Mesa — Mali graphics without closed drivers.

## Authors

- [chelovechishee](https://github.com/chelovechishee) — author.
- Claude (Anthropic), in Claude Code — co-author of the code, patches and
  documentation; named in the patches themselves (`Co-Authored-By`).

## License

© chelovechishee. GPL-2.0 ([LICENSE](LICENSE)). Kernel patches ported from Samsung's
kernel keep the authorship and copyright notices of the original.
