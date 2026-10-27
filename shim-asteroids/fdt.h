#pragma once
#include <stdint.h>

/* /chosen property as u32 or u64 (loader.c: linux,initrd-start/end) */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/* What main.c needs from the ABL DTB. All addresses are physical. */
#define FDT_MAXMEM 32
struct board {
	uint64_t splash_pa, splash_size;
	uint64_t mem_pa[FDT_MAXMEM], mem_size[FDT_MAXMEM];
	int nmem;
	uint64_t gicd, gicr, gicr_size, gicr_stride;
	int psci_hvc;
	int ncpus;
};

/* 0 on success, else a short reason */
const char *fdt_board(uintptr_t fdt, struct board *b);
