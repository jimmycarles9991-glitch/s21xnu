#!/bin/bash
# S21 (o1s, SM-G991N) Bluetooth: the BT half of the same Broadcom BCM4375 B1 as Jesvi's S20+.
#
# The HCI/UART bring-up, the ACL flow control, the vendor-command handling and the
# com.apple.uart.bluetooth kernel control are Jesvi's (MIT; XNU-derived parts APSL 2.0;
# brcmfmac-derived parts ISC), pinned to the same public commit as 0142 and checked by SHA-256.
# This script rewrites the Exynos 990 hardware facts for Exynos 2100 and removes the two APIs
# this XNU patch set does not have (its GPIO helper library and GIC v3 SPI attach): the reader
# polls the UART FIFO instead. The HCD is read from the mounted root by 0142's firmware loader.
# dxb runs patches/*.sh from the XNU source root before applying *.patch.
set -euo pipefail
[[ "${MACHINE_CONFIG:-}" == SWEET ]] || exit 0
[[ -f bsd/conf/files ]] || exit 1

JESVI_SHA=d16b3b98e34fd625dd931c98f19644f7498aab04
JESVI_RAW=https://raw.githubusercontent.com/jesvi-eulah/Jesvi-iOS/$JESVI_SHA/xnu/files
bt_tmp=$(mktemp -d)
trap 'rm -rf "$bt_tmp"' EXIT
curl -fsSL --retry 5 -o "$bt_tmp/redwood_exynos_bt.c" "$JESVI_RAW/bsd/dev/redwood_exynos_bt.c"
got=$(shasum -a 256 "$bt_tmp/redwood_exynos_bt.c" | cut -c1-64)
[[ "$got" == 7e85ceb3386e4d4da889a9920bca53678355e502a3c97dac26024649778b812c ]] || {
	echo "Jesvi redwood_exynos_bt.c: sha256 $got, want 7e85ceb3386e4d4da889a9920bca53678355e502a3c97dac26024649778b812c" >&2
	exit 1
}

cat > "$bt_tmp/prelude.c" <<'EOF_BT_PRELUDE'
/*
 * S21 (Exynos 2100) Bluetooth, adapted from Jesvi's S20+ redwood_exynos_bt.c.
 *
 * Hardware (Samsung's exynos2100.dts, cmucal-sfr.c, pinctrl-exynos-arm64.c and the o1s
 * t2s overlay; not the Exynos 990 values in the Jesvi source):
 *   UART1            0x10840000, DTS SPI 533 (GIC intid 565), USI v2, 256-byte FIFOs
 *   USI mode         SYSREG_PERIC1 0x10721000 +0x0: 1 = UART
 *   clocks           CMU_PERIC1 0x10700000: MUX_CLK_PERIC1_UART_BT +0x1000 (bits 1:0:
 *                    0 OSC 26 MHz, 1 IP0_USER, 2 IP1_USER), DIV +0x1800 (bits 3:0 + 1),
 *                    gates +0x2008/+0x203c/+0x2058, QCH +0x302c
 *   pins             gpq0-0 RXD, -1 TXD, -2 RTS, -3 CTS, function 2 at ALIVE pinctrl 0x15850000
 *                    +0x0; RTS held low as a GPIO, CTS honoured by the UART
 *   BT_EN            gpm8-0, CMGP pinctrl 0x14e30000 +0x100, function 1
 *   BT_WAKE          gpp14-0, PERIC0 pinctrl 0x10430000 +0xe0, function 1, held high
 *   HOST_WAKE        gpa2-3, unused
 *
 * This patch set has no gic_v3_spi_attach() and no redwood_gpio_* library, so the UART reader
 * polls at 1 ms while bytes move and 2 ms when idle; at the default 1 Mbaud the FIFO holds
 * about 2.5 ms. bt_baud=N raises the working speed, but 3 Mbaud overran the FIFO on the S20+.
 */
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern void RedwoodBluetoothStart(const uint8_t *address, uint32_t speed);

#define BT_GPIO_CON             0x00U
#define BT_GPIO_DAT             0x04U
#define BT_GPIO_PUD             0x08U
#define BT_GPIO_DRV             0x0cU
#define BT_GPIO_OUTPUT          1U
#define BT_GPIO_PULL_UP         1U

static volatile uint8_t *
bt_map(uint64_t pa, uint64_t sz)
{
	uint64_t base = pa & ~0x3fffULL, off = pa - base;
	vm_offset_t v = ml_io_map((vm_offset_t)base, (vm_size_t)((off + sz + 0x3fffULL) & ~0x3fffULL));

	return v ? (volatile uint8_t *)(v + off) : NULL;
}

static inline uint32_t
grd(volatile uint8_t *b, uint32_t o)
{
	return *(volatile uint32_t *)(b + o);
}

static inline void
gwr(volatile uint8_t *b, uint32_t o, uint32_t v)
{
	*(volatile uint32_t *)(b + o) = v;
}

