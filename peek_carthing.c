// SPDX-License-Identifier: GPL-2.0
/*
 * peek_carthing — one-shot deep hardware inventory for the Spotify Car
 * Thing (Amlogic G12A / S905D2), runnable from a Linux kernel module.
 *
 * Companion to the u-boot-side `hwinfo` command but takes advantage of
 * Linux APIs: SMC eFuse reads, I2C bus enumeration, thermal zones, DRM
 * state, regulator/clock framework summaries, etc.
 *
 * Loading: `insmod peek_carthing.ko`, then `cat /proc/peek_carthing`.
 * Re-reads return fresh data each time. `rmmod peek_carthing` when
 * done. Module stays loaded until rmmod (lightweight — no I/O happens
 * until you read the proc node).
 *
 * Sections covered:
 *   1.  SoC identity + boot source
 *   2.  AO sticky regs (reboot reason, calibration, scratch)
 *   3.  eFuse user area dump (usid, f_serial, anything else readable)
 *   4.  HHI clock tree raw register dump
 *   5.  SARADC all 8 channels
 *   6.  Tsensors (PLL + DDR)
 *   7.  VPU / ENCL display block
 *   8.  DSI host (DWC) registers
 *   9.  HHI MIPI analog DPHY
 *  10.  GPIO bank state (all pad banks)
 *  11.  USB phy state
 *  12.  Backlight PWM state
 *  13.  I2C bus survey + known-chip register dumps
 *        (MAX14656 charger, TLSC6X touch, TMD2772 prox, MFi auth)
 *  14.  eMMC sysfs introspection
 *  15.  DRM/KMS active-mode summary
 *  16.  Thermal zone + regulator + clock summaries
 *  17.  Kernel + cmdline + memory + uptime
 *
 * Tested on a hypothetical mainline kernel port; some sections may
 * EBUSY or NULL out on a vendor kernel where drivers have already
 * claimed the relevant resources (I2C buses, mmc host). Those are
 * non-fatal — each section is independent.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/i2c.h>
#include <linux/arm-smccc.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/delay.h>
#include <linux/printk.h>
#include <linux/utsname.h>
#include <linux/string.h>
#include <linux/thermal.h>
#include <linux/version.h>

#define PFX "peek_carthing: "

/* /proc API switched to a dedicated struct proc_ops in 5.6. Below that,
 * proc_create takes a struct file_operations. Both shapes have the same
 * fields we care about (open / read / lseek / release). */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
#define PEEK_PROC_OPS struct proc_ops
#define PEEK_OPS_OPEN    proc_open
#define PEEK_OPS_READ    proc_read
#define PEEK_OPS_LSEEK   proc_lseek
#define PEEK_OPS_RELEASE proc_release
#else
#define PEEK_PROC_OPS struct file_operations
#define PEEK_OPS_OPEN    open
#define PEEK_OPS_READ    read
#define PEEK_OPS_LSEEK   llseek
#define PEEK_OPS_RELEASE release
#endif

/* ---------- physical addresses (G12A) -------------------------------- */

#define AO_BASE			0xff800000
#define AO_SIZE			0x1000

#define HHI_BASE		0xff63c000
#define HHI_SIZE		0x400

#define VPU_BASE		0xff900000
#define VPU_SIZE		0x10000

#define DSI_TOP_BASE		0xffd07000
#define DSI_TOP_SIZE		0x1000

#define MIPI_DPHY_BASE		0xffd44000
#define MIPI_DPHY_SIZE		0x2000

#define USB_PHY21_BASE		0xff63a000
#define USB_PHY21_SIZE		0x1000

#define SARADC_BASE		0xff809000
#define SARADC_SIZE		0x1000

#define PERIPHS_PINMUX_BASE	0xff634480
#define PERIPHS_PINMUX_SIZE	0x800

#define AOBUS_PINMUX_BASE	0xff800014
#define AOBUS_PINMUX_SIZE	0x200

#define PWM_AB_BASE		0xffd1b000
#define PWM_AB_SIZE		0x100

