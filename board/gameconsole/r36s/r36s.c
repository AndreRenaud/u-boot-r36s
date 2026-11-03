// SPDX-License-Identifier: GPL-2.0+
/*
 * R36S handheld
 *
 * Shows the boot splash from late board init, as soon as the display can be
 * brought up. The image is built into U-Boot (r36s_splash.bmp, generated from
 * splash.svg by make-splash.sh) and drawn centred on the panel.
 */

#include <dm.h>
#include <env.h>
#include <log.h>
#include <mapmem.h>
#include <splash.h>
#include <video.h>

extern u8 __splash_r36s_splash_begin[];

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
