#pragma once
#include <stdint.h>

/* Boot the SWEETXNU payload found in the ABL ramdisk; returns only on error. */
const char *loader_boot(uintptr_t dtb);

/* main.c: is [pa, pa + size) inside one /memory bank of the ABL DTB? */
int ram_ok(uint64_t pa, uint64_t size);
