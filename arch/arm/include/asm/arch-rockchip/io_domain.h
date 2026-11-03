/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Rockchip I/O voltage domains
 */

#ifndef _ASM_ARCH_IO_DOMAIN_H
#define _ASM_ARCH_IO_DOMAIN_H

#include <linux/errno.h>

struct udevice;

#if CONFIG_IS_ENABLED(ROCKCHIP_IODOMAIN)
/**
 * rockchip_io_domain_set_voltage() - set the I/O domain of a supply
 *
 * Selects the pad voltage mode (1.8 V or 3.3 V) of every I/O domain fed by
 * @supply, for when its voltage is changed after the io-domain driver probed.
 *
 * @supply:	Regulator feeding the I/O domain(s)
 * @uV:		The supply's voltage in microvolts
 * Return: 0 if OK, -ENOENT if no I/O domain uses @supply, other -ve on error
 */
int rockchip_io_domain_set_voltage(struct udevice *supply, int uV);
#else
static inline int rockchip_io_domain_set_voltage(struct udevice *supply, int uV)
{
	return -ENOENT;
}
#endif

#endif