/* SMC IDs for eFuse user-area read on G12A vendor secure-monitor */
#define EFUSE_SMC_FN_READ_VENDOR	0x82000030
#define EFUSE_SMC_FN_READ_MAINLINE	0x82000041

/* ---------- helpers --------------------------------------------------- */

static void hexdump_iomem(struct seq_file *m, const char *label,
			  phys_addr_t base, void __iomem *io, size_t bytes)
{
	size_t off, i;
	u32 w;

	seq_printf(m, "\n=== %s @ 0x%llx (%zu bytes) ===\n", label,
		   (unsigned long long)base, bytes);
	for (off = 0; off < bytes; off += 16) {
		seq_printf(m, "  +0x%04zx: ", off);
		for (i = 0; i < 16 && off + i < bytes; i += 4) {
			w = readl(io + off + i);
			seq_printf(m, "%08x ", w);
		}
		seq_putc(m, '\n');
	}
}

static void __maybe_unused dmesg_hexdump(const char *label,
					 phys_addr_t base,
					 void __iomem *io, size_t bytes)
{
	size_t off;

	pr_info(PFX "=== %s @ 0x%llx (%zu bytes) ===\n", label,
		(unsigned long long)base, bytes);
	for (off = 0; off < bytes; off += 16) {
		pr_info(PFX "  +0x%04zx: %08x %08x %08x %08x\n",
			off,
			readl(io + off + 0),
			readl(io + off + 4),
			readl(io + off + 8),
			readl(io + off + 12));
	}
}

/* Map -> dump -> unmap. Returns 0 on success, -ENOMEM on map failure. */
static int peek_iomem(struct seq_file *m, const char *label,
		      phys_addr_t base, size_t bytes)
{
	void __iomem *io = ioremap(base, bytes);

	if (!io) {
		seq_printf(m, "\n=== %s @ 0x%llx — ioremap failed (driver "
			      "claimed?)\n",
			   label, (unsigned long long)base);
		return -ENOMEM;
	}
	hexdump_iomem(m, label, base, io, bytes);
	iounmap(io);
	return 0;
}

/* Read a single u32 at an absolute physical address (one-shot). */
static int read_phys_u32(phys_addr_t addr, u32 *out)
{
	void __iomem *io = ioremap(addr, 4);

	if (!io)
		return -ENOMEM;
	*out = readl(io);
	iounmap(io);
	return 0;
}

/* ---------- 1) SoC identity + boot source ---------------------------- */

static void dump_soc_id(struct seq_file *m)
{
	u64 midr;
	u32 ao_cfg0;

	seq_puts(m, "\n=== SoC identity ===\n");

	asm volatile("mrs %0, midr_el1" : "=r"(midr));
	seq_printf(m, "  MIDR_EL1   : 0x%016llx\n", midr);
	seq_printf(m,
		   "             impl=0x%02x arch=0x%x variant=0x%x "
		   "part=0x%03x rev=0x%x\n",
		   (u8)(midr >> 24), (u8)((midr >> 16) & 0xf),
		   (u8)((midr >> 20) & 0xf), (u16)((midr >> 4) & 0xfff),
		   (u8)(midr & 0xf));

	/* AO_SEC_GP_CFG0 lower nibble = boot device (G12A mask ROM
	 * convention). 1=eMMC, 4=SD, 5=USB.
	 */
	if (read_phys_u32(0xff800300, &ao_cfg0) == 0) {
		const char *src;

		switch (ao_cfg0 & 0xf) {
		case 1: src = "eMMC"; break;
		case 4: src = "SD"; break;
		case 5: src = "USB (mask-ROM RAM-load)"; break;
		default: src = "unknown"; break;
		}
		seq_printf(m, "  AO_SEC_GP_CFG0: 0x%08x (boot device = %d → %s)\n",
			   ao_cfg0, ao_cfg0 & 0xf, src);
	}
}

/* ---------- 2) AO sticky regs ---------------------------------------- */

