// SPDX-License-Identifier: GPL-2.0
/*
 * peek_mmio — minimal Car Thing MMIO register dumper.
 *
 * Companion to peek-userspace.sh, which covers everything sysfs +
 * i2c-tools + /proc can reach. THIS module covers the gap on stock
 * vendor kernels with CONFIG_DEVMEM=n: raw MMIO reads of the HHI
 * clock controller, VPU/ENCL display block, DSI host, AO sticky
 * regs, and the analog MIPI DPHY triplet — none of which userspace
 * can touch when /dev/mem is disabled.
 *
 * Deliberately minimal:
 *
 *   - Uses ONLY printk + ioremap/iounmap + a few SMC calls. That keeps
 *     the modversions surface tiny — fewer mismatches when building
 *     against a kernel-common whose .config doesn't exactly match the
 *     running kernel's. (Out-of-tree CRCs are derived from struct
 *     layouts which are CONFIG-sensitive.)
 *   - No /proc entry — output goes to dmesg.
 *   - Dumps on init, returns -EBUSY so the module doesn't stay loaded.
 *     (`rmmod` not needed.)
 *   - No I2C — userspace `i2cdetect` + `i2cdump` already do this fine.
 *
 * Compiled with -fno-instrument-functions / no -pg to avoid pulling
 * in _mcount, which the stock buildroot kernel doesn't export.
 *
 * Build: see ./Makefile.mmio. Load: `insmod peek_mmio.ko` then
 * `dmesg | grep peek_mmio:`. Module immediately exits with -EBUSY.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/printk.h>
#include <linux/arm-smccc.h>

#define PFX "peek_mmio: "

/* G12A MMIO addresses */
#define HHI_BASE		0xff63c000
#define VPU_BASE		0xff900000
#define DSI_TOP_BASE		0xffd07000
#define MIPI_DPHY_BASE		0xffd44000
#define AO_BASE			0xff800000
#define USB_PHY21_BASE		0xff63a000
#define SARADC_BASE		0xff809000
#define PERIPHS_PINMUX_BASE	0xff634480
#define PWM_AB_BASE		0xffd1b000

/* G12A vendor SMC: eFuse user-area read by byte */
#define EFUSE_SMC_FN_VENDOR	0x82000030
#define EFUSE_SMC_FN_MAINLINE	0x82000041

/* Dump `bytes` bytes from physical `base` to dmesg via 16-byte rows. */
static void dump_block(const char *label, phys_addr_t base, size_t bytes)
{
	void __iomem *io;
	size_t off;
	u32 w0, w1, w2, w3;

	io = ioremap(base, bytes);
	if (!io) {
		pr_info(PFX "%s @ 0x%llx — ioremap FAILED\n",
			label, (unsigned long long)base);
		return;
	}
	pr_info(PFX "=== %s @ 0x%llx (%zu B) ===\n", label,
		(unsigned long long)base, bytes);
	for (off = 0; off < bytes; off += 16) {
		w0 = readl(io + off + 0);
		w1 = readl(io + off + 4);
		w2 = readl(io + off + 8);
		w3 = readl(io + off + 12);
		pr_info(PFX "  +0x%04zx: %08x %08x %08x %08x\n",
			off, w0, w1, w2, w3);
	}
	iounmap(io);
}

static void dump_soc_id(void)
{
	u64 midr;

	asm volatile("mrs %0, midr_el1" : "=r"(midr));
	pr_info(PFX "MIDR_EL1 = 0x%016llx (impl=0x%02x part=0x%03x rev=0x%x)\n",
		midr,
		(u8)(midr >> 24),
		(u16)((midr >> 4) & 0xfff),
		(u8)(midr & 0xf));
	/* AO_SEC_GP_CFG0 lives in the secure AO domain — readl from EL1
	 * faults. Boot source is exposed through /proc/cmdline anyway. */
}

/* Try to read efuse_max bytes from offset 0 via SMC. Two known
 * function-ID conventions (vendor pre-2020 vs mainline). */
