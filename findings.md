# Stock Car Thing hardware — interesting findings

Notes on what the 275 KiB hwinfo JSON reveals about the device, distilled
to "things you'd actually want to know." Captured from a running stock
unit (kernel 4.9.113, May 2020 build) via peek-collect.py + peek_mmio.ko.

## SoC + memory

  - **Cortex-A53 quad-core**, MIDR `0x410fd034` (rev 4, r0p4)
  - **DVFS range: 100 MHz → 1800 MHz** across 11 OPPs. Voltage jumps
    from 0.731 V (used for 100-1200 MHz) to 0.981 V at 1800 MHz peak.
    Spotify presumably keeps it well under 1800 to manage thermals on
    a passively-cooled board.
  - **488 MiB usable RAM** out of 512 MiB total. DTB reserves up to
    520 MiB of CMA regions (overcommitted — lazily allocated):
      | region            | size | notes |
      |-------------------|------|-------|
      | codec_mm_cma      | 308 MiB | video codec memory (mostly unused on Car Thing) |
      | ion-dev           | 128 MiB | Android ION graphics buffers |
      | di_cma            | 40 MiB | deinterlacer |
      | vm0_cma           | 32 MiB | video manager |
      | meson-fb          | 8 MiB | framebuffer |
      | secmon            | 4 MiB | secure monitor shared mem |
    Current actual CMA usage: ~47 MiB. **Stripping the unused video
    codec/ion reservations from a custom firmware DT would reclaim
    ~480 MiB for general use.**
  - **No swap** active. zram0 device exists but size 0 (not initialised).

## What it could do but doesn't

The SoC is *fully populated* with peripherals the Car Thing's Spotify
firmware never uses:

  - **Hardware video accelerators all present and `status="okay"`**:
      - `amvenc_avc` — H.264 encoder (Amlogic)
      - `hevc_enc` — H.265 encoder (Chips & Media)
      - `vdec` / `vcodec_dec` — H.264/H.265 decoders
      - `ge2d` — 2D graphics blitter
  - **HDMI PLL is actively configured** (`HHI_HDMI_PLL_CNTL0 = 0xda1504df`)
    even though there's no HDMI port. Probably reused as a clock source
    for video subsystem dependencies.
  - **DRM keyboxes provisioned** in unifykey storage:
      - `widevinekeybox` (Google)
      - `netflix_mgkid`
      - `PlayReadykeybox25`, `prpubkeybox`, `prprivkeybox` (Microsoft)
      - `hdcp22_fw_private`, `hdcp2_tx`, `hdcp2_rx`, `hdcp`
      - `attestationkeybox`
    Vestigial from a generic Amlogic AndroidTV image that Spotify
    repurposed. Could be useful if anyone wanted to turn a Car Thing
    into a tiny Netflix/Widevine playback device.
  - **Mali-G31 GPU** with 7 DVFS levels (250 → 850 MHz). Only used to
    composite the Spotify Qt app's UI today; capable of much more.
  - **Ethernet controller** in the SoC DT (`ethernet@ff3f0000`) — no
    physical jack on the board, but the MAC + PHY drivers are
    instantiated.
  - **Bluetooth chip**: `brcm,bcm4345c0` (Broadcom BCM4345 rev C0) —
    we knew it was on board but now confirmed by DT compatible string.
    On the dev unit BT init times out (`Bluetooth: hci0 BCM: Reset
    failed (-110)`) — possibly broken on this specific unit or stuck
    in a bad state.
  - **WiFi MAC stored in unifykey** (`mac_wifi`) — even though Car
    Thing has no WiFi radio. More AndroidTV-image leftover.

## UI stack

```
Linux 4.9.113
  ↓
Weston (Wayland compositor, --tty=4, aml-weston.ini)
  ├─ weston-desktop-shell
  ├─ weston-keyboard
  ↓
Chromium (--no-sandbox --in-process-gpu --remote-debugging-port=...)
  ├─ main process: 92 MB
  ├─ renderer: 83 MB
  ├─ utility: 28 MB
  ├─ zygote: 24 MB
qt-superbird-app (--config=/etc/qt-superbird-app/superbird_target.ini, 23 MB)
```

So the Spotify UI is a **hybrid Qt + Chromium web app** running under
Wayland (no X11). That's modern and surprising — most Amlogic-based
embedded systems still run on framebuffer or DirectFB.

## Storage layout

  - **eMMC running HS200** (200 MHz, 8-bit bus, SDR) = 1.6 GB/s
    theoretical bus rate. Vendor u-boot uses HS52 (52 MHz, 8-bit), so
    Linux re-tunes after kernel-side mmc init.
  - 18 partitions (A/B for the bootloader/system/dtb/vbmeta):
      | partition | size |
      |-----------|------|
      | boot_a, boot_b | 16 MiB each (Android boot image: kernel+ramdisk) |
      | dtbo_a, dtbo_b | 4 MiB each (DTB overlays) |
      | logo | 8 MiB |
      | misc | 8 MiB |
      | settings | 256 MiB |
      | system_a, system_b | 516 MiB each (ext4 rootfs) |
      | vbmeta_a, vbmeta_b | 1 MiB each (verified boot meta) |
      | data | rest of disk (userdata) |
  - **Rootfs is mounted read-only**. /var, /home, /tmp are tmpfs or
    separate writable partitions.
  - **eMMC has RPMB partition** (Replay-Protected Memory Block) — 4 MiB,
    used by vendor for protected key storage. We have access via
    `/dev/mmcblk0rpmb` but accessing it requires the RPMB key.

