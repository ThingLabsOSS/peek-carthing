# peek_carthing — deep Linux hardware inventory for the Car Thing

One-shot Linux kernel module that dumps **everything we can read** about
the Car Thing's hardware state. Companion to the u-boot-side `hwinfo`
command but pulls way more data because Linux has APIs we don't in
u-boot (SMC, thermal framework, I2C subsystem, regulator/clock summary,
DRM debugfs, etc).

## What you get

Output via `/proc/peek_carthing` (also dumped to dmesg on load):

1. **System summary** — kernel build, RAM, uptime, loadavg, machine type
2. **SoC identity** — `MIDR_EL1` decoded into impl/arch/variant/part/rev,
   `AO_SEC_GP_CFG0` boot-source decode
3. **AO sticky regs** — full 16-slot scratch + GP CFG dump (reboot reason
   sits in `CFG15`, calibration trim in `CFG10/12`)
4. **eFuse user area** — up to 256 bytes via secure-monitor SMC. Tries
   vendor SMC ID `0x82000030` first, then mainline `0x82000041`. Highlights
   `usid` (offset 18, 16 ASCII) and `f_serial` (offset 34, 15 ASCII).
5. **HHI clock tree** — raw 1 KiB dump including all G12A PLLs, video
   clock divider regs, MIPI PHY clock control, tsensor clock control
6. **SARADC** — register block dump + sysfs path hint for cooked values
7. **Tsensors** — PLL + DDR raw config/status registers
8. **VPU near-ENCL** — 1.25 KiB around the display-timing block. Includes
   `ENCL_VIDEO_MAX_PXCNT/LNCNT`, `HAVON_BEGIN/END`, `VAVON_BLINE/ELINE`,
   `HSO_END`, `VSO_ELINE` — i.e. live h/v_active, htotal, vtotal,
   hsync/vsync widths
9. **DSI host** — full DWC core + Amlogic TOP wrapper register dump
10. **MIPI DPHY** — digital block (usually all zeros — clocking is HHI-side)
    plus the analog `MIPI_CNTL0..CNTL2` triplet that controls dphy lane bias
11. **Pinmux / GPIO state** — `PERIPHS_PINMUX` (16-bank pad function
    select), `AOBUS_PINMUX`, and the actual GPIO input/output state
    registers for banks BOOT/A/C/H/Z. Lets you correlate what every pin
    is muxed to vs its current level.
12. **USB phy** — PHY21 control block. Tells you if VBUS is detected,
    which mode the OTG is in, etc.
13. **PWM** — PWM AB block (BL_PWM channel A drives the backlight)
14. **I2C bus survey** — for buses 0..7, lists every address that ACKs.
    Then dumps register windows for known chips:
      - `MAX14656` (charger detector @ 0x35 on bus 2): regs 0x00..0x09
      - `TMD2772` (prox/ALS @ 0x39 on bus 2): regs 0x00..0x1f
      - `TLSC6X` (touch + panel-variant probe @ 0x2e on bus 0): regs 0x00..0x3f
      - Apple MFi auth chip @ 0x10 on bus 3: version reg only (full
        cert/serial dump lives in u-boot `mfi` command — needs ~500 ms
        of SMBus dance that's awkward from a procfs read handler)
15. **eMMC sysfs hint** — paths to `manfid`/`cid`/`csd`/`name`/`oemid`
    and the debugfs EXT_CSD dump
16. **DRM/KMS hints** — `/sys/kernel/debug/dri/0/state` and friends
17. **Thermal zones** — every registered zone + current temp
18. **Other introspection sources** — clk_summary, regulator_summary,
    pinmux-pins, gpio, iio, pwm, backlight, drm

## Building

```bash
# 1) Get kernel sources for the target. For mainline-based ports:
git clone --depth 1 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
# (then configure + make modules_prepare for aarch64)

# 2) Build the module against those headers
make KDIR=/path/to/linux ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

# Result: peek_carthing.ko
```

Native build on the device works too if you have a kernel-headers
package installed:

```bash
make   # uses /lib/modules/$(uname -r)/build by default
```

## Loading

```bash
insmod peek_carthing.ko
cat /proc/peek_carthing | tee hwinfo.txt   # also goes to dmesg

# when done:
rmmod peek_carthing
```

The module exposes `/proc/peek_carthing` for repeated reads. Each read
re-runs the full dump (so you get fresh thermal / clock / sysfs state).

## Known limitations

- **eMMC EXT_CSD direct read isn't implemented.** Doing the
  `mmc_get_card` + `mmc_send_ext_csd` dance from a procfs read handler
  would block on the host mutex if the FS is busy. Easier to read from
  sysfs/debugfs in userspace.
- **Some MMIO blocks may EBUSY** if a driver has already claimed them
  with `request_mem_region`. `ioremap` itself succeeds (no exclusivity
  check), but if you see "ioremap failed" in the output, that region is
  exclusively held by another driver. Workaround: `rmmod` that driver
  before running peek_carthing, or read via the driver's own debugfs.
- **SMC eFuse read may fail on vendor BL31** if it requires a different
  function ID than the two we try. The fallback chain covers the two
  ID conventions I know of (vendor `0x82000030`, mainline `0x82000041`).
- **I2C bus numbering** depends on DT aliases. The known-chip dumps
  assume mainline DT numbering (`i2c0=touch, i2c2=charger+prox,
  i2c3=MFi`); on vendor kernel the buses may be at different indices.
  The survey loop probes 0..7 so you can find them either way.

## Adjusting for your kernel

If you find the I2C buses are numbered differently, edit the bus_nr
arguments in `dump_i2c_bus_survey()` at the bottom of `peek_carthing.c`.

If you want to add more register blocks: copy the pattern of
`dump_hhi()` etc — `peek_iomem(m, "label", PHYS_ADDR, SIZE)` does the
ioremap-dump-iounmap dance for you.

## License

GPL-2.0. Uses kernel-internal APIs that require GPL.
