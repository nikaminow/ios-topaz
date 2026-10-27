/*
 * asteroids probe: a read-only scout for the Nothing Phone (3a) (SM7635).
 *
 * Started with `fastboot boot` (RAM only). It touches no MMIO at all: it
 * only reads the DTB that ABL passes in x0 and the CPU ID registers, draws
 * them into the splash framebuffer it finds in that DTB, and after a while
 * asks PSCI for a system reset (back to Android).
 *
 * v2: full memory map, every reserved-memory carve-out, the CPUs from the
 * DTB, the arch timer PPIs, and a PSCI CPU_ON round trip on each secondary
 * (it reports MIDR/MPIDR/MMFR0, then CPU_OFF). Shown as pages, one per
 * PAGE_S seconds, so each can be photographed.
 *
 * MMU and caches are off, so every access is Device memory: keep loads and
 * stores naturally aligned (-mstrict-align) and read the DTB one aligned
 * word at a time.
 */
#include <stdint.h>
#include "font.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_AFFINITY_INFO	0xc4000004UL
#define PSCI_SYSTEM_RESET	0x84000009UL

#define PAGE_S			45
#define NR_CPUS			8

/* ---- framebuffer console ---- */

#define SCALE	2
#define CH_W	(8 * SCALE)
#define CH_H	(16 * SCALE)
#define MARGIN	16

#define BG	0xff101830U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define CYAN	0xff00ffffU
#define RED	0xffff0000U
#define GREEN	0xff00ff00U
#define BLUE	0xff0000ffU

static volatile uint32_t *fb;
static uint32_t fb_w = 1080, fb_h = 2392, fb_stride = 1080 * 4;
static int row, col;

static void fill(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t c)
{
	for (uint32_t y = y0; y < y0 + h && y < fb_h; y++) {
		volatile uint32_t *line = fb + y * (fb_stride / 4);
		for (uint32_t x = x0; x < x0 + w && x < fb_w; x++)
			line[x] = c;
	}
}

static void putc_fb(char ch, uint32_t fg)
{
	if (ch == '\n') {
		row++;
		col = 0;
		return;
	}
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	uint32_t x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	col++;
	if (!fb || x0 + CH_W > fb_w || y0 + CH_H > fb_h)
		return;
	const unsigned char *g = font8x16[ch - 0x20];
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : BG;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint32_t *p = fb + (y0 + gy * SCALE + sy) * (fb_stride / 4)
						       + x0 + gx * SCALE;
				for (int sx = 0; sx < SCALE; sx++)
					p[sx] = c;
			}
		}
}

static void puts_c(const char *s, uint32_t fg)
{
	while (*s)
		putc_fb(*s++, fg);
}

/* exactly w columns: truncated or space-padded */
static void puts_w(const char *s, int w, uint32_t fg)
{
	int i = 0;
	for (; i < w && s && s[i]; i++)
		putc_fb(s[i], fg);
	for (; i < w; i++)
		putc_fb(' ', fg);
}

static void hex(uint64_t v, int digits, uint32_t fg)
{
	static const char h[] = "0123456789ABCDEF";
	for (int i = digits - 1; i >= 0; i--)
		putc_fb(h[(v >> (4 * i)) & 0xf], fg);
}

static void kv(const char *label, uint64_t v)
{
	puts_c(label, YELLOW);
	putc_fb(' ', WHITE);
	hex(v, 16, WHITE);
	putc_fb('\n', WHITE);
}

static void dec(uint32_t v)
{
	char b[10];
	int n = 0;
	do
		b[n++] = '0' + v % 10;
	while ((v /= 10) && n < 10);
	while (n)
		putc_fb(b[--n], WHITE);
}

/* clang lowers zero-initialised locals to memset */
void *memset(void *d, int c, unsigned long n)
{
	volatile unsigned char *p = d;
	while (n--)
		*p++ = (unsigned char)c;
	return d;
}

/* ---- timer / PSCI ---- */

static int use_hvc;

static uint64_t psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	if (use_hvc)
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
	__asm__ volatile("isb" ::: "memory");
	return rsr("cntpct_el0");
}

