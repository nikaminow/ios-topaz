/*
 * Flattened-device-tree reading for the ABL DTB: /chosen (initrd), the
 * splash framebuffer, memory banks, the GICv3 and PSCI. Big-endian,
 * 4-byte aligned tokens; MMU off, so only aligned word loads.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static uint64_t be64(uintptr_t a)
{
	return (uint64_t)be32(a) << 32 | be32(a + 4);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
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
	for (uint32_t i = 0; i < len; i++) {
		uint32_t j = 0;
		while (needle[j] && i + j < len && s[i + j] == needle[j])
			j++;
		if (!needle[j])
			return 1;
	}
	return 0;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = be64(val);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

/*
 * Assumes #address-cells = #size-cells = 2 for the root, reserved-memory
 * and soc (checked on asteroids by the probe), so every reg entry is 16 bytes.
 */
const char *fdt_board(uintptr_t fdt, struct board *b)
{
	if (!fdt || (fdt & 3))
		return "no DTB pointer";
	if (be32(fdt) != FDT_MAGIC)
		return "bad DTB magic";
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	uintptr_t end = fdt + be32(fdt + 4);
	const char *stack[16];
	int depth = 0;
	/*
	 * The GIC node's reg and compatible may come in either order, and it has
	 * child nodes (msi-controller@): keep its state per depth.
	 */
	int node_is_gic[16] = { 0 };
	uint64_t node_reg[16][4];
	int node_nreg[16] = { 0 };
	uint64_t node_stride[16] = { 0 };

	for (int i = 0; i < 16; i++)
		stack[i] = "";
	while (p < end) {
		uint32_t tok = be32(p);
		p += 4;
		if (tok == FDT_BEGIN_NODE) {
			const char *name = (const char *)p;
			if (depth < 16)
				stack[depth] = name;
			depth++;
			if (depth == 3 && streq(stack[1], "cpus") && has_prefix(name, "cpu@"))
				b->ncpus++;
			if (depth < 16) {
				node_is_gic[depth] = 0;
				node_nreg[depth] = 0;
				node_stride[depth] = 0;
			}
			p = align4(skip_str(p));
		} else if (tok == FDT_END_NODE) {
			int d = depth < 16 ? depth : 15;
			if (node_is_gic[d] && node_nreg[d] >= 4 && !b->gicd) {
				b->gicd = node_reg[d][0];
				b->gicr = node_reg[d][2];
				b->gicr_size = node_reg[d][3];
				b->gicr_stride = node_stride[d] ? node_stride[d] : 0x20000;
			}
			if (--depth <= 0)
				break;
		} else if (tok == FDT_PROP) {
			uint32_t len = be32(p), off = be32(p + 4);
			uintptr_t val = p + 8;
			const char *pn = (const char *)(strings + off);
			const char *name = depth >= 1 && depth <= 16 ? stack[depth - 1] : "";
			const char *parent = depth >= 2 && depth <= 16 ? stack[depth - 2] : "";

			if (streq(pn, "compatible") && has_prefix(name, "interrupt-controller@")) {
				if (!b->ic_compat[0])
					b->ic_compat[0] = (const char *)val;
				else if (!b->ic_compat[1])
					b->ic_compat[1] = (const char *)val;
			}
			if (depth == 2 && streq(name, "psci") && streq(pn, "method")) {
				b->psci_hvc = contains((const char *)val, len, "hvc");
			} else if (depth == 2 && has_prefix(name, "memory") && streq(pn, "reg")) {
				for (uint32_t o = 0; o + 16 <= len && b->nmem < FDT_MAXMEM; o += 16) {
					b->mem_pa[b->nmem] = be64(val + o);
					b->mem_size[b->nmem] = be64(val + o + 8);
					b->nmem++;
				}
			} else if (depth == 3 && streq(parent, "reserved-memory") && streq(pn, "reg") &&
				   contains(name, 64, "splash") && !contains(name, 64, "non_secure") &&
				   len >= 16 && !b->splash_pa) {
				b->splash_pa = be64(val);
				b->splash_size = be64(val + 8);
			} else if (depth < 16 && ((streq(pn, "compatible") &&
				   contains((const char *)val, len, "gic-v3")) ||
				   streq(pn, "#redistributor-regions"))) {
				/* vendor DTBs may not say "arm,gic-v3"; only a GICv3 has redistributors */
				node_is_gic[depth] = 1;
			} else if (depth < 16 && streq(pn, "reg") && has_prefix(name, "interrupt-controller@")) {
				/*
				 * The vendor /soc uses #address-cells = #size-cells = 1 (16 bytes
				 * for GICD + GICR), mainline uses 2 (32 bytes).
				 */
				uint32_t cell = len == 16 ? 4 : 8;
				for (uint32_t o = 0; o + cell <= len && node_nreg[depth] < 4; o += cell)
					node_reg[depth][node_nreg[depth]++] = cell == 4 ? be32(val + o) : be64(val + o);
			} else if (depth < 16 && streq(pn, "redistributor-stride") &&
				   has_prefix(name, "interrupt-controller@")) {
				node_stride[depth] = len == 8 ? be64(val) : be32(val);
			}
			p = align4(val + len);
		} else if (tok != FDT_NOP) {
			break;
		}
	}
	return 0;
}
