# The mainline kernel on the SM-T510: technical notes

Ubuntu 26.04 (systemd 259, cgroup v2 only) does not run on the 4.4 kernel, so a
new kernel had to come first. Samsung released nothing newer than 4.4 for the
Exynos 7904, which leaves mainline. Mainline has the Exynos 7885 (Galaxy A8
2018, `jackpotlte`), and the 7904 is the same SoC: even Samsung's kernel shares
one DT, `exynos7885.dts`, between them.

## Status

| Stage | What | Status |
|---|---|---|
| 0 | 7.3 kernel, display (simplefb), 8 cores, 2 GB, eMMC HS200, keys, ramoops | **works** |
| 1 | USB gadget (dwc3 + PHY) → RNDIS, ssh; Ubuntu boots on 7.3, Xorg on fbdev | **works** |
| 2 | STM FTS1BA90A touchscreen (own `stm-fts1ba90a` driver), charge level (S2MU005 fuel gauge) | **works** |
| 3 | SPEEDY bus (own driver), S2MPU08 PMIC: RTC, regulators; microSD slot | **works** |
| 4 | sound: ABOX (Samsung's DSP subsystem) + TFA9896 amplifiers → speakers, PipeWire | **works** |
| 5 | Wi-Fi: SCSC core + `scsc_wlan` driver (cfg80211), NetworkManager, 2.4/5 GHz | **works** |
| 6 | display through KMS (own `exynos7885-decon` driver), Mali-G71 GPU on Panfrost, X on modesetting + glamor | **works** |
| — | cpuidle: idle cores powered down (C2), tick from the MCT | **works** |
| — | S2MU005 charger: stock currents by adapter type | **works** |
| — | Ubuntu 26.04.1 LTS on the 7.3 kernel | **works** |
| — | COD3035 codec (headphones, microphones) | — |
| — | Bluetooth (SCSC BT) | — |

The volume keys work together with the sound card (without it Xfce showed
"no sound").

For the 7885, mainline 7.3 has only the CPU, GIC, clocks (core/fsys/peri/top),
pinctrl, UART, eMMC, I2C, PMU and chipid.

## USB (stage 1)

The USB block of the 7885 is built like the Exynos 850's: the same addresses
(`0x13600000` dwc3, `0x135d0000` PHY) and the same PHY register layout. The
`phy-exynos5-usbdrd` driver got a `samsung,exynos7885-usbdrd-phy` variant (the
850 code, PHY power bit in the PMU at `0x674` instead of `0x704`, HS tuning
`HSPPARACON` from Samsung's DT). For data to flow (without this the host sees
the device but fails with `error -71`):

- `phy_type = "utmi"` — the PHY has an 8-bit UTMI+ (`GUSB2PHYCFG` as in 4.4: `0x40102400`);
- the PHY reference clock is the 50 MHz USB PLL (`assigned-clocks`): on a
  normal boot S-Boot does not start it, 4.4 does so itself (`PLL_CON0 = 0xa1900d14`).

The 4.4 registers were read with the `tools/usbregs` module; the stage 1 init
prints the same registers on screen through `devmem` when it fails.

The initramfs (`initramfs/init`) takes the RNDIS interface name from configfs
(`rndis0` on 4.4, `usb0` on mainline), and Ubuntu's network (`50-usb.network`)
matches both.

USB is device-only for now (`dr_mode = "peripheral"`): the S2MM005 CCIC is not
supported. A running mainline system can rewrite its kernel with `dd` to `p15`.

## Touchscreen

STM FTS1BA90A on `i2c_4` (`0x49`), interrupt `gpa0-0` (active low), powered by
fixed regulators on `gpg3-2` (3.3 V) and `gpg3-3` (1.8 V). Its protocol is newer
than `stmfts`', hence our own `drivers/input/touchscreen/stm-fts1ba90a.c`
(commands and event format from Samsung's driver): reset `FA 20 00 00 24 81`,
wait for READY, ID `39 36`, touch type, scan `A0 00 01` with an echo, chip
interrupts `A4 01 01`; 8-byte events (`60` — one, `61` — the rest). The chip
firmware is not touched. Rotation: `t510-rotate` matches the touchscreen with
`MatchIsTouchscreen`, not by the 4.4 name `sec_touchscreen`.

On simplefb `fbtool unblank` is not needed (and fails): `t510-display-setup`
and `t510-display.service` skip it, otherwise LightDM restarts X in a loop.

## Battery and clock

The S2MU005 fuel gauge (`i2c_0`, `0x3b`) runs on mainline's `s2mu005-battery`
driver: `/sys/class/power_supply/s2mu005-fuel-gauge` (charge, current,
voltage, status). Its interrupt (`gpa2-4`) is not wired up yet. A
`simple-battery` node (6000 mAh, Li-ion, 3.4–4.35 V) as `monitored-battery`
lets the kernel report technology and capacity, and UPower its estimate.

The S2MU005 charger (`0x3d`) sits on another bus, `i2c_1` (`13840000.i2c`,
`i2c-2` in Linux), node `pmic@3d` with interrupt `gpa2-2`. S-Boot leaves the
input at 500 mA (`CTRL2 = 0x08`), the charge current at 450 mA (`CTRL7`) and
4.30 V (`CTRL8`) — about 0.25 A reached the battery with any adapter.
Mainline's `s2m-charger` waited for a MUIC extcon and set no currents; patch
0032: without a `muic` node (the USB-C port is behind the S2MM005 CCIC) the
driver reads VBUS (`STATUS0` bit 7) and the BC1.2 result of the MUIC (`DEV1`:
`0x40` DCP, `0x20` CDP, `0x04` SDP) on the vbus/attach/detach interrupts and
sets Samsung's stock currents (`cable-info`): adapter 1500/2300 mA, CDP
1500/1500, PC and unknown 500/500, no VBUS 500; the voltage comes from
`monitored-battery` (4.35 V). `/sys/class/power_supply/s2mu005-charger`:
`online`, `usb_type`, `input_current_limit`, `constant_charge_*`; UPower sees
mains power. With an adapter 1.1–1.25 A go into the battery. 5 V only: the
SM-T510 has `max_input_voltage = 5000`, no 9 V AFC. The charging watchdog is
off (`CTRL12 = 0x81`). USB-C PD is not supported — C-to-C charging without
BC1.2 gets 500 mA.

The real-time clock is the RTC in the S2MPU08 PMIC on the SPEEDY bus (below).

Samsung's DT has `/memreserve/ 0xe0000000 0xd00000` (secure world); in our DT
that is `secure@e0000000` with `no-map`.

## SPEEDY, PMIC, clock, microSD

SPEEDY is Samsung's one-wire bus to the S2MPU08 PMIC (and the codec inside it).
The driver `drivers/i2c/busses/i2c-exynos-speedy.c` (after Samsung's) sends
every transfer through the batcher IP, which takes a hardware semaphore shared
with the ACPM firmware (it changes the DVFS voltages over the same bus). A
SPEEDY address is 12 bits: block number (0 TOP, 1 PMIC, 2 RTC, 3/7 codec) and
register; the adapter turns the usual pair of I2C messages "register + read"
into one transfer, so regmap-i2c works without changes to the I2C core
(Samsung patched i2c-core). Debugging: `tools/speedy-read.py BUS DEV REG [N]`
(read-only).

On 4.4 the PMIC was driven through the ACPM firmware (`exynos_acpm_read_reg`),
and only the codec used SPEEDY directly; on mainline the PMIC goes directly
through the batcher.

- MFD: S2MPU08 added to `sec-*` (`samsung,s2mpu08-pmic`, node `pmic@1`), no
  interrupts yet.
- RTC: `rtc-s5m` with the S2MPU08 layout (update bits as on the S2MPS15, but
  time and alarms one register further — there is a millisecond register).
  `rtc0`; the time at boot comes from the PMIC.
- Regulators: S2MPU08 in `s2mps11.c` (LDO1–14, 33–37, BUCK1–8). The DT lists
  only regulators with consumers (ldo2 `vqmmc_sd` 1.8–2.8 V, ldo35 `vmmc_sd`
  3.0 V): the kernel leaves the rest alone. BUCK1–5 are the DVFS rails (MIF,
  CPU, INT, G3D) and belong to ACPM.
- microSD: `mmc@13550000` (MMC_CARD clocks, sd2 pins, card detect `gpa0-7`
  active low), high-speed 50 MHz; needs `CONFIG_MSDOS_PARTITION` (MBR on the card).

Sound: without ABOX (the DSP subsystem with firmware: DMA, UAIF/I2S, routing)
mainline has no audio channel at all — the codec driver alone is useless (see
"Sound: ABOX and speakers").

## Ubuntu 26.04 on this kernel

- systemd 259: `71-seat.rules` makes fbdev the master device of a seat only
  with `nomodeset` — without `overlay/etc/udev/rules.d/72-t510-fb-seat.rules`
  seat0 is not graphical and LightDM does not start X (fbdev fallback).
- Snaps do not run here; the image blocks snapd and the snap-only debs
  (`overlay/etc/apt/preferences.d/no-snap`).

## Backlight, splash, keys

- The panel itself sets the brightness (Himax HX8279D): DCS `B0 04` (page),
  then `B8 <level>`. Mainline has no DSI host for the 7885, but after S-Boot
  the DSIM runs in video mode — `drivers/video/backlight/gta3xl_bl.c` puts short
  packets straight into the header FIFO (packet-go, as Samsung does). The table
  is the lower of Samsung's two (panel revisions), maximum 156 (199 in Samsung's
  outdoor mode only). `/sys/class/backlight/panel`, 0–255, 117 by default.
  Prototype: `tools/dsi-cmd.py`.
- Power key: `t510-powerkey` turns the backlight off and on through `bl_power`
  (video group permissions — `70-t510-backlight.rules`); the key device is
  called `gpio-keys` on mainline (`gpio_keys` on 4.4).
- By default the boot shows the kernel and systemd log on screen. `SPLASH=1
  mainline/make-boot.sh` — `quiet loglevel=3 splash`: with
  `FRAMEBUFFER_CONSOLE_DEFERRED_TAKEOVER` the S-Boot logo stays until Plymouth
  (xubuntu-logo theme, frame-buffer renderer, no rotation). `FB_MARK=1` — the
  wrapper's green mark.

## Performance

- `capacity-dmips-mhz` in the DT (A53 485, A73 1024): without it every core has
  `cpu_capacity` 1024 and heavy tasks land on the slow A53s.
- X draws on the GPU (glamor), rotation is RandR on the GPU. In the fbdev
  fallback (`/etc/X11/t510/force-fbdev`) X rotates every redraw on the CPU,
  and `t510-xorg` pins it to the big cores. xfwm4 compositing is off.
- Touchscreen: no `INPUT_MT_DROP_UNUSED` — the FTS reports changes only, and a
  finger that does not move must not be "released".

## Display (KMS) and GPU

S-Boot leaves the whole path running: DECON0 (`0x14860000`) in video mode,
DSIM, the panel; DMA channel G0 (`0x148b1000`) reads the frame from the
physical address `IDMA_IN_BASE_ADDR_Y` (`+0x40`, holding `0xEC000000`), the DPU
SysMMU in bypass. `drivers/gpu/drm/tiny/exynos7885-decon.c` (patch 0024)
reconfigures nothing: a flip writes the address to G0 and
`SHADOW_REG_UPDATE_REQ` (`0x60`, `0x8000000f`), and the DECON latches it at the
start of the next frame; the frame start interrupt (SPI 114,
`INTERRUPT_PENDING` bit 12, W1C) gives the vblank events — `kmscube` runs at
exactly 60 fps. It is enabled only while vblanks are needed or a flip is
pending (patch 0026): 0 interrupts instead of 60/s when idle. Mode and format
come from S-Boot (1200×1920, XRGB8888, line stride exactly 4800). Buffers are
GEM DMA from CMA (`CMA_SIZE_MBYTES=128`). A disabled CRTC shows S-Boot's
cleared buffer and turns the backlight off (`backlight = <&backlight>`).
simplefb stays for early output and is removed through the aperture.
`/dev/fb0` is now fbdev emulation on top of DRM; devices: `card0` — display,
`card1`/`renderD128` — Panfrost (stable path for X:
`/dev/dri/by-path/platform-14860000.display-controller-card`).

The GPU is a Mali-G71 MP2 r0p0 (`0x60a00002`) on stock Panfrost and Ubuntu's
stock Mesa 26.0.8 (kmsro finds Panfrost for an unknown KMS driver by itself).
S-Boot leaves the G3D domain on (`G3D_STATUS` `0x11c840a4` = `0xf`), PLL_G3D at
343 MHz. Panfrost devfreq (simple_ondemand) changes the frequency through the
ACPM G3D clock (index 4, the same `FREQ_REQ` command as for the CPU; ACPM picks
the voltage): levels 343/450/545/676/845 MHz from Samsung's DT for the 7904
(patch 0025). PLL_G3D (`0x11400120`) matches every request; 372 runs of the GL
tests at 845 MHz without errors. The G3D sensor (`0x10074000`, started by
S-Boot) drives the `gpu-thermal` zone: GPU throttling from 75 °C
(`CONFIG_DEVFREQ_THERMAL`, cooling device `devfreq-11500000.gpu`, states 0–4 =
845…343 MHz). Manual control: `/sys/class/devfreq/11500000.gpu/{min,max}_freq`.
None of the G71 problems of the earlier kbase-shim era (IDVS, several tiler
jobs in a chain, AFBC, 4 GB shader windows) show up on Panfrost: the
`tools/gpu` tests (`gles-*`) pass without workarounds, `gles-multidraw` with
1000 draws included.

**Shader derivatives on the G71.** In Firefox and GTK4 (baobab, pavucontrol)
rounded corners are blurred squares: `dFdx`/`dFdy`/`fwidth` return `-value`
(`tools/gpu/gles-derivative.c`). For v6 (`BIFROST_LIMITED_CLPER`) Mesa builds
derivatives with `CLPER_OLD.i32`, which always gives 0 on the G71 r0p0 —
tested with our own lane, lane 0, an index without byte replication
(`bi_imm_u8` → `#0x1010101`) and swapped operands; the encoding was worked out
on the G72, and Panfrost does not officially support the G71. The change
`gpu/mesa-26.0.8-g71-zero-derivatives.patch` (quirk `BIFROST_NO_CLPER`,
derivative = 0: `fwidth` antialiasing becomes a sharp edge) fixes the corners,
but GTK4 on GL also loses text, so the system Mesa is not replaced: GTK4 draws
in software (`GSK_RENDERER=cairo` in `/etc/environment`); Chrome (Skia) draws
correctly. Building `libgallium` of the same version (Panfrost + softpipe, no
LLVM) — `gpu/build-mesa.sh` on a PC: native `mesa_clc`/`panfrost_compile`
(llvm/clang/libclc/SPIRV-LLVM-Translator/spirv-tools-dev 21), then a cross
build with arm64 `-dev` packages (libdrm, expat, zlib, zstd, xcb/X11, wayland,
wayland-egl-backend, glvnd); test without replacing it through
`LD_LIBRARY_PATH`.

Panfrost buffers are pinned in memory: a game that loads more than ~1.3 GB into
the GPU runs into the OOM killer with 2 GB.

## CPU frequency (DVFS through ACPM)

The ACPM firmware on the APM core changes frequency and voltage; Linux runs
cpufreq-dt + schedutil on top of the ACPM clocks. The protocol is mainline's
`exynos-acpm` (Google Tensor): channels in SRAM at `0x02052000` (initdata
`0x4b80`, 18 words per channel), the doorbell is the mailbox at `0x11c40000`
(`samsung,exynos850-mbox`: `INTGR0`, bit `(1 << channel) << 16`; its clock is
optional). The APM SRAM takes 32-bit accesses only.

- A channel's owner is a plugin bit mask. DVFS on 4.4 went through the
  **fvp (3)** plugin on channel 1 (`exynos-dvfs-manager`,
  `FREQ_REQ`/`POLICY_REQ`); the kernel loads fvp itself: the file
  `exynos7885_acpm_fvp.fw` (built into Samsung's kernel, published in the
  `gta3xlwifi-dev/android_kernel_gta3xlwifi` tree) is written to SRAM at the
  address from the plugin table and attached with the `DP_A` command (bit 25,
  plugin id in bits 28:26) on channel 4 — `acpm_attach_plugins()` does that.
- The plugin is not built into our kernel: it is read from `/lib/firmware`.
  Until the root file system is there, ACPM defers its probe (`-EPROBE_DEFER`)
  before it requests any mailbox channel — a channel freed on deferral comes
  back from `mbox_free_channel()` in TXDONE_BY_POLL mode, this mailbox has no
  poll timer, and the next probe crashed in `hrtimer_start()`. cpufreq and
  Panfrost wait for its clocks; once the root is mounted,
  `t510-firmware.service` re-probes it (`/sys/bus/platform/drivers_probe`).
- fvp reads the ECT table that S-Boot puts into DRAM at `0x90000000`
  (`PARA002s`). The wrapper used to copy the kernel there — the plugin crashed
  (`hard_fault` in the ACPM log). Now the kernel goes to `0xA0000000`
  (`KERNEL_LOAD`), and the ECT is reserved in the DT.
- fvp does not answer `FREQ_GET` (reply `...ffff`): `clk-acpm` remembers the
  last frequency it set, starting with S-Boot's.
- Domains: 0 MIF, 1 INT, 2 CPUCL0 = A73 (936–1768 MHz), 3 CPUCL1 = A53
  (449–1690 MHz), 4 G3D, 5 fsys, 6 cam, 7 disp, 8 aud. Levels and minimums come
  from Samsung's DT (`cpufreq` nodes).
- Temperature: `drivers/thermal/samsung/exynos7885_tmu.c` only reads the TMU
  started by S-Boot (T = code − TRIMINFO + 25 °C); zone `cpu-thermal`: 75 °C —
  big cores throttle, 90 °C — all of them, 110 °C — critical. Under full load
  with 8 cores at maximum it reaches ~76 °C in a minute.
- Debugging: `tools/acpm-probe.py`; the ACPM log is a ring in SRAM.

## Idle CPU (cpuidle)

State `c2` powers a core down (PSCI `CPU_SUSPEND`, parameter `0x10000`,
latencies from Samsung's DT), driver `psci_idle`, menu governor. Idle cores
spend more than 90 % of the time in C2. Tested: 8 cores, 2 min under load
(GPU, Wi-Fi, eMMC) — ~56 000 entries, ~790 refusals by the firmware, no
failures. What it took (patches 0027–0030):

- **PMU** (`drivers/soc/samsung/exynos7885-pmu.c`): PSCI in EL3 only does WFI;
  the core loses power if its `CLUSTERx_CPUy_CONFIGURATION[3:0]` was cleared
  before (like `cpu_disable()` on 4.4). A CPU PM notifier writes 0 on entry and
  `0xf` on exit. Registers (`0x11c8xxxx`): A73 (MPIDR Aff1 0) —
  `0x2000`/`0x2080`, A53 cpu0–3 (Aff1 1) — `0x2200`…`0x2380`, A53 cpu4–5 (Aff1
  2) — `0x2100`/`0x2180`; status at `+4` (bit 28 = core in WFI, `0x104c0000` =
  powered down). Checked by reading the status from every core.
- **Clusters**: S-Boot leaves `CLUSTERx_CPUSEQUENCER_OPTION` (`0x2488`,
  `0x24a8`, `0x24c8`) of the A73 and the second A53 at `1` — the PMU powers
  down a cluster when all its cores sleep, together with its L2, which nobody
  flushed. The result: memory corruption anywhere (crashes even on cores
  without C2). The driver sets all of them to `0x10` (the cluster stays on,
  like `cluster_enable()` on 4.4). Cluster power-down (CPD, PSCI `0x1010000`)
  is not done.
- **PSCI**: when it refuses `CPU_SUSPEND` (an interrupt came in the meantime),
  EL3 returns with x19–x28 clobbered — an SMCCC violation. The call now
  declares every register except x29 clobbered (`drivers/firmware/psci`).
- **Tick**: the arch timer stops in C2. As on 4.4, the tick comes from the MCT
  local timers (`0x10040000`, SPI 234–245, `samsung,local-timers-tick` — rated
  above the arch timer); they are always-on and wake the core with their own
  interrupt, no broadcast needed. The clocksource and `sched_clock` stay on the
  arch timer (26 MHz, the same counter as the MCT FRC).

To watch: `/sys/devices/system/cpu/cpu*/cpuidle/state1/{usage,time,rejected}`;
off at run time: `echo 1 > …/state1/disable`, off completely: `cpuidle.off=1`
in the `CMDLINE` of `make-boot.sh`. Debugging hangs: `sysctl kernel.panic=10
kernel.panic_on_rcu_stall=1` — a panic then reboots with a warm reset, and
the dump stays in `/var/lib/systemd/pstore/dmesg-ramoops-0` (a hardware reset
after a hang wipes it).

`clk_ignore_unused` cannot go without more work: 103 clocks are on without a
consumer — ACPM domains (`mif`, `int`, `aud`, `disp`…), the ABOX clocks (the
driver runs the DSP around the clk API), bus gates of unused UART/SPI/I2C/USI.
`pd_ignore_unused` changes nothing — there are no genpd domains.

## Wi-Fi: SCSC (Maxwell WLBT) and the scsc_wlan driver

**Works**: `wlan0` comes up at boot and NetworkManager connects by itself
(WPA2-PSK, 2.4 and 5 GHz, HT/VHT). Chip S612 EVT1.0, firmware
`S612_E788X_C1_Y21W42A`.

- Samsung's SCSC core — `drivers/misc/samsung/scsc/` (headers in
  `include/scsc/`): `scsc_mx` (mxman, firmware loading), `platform_mif` (WLBT
  power/reset, shared memory, mailbox), a test client. The cfg80211 driver —
  `drivers/net/wireless/scsc/` (without Android: GSCAN, NCHO, KMO off).
  Incompatible interfaces are covered by the forced includes
  `misc/.../compat/scsc_compat_inc.h` and `net/.../wlan_compat.h`.
- DT: `wlbt@120c0000` (AP and M4 mailboxes, IRQ MBOX 57 / ALIVE 21 / WDOG 286,
  PMU syscon), `compatible = "exynos,wifibt_if"` on the `0xE9000000` reserve
  (4 MB; `memremap(WC)`, it is `no-map`), and **LDO33/34/36/37 always-on** (as
  in Samsung's DT). S-Boot does not turn them on; without them the radio is
  almost deaf: the network shows at −84 dBm and authentication fails
  (`Connect failed, 0x800c`).
- Firmware and configuration — `/vendor/etc/wifi` (`mx140.bin`,
  `mx140_wlan.hcf`, flat layout): the Android VENDOR partition (p26) mounted
  read-only at `/vendor`; MAC address — `/efs/wifi/.mac.info`.
- Debugging: `scsc_platform_mif.enable=0` on the command line leaves WLBT alone
  at boot, then `echo 1 > /sys/module/scsc_platform_mif/parameters/enable;
  echo 120c0000.wlbt > /sys/bus/platform/drivers/scsc_wlbt/bind`.
  `echo 1 > /dev/mx_client_test_0` starts the firmware with the NULL service.
  `configs/debug.fragment` (`EXTRA_FRAGMENTS=` for `build-kernel.sh`):
  netconsole, lockdep, hung task 30 s. netconsole over USB: on the PC
  `socat -u UDP-RECV:6666,bind=172.16.42.104 -`, on the tablet configfs
  `/sys/kernel/config/netconsole/` (usb0 → 172.16.42.104:6666). The firmware
  event log (`MLME_EVENT_LOG_IND`) prints as `fw event N` (3 — authentication
  starts, 1 — authenticated, 21/22 — EAPOL).
- Porting pitfalls:
  - `RESERVEDMEM_OF_DECLARE` takes a `struct reserved_mem_ops *`, not a
    function — the old code jumps into garbage while the DT is parsed (stuck on
    the logo, ramoops empty). It built because of `-Wno-error`: read the
    warnings.
  - `wiphy_apply_custom_regulatory()` takes rtnl and the wiphy lock itself, and
    the driver called it from `ndo_open` (rtnl already held) — a deadlock of
    the whole network stack and userspace. The domain is now applied from a
    work item (`slsi_regd_apply()`).
  - cfg80211_ops in 7.3: link_id/radio_idx, keys and stations by
    `wireless_dev`, cfg80211 hands out the remain-on-channel/mgmt_tx cookies;
    `ieee80211_mgmt`: `action_code` moved out of the union (offsets kept).
  - qdisc: `q` is a `qdisc_skb_head`, `gso_skb` a list, `enqueue` takes `to_free`.
  - The DT no longer provides IRQ resources — `platform_get_irq_byname()`.
  - lockdep "possible recursive locking" in `slsi_tx_data` is a false alarm:
    all `slsi_spinlock`s are of one class.
  - A forced reset (power + volume down) wipes ramoops — catch the log of a
    hang with netconsole.
  - Reading the CHIPID registers (0x10000000) through `/dev/mem` hangs the tablet.

## Sound: ABOX and speakers

- **Driver** — ABOX from the 4.4 kernel (`sound/soc/samsung/abox/`), ported to
  ASoC in 7.3 (codec/platform → component, `tools/asoc-platform-to-component.py`);
  what mainline lacks (wakelock, smc, PMU, iovmm, pm_qos…) is stubbed in
  `abox_compat.h` and `compat/`. The DSP firmware `calliope_sram.bin`/
  `calliope_dram.bin` is read from `/lib/firmware` at the very start of probe,
  before the IOMMU and the hardware: if it is not there yet, the probe is
  deferred, and `t510-firmware` repeats it after copying the files from the
  VENDOR partition (`/vendor/firmware`). Synchronously, not asynchronously:
  otherwise `abox_enable()` ran in the middle of probe.
- **Clocks** — CMU_DISPAUD (`clk-exynos7885.c`, PLL1431x with K at +0xc).
- **SysMMU** v7.2: the DSP goes through page table 1 (`samsung,sysmmu-pt-id1`,
  base also at 0x1020); the default IOMMU domain is passthrough, the 12 MB
  firmware area comes from CMA.
- **Power**: on Android the DISPAUD domain driver turns ABOX off
  (`abox_disable_callback()`); mainline has nothing like it — runtime suspend
  only cleared a flag, and every resume ran `abox_enable()` again over the
  running DSP: failsafe and random hangs when PipeWire started. ABOX stays on
  (`pm_runtime_forbid`), with the lowest DSP CPU frequency when idle.
  `snd_soc_abox.enable=0` on the command line leaves ABOX off.
- **Controls**: prefix "ABOX" (`sound-name-prefix` in the DT, like codec_conf on
  4.4). The DSP components are added after the card binds (logged as
  `abox_register_component`): `PCM ext`, and, since the DSP boots with the
  firmware loaded late, also `ERAP info` and Forte Media; the speakers need
  none of them (`ERAP info Bypass On` in the `t510-audio` log is an expected
  error). Component values, rates, RDMA synchronisation, ERAP and Sound Type
  are cached, IPC goes to a running DSP only, and `abox_restore_data()` runs
  when the DSP starts.
- **RDMA buffer** — 64 KB at most: the DSP wraps the position at 64 KB; with
  128 KB aplay clicked and PipeWire restarted the stream every 0.34 s.
- **Card** `gta3xl.c` (`samsung,gta3xlwifi-sound`): DPCM, front end RDMA0
  (`hw:0,0`) → back end UAIF3 (I2S, ABOX is master) → two TFA9896 (`tfa989x`,
  0x96 added; i2c_3: 0x34 right, 0x35 left, `SpkR`/`SpkL Amp Input`).
  `t510-audio.service` sets the route SPUS OUT0 → SIFS1 → UAIF SPK3.
- **PipeWire**: WirePlumber 0.5 does not read `main.lua.d` — the rule "no
  automatic card setup" is in `/etc/wireplumber/wireplumber.conf.d/51-t510.conf`,
  otherwise WirePlumber tries every format (the DSP firmware crashes) and makes
  its own sink the default. The output is the fixed node `t510-output` (`hw:0,0`).

## Session: rotation, battery, SSH, hangs

- **Rotation** — `t510-rotate` knows 4 orientations (`portrait`,
  `landscape-cw`, `portrait-inverted`, `landscape-ccw`); the panel has
  "Rotate by sensor" (`t510-rotate auto`: the way the tablet is held) and
  "Rotate" buttons (`t510-rotate next`, +90° clockwise). Every
  rotation restarts the X session (fbdev rotates only at start), ~7 s.
- **Accelerometer** ST LIS2DS12 (`st_accel`, IIO `iio:device0`) on USI2 HSI2C1
  (0x13990000; USI v1 `exynos8895-usi` in I2C1 mode through sysreg PERI
  +0x2008, HSI2C with 8895 timings). Axes: portrait — gravity along −x, upside
  down +x, on the right side −y (`landscape-ccw`). `t510-rotate auto` takes
  any of the four from the sensor (lying flat: no change), `portrait` and
  `landscape` pick only the side, upside down selects `portrait-inverted`, `t510-rotate status` prints the position. A
  rotation hint icon for the tray (`t510-rotate-hint`) exists but is off; its
  autostart file is in `/usr/local/share/t510/`.
- **Applications hanging for minutes** — PipeWire crashed in a loop: the
  fixed nodes in `/etc/pipewire/pipewire.conf.d/t510-audio*.conf` point at
  `hw:0,0`/`hw:0,9` of ABOX, and when there is no card the portal
  (`xdg-desktop-portal`) got stuck on PipeWire at session start, and every GTK
  application call waited for a 25 s timeout. The nodes now have
  `flags = [ nofail ]` (also in the `t510-rotate` template).
- **Stuck Onboard keys** — when its event queue overflows (the IRQ thread is
  late) the FTS controller loses events, releases included; a finger "hung"
  and X repeated the key. The driver now releases all contacts on the
  `QUEUE_FULL`/ESD status and when reading the queue fails (as downstream does).
- **Login and lock** — LightDM without autologin, `greeter-hide-users=false`;
  the on-screen keyboard of the login screen is in the accessibility menu
  (`/etc/lightdm/lightdm-gtk-greeter.conf.d/50-t510.conf`, `onboard --xid`,
  full width, bottom 30 % of the screen). The login screen runs with
  `GDK_SCALE=2` (`greeter-session=t510-gtk-greeter`,
  `/usr/share/xgreeters/t510-gtk-greeter.desktop`): `xft-dpi` alone enlarges
  the font while the login window and buttons stay small. Locking is
  `light-locker` (through the LightDM login screen): the power key
  (`t510-powerkey`) and `xfce4-session /general/LockCommand` call
  `light-locker-command -l`; on inactivity `xfce4-power-manager` blanks the
  screen (5 min on battery, 10 on mains). xfce4-screensaver is hidden from
  autostart: its lock window with the embedded keyboard is wider than the
  portrait screen. Rotation restarts the session — `t510-rotate` writes a
  one-time autologin (`60-t510-rotate-autologin.conf`), and
  `t510-display-setup` removes it.
- **VPN** — TUN, WireGuard, policy routing and nftables (iptables-nft) are in
  the kernel; VPN apps that need TUN work.
- The Bluetooth applet (blueman) is hidden from autostart until there is BT:
  without an adapter it crashes ("system program problem" windows).
- `ayatana-indicator-application` is hidden from autostart
  (`/etc/xdg/xdg-xfce/autostart`, `Hidden=true`): it takes
  `org.kde.StatusNotifierWatcher` but shows icons only through the Indicators
  plugin, which is not on the panel — the network icon (nm-applet) disappeared
  from the tray when the service started before the panel.

## How it boots

S-Boot loads from `boot.img` not the kernel but
[minimal_sboot_wrapper](https://github.com/VDavid003/minimal_sboot_wrapper)
(`make-boot.sh` clones it into `minimal_sboot_wrapper/`, our changes are
`patches/sboot-wrapper.patch`): the wrapper ignores S-Boot's DTB and command
line, copies `Image` to `0xA0000000` and starts it with its own DTB. S-Boot
always puts the ramdisk at `0x89000000`; its end goes into the DTB with the
exact size.

Our changes to the wrapper:
- `INITRD_START/END` — the exact size of the initramfs;
- `KERNEL_LOAD` — where the kernel is copied (keeps S-Boot's ECT at `0x90000000`);
- `FB_MARK=0xEC000000` — a green bar at the top of the screen: the wrapper got control;
- `SEC_REBOOT_REASON_RESET` — INFORM2/INFORM3 set to "normal reboot" (did not
  help to get out of RECOVERY, see below, but does no harm);
- `cpp -undef` — otherwise the path `linux-7.3` turns into `1-7.3`.

## Building and flashing

```bash
mainline/build-kernel.sh                          # Linux 7.3 + patches/ → Image and DTB
mainline/make-boot.sh initramfs                   # → mainline/out/boot-mainline.img
heimdall flash --BOOT mainline/out/boot-mainline.img   # in Download mode
```

Our kernel changes are `patches/0001…`, on top of the Linux 7.3 tag; after
changing the tree, `mainline/export-patches.sh` writes them out again. The
toolchain is the system's `aarch64-linux-gnu-gcc`.

## Debugging without USB

- Screen: simplefb on top of the bootloader's buffer (`0xEC000000`, 1200×1920,
  stride 4800), font TER16x32. `panic=0` — a panic stays on screen; the test
  init (`initramfs-phase0/init`) does not reboot.
- ramoops: the same area as on the 4.4 kernel (`0x91500000`, 0x8000: record
  0x4000 + 0x4000). The mainline console lies where 4.4 expects pmsg, so after
  going back to 4.4 it shows as `pmsg-ramoops-0` (systemd-pstore moves it to
  `/var/lib/systemd/pstore`). The mainline buffer survives Download mode (4.4
  prints `found existing buffer, size 16372`) but does not appear in
  `/sys/fs/pstore` — not worked out yet; the screen is the main channel.

## Pitfalls

- **Do not put mainline into RECOVERY**: after `reboot recovery` S-Boot booted
  recovery after every reset (boot loop), and resetting INFORM3 in the wrapper
  did not help. Cured by flashing the stock recovery and "Reboot system now"
  in it. `param` holds only a boot log then (`N,`/`R,` around `0x700E90`).
- `systemctl reboot recovery` does not work, use `--reboot-argument=`.
- A background command in an ssh session dies with it — use `systemd-run`.
- The eMMC shows up ~0.3 s after init starts — wait for `/dev/mmcblk0p33`.
- HS400 is off in the DT for now (HS200 works, 200 MHz).
- In 7.3 `/dev/fb0` is a separate option, `CONFIG_FB_DEVICE` (Xorg fbdev does
  not start without it).
- `CONFIG_DEVMEM` without `STRICT_DEVMEM` is on for debugging only (devmem in init).
- A failed device lock can hang `reboot.target` (seen after a driver oops in
  a deferred probe): a forced reset (power + volume down, 7–10 s) gets out.
