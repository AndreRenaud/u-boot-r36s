// SPDX-License-Identifier: GPL-2.0+
/*
 * Rockchip: MMU and data cache on before relocation
 *
 * With the MMU off, every data access is uncached, so board_init_f() (driver
 * model setup, which scans the whole device tree, the serial and clock
 * probes, relocation) runs from DRAM at a fraction of its cached speed. On
 * the PX30 that is about 0.4 s. Turning the MMU on needs page tables, and
 * the usual ones are only reserved late in board_init_f(), so this uses a
 * fixed area of DRAM until board_init_r() enables the caches again.
 *
 * relocate_code() copes with the data cache on: it cleans the relocated
 * image to memory and invalidates the I-cache.
 */

#include <cpu_func.h>
#include <asm/cache.h>
#include <asm/global_data.h>
#include <asm/system.h>
#include <asm/arch-rockchip/early_dcache.h>

DECLARE_GLOBAL_DATA_PTR;

void rockchip_early_dcache_enable(void)
{
	gd->arch.tlb_addr = CONFIG_ROCKCHIP_EARLY_DCACHE_TLB_ADDR;
	gd->arch.tlb_size = PGTABLE_SIZE;
	icache_enable();
	dcache_enable();
}

/* Replaces the generic one in board_init_r() */
void enable_caches(void)
{
	/*
	 * Still on the early page tables: flush and turn the MMU off, then
	 * let dcache_enable() build the tables that arch_reserve_mmu()
	 * reserved (gd->arch.tlb_addr now points there).
	 */
	if (dcache_status()) {
		dcache_disable();
		gd->arch.tlb_fillptr = 0;
	}
	icache_enable();
	dcache_enable();
}
