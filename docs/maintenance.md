# Updates and versions

How a fix, a new feature or a kernel security update gets from this
repository onto the tablets that already run the system.

The apt repository and the scripts `scripts/make-debs.sh` and
`scripts/publish.sh` named below came with 0.7.1; a tablet installed with
0.7.0 connects to the repository once (README.md, "Updates"). New installs
bring themselves up to date: `t510-update.service` takes the newest packages
the first time the tablet is online, so the last image stays good for every
version after it that apt can bring.

## Version numbers: x.y.z

| Part | What changes | How an installed tablet gets it |
|---|---|---|
| **x** — very large update | a new Ubuntu release as the base (26.04 → 28.04), a new partition layout, anything that does not upgrade in place | a new image, installed with `./install.sh` (erases the tablet's data); the release notes say so first |
| **y** — large feature update | a new piece of hardware working (Bluetooth, microphone, cameras), a new kernel line (7.3 → the next one), a larger change of the desktop | `sudo apt upgrade`; a new image only when apt cannot bring it (see below) |
| **z** — security and bug fixes | a kernel stable release with security fixes, a bug fix in our services, scripts, Xfce defaults or programs | `sudo apt upgrade` only |

- Raising one part sets the parts after it to 0: 0.7.3 → 0.8.0 → 1.0.0.
- Every published update gets its own version: two fixes published on
  different days are two z versions, two fixes published together are one.
- The version is the top `## x.y.z — title` heading of
  [CHANGELOG.md](../CHANGELOG.md), and only that heading. The scripts read
  it from there, so there is nothing else to change by hand. `install.sh`
  (`RELEASE=`) names the newest version that has an image: it moves only
  with a new image.
- A new image comes with every x, and with a y only when apt cannot bring
  the change: files of `image/` (they are in no package), a new partition
  layout, or when the first update of a new install has grown too long.
  Everything else reaches new installs through `t510-update.service`, and
  making an image (a clean system from the tablet, a reinstall to test it)
  is not worth it for that.
- The kernel build counter (`uname -v`, `#N`) goes on counting every compile
  as before. It is not part of the project version.

## Packages

| Package | Contents | Version |
|---|---|---|
| `t510-support` | `overlay/` (services, udev rules, Xorg, PipeWire, Xfce defaults, apt pins) and the programs of `tools/` (`longpress`, `fbtool`, `nograb.so`, `noblank.so`, `t510-camera`) | the project version, `0.7.1` |
| `t510-kernel` | the boot image (`boot-mainline.img`: kernel, DTB, initramfs); its install script writes it to BOOT (`mmcblk0p15`) | kernel version + build, `7.3.0+66` |
| `t510-archive-keyring` | the key the repository is signed with and its apt source | changes only with the key |
| `libcamera0.7`, `libcamera-ipa` | Ubuntu's libcamera with `camera/libcamera-0.7.0-t510.patch` (the CSIS in the simple pipeline, the sensors, software ISP fixes), built by `camera/build-libcamera.sh` | Ubuntu's version + `+t510.N`, `0.7.0-1ubuntu2+t510.6` |

The files of `image/` are in no package: they change on the tablet itself
(rotation, keyboard layout, hardware name), so a change to them reaches new
installs only. A fix that installed tablets need goes into `overlay/`.

The repository holds only these. Ubuntu's packages come from Ubuntu's
archive as on any Ubuntu system. Our own packages own no file of Ubuntu's, and
Ubuntu's kernel packages (`linux-image-*`, `linux-generic`, `flash-kernel`)
are pinned off: they cannot reach the BOOT partition.

A package of Ubuntu's that we have to replace is Ubuntu's version with a
suffix, pinned above Ubuntu's (`overlay/etc/apt/preferences.d/`). So far
that is libcamera: `0.7.0-1ubuntu2+t510.6`. Each Ubuntu update of it then
needs our rebuild on top (`camera/build-libcamera.sh`: its comments say what
it needs), or the tablets keep the old one; `make-debs.sh` leaves those
packages in `out/debs/`, and `publish.sh` publishes them with ours. Avoid
more of these while there is another way (a Mesa with
`gpu/mesa-26.0.8-g71-zero-derivatives.patch`, say).

## A fix, step by step

1. **Reproduce** the bug on your own tablet (or from the reporter's logs:
   `./install.sh --logs`).
2. **Fix** it in this repository: `overlay/`, `tools/`, `initramfs/`, or the
   kernel tree (see [Kernel updates](#kernel-updates)).
3. **Build the packages:** `scripts/make-debs.sh` → `out/debs/`.
4. **Install them on your tablet the way users will get them**, not by
   copying files by hand:
   `scp out/debs/*.deb tablet:` and `sudo apt install ./t510-*.deb`.
   Restart the tablet when the kernel changed. Check the fix and the
   [checklist](#checklist).
5. **CHANGELOG.md:** a new `## x.y.z — title` section on top. List the
   change in the users' words, plus the kernel builds (`Builds:` with #N)
   when the kernel changed.
6. **Commit and push**: one commit per published version, its subject saying
   what changed.
7. **Publish:** `scripts/publish.sh` signs the packages you tested and puts
   them into the apt repository. The tablets get them with their next
   `apt upgrade`; Xfce's update notifier shows them by itself. New installs
   take them the first time they are online (`t510-update.service`).
8. For **every version**, x, y and z, a GitHub release (tag `vx.y.z`)
   whose notes are the version's CHANGELOG section, so everything that came
   out shows on the project page; README.md changes with it wherever the
   version changes what it says.
   With a new image (see "Version numbers"): README.md, "Building", then
   `scripts/make-release.sh`, and its files go to the release; the image
   carries the same packages, installed. Without one, the release is the
   tag and the notes only, and says which image a new install starts from.

Only the files you tested get published. That is why packages are built
on this PC and never by a CI service after the push.

## Kernel updates

Our kernel is not Ubuntu's: its security fixes come from the stable releases
of the 7.3 line on [kernel.org](https://www.kernel.org/), never with Ubuntu's
updates. Watch the stable announcements (kernel.org, the `linux-stable`
mailing list) and take each new 7.3.N, at least the ones that fix CVEs.

1. Keep the current tree aside: `mv src/linux-7.3 src/linux-7.3-prev`.
2. `VER=7.3.N` in `mainline/build-kernel.sh`, then run it: it downloads that
   release, makes a new tree and applies `mainline/patches/` on top. If a
   patch does not apply, fix it in the new tree (compare with the old one)
   and run `mainline/export-patches.sh`.
3. `mainline/make-boot.sh initramfs` (and `INSTALL=1` for the installer
   image, for an image release). Without the argument it packs the bring-up
   initramfs, which stops after printing its report; `make-debs.sh` refuses
   that image.
4. Try it on your tablet first without a package: copy the boot image over
   and `dd` it to `/dev/mmcblk0p15`, restart. Keep the previous image on the
   PC.
5. If it works through the [checklist](#checklist), go on from step 3 of
   [A fix, step by step](#a-fix-step-by-step) (z version).

The 7.3 line gets stable releases only for a while. When it reaches its end
(kernel.org marks it EOL), the patches move to the next kernel line,
preferably a long-term one: that is a y update with a full check, not a
routine one.

### When a kernel does not boot

The initramfs rescue mode (`/.boot-attempt`) helps only when the kernel
itself starts. A kernel that does not get that far needs the PC:

```bash
heimdall flash --BOOT boot-mainline.img
```

in Download mode, with the boot image of the last version that worked. Every
release page has one, and `t510-kernel` keeps the one it replaced at
`/var/lib/t510/boot-previous.img` on the tablet. Do not put a kernel into
RECOVERY as a fallback: S-Boot then boots RECOVERY after every reset
([mainline/README.md](../mainline/README.md), "Pitfalls").

## Checklist

On your own tablet, after the change and after a restart:

- it boots to the login screen; `uname -v` shows the new build when the
  kernel changed;
- touch, the on-screen keyboard, long press (right click), rotation;
- Wi-Fi, sound from the speakers, brightness, the charging indicator;
- the GPU desktop (Panfrost; `glxinfo -B` does not say llvmpipe);
- the power key (short press, screenshot, power dialog);
- `systemctl --failed` is empty; `journalctl -b -p err` holds nothing new;
- after the package update: `apt upgrade` asked no questions about
  configuration files that you did not change yourself.

## Ubuntu's own updates

Ubuntu's updates come to the tablets as on any Ubuntu, security ones by
themselves (unattended-upgrades). Updates of Mesa, the X server, PipeWire,
LightDM or Xfce can break something of ours. Install them on your own tablet
the day they come and go through the checklist. A breakage found that way is
a z fix of ours.

## Bug reports

GitHub issues. Ask for:

- `uname -v` and `dpkg -l 't510-*'`;
- what happens and since when (after which update);
- the logs: `./install.sh --logs` on the PC (it changes nothing on the
  tablet) or `journalctl -b` from the tablet.

## The signing key

The repository is signed with a key of its own: the public part goes into
`t510-archive-keyring`, the private part stays on the maintainer's PC.

- Keep a copy of the private key off the PC (a USB stick in a drawer).
  Without it no tablet accepts an update anymore until its owner installs a
  new key by hand.
- If the key expires, extend it before it does, then publish a new
  `t510-archive-keyring`.
- A key that leaked is replaced at once: a new `t510-archive-keyring`
  carrying both keys, signed with the old one, then only the new key.