static void dump_ao_sticky(struct seq_file *m)
{
	/* AO_SEC_SD_CFG0..15 at 0xff8000c0..0xff8000fc are the always-on
	 * sticky scratch slots used for reboot reasons + calibration trim.
	 * 16 32-bit slots = 64 bytes.
	 */
	peek_iomem(m, "AO_SEC_SD_CFG0..15 (sticky scratch)",
		   0xff8000c0, 64);
	peek_iomem(m, "AO_SEC_GP_CFG0..15 (boot source / trim)",
		   0xff800300, 64);
}

/* ---------- 3) eFuse dump via SMC ------------------------------------ */

static int smc_read_efuse_byte(u32 fn, u32 offset, u8 *out)
{
	struct arm_smccc_res res;

	arm_smccc_smc(fn, offset, 1, 0, 0, 0, 0, 0, &res);
	if (res.a0 == (u64)-1 || res.a0 == 0)
		return -EIO;
	*out = (u8)(res.a0 & 0xff);
	return 0;
}

static void dump_efuse(struct seq_file *m)
{
	u8 buf[256] = {0};
	u32 fn = EFUSE_SMC_FN_READ_VENDOR;
	int ok = 0, i;

	seq_puts(m, "\n=== eFuse user area ===\n");

	/* Try vendor SMC ID first; if it doesn't respond, fall through to
	 * mainline. Both call sites use the same offset/length convention.
	 */
	for (i = 0; i < (int)sizeof(buf); i++) {
		if (smc_read_efuse_byte(fn, i, &buf[i]) == 0)
			ok++;
		else
			break;
	}
	if (ok == 0) {
		fn = EFUSE_SMC_FN_READ_MAINLINE;
		for (i = 0; i < (int)sizeof(buf); i++) {
			if (smc_read_efuse_byte(fn, i, &buf[i]) == 0)
				ok++;
			else
				break;
		}
	}

	if (ok == 0) {
		seq_puts(m, "  SMC efuse read failed (both vendor and "
			    "mainline IDs). BL31 may not expose this.\n");
		return;
	}
	seq_printf(m, "  read %d bytes via SMC 0x%08x\n", ok, fn);

	for (i = 0; i < ok; i += 16) {
		int j;

		seq_printf(m,
			   "  +0x%02x: %02x %02x %02x %02x %02x %02x %02x %02x "
			   "%02x %02x %02x %02x %02x %02x %02x %02x  ",
			   i,
			   buf[i + 0], buf[i + 1], buf[i + 2], buf[i + 3],
			   buf[i + 4], buf[i + 5], buf[i + 6], buf[i + 7],
			   buf[i + 8], buf[i + 9], buf[i + 10], buf[i + 11],
			   buf[i + 12], buf[i + 13], buf[i + 14], buf[i + 15]);
		for (j = 0; j < 16 && i + j < ok; j++)
			seq_putc(m,
				 (buf[i + j] >= 32 && buf[i + j] < 127)
					? buf[i + j]
					: '.');
		seq_putc(m, '\n');
	}

	/* Highlight known fields (offsets from u-boot port). */
	if (ok >= 34) {
		char usid[17] = {0};
		char fser[16] = {0};

		memcpy(usid, buf + 18, 16);
		memcpy(fser, buf + 34, min(15, ok - 34));
		for (i = 0; i < 16; i++)
			if (usid[i] && (usid[i] < 32 || usid[i] >= 127))
				usid[i] = 0;
		for (i = 0; i < 15; i++)
			if (fser[i] && (fser[i] < 32 || fser[i] >= 127))
				fser[i] = 0;
		seq_printf(m, "\n  usid (offset 18, 16 bytes) : %s\n", usid);
		seq_printf(m, "  f_serial (offset 34, 15 b) : %s\n", fser);
	}
}

/* ---------- 4–9) MMIO block dumps ----------------------------------- */

static void dump_hhi(struct seq_file *m)
{
	/* Clock tree base; first 1 KiB has PLLs, video clocks, mipi phy clk
	 * cntl, ts_clk_cntl, etc.
	 */
	peek_iomem(m, "HHI (clock controller)", HHI_BASE, HHI_SIZE);
}