## Display chain (already confirmed values)

  - **480 × 800 panel, 60 Hz** (BOE variant on this unit)
  - htotal = 550, vtotal = 846 → pclk ≈ 27.92 MHz
  - hsync_width = 10, vsync_width = 6
  - **MIPI DPHY bias triplet** (the panel-dark smoking gun):
      | reg | value | notes |
      |-----|-------|-------|
      | MIPI_CNTL0 | `0xa4870008` | bias byte `0x87` (vs mainline default `0x1b`) |
      | MIPI_CNTL1 | `0x0001002e` | |
      | MIPI_CNTL2 | `0x2680e45a` | |
  - DSI controller: DesignWare DSI v1.21 (`DSI_VERSION = 0x3132312a`)
  - CLKMGR_CFG = `0x0115` (TX_ESC_DIV=21, TO_CLK_DIV=1)
  - **One sys_led** (per DTB) — `sys_led` on AO GPIO 5, default-state "on".
    Probably the small indicator LED hidden inside the chassis.

## Input devices (DT-confirmed)

  - **7 buttons** via gpio-keys:
      | DT label | Linux keycode |
      |----------|---------------|
      | back | KEY_ESC (1) |
      | mute | KEY_M (50) |
      | preset1 | KEY_1 (2) |
      | preset2 | KEY_2 (3) |
      | preset3 | KEY_3 (4) |
      | preset4 | KEY_4 (5) |
      | select (knob press) | KEY_ENTER (28) |
  - **Rotary encoder** via GPIO 9 + GPIO 10 (gray code, 2 steps per
    period). Maps to Linux input axis 6 (`REL_WHEEL`).
  - **TLSC6X touch** at I²C 0x2e bus 0 — capacitive touch over the
    480×800 screen. Driver-bound.
  - **4-mic PDM array**: actively capturing right now! TODDR
    ringbuffer at `0x14900000`, 384 KiB, write pointer advancing.

## Sensors (SARADC + thermal)

  - 5 thermal zones, all reading low (warm idle):
      - soc_thermal: 37.7 °C
      - ddr_thermal: 37.1 °C
      - bluetooth_thermal: 34.81 °C (probably NTC near BT chip)
      - dram_thermal: 34.75 °C (separate from ddr_thermal — likely
        package vs die)
      - pcb_thermal: 34.77 °C
  - 8 SARADC channels, 4 of which we know are used:
      | ch | raw | function |
      |----|-----|----------|
      | 0 | 2471 | PCB NTC |
      | 1 | 1339 | board revision divider |
      | 2 | 2459 | BT NTC |
      | 3 | 2475 | DRAM NTC |
      | 4-7 | various | unused / spare |
    Board rev decoded: 1339 → Rev 4 per the vendor lookup table.

## Tainted flag breakdown

Stock tainted = **0x1001**:
  - bit 0: TAINT_PROPRIETARY_MODULE (P) — from mali_kbase or apple_mfi_auth
  - bit 12: TAINT_OOT_MODULE (O) — same

After our peek_mmio.ko load + earlier crash, tainted grew to **0x1281**:
  - bits 0, 7, 9, 12: P + USER + DIE + O. The DIE bit (0x200) is
    a one-shot — once set, only a reboot clears it.

## Bootargs (Spotify's kernel cmdline)

```
init=/sbin/pre-init ramoops.pstore_en=1 ramoops.record_size=0x8000
  ramoops.console_size=0x4000 rootfstype=ext4 console=ttyS0,115200n8
  no_console_suspend earlycon=aml-uart,0xff803000 root=/dev/mmcblk0p14
  ro rootwait skip_initramfs reboot_mode_android=normal
  logo=osd0,loaded,0x1f800000 fb_width=480 fb_height=800
  vout=panel,enable panel_type=lcd_8 frac_rate_policy=1 osd_reverse=0
  video_reverse=0 irq_check_en=0 androidboot.selinux=enforcing
  androidboot.firstboot=0 jtag=disable
  uboot_version=v1.0-74-gfd61b37038 androidboot.hardware=amlogic
  androidboot.slot_suffix=_a
```

Notable: `androidboot.selinux=enforcing` (SELinux active), A/B partitioning
visible via slot_suffix, ramoops persistent crash logging configured.

## What's missing from the DTB

  - No `wifi/` node — confirms no WiFi radio on this hardware
  - No `audio_jack` / headphone-related nodes — no analog out
  - The `mac_wifi`, `hdcp*`, `widevinekeybox`, `netflix_mgkid` unifykey
    entries are all "ghost" provisions from the AndroidTV-template
    image — provisioned at factory but no driver/path uses them.

## Storage of secrets (unifykey)

40 entries; named ones include:
  - usid, mac, mac_bt, mac_wifi
  - region_code, deviceid, f_serial
  - secure_boot_set
  - hdcp22_fw_private, hdcp2_tx, hdcp2_rx, hdcp (4 separate HDCP keys)
  - PlayReadykeybox25, prpubkeybox, prprivkeybox
  - attestationkeybox
  - widevinekeybox
  - netflix_mgkid

These live in the `key` partition on eMMC, indexed by name through
vendor's `aml_unifykey` driver. The actual keys are AES-encrypted with
a per-chip key derived from eFuse, so even with `/dev/mmcblk0p3` (or
wherever key partition lands) raw data you can't read the cleartext
keys without the per-chip wrap key.

## What could be a quick win for a custom firmware

  - **Reclaim ~480 MiB** by stripping unused codec_mm/ion/di_cma/vm0
    reservations from DT
  - **Reuse hardware codec accel** for a video-playing kiosk app —
    H.264 and H.265 both supported
  - **Reuse the GPU** for proper 3D rendering instead of Chromium's
    2D-only swrast
  - **Drive sys_led** for a heartbeat / status indicator
  - **Bluetooth** *could* work if the BT chip itself isn't dead —
    BCM4345C0 has well-supported mainline drivers (`hci_bcm`)
