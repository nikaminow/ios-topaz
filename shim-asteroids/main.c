/*
 * asteroids shim: boot xnu on the Nothing Phone (3a) (SM7635) from RAM.
 *
 *   fastboot boot shim-boot.img -> ABL enters us at EL1 on CPU 0x000
 *   -> framebuffer, memory banks and GIC from the ABL DTB
 *   -> GIC prepared for xnu (as on topaz) -> loader_boot() -> xnu
 *
 * Unlike the topaz shim there is no gold hand-off (every core here has the
 * 16K granule) and no UFS, cpufreq, IMEM or PS_HOLD access: the only MMIO
 * written is the GICv3 the DTB names. Anything unexpected ends in an
 * on-screen message and a PSCI reset back to Android.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"

#define PSCI_SYSTEM_RESET	0x84000009UL

#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define VTIMER_INTID		27	/* milos.dtsi: <GIC_PPI 11 ...> */
#define PTIMER_INTID		30	/* <GIC_PPI 14 ...> */
#define SGI_INTID		1

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

static struct board board;
static const char *stage;	/* set at runtime: no absolute pointers in data */

/* clang lowers zero-initialised locals to memset */
void *memset(void *d, int c, unsigned long n)
{
	volatile unsigned char *p = d;
	while (n--)
		*p++ = (unsigned char)c;
	return d;
}

static uint64_t psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	if (board.psci_hvc)
		__asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
				 "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
				 "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	else
		__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
				 "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
				 "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_hex2("RESET", "in s=", after_s, 0, 0);
	delay_ms(after_s * 1000);
	psci(PSCI_SYSTEM_RESET, 0, 0, 0);
	con_str("RESET", "PSCI refused: hold POWER");
	for (;;)
		__asm__ volatile("wfe");
}

/* Is [pa, pa + size) covered by /memory banks, adjacent ones joined? */
int ram_ok(uint64_t pa, uint64_t size)
{
	uint64_t at = pa, end = pa + size;
	for (int pass = 0; pass < board.nmem && at < end; pass++) {
		int moved = 0;
		for (int i = 0; i < board.nmem; i++)
			if (at >= board.mem_pa[i] && at < board.mem_pa[i] + board.mem_size[i]) {
				at = board.mem_pa[i] + board.mem_size[i];
				moved = 1;
			}
		if (!moved)
			return 0;
	}
	return at >= end;
}

/* ---- exceptions ---- */

void exc_handler(uint64_t slot, uint64_t *regs)
{
	con_clear(BLUE);
	con_str("PANIC", "shim exception");
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	reset(60);
}

void secondary_main(uint64_t idx)
{
	(void)idx;
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- GIC: the setup topaz found xnu needs before it starts ---- */

static void gicr_prepare(uintptr_t r)
{
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	/* a thread whose wakeup timer is armed on a secondary must wake (topaz) */
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
}

static void gic_prepare(void)
{
	stage = "gic";
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg interface");
		return;
	}
	uint32_t dctlr = rd32(board.gicd);
	/* Non-secure view: EnableGrp1, EnableGrp1A, ARE_NS (what Linux writes) */
	wr32(board.gicd, dctlr | 0x13);
	for (int i = 0; i < 1000000 && (rd32(board.gicd) & (1U << 31)); i++)
		;
	con_hex2("GICD", "was=", dctlr, "now=", rd32(board.gicd));

	/* every core's redistributor (xnu starts the others with PSCI itself) */
	int n = 0;
	for (uintptr_t off = 0; off < board.gicr_size; off += board.gicr_stride) {
		uintptr_t r = board.gicr + off;
		uint64_t typer = rd64(r + GICR_TYPER);
		gicr_prepare(r);
		n++;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	con_hex2("GICR", "frames=", n, "stride=", board.gicr_stride);

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
}

/* ---- main ---- */

void shim_main(uint64_t dtb)
{
	stage = "dtb";
	const char *err = fdt_board(dtb, &board);
	if (err || !board.splash_pa) {
		/* nowhere to draw: reboot quickly so a black screen means "no DTB" */
		delay_ms(15000);
		psci(PSCI_SYSTEM_RESET, 0, 0, 0);
		for (;;)
			__asm__ volatile("wfe");
	}
	fb_base = board.splash_pa;
	fb_w = 1080;
	fb_stride = 1080 * 4;
	fb_h = board.splash_size / fb_stride < 2392 ? board.splash_size / fb_stride : 2392;

	con_clear(NAVY);
	con_str("SHIM", "asteroids: xnu loader (RAM only)");
	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1");
	con_hex2("MIDR", "el1=", (uint32_t)rsr("midr_el1"), "mpidr=", (uint32_t)rsr("mpidr_el1"));
	con_hex2("MMFR0", "lo=", (uint32_t)mmfr0, "tg16=", (mmfr0 >> 20) & 0xf);
	con_hex2("FB", "pa=", (uint32_t)fb_base, "h=", fb_h);
	con_hex2("MEM", "banks=", board.nmem, "cpus=", board.ncpus);
	if (((mmfr0 >> 20) & 0xf) == 0) {
		con_str("STOP", "no 16K granule on the boot core");
		reset(60);
	}
	if ((rsr("mpidr_el1") & 0xffff) != 0) {
		con_str("STOP", "ABL did not start us on core 0x000");
		reset(60);
	}
	if (!board.gicd) {
		con_str("STOP", "no arm,gic-v3 in the DTB");
		con_str("IC0", board.ic_compat[0] ? board.ic_compat[0] : "-");
		con_str("IC1", board.ic_compat[1] ? board.ic_compat[1] : "-");
		reset(60);
	}
	/* The ABL DTB may omit redistributor-stride: GICv4 frames are 0x40000. */
	if (board.gicr_stride == 0x20000 && board.ncpus && board.gicr_size / board.ncpus == 0x40000)
		board.gicr_stride = 0x40000;
	con_hex2("GIC", "d=", (uint32_t)board.gicd, "r=", (uint32_t)board.gicr);
	gic_prepare();

	con_skip();
	stage = "loader";
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	reset(60);
}