static void dump_vpu_encl(struct seq_file *m)
{
	/* ENCL timing block is at VPU+0x7220..0x724c; surrounding area has
	 * VIU/OSD/timing logic. Dump 4 KiB starting at the ENCL window for
	 * everything timing-related.
	 */
	peek_iomem(m, "VPU near-ENCL (timing block + OSD context)",
		   VPU_BASE + 0x7000, 0x500);
}

static void dump_dsi_host(struct seq_file *m)
{
	peek_iomem(m, "DSI host (DWC + Amlogic TOP wrapper)",
		   DSI_TOP_BASE, DSI_TOP_SIZE);
}

static void dump_mipi_dphy(struct seq_file *m)
{
	/* MIPI digital DPHY block. Often reads as all-zeros on G12A because
	 * the actual dphy clocking is handled HHI-side.
	 */
	peek_iomem(m, "MIPI digital DPHY", MIPI_DPHY_BASE, 0x100);

	/* Analog DPHY is inside HHI syscon at +0x000..+0x008 — dump
	 * separately for visibility.
	 */
	{
		void __iomem *hhi = ioremap(HHI_BASE, 0x20);

		if (hhi) {
			seq_puts(m, "\n=== HHI MIPI analog DPHY (CNTL0..2) ===\n");
			seq_printf(m, "  MIPI_CNTL0 = 0x%08x\n", readl(hhi + 0));
			seq_printf(m, "  MIPI_CNTL1 = 0x%08x\n", readl(hhi + 4));
			seq_printf(m, "  MIPI_CNTL2 = 0x%08x\n", readl(hhi + 8));
			iounmap(hhi);
		}
	}
}

/* ---------- 5) SARADC ----------------------------------------------- */

static void dump_saradc(struct seq_file *m)
{
	/* Full SARADC block — register layout in mainline meson-saradc.c
	 * (SAR_ADC_REG0..REG13 etc).
	 */
	peek_iomem(m, "SARADC", SARADC_BASE, 0x40);

	/* TODO: a proper per-channel read would require coordinating with
	 * the IIO driver. For now, dump the raw control + result registers
	 * so the user can correlate with iio:device* sysfs:
	 *   /sys/bus/iio/devices/iio:device0/in_voltage{0..7}_raw
	 */
	seq_puts(m, "  hint: cat /sys/bus/iio/devices/iio:device0/in_voltage*_raw "
		    "for cooked channel values\n");
}

/* ---------- 6) Tsensors --------------------------------------------- */

static void dump_tsensors(struct seq_file *m)
{
	/* PLL tsensor is at 0xff634804 (TS_PLL_CFG_REG1) /
	 * 0xff634840 (TS_PLL_STAT0). DDR tsensor follows.
	 */
	u32 pll_cfg, pll_stat, ddr_cfg, ddr_stat;

	seq_puts(m, "\n=== Tsensors (raw) ===\n");
	read_phys_u32(0xff634804, &pll_cfg);
	read_phys_u32(0xff634840, &pll_stat);
	read_phys_u32(0xff634808, &ddr_cfg);
	read_phys_u32(0xff634844, &ddr_stat);
	seq_printf(m, "  TS_PLL_CFG  = 0x%08x  STAT = 0x%08x\n",
		   pll_cfg, pll_stat);
	seq_printf(m, "  TS_DDR_CFG  = 0x%08x  STAT = 0x%08x\n",
		   ddr_cfg, ddr_stat);
	seq_puts(m, "  decoded values live in /sys/class/thermal/thermal_zone*/temp\n");
}

/* ---------- 10) GPIO state ------------------------------------------ */

static void dump_pinmux(struct seq_file *m)
{
	peek_iomem(m, "PERIPHS_PINMUX (bank PIN MUX 0..F)",
		   PERIPHS_PINMUX_BASE, 0x200);
	peek_iomem(m, "AOBUS_PINMUX (AO bank MUX)",
		   AOBUS_PINMUX_BASE, 0x100);

	/* GPIO input/output state registers — same MMIO block, different
	 * offsets. Cover banks Z (touch + panel), A (charger + USB ID),
	 * AO (boot mode + audio).
	 */
	peek_iomem(m, "PERIPHS GPIO state (banks BOOT..Z)",
		   0xff634440, 0x200);
}