static inline void
grmw(volatile uint8_t *b, uint32_t o, uint32_t clr, uint32_t set)
{
	gwr(b, o, (grd(b, o) & ~clr) | set);
}

static void
gpio_func(volatile uint8_t *bank, uint32_t pin, uint32_t func)
{
	uint32_t sh = pin * 4;

	grmw(bank, BT_GPIO_CON, 0xfU << sh, func << sh);
}

static void
gpio_pull(volatile uint8_t *bank, uint32_t pin, uint32_t pull)
{
	uint32_t sh = pin * 4;

	grmw(bank, BT_GPIO_PUD, 0xfU << sh, pull << sh);
}

static void
bt_msleep(uint32_t ms)
{
	delay((int)(ms * 1000));
}

/* Jesvi's live debug port (kern.redwood.poke) is not in this port; the boot-args below replace it. */
typedef void (*redwood_dbgport_fn)(const uint64_t *v, unsigned n, char *out, size_t cap);
static int
redwood_dbgport_add(const char *name, const char *help, redwood_dbgport_fn fn)
{
	(void)name; (void)help; (void)fn;
	return 0;
}
EOF_BT_PRELUDE

python3 - "$bt_tmp/redwood_exynos_bt.c" "$bt_tmp/prelude.c" bsd/dev/sweet_bt.c <<'EOF_BT_PY'
import sys

src, prelude, out = sys.argv[1:]
s = open(src, encoding='utf-8').read()
body = s[s.index('#define UART_PHYS'):]
s = open(prelude, encoding='utf-8').read() + body


def once(old, new):
    global s
    if s.count(old) != 1:
        raise SystemExit('BT adaptation: expected one occurrence of %r, found %d' % (old[:80], s.count(old)))
    s = s.replace(old, new, 1)


# The S21's interrupt is SPI 533 / GIC intid 565; this patch set uses the polled reader.
once('#define UART_INTID      (416 + 32)',
     '#define UART_INTID      (533 + 32)      /* S21 DTS: SPI 533, GIC intid 565; polled below */')
once('#define PINCTRL_CMGP    0x15c30000U',
     '#define PINCTRL_ALIVE   0x15850000U\n#define PINCTRL_CMGP    0x14e30000U\n#define PINCTRL_PERIC0  0x10430000U')

# 3 Mbaud fills the 256-byte RX FIFO in 0.85 ms; start at 1 Mbaud and let bt_baud=N opt in.
once('static uint32_t bt_fast = 3000000;     /* (back from 1 Mbaud: the chip sends ~100 KB/s of vendor events with audio) */',
     'static uint32_t bt_fast = 1000000;     /* S21: 3 Mbaud overruns the 256-byte FIFO; bt_baud=N can override */')

# Exynos 2100 clock setup: force the 26 MHz OSC parent and divide by one, whatever S-Boot left.
a = s.index('\tcmu = (volatile uint8_t *)ml_io_map(CMU_PERIC1, 0x4000);')
b = s.index('\t/* the USI as a UART', a)
s = s[:a] + '''\tcmu = bt_map(CMU_PERIC1, 0x4000);
\tsw_conf = bt_map(SW_CONF_PHYS, 0x4);
\tuart = bt_map(UART_PHYS, 0x1000);
\tif (cmu == NULL || sw_conf == NULL || uart == NULL) {
\t\tbt_log("bt: UART1 map failed\\n");
\t\treturn false;
\t}
\t/* the gates on (CG_VAL bit 21 with MANUAL bit 20: forced on, whatever automatic gating would do) */
\t{
\t\tstatic const uint32_t gates[] = { 0x2008, 0x203c, 0x2058 };

\t\tfor (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
\t\t\tvolatile uint32_t *g = (volatile uint32_t *)(cmu + gates[i]);

\t\t\t*g = *g | (1u << 21) | (1u << 20);
\t\t}
\t}
\t/* S21 MUX_CLK_PERIC1_UART_BT +0x1000 bits 1:0: 0 = OSCCLK_PERIC1 (26 MHz). DIV +0x1800 bits 3:0: /1. */
\t{
\t\tvolatile uint32_t *muxr = (volatile uint32_t *)(cmu + 0x1000);
\t\tvolatile uint32_t *divr = (volatile uint32_t *)(cmu + 0x1800);

\t\t*muxr = (*muxr & ~0x3U);
\t\t*divr = (*divr & ~0xfU);
\t\tfor (unsigned n = 0; n < 1000 && (*muxr & (1u << 16)); n++) {
\t\t\tdelay(1);
\t\t}
\t\tmux = *muxr;
\t\tdiv = *divr;
\t}
\tg1 = grd(cmu, 0x2008);
\tg2 = grd(cmu, 0x203c);
\tg3 = grd(cmu, 0x2058);
\tuart_clock_hz = 26000000U / ((div & 0xf) + 1);
\tbt_log("bt: UART1 clock S21: mux %08x (OSC 26 MHz), div %08x -> %u Hz; gates %08x %08x %08x; QCH %08x\\n", mux,
\t    div, uart_clock_hz, g1, g2, g3, grd(cmu, 0x302c));

''' + s[b:]

