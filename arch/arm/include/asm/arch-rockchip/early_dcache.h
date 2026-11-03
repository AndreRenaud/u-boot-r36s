/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Rockchip: MMU and data cache on before relocation
 */

#ifndef _ASM_ARCH_EARLY_DCACHE_H
#define _ASM_ARCH_EARLY_DCACHE_H

#if IS_ENABLED(CONFIG_ROCKCHIP_EARLY_DCACHE) && !defined(CONFIG_XPL_BUILD)
/**
 * rockchip_early_dcache_enable() - turn on the MMU and caches in board_init_f()
 *
 * Call from arch_cpu_init(). Uses page tables at
 * CONFIG_ROCKCHIP_EARLY_DCACHE_TLB_ADDR until enable_caches() in
 * board_init_r() moves to the ones arch_reserve_mmu() reserved.
 */
void rockchip_early_dcache_enable(void);
#else
static inline void rockchip_early_dcache_enable(void)
{
}
#endif

#endif