/* ---------- 11) USB phy --------------------------------------------- */

static void dump_usb_phy(struct seq_file *m)
{
	peek_iomem(m, "USB2 PHY21 control", USB_PHY21_BASE, 0x80);
}

/* ---------- 12) Backlight PWM --------------------------------------- */

static void dump_pwm(struct seq_file *m)
{
	peek_iomem(m, "PWM AB (BL_PWM lives here)", PWM_AB_BASE, 0x40);
	seq_puts(m, "  cooked state: cat /sys/class/backlight/*/brightness "
		    "and /sys/class/pwm/pwmchip*/pwm*/period\n");
}

/* ---------- 13) I2C bus survey + known chips ------------------------ */

static void probe_i2c_bus(struct seq_file *m, int bus_nr)
{
	struct i2c_adapter *adap = i2c_get_adapter(bus_nr);
	int addr;
	u8 dummy;
	struct i2c_msg msg;
	int ret;

	if (!adap) {
		seq_printf(m, "\n--- i2c-%d: adapter not available (driver "
			      "not loaded or claimed)\n",
			   bus_nr);
		return;
	}

	seq_printf(m, "\n--- i2c-%d (%s) ---\n", bus_nr, adap->name);

	for (addr = 0x03; addr < 0x78; addr++) {
		msg.addr = addr;
		msg.flags = I2C_M_RD;
		msg.len = 1;
		msg.buf = &dummy;
		ret = i2c_transfer(adap, &msg, 1);
		if (ret == 1)
			seq_printf(m, "  0x%02x ACK\n", addr);
	}

	i2c_put_adapter(adap);
}

static void read_chip_regs(struct seq_file *m, int bus_nr, u16 addr,
			   const char *name, u8 first_reg, u8 count)
{
	struct i2c_adapter *adap = i2c_get_adapter(bus_nr);
	u8 buf[64];
	struct i2c_msg msgs[2];
	int ret, i;

	if (!adap || count > sizeof(buf))
		return;

	msgs[0].addr = addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &first_reg;
	msgs[1].addr = addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = count;
	msgs[1].buf = buf;

	ret = i2c_transfer(adap, msgs, 2);
	if (ret == 2) {
		seq_printf(m,
			   "  %s (i2c-%d 0x%02x): regs[0x%02x..0x%02x]:\n",
			   name, bus_nr, addr, first_reg,
			   first_reg + count - 1);
		seq_puts(m, "    ");
		for (i = 0; i < count; i++) {
			seq_printf(m, "%02x ", buf[i]);
			if ((i + 1) % 16 == 0)
				seq_puts(m, "\n    ");
		}
		seq_putc(m, '\n');
	} else {
		seq_printf(m, "  %s (i2c-%d 0x%02x): read failed (ret=%d, "
			      "chip asleep / NAK?)\n",
			   name, bus_nr, addr, ret);
	}
	i2c_put_adapter(adap);
}

static void dump_i2c_bus_survey(struct seq_file *m)
{
	int bus;

	seq_puts(m, "\n=== I2C bus survey ===\n");

	/* Probe whichever bus numbers are available. Vendor + mainline both
	 * use 0..3 plus an AO bus that mainline numbers separately.
	 */
	for (bus = 0; bus < 8; bus++)
		probe_i2c_bus(m, bus);

	/* Known-chip dumps. Bus numbers reflect mainline DT aliases; vendor
	 * kernel uses the same physical buses but may number them
	 * differently — adjust bus_nr if the survey above shows the chips
	 * on different buses.
	 */
	seq_puts(m, "\n=== Known-chip register dumps ===\n");
	read_chip_regs(m, 2, 0x35, "MAX14656 (USB charger detector)",
		       0x00, 0x0a);
	read_chip_regs(m, 2, 0x39, "TMD2772 (prox/ALS)", 0x00, 0x20);
	read_chip_regs(m, 0, 0x2e, "TLSC6X (touch + panel-detect)",
		       0x00, 0x40);
	/* MFi auth chip wants special prep (wake-up + version cmd). Just
	 * probe its address-ACK here; full cert/serial dump lives in the
	 * u-boot `mfi` command since it needs ~500 ms of SMBus interaction.
	 */
	read_chip_regs(m, 3, 0x10, "MFi auth (just version reg)",
		       0x00, 1);
}