# S21 pins: gpq0 is at ALIVE +0x0 (not the S20+'s +0xa0). RTS is a low GPIO.
once('''\tbank_q0 = redwood_gpio_bank(REDWOOD_PINCTRL_ALIVE, 0xa0);
\tredwood_gpio_func(bank_q0, 0, 2);
\tredwood_gpio_func(bank_q0, 1, 2);
\tredwood_gpio_func(bank_q0, 3, 2);
\tpin_out(bank_q0, 2, 0);
\tredwood_gpio_pull(bank_q0, 0, REDWOOD_GPIO_PULL_UP);
\tredwood_gpio_pull(bank_q0, 3, REDWOOD_GPIO_PULL_UP);''',
     '''\tbank_q0 = bt_map(PINCTRL_ALIVE + 0x0, 0x20);\t/* gpq0 */
\tif (bank_q0 == NULL) {
\t\tbt_log("bt: gpq0 map failed\\n");
\t\treturn false;
\t}
\tgpio_func(bank_q0, 0, 2);\t/* RXD */
\tgpio_func(bank_q0, 1, 2);\t/* TXD */
\tgpio_func(bank_q0, 3, 2);\t/* CTS */
\tpin_out(bank_q0, 2, 0);\t\t/* RTS: always ready to receive */
\tgpio_pull(bank_q0, 0, BT_GPIO_PULL_UP);
\tgpio_pull(bank_q0, 3, BT_GPIO_PULL_UP);''')

# BT_EN = gpm8-0, BT_WAKE = gpp14-0 on the S21 (not gpm16-0 / gpg1-1).
once('''\tbank_m16 = redwood_gpio_bank(PINCTRL_CMGP, 0x200);
\tbank_g1 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC1, 0xc0);
\tpin_out(bank_g1, 1, 1);''',
     '''\tbank_m16 = bt_map(PINCTRL_CMGP + 0x100, 0x20);\t/* gpm8-0: BT_EN */
\tbank_g1 = bt_map(PINCTRL_PERIC0 + 0xe0, 0x20);\t/* gpp14-0: BT_WAKE */
\tif (bank_m16 == NULL || bank_g1 == NULL) {
\t\tbt_log("bt: BT_EN / BT_WAKE map failed\\n");
\t\treturn false;
\t}
\tpin_out(bank_g1, 0, 1);\t\t/* keep the chip awake */''')

# Rest of the Jesvi source: local GPIO helpers, root firmware, no IOSleep in this kernel.
s = s.replace('REDWOOD_GPIO_DAT', 'BT_GPIO_DAT')
s = s.replace('REDWOOD_GPIO_OUTPUT', 'BT_GPIO_OUTPUT')
s = s.replace('redwood_gpio_func', 'gpio_func')
s = s.replace('redwood_gpio_pull', 'gpio_pull')
s = s.replace('IOSleep(', 'bt_msleep(')
s = s.replace('no fw/%s in the image: the chip is not patched (userland/build.sh)',
              'no /fw/%s: the chip is not patched')

# No GIC v3 attach helper here; the reader below polls. The intid stays in the log for bring-up.
once('''\tif (gic_v3_spi_attach(UART_INTID, false, bt_irq, NULL) != KERN_SUCCESS) {
\t\tbt_log("bt: UART1's interrupt (GIC %u) not attached: polled every 2 ms\\n", UART_INTID);
\t}''',
     '''\tbt_log("bt: UART1 GIC %u not wired in this XNU patch set: polled reader\\n", UART_INTID);
\t(void)bt_irq;   /* the handler stays for a future GIC v3 attach */''')
once('''\t\tassert_wait_timeout((event_t)&bt_event, THREAD_UNINT, 100, NSEC_PER_MSEC);
\t\twhy = thread_block(THREAD_CONTINUE_NULL);''',
     '''\t\t/* Bounded polling: 1 ms while the ring moves, 2 ms idle. 1 Mbaud gives ~2.5 ms of FIFO. */
\t\tdelay_for_interval(ring_count() != 0 ? 1 : 2, NSEC_PER_MSEC);
\t\twhy = THREAD_AWAKENED;''')
once('kernel_thread_start_priority(bt_reader, NULL, 93, &reader)',
     'kernel_thread_start(bt_reader, NULL, &reader)')

# The HCD is on the mounted root, like the Wi-Fi firmware. Wait for it before patchram download.
once('''\tif (!bt_bringup()) {''',
     '''\tfor (unsigned n = 0; n < 600 && rootvnode == NULLVP; n++) {
\t\tdelay(100000);\t/* up to 60 s for the iOS root; the HCD is read from /fw */
\t}
\tif (!bt_bringup()) {''')

open(out, 'w', encoding='utf-8').write(s)
EOF_BT_PY

grep -q 'bsd/dev/sweet_bt.c' bsd/conf/files || printf 'bsd/dev/sweet_bt.c\t\tstandard\n' >> bsd/conf/files