static void delay_ms(uint32_t ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void countdown(const char *what, uint32_t secs)
{
	int r = row;
	for (uint32_t s = secs; s > 0; s--) {
		row = r;
		col = 0;
		puts_c(what, CYAN);
		dec(s);
		puts_c(" s   ", CYAN);
		delay_ms(1000);
	}
}

static void __attribute__((noreturn)) reset_now(void)
{
	psci(PSCI_SYSTEM_RESET, 0, 0, 0);
	/* still here: PSCI refused. Hold Power (+Vol-) to restart. */
	row++;
	col = 0;
	puts_c("PSCI reset failed: hold POWER", RED);
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- DTB walk ---- */

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static uint64_t be64(uintptr_t a)
{
	return (uint64_t)be32(a) << 32 | be32(a + 4);
}

static int has_prefix(const char *s, const char *p)
{
	while (*p)
		if (*s++ != *p++)
			return 0;
	return 1;
}

static int contains(const char *s, uint32_t len, const char *needle)
{
	for (uint32_t i = 0; i < len && s[i]; i++) {
		uint32_t j = 0;
		while (needle[j] && i + j < len && s[i + j] == needle[j])
			j++;
		if (!needle[j])
			return 1;
	}
	return 0;
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

/* all ranges here assume #address-cells = #size-cells = 2 (checked on page 1) */
struct range {
	const char *name;	/* points into the DTB */
	uint64_t pa, size;
	int dyn;		/* reserved-memory node with "size" but no "reg" */
};

#define MAXMEM	32
#define MAXRSV	140
static struct range mem[MAXMEM], rsv[MAXRSV];
static int nmem, nrsv;

static const char *cpu_name[NR_CPUS];
static uint64_t cpu_reg[NR_CPUS];
static int ncpus;
static uint32_t timer_irq[12], ntimer;
static uint32_t root_ac, root_sc, rsv_ac, rsv_sc, cpus_ac;
static uint64_t splash_pa, splash_sz;

static const char *walk(uintptr_t dtb)
{
	if (!dtb || (dtb & 3))
		return "no DTB pointer";
	if (be32(dtb) != FDT_MAGIC)
		return "bad DTB magic";
	uintptr_t p = dtb + be32(dtb + 8);
	uintptr_t strings = dtb + be32(dtb + 12);
	uint32_t total = be32(dtb + 4);
	const char *stack[16] = { 0 };
	int depth = 0;
	int rsv_has_reg = 0;

	while (p < dtb + total) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *name = (const char *)p;
			if (depth < 16)
				stack[depth] = name;
			depth++;
			if (depth == 3 && streq(stack[1], "reserved-memory"))
				rsv_has_reg = 0;
			if (depth == 3 && streq(stack[1], "cpus") && has_prefix(name, "cpu@") && ncpus < NR_CPUS)
				cpu_name[ncpus++] = name;
			while (*(volatile char *)p)
				p++;
			p = (p + 4) & ~3UL;
		} else if (tok == FDT_END_NODE) {
			if (--depth <= 0)
				break;
		} else if (tok == FDT_PROP) {
			uint32_t len = be32(p), off = be32(p + 4);
			uintptr_t val = p + 8;
			const char *pn = (const char *)(strings + off);
			const char *name = depth >= 1 && depth <= 16 ? stack[depth - 1] : "";
			const char *parent = depth >= 2 && depth <= 16 ? stack[depth - 2] : "";

			if (depth == 1 && streq(pn, "#address-cells")) {
				root_ac = be32(val);
			} else if (depth == 1 && streq(pn, "#size-cells")) {
				root_sc = be32(val);
			} else if (depth == 2 && streq(name, "reserved-memory") && streq(pn, "#address-cells")) {
				rsv_ac = be32(val);
			} else if (depth == 2 && streq(name, "reserved-memory") && streq(pn, "#size-cells")) {
				rsv_sc = be32(val);
			} else if (depth == 2 && streq(name, "cpus") && streq(pn, "#address-cells")) {
				cpus_ac = be32(val);
			} else if (depth == 2 && streq(name, "psci") && streq(pn, "method")) {
				use_hvc = contains((const char *)val, len, "hvc");
			} else if (depth == 2 && has_prefix(name, "memory") && streq(pn, "reg")) {
				for (uint32_t o = 0; o + 16 <= len && nmem < MAXMEM; o += 16) {
					mem[nmem].name = name;
					mem[nmem].pa = be64(val + o);
					mem[nmem].size = be64(val + o + 8);
					nmem++;
				}
			} else if (depth == 2 && streq(name, "timer") && streq(pn, "interrupts")) {
				for (uint32_t o = 0; o + 12 <= len && ntimer < 12; o += 12)
					timer_irq[ntimer++] = be32(val + o + 4);	/* <type num flags> */
			} else if (depth == 3 && streq(parent, "cpus") && has_prefix(name, "cpu@") &&
				   streq(pn, "reg")) {
				cpu_reg[ncpus - 1] = len == 8 ? be64(val) : be32(val);
			} else if (depth == 3 && streq(parent, "reserved-memory") && streq(pn, "reg") &&
				   nrsv < MAXRSV) {
				for (uint32_t o = 0; o + 16 <= len && nrsv < MAXRSV; o += 16) {
					rsv[nrsv].name = name;
					rsv[nrsv].pa = be64(val + o);
					rsv[nrsv].size = be64(val + o + 8);
					rsv[nrsv].dyn = 0;
					nrsv++;
				}
				rsv_has_reg = 1;
				if (!splash_pa && contains(name, 64, "splash") && !contains(name, 64, "non_secure") &&
				    len >= 16) {
					splash_pa = be64(val);
					splash_sz = be64(val + 8);
				}
			} else if (depth == 3 && streq(parent, "reserved-memory") && streq(pn, "size") &&
				   !rsv_has_reg && nrsv < MAXRSV) {
				rsv[nrsv].name = name;
				rsv[nrsv].pa = 0;
				rsv[nrsv].size = len == 8 ? be64(val) : be32(val);
				rsv[nrsv].dyn = 1;
				nrsv++;
			}
			p = (val + len + 3) & ~3UL;
		} else if (tok != FDT_NOP) {
			break;
		}
	}
	return 0;
}

/* ---- secondaries ---- */

static volatile uint64_t sec_midr[NR_CPUS], sec_mpidr[NR_CPUS], sec_mmfr0[NR_CPUS];
static volatile uint64_t sec_el[NR_CPUS];

void secondary_main(uint64_t idx)
{
	if (idx < NR_CPUS) {
		sec_midr[idx] = rsr("midr_el1");
		sec_mmfr0[idx] = rsr("id_aa64mmfr0_el1");
		sec_el[idx] = rsr("CurrentEL") >> 2;
		__asm__ volatile("dsb sy" ::: "memory");
		sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 62);	/* "done" flag */
		__asm__ volatile("dsb sy" ::: "memory");
	}
	psci(PSCI_CPU_OFF, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}

extern char secondary_entry[];

/* ---- exceptions ---- */

void exc_handler(uint64_t slot, uint64_t *regs)
{
	(void)regs;
	row += 1;
	col = 0;
	puts_c("EXCEPTION slot ", RED);
	dec((uint32_t)slot);
	puts_c(" cpu ", RED);
	hex(rsr("mpidr_el1"), 8, WHITE);
	putc_fb('\n', WHITE);
	kv("ESR ", rsr("esr_el1"));
	kv("ELR ", rsr("elr_el1"));
	kv("FAR ", rsr("far_el1"));
	countdown("REBOOT to Android in ", 60);
	reset_now();
}

/* ---- pages ---- */

static void page_start(int n, int total)
{
	fill(0, 0, fb_w, fb_h, BG);
	row = 0;
	col = 0;
	puts_c("ASTEROIDS PROBE v2  page ", CYAN);
	dec(n);
	putc_fb('/', CYAN);
	dec(total);
	puts_c("  (RAM only, read-only)\n", CYAN);
}

static void range_line(const struct range *r, int namew)
{
	puts_w(r->name, namew, GREEN);
	putc_fb(' ', WHITE);
	if (r->dyn)
		puts_c("   dynamic", YELLOW);
	else
		hex(r->pa, 10, WHITE);
	putc_fb(' ', WHITE);
	hex(r->size, 9, WHITE);
	putc_fb('\n', WHITE);
}

void shim_main(uint64_t dtb)
{
	const char *err = walk(dtb);

	if (!splash_pa) {
		/* Nowhere to draw: reboot quickly so the user sees "nothing found". */
		delay_ms(15000);
		reset_now();
	}
	fb = (volatile uint32_t *)(uintptr_t)splash_pa;
	if (splash_sz / fb_stride < fb_h)
		fb_h = splash_sz / fb_stride;
	int rows = (fb_h - 2 * MARGIN) / CH_H;

	/* SMP round trip first, so page 1 can show it */
	uint64_t self = rsr("mpidr_el1") & 0xff00ffffffUL;
	int64_t on_ret[NR_CPUS] = { 0 };
	for (int i = 0; i < ncpus; i++) {
		if ((cpu_reg[i] & 0xff00ffffffUL) == self) {
			sec_mpidr[i] = rsr("mpidr_el1") | (1ULL << 62);
			sec_midr[i] = rsr("midr_el1");
			sec_mmfr0[i] = rsr("id_aa64mmfr0_el1");
			sec_el[i] = rsr("CurrentEL") >> 2;
			continue;
		}
		on_ret[i] = (int64_t)psci(PSCI_CPU_ON, cpu_reg[i], (uintptr_t)secondary_entry, i);
		delay_ms(50);
	}
	delay_ms(500);

	/* a page holds rows-3 entries: title, header and countdown take the rest */
	int rsv_pages = (nrsv + rows - 4) / (rows - 3);
	int total = 1 + (rsv_pages ? rsv_pages : 1);

	/* page 1: CPUs, timer, memory */
	page_start(1, total);
	if (err) {
		puts_c(err, RED);
		putc_fb('\n', WHITE);
	}
	puts_c("cells root ", YELLOW);
	dec(root_ac);
	putc_fb('/', WHITE);
	dec(root_sc);
	puts_c(" rsv ", YELLOW);
	dec(rsv_ac);
	putc_fb('/', WHITE);
	dec(rsv_sc);
	puts_c(" cpus ", YELLOW);
	dec(cpus_ac);
	puts_c(" psci ", YELLOW);
	puts_c(use_hvc ? "hvc" : "smc", WHITE);
	putc_fb('\n', WHITE);
	kv("PSCI ver", psci(PSCI_VERSION, 0, 0, 0));
	putc_fb('\n', WHITE);

	puts_c("cpu  dt-reg     on   midr     mpidr    tg16 EL\n", YELLOW);
	for (int i = 0; i < ncpus; i++) {
		dec(i);
		puts_c("    ", WHITE);
		hex(cpu_reg[i], 8, WHITE);
		putc_fb(' ', WHITE);
		hex((uint64_t)on_ret[i], 4, on_ret[i] ? RED : WHITE);
		putc_fb(' ', WHITE);
		if (sec_mpidr[i]) {
			hex(sec_midr[i], 8, WHITE);
			putc_fb(' ', WHITE);
			hex(sec_mpidr[i] & 0xffffffffUL, 8, WHITE);
			putc_fb(' ', WHITE);
			hex((sec_mmfr0[i] >> 20) & 0xf, 1, WHITE);
			puts_c("    ", WHITE);
			dec((uint32_t)sec_el[i]);
		} else {
			puts_c("no answer", RED);
		}
		putc_fb('\n', WHITE);
	}
	putc_fb('\n', WHITE);

	puts_c("timer PPIs", YELLOW);
	for (uint32_t i = 0; i < ntimer; i++) {
		putc_fb(' ', WHITE);
		dec(timer_irq[i]);
	}
	putc_fb('\n', WHITE);
	putc_fb('\n', WHITE);

	puts_c("memory: ", YELLOW);
	dec(nmem);
	puts_c(" ranges   (pa, size)\n", YELLOW);
	uint64_t total_mem = 0;
	for (int i = 0; i < nmem; i++) {
		putc_fb(' ', WHITE);
		hex(mem[i].pa, 10, WHITE);
		putc_fb(' ', WHITE);
		hex(mem[i].size, 10, WHITE);
		putc_fb('\n', WHITE);
		total_mem += mem[i].size;
	}
	puts_c("total MiB ", YELLOW);
	dec((uint32_t)(total_mem >> 20));
	putc_fb('\n', WHITE);
	putc_fb('\n', WHITE);
	countdown("next page in ", PAGE_S);

	/* pages 2..: reserved-memory carve-outs */
	int i = 0;
	for (int pg = 2; pg <= total; pg++) {
		page_start(pg, total);
		puts_c("reserved-memory: ", YELLOW);
		dec(nrsv);
		puts_c("   (pa, size)\n", YELLOW);
		for (int r = 0; r < rows - 3 && i < nrsv; r++, i++)
			range_line(&rsv[i], 36);
		if (pg == total)
			countdown("REBOOT to Android in ", PAGE_S);
		else
			countdown("next page in ", PAGE_S);
	}
	reset_now();
}