__attribute__((unused))
static void dump_efuse(void)
{
	u8 buf[256];
	int i, ok;
	u32 fn = EFUSE_SMC_FN_VENDOR;
	struct arm_smccc_res res;

	memset(buf, 0, sizeof(buf));
	for (i = 0; i < (int)sizeof(buf); i++) {
		arm_smccc_smc(fn, i, 1, 0, 0, 0, 0, 0, &res);
		if (res.a0 == 0 || res.a0 == (u64)-1)
			break;
		buf[i] = (u8)(res.a0 & 0xff);
	}
	ok = i;
	if (ok == 0) {
		fn = EFUSE_SMC_FN_MAINLINE;
		for (i = 0; i < (int)sizeof(buf); i++) {
			arm_smccc_smc(fn, i, 1, 0, 0, 0, 0, 0, &res);
			if (res.a0 == 0 || res.a0 == (u64)-1)
				break;
			buf[i] = (u8)(res.a0 & 0xff);
		}
		ok = i;
	}
	if (ok == 0) {
		pr_info(PFX "eFuse: no SMC ID worked\n");
		return;
	}
	pr_info(PFX "=== eFuse user area (%d bytes via SMC 0x%08x) ===\n",
		ok, fn);
	for (i = 0; i < ok; i += 16) {
		pr_info(PFX
			"  +0x%02x: %02x %02x %02x %02x %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x %02x %02x %02x\n",
			i,
			buf[i + 0], buf[i + 1], buf[i + 2], buf[i + 3],
			buf[i + 4], buf[i + 5], buf[i + 6], buf[i + 7],
			buf[i + 8], buf[i + 9], buf[i + 10], buf[i + 11],
			buf[i + 12], buf[i + 13], buf[i + 14], buf[i + 15]);
	}
}

static int __init peek_init(void)
{
	pr_info(PFX "=== Car Thing MMIO dump (companion to peek-userspace.sh) ===\n");

	dump_soc_id();
	/* eFuse SMC read: skipped on stock kernel. Stock BL31 doesn't
	 * respond to our function IDs and may hang the calling CPU,
	 * leaving insmod un-completable and the module stuck in
	 * [permanent] Loading state until reboot. eFuse contents are
	 * available indirectly via env (set by u-boot misc_init_r). */
	pr_info(PFX "eFuse SMC read: skipped (stock BL31 SMC ID unknown).\n");

	/* AO_SEC_SD_CFG0..15 (0xff8000c0) and AO_SEC_GP_CFG (0xff800300):
	 * SKIPPED. Secure-world only on G12A — readl() from EL1 raises an
	 * SError external abort. Boot source is decoded indirectly from
	 * MIDR / dmesg above. Reboot reason needs SMC mediation, which we
	 * skip for now since we don't have a known SMC ID for it. */

	dump_block("HHI clock controller (PLLs, dividers, MIPI clk cntl)",
		   HHI_BASE, 0x400);
	dump_block("MHU mailbox (SCP coprocessor IPC)",     0xff63c400, 0x80);
	dump_block("DMC / DDR controller",                  0xff638000, 0x200);
	dump_block("Mali Bifrost GPU (Mali-G31 product 0x7093, id/freq/status)",
		   0xffe40000, 0x100);
	dump_block("eMMC controller (mmc@ffe07000) — live bus state",
		   0xffe07000, 0x100);
	dump_block("Audio bus (PDM mics, TDM, TODDR ringbufs)",
		   0xff642000, 0x200);
	dump_block("t9015 internal audio codec",            0xff632000, 0x100);
	dump_block("PWM EF",                                0xffd19000, 0x40);
	dump_block("AO bus PWM (separate from CBUS PWM)",   0xff802000, 0x40);
	dump_block("DWC3 USB3 controller (events, GSTS)",   0xff500000, 0x100);
	dump_block("USB2 PHY (ffe09000)",                   0xffe09000, 0x80);
	dump_block("Reset controller",                      0xff634404, 0x40);
	dump_block("VPU near-ENCL (display timing block)",
		   VPU_BASE + 0x7000, 0x500);
	dump_block("DSI host (DWC + Amlogic TOP wrapper)",
		   DSI_TOP_BASE, 0x400);
	dump_block("MIPI digital DPHY",                     MIPI_DPHY_BASE, 0x100);
	dump_block("PERIPHS_PINMUX",                        PERIPHS_PINMUX_BASE, 0x200);
	dump_block("PERIPHS GPIO state (banks BOOT..Z)",    0xff634440, 0x200);
	dump_block("SARADC",                                SARADC_BASE, 0x40);
	dump_block("USB2 PHY21 control",                    USB_PHY21_BASE, 0x80);
	dump_block("PWM AB (BL_PWM channel A)",             PWM_AB_BASE, 0x40);

	/* Tsensors raw — small, single regs. */
	{
		void __iomem *p1 = ioremap(0xff634800, 0x60);
		if (p1) {
			pr_info(PFX "=== Tsensors (raw) ===\n");
			pr_info(PFX "  TS_PLL_CFG  = 0x%08x  STAT = 0x%08x\n",
				readl(p1 + 0x04), readl(p1 + 0x40));
			pr_info(PFX "  TS_DDR_CFG  = 0x%08x  STAT = 0x%08x\n",
				readl(p1 + 0x08), readl(p1 + 0x44));
			iounmap(p1);
		}
	}

	pr_info(PFX "=== end of dump ===\n");
	return -EBUSY;  /* don't stay loaded — one-shot tool */
}

static void __exit peek_exit(void) { }

module_init(peek_init);
module_exit(peek_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Spotify Car Thing — MMIO register dump (minimal)");
