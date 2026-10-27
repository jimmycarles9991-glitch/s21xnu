#!/bin/bash
# S21+ (Exynos 2100) DWC3 / USB2 PHY probe for the SWEET XNU build.
# dxb runs patches/*.sh from the XNU source root before applying *.patch.
# This adds bsd/dev/sweet_s21usb.c and its bsd/conf/files line. The call is
# added by 0099-s21-usb-hook.patch (it must apply after 0094).
set -euo pipefail

[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
[[ -f bsd/conf/files ]] || exit 1

cat > bsd/dev/sweet_s21usb.c <<'EOF'
/*
 * S21+ (Exynos 2100) USB bring-up, step 1: look, then (optionally) power the PHY.
 *
 * DWC3 at 0x10e00000, USB2 PHY control block at 0x10c00000, PMU 0x15860000
 * (isolation: +0x72c bit 0, set = released). The PHY sequence below is a
 * line-for-line port of t2s_usbphy.c (phy_exynos_usb_v3p1_enable(), main 0x300,
 * minor 1, high-speed only), which was proven on this hardware under Linux.
 *
 * Boot argument (nothing is touched without it):
 *   s21usb=1   read only: print PHY/PMU/DWC3 registers, then panic with a summary
 *   s21usb=2   release isolation + run the PHY power-on sequence, read the
 *              same registers again, then panic with both snapshots
 *
 * Every access is a single aligned 32-bit read or write: reading several words
 * past a mapped register bus-faulted on this SoC (async SError panics the kernel).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
void s21usb_init(void);

#define S21_DWC3_BASE   0x10e00000U
#define S21_PHY_BASE    0x10c00000U
#define S21_PMU_BASE    0x15860000U

/* DWC3 global / device registers (offsets from the core base) */
#define DWC3_GCTL       0xc110U
#define DWC3_GSNPSID    0xc120U
#define DWC3_DCTL       0xc704U
#define DWC3_DSTS       0xc70cU

/* PHY control block */
#define PHY_VER         0x00U
#define PHY_LINKCTRL    0x04U
#define PHY_CLKRST      0x20U
#define PHY_UTMI        0x50U
#define PHY_HSP         0x54U
#define PHY_HSP_TEST    0x5cU
#define PMU_USB_ISOL    0x72cU

#define LINKCTRL_FORCE_QACT     (1U << 8)
#define LINKCTRL_QCH_WA         ((1U << 9) | (1U << 10) | (1U << 11) | (1U << 12))
#define LINKCTRL_BVALID_FILT    (0xfU << 4)
#define CLKRST_PHY20_SW_RST     (1U << 2)
#define CLKRST_PHY30_SW_RST     (1U << 3)
#define CLKRST_PHY20_RST_SEL    (1U << 12)
#define CLKRST_PHY30_RST_SEL    (1U << 13)
#define CLKRST_LINK_SW_RST      (1U << 1)
#define CLKRST_POR_HIGH         (CLKRST_PHY30_RST_SEL | CLKRST_PHY20_RST_SEL | \
                                 CLKRST_PHY30_SW_RST | CLKRST_PHY20_SW_RST)
#define UTMI_FORCE_MASK         0xfU
#define UTMI_FORCE_VBUSVALID    (0x3U << 4)
#define HSP_COMMONONN           (1U << 8)
#define HSP_EN_UTMISUSPEND      (1U << 9)
#define HSP_VBUSVLDEXT          (0x3U << 12)
#define HSP_TEST_SIDDQ          (1U << 24)

static vm_offset_t dwc_va, phy_va, pmu_va;
static char summary[900];

static void
usb_note(const char *fmt, ...) __printflike(1, 2);
static void
usb_note(const char *fmt, ...)
{
	char line[200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("s21usb: %s\n", line);
	if (strlen(summary) + strlen(line) + 3 < sizeof(summary)) {
		strlcat(summary, line, sizeof(summary));
		strlcat(summary, "; ", sizeof(summary));
	}
}

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

static inline void
rmw(vm_offset_t base, uint32_t off, uint32_t clr, uint32_t set)
{
	wr(base, off, (rd(base, off) & ~clr) | set);
}

static void
usb_sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
snapshot(const char *tag)
{
	usb_note("%s phy ver %08x link %08x clkrst %08x utmi %08x hsp %08x test %08x pmu72c %08x",
	    tag, rd(phy_va, PHY_VER), rd(phy_va, PHY_LINKCTRL), rd(phy_va, PHY_CLKRST),
	    rd(phy_va, PHY_UTMI), rd(phy_va, PHY_HSP), rd(phy_va, PHY_HSP_TEST),
	    rd(pmu_va, PMU_USB_ISOL));
	usb_note("%s dwc3 gsnpsid %08x gctl %08x dctl %08x dsts %08x",
	    tag, rd(dwc_va, DWC3_GSNPSID), rd(dwc_va, DWC3_GCTL),
	    rd(dwc_va, DWC3_DCTL), rd(dwc_va, DWC3_DSTS));
}

/* t2s_usbphy.c: t2s_usbphy_init() + t2s_usbphy_power_on() */
static void
phy_power_on(void)
{
	rmw(pmu_va, PMU_USB_ISOL, 0, 1U);                     /* release isolation */
	usb_sleep_us(1500);

	rmw(phy_va, PHY_LINKCTRL, LINKCTRL_FORCE_QACT, LINKCTRL_QCH_WA);
	rmw(phy_va, PHY_LINKCTRL, 0, LINKCTRL_FORCE_QACT);
	rmw(phy_va, PHY_CLKRST, 0, CLKRST_POR_HIGH);          /* POR asserted */
	rmw(phy_va, PHY_UTMI, UTMI_FORCE_MASK, 0);
	rmw(phy_va, PHY_HSP, 0, HSP_EN_UTMISUSPEND | HSP_COMMONONN);
	usb_sleep_us(1500);

	rmw(phy_va, PHY_LINKCTRL, 0, LINKCTRL_BVALID_FILT);   /* no VBUS pad */
	rmw(phy_va, PHY_UTMI, 0, UTMI_FORCE_VBUSVALID);
	rmw(phy_va, PHY_HSP, 0, HSP_VBUSVLDEXT);

	rmw(phy_va, PHY_HSP_TEST, HSP_TEST_SIDDQ, 0);         /* analog PHY out of SIDDQ */
	usb_sleep_us(1500);

	rmw(phy_va, PHY_CLKRST, 0, CLKRST_PHY20_RST_SEL | CLKRST_PHY20_SW_RST);
	rmw(phy_va, PHY_CLKRST, CLKRST_PHY30_RST_SEL | CLKRST_PHY30_SW_RST | CLKRST_LINK_SW_RST, 0);
	usb_sleep_us(1500);
}

void
s21usb_init(void)
{
	int mode = 0;

	if (!PE_parse_boot_argn("s21usb", &mode, sizeof(mode)) || mode == 0) {
		return;
	}
	dwc_va = ml_io_map(S21_DWC3_BASE, 0x10000);
	phy_va = ml_io_map(S21_PHY_BASE, 0x100);
	pmu_va = ml_io_map(S21_PMU_BASE, 0x1000);

	snapshot("before");
	if (mode >= 2) {
		phy_power_on();
		snapshot("after");
	}
	panic("s21usb: %s", summary);
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
EOF

grep -q 'sweet_s21usb.c' bsd/conf/files ||
    printf 'bsd/dev/sweet_s21usb.c\t\t\tstandard\n' >> bsd/conf/files
echo "0099: bsd/dev/sweet_s21usb.c staged"
