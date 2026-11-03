// SPDX-License-Identifier: GPL-2.0+
/*
 * R36S handheld
 *
 * Raises the CPU clock once the PMIC is available, and shows the boot splash
 * from late board init, as soon as the display can be brought up. The image
 * is built into U-Boot (r36s_splash.bmp, generated from splash.svg by
 * make-splash.sh) and drawn centred on the panel.
 */

#include <clk.h>
#include <dm.h>
#include <env.h>
#include <event.h>
#include <log.h>
#include <mapmem.h>
#include <splash.h>
#include <video.h>
#include <dt-bindings/clock/px30-cru.h>
#include <linux/delay.h>
#include <power/regulator.h>

extern u8 __splash_r36s_splash_begin[];

#ifndef CONFIG_XPL_BUILD
/*
 * The clock driver runs the CPUs at APLL_HZ (600 MHz). Run them at 1.2 GHz,
 * the fastest rate it has, at that operating point's voltage from the
 * px30.dtsi OPP table, raised first. Linux's cpufreq takes over later.
 */
#define R36S_CPU_HZ	1200000000
#define R36S_CPU_UV	1300000

static int r36s_raise_cpu_clock(void)
{
	struct clk armclk = { .id = ARMCLK };
	struct udevice *vdd_arm;
	ulong rate;
	int ret;

	ret = regulator_get_by_platname("vdd_arm", &vdd_arm);
	if (!ret)
		ret = regulator_set_value(vdd_arm, R36S_CPU_UV);
	if (ret) {
		log_warning("CPU clock left at 600 MHz: vdd_arm: %d\n", ret);
		return 0;
	}
	/* Ramp is 6 mV/us (≈ 60 us from 0.95 V), plus margin to settle */
	udelay(200);

	/* Probing the CRU resets ARMCLK to APLL_HZ, so do that first */
	ret = uclass_get_device_by_driver(UCLASS_CLK, DM_DRIVER_GET(rockchip_px30_cru),
					  &armclk.dev);
	if (!ret) {
		rate = clk_set_rate(&armclk, R36S_CPU_HZ);
		if (IS_ERR_VALUE(rate))
			ret = rate;
	}
	if (ret)
		log_warning("CPU clock left at 600 MHz: %d\n", ret);

	return 0;
}
EVENT_SPY_SIMPLE(EVT_DM_POST_INIT_R, r36s_raise_cpu_clock);
#endif

int rk_board_late_init(void)
{
	struct udevice *dev;
	int ret;

	if (!IS_ENABLED(CONFIG_VIDEO))
		return 0;

	/* For testing Linux's panel bring-up without U-Boot touching it */
	if (env_get_yesno("display_off") == 1) {
		printf("display_off=1, not starting the display\n");
		return 0;
	}

	ret = uclass_first_device_err(UCLASS_VIDEO, &dev);
	if (ret) {
		log_warning("No display for the splash screen: %d\n", ret);
		return 0;
	}

	/* The rest of the screen stays black, matching the image background */
	ret = video_bmp_display(dev, map_to_sysmem(__splash_r36s_splash_begin),
				BMP_ALIGN_CENTER, BMP_ALIGN_CENTER, true);
	if (ret)
		log_warning("Failed to show the splash screen: %d\n", ret);

	video_sync(dev, true);

	return 0;
}