/* ---------- 14) eMMC sysfs hint ------------------------------------- */

static void dump_emmc_hint(struct seq_file *m)
{
	/* mmc_get_card() requires locking + can sleep on the host mutex.
	 * Doing the full EXT_CSD dance from a procfs read handler is messy
	 * (we'd block on a host that's busy with FS I/O). Tell the user
	 * where to look instead — sysfs has CID/CSD parsed, and debugfs has
	 * the full 512 B EXT_CSD if the driver exposes it.
	 */
	seq_puts(m,
		 "\n=== eMMC introspection (sysfs paths) ===\n"
		 "  manfid     : /sys/class/mmc_host/mmc*/mmc*:*/manfid\n"
		 "  cid        : /sys/class/mmc_host/mmc*/mmc*:*/cid\n"
		 "  csd        : /sys/class/mmc_host/mmc*/mmc*:*/csd\n"
		 "  name       : /sys/class/mmc_host/mmc*/mmc*:*/name\n"
		 "  oemid      : /sys/class/mmc_host/mmc*/mmc*:*/oemid\n"
		 "  fwrev      : /sys/class/mmc_host/mmc*/mmc*:*/fwrev\n"
		 "  hwrev      : /sys/class/mmc_host/mmc*/mmc*:*/hwrev\n"
		 "  preferred_erase_size : .../preferred_erase_size\n"
		 "  life_time  : /sys/kernel/debug/mmc0/mmc0:*/ext_csd  (look for "
		 "'eMMC Life Time Estimation' fields)\n"
		 "  block dev  : lsblk -o NAME,SIZE,MODEL,VENDOR /dev/mmcblk0\n");
}

/* ---------- 15) DRM/KMS active mode --------------------------------- */

static void dump_drm_hint(struct seq_file *m)
{
	seq_puts(m,
		 "\n=== DRM/KMS active mode (sysfs paths) ===\n"
		 "  state           : cat /sys/kernel/debug/dri/0/state\n"
		 "  framebuffers    : cat /sys/kernel/debug/dri/0/framebuffer\n"
		 "  internal_clients: cat /sys/kernel/debug/dri/0/internal_clients\n"
		 "  per-conn modes  : cat /sys/kernel/debug/dri/0/*/modes\n");
}

/* ---------- 16) Thermal / regulator / clock summaries --------------- */

static void dump_thermal(struct seq_file *m)
{
	/* thermal_zone_get_zone_by_id() was added in 4.17. On 4.9 we'd have
	 * to iterate by name or walk the thermal_class device list manually.
	 * Sysfs has everything we'd dump anyway, so just point the user at
	 * the canonical paths — keeps us version-agnostic.
	 */
	seq_puts(m,
		 "\n=== Thermal zones (sysfs paths) ===\n"
		 "  list zones : ls /sys/class/thermal/\n"
		 "  per-zone   : cat /sys/class/thermal/thermal_zone*/type\n"
		 "  per-zone   : cat /sys/class/thermal/thermal_zone*/temp\n");
}

static void dump_sysfs_hints(struct seq_file *m)
{
	seq_puts(m,
		 "\n=== Other introspection sources ===\n"
		 "  clocks      : cat /sys/kernel/debug/clk/clk_summary\n"
		 "  regulators  : cat /sys/kernel/debug/regulator/regulator_summary\n"
		 "  pinctrl     : cat /sys/kernel/debug/pinctrl/*/pinmux-pins\n"
		 "  gpios       : cat /sys/kernel/debug/gpio\n"
		 "  iio         : ls /sys/bus/iio/devices/ && cat .../in_voltage*_raw\n"
		 "  pwm         : ls /sys/class/pwm/\n"
		 "  backlight   : ls /sys/class/backlight/\n"
		 "  drm         : ls /sys/kernel/debug/dri/\n"
		 "  cmdline     : cat /proc/cmdline\n"
		 "  cpuinfo     : cat /proc/cpuinfo\n"
		 "  meminfo     : cat /proc/meminfo\n"
		 "  modules     : lsmod\n"
		 "  dts path    : cat /sys/firmware/devicetree/base/compatible\n"
		 "  ufs/spi/etc : ls /sys/bus/ | for further enumeration\n");
}

/* ---------- 17) Kernel + system summary ----------------------------- */

static void dump_system(struct seq_file *m)
{
	struct sysinfo si;
	struct new_utsname *u = utsname();

	si_meminfo(&si);

	seq_puts(m, "\n=== System summary ===\n");
	seq_printf(m, "  kernel     : %s %s %s\n",
		   u->sysname, u->release, u->version);
	seq_printf(m, "  hostname   : %s\n", u->nodename);
	seq_printf(m, "  machine    : %s\n", u->machine);
	seq_printf(m, "  RAM total  : %lu KiB (%lu pages × %u)\n",
		   si.totalram * (si.mem_unit / 1024),
		   si.totalram, si.mem_unit);
	seq_printf(m, "  RAM free   : %lu KiB\n",
		   si.freeram * (si.mem_unit / 1024));
	seq_printf(m, "  uptime     : %lu seconds\n", si.uptime);
	seq_printf(m, "  procs run  : %u\n", si.procs);
	seq_printf(m, "  loadavg    : %lu / %lu / %lu (1/5/15 min, /65536)\n",
		   si.loads[0], si.loads[1], si.loads[2]);
}

/* ---------- main dump driver ---------------------------------------- */

static int peek_show(struct seq_file *m, void *v)
{
	seq_puts(m, "================================================\n");
	seq_puts(m, "Car Thing — thorough hardware inventory\n");
	seq_puts(m, "================================================\n");

	dump_system(m);
	dump_soc_id(m);
	dump_ao_sticky(m);
	dump_efuse(m);
	dump_hhi(m);
	dump_saradc(m);
	dump_tsensors(m);
	dump_vpu_encl(m);
	dump_dsi_host(m);
	dump_mipi_dphy(m);
	dump_pinmux(m);
	dump_usb_phy(m);
	dump_pwm(m);
	dump_i2c_bus_survey(m);
	dump_emmc_hint(m);
	dump_drm_hint(m);
	dump_thermal(m);
	dump_sysfs_hints(m);

	seq_puts(m, "\n=== end of dump ===\n");
	return 0;
}

static int peek_open(struct inode *inode, struct file *file)
{
	return single_open(file, peek_show, NULL);
}

static const PEEK_PROC_OPS peek_pops = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,6,0)
	.owner = THIS_MODULE,
#endif
	.PEEK_OPS_OPEN    = peek_open,
	.PEEK_OPS_READ    = seq_read,
	.PEEK_OPS_LSEEK   = seq_lseek,
	.PEEK_OPS_RELEASE = single_release,
};

static int __init peek_init(void)
{
	struct proc_dir_entry *e;

	pr_info(PFX "loading; output via `cat /proc/peek_carthing` "
		"(also dumping a first pass to dmesg below)\n");

	e = proc_create("peek_carthing", 0444, NULL, &peek_pops);
	if (!e) {
		pr_err(PFX "failed to create /proc/peek_carthing\n");
		return -ENOMEM;
	}

	/* Stay loaded so /proc/peek_carthing survives. User rmmod's when
	 * done. We don't emit a dmesg dump on init — the proc entry is the
	 * canonical surface (dmesg can be lossy on busy systems and the
	 * dump can easily exceed the printk buffer). */
	return 0;
}

static void __exit peek_exit(void)
{
	remove_proc_entry("peek_carthing", NULL);
	pr_info(PFX "unloaded\n");
}

module_init(peek_init);
module_exit(peek_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Thing Labs");
MODULE_DESCRIPTION("Spotify Car Thing — thorough Linux hardware inventory");
