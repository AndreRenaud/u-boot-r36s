// SPDX-License-Identifier: GPL-2.0+
/*
 * NewVision NV3051D MIPI DSI panel driver
 *
 * Power sequence and the R36S init table come from the Linux
 * panel-newvision-nv3051d driver; the init table was supplied by the
 * device vendor without documentation.
 *
 * The power-on delays add up to half a second, so U-Boot doesn't wait for
 * them: a cyclic function takes each step once its delay is up, while U-Boot
 * carries on. Probed early, the reset overlaps loading the environment, and
 * everything after sleep-out overlaps loading the kernel.
 */

#include <backlight.h>
#include <cyclic.h>
#include <dm.h>
#include <dsi_host.h>
#include <env.h>
#include <mipi_dsi.h>
#include <panel.h>
#include <time.h>
#include <asm/gpio.h>
#include <dm/device_compat.h>
#include <linux/delay.h>
#include <power/regulator.h>

struct nv3051d_panel_desc {
	const struct display_timing *timing;
	/* Init commands as { length, bytes... }, terminated by a zero length */
	const u8 *init_seq;
	unsigned long mode_flags;
};

/* Power-on steps, in order */
enum nv3051d_step {
	NV3051D_POWERED,	/* vdd on */
	NV3051D_IN_RESET,
	NV3051D_RESET_DONE,	/* reset released */
	NV3051D_READY,		/* for the init sequence, from enable_backlight() */
	NV3051D_SLEEP_OUT,	/* init sequence and sleep-out sent */
	NV3051D_DISPLAY_ON,	/* display on, DSI host in video mode */
	NV3051D_DISPON_DONE,
	NV3051D_ON,		/* backlight on */
};

struct nv3051d_panel_priv {
	struct udevice *dev;
	struct udevice *vdd;
	struct udevice *backlight;
	struct udevice *dsi_host;
	struct gpio_desc reset;
	const struct nv3051d_panel_desc *desc;
	/* Power-on progress, advanced by nv3051d_cyclic() */
	struct cyclic_info cyclic;
	enum nv3051d_step step;
	u64 step_us;			/* when @step was reached */
	ulong delay_ms[NV3051D_ON];	/* the delays used, for the log */
};

static const struct display_timing r36s_timing = {
	.pixelclock.typ		= 24150000,
	.hactive.typ		= 640,
	.hfront_porch.typ	= 40,
	.hsync_len.typ		= 2,
	.hback_porch.typ	= 80,
	.vactive.typ		= 480,
	.vfront_porch.typ	= 18,
	.vsync_len.typ		= 2,
	.vback_porch.typ	= 4,
	.flags			= DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW |
				  DISPLAY_FLAGS_DE_LOW | DISPLAY_FLAGS_PIXDATA_NEGEDGE,
};

/* R36S "panel 4" init sequence */
static const u8 r36s_init_seq[] = {
	2, 0xff, 0x30,
	2, 0xff, 0x52,
	2, 0xff, 0x01,
	2, 0xe3, 0x00,
	2, 0x25, 0x10,
	2, 0x28, 0x6f,
	2, 0x29, 0x01,
	2, 0x2a, 0xdf,
	2, 0x2c, 0x22,
	2, 0xc3, 0x0f,
	2, 0x37, 0x9c,
	2, 0x38, 0xa7,
	2, 0x39, 0x41,
	2, 0x80, 0x20,
	2, 0x91, 0x67,
	2, 0x92, 0x67,
	2, 0xa0, 0x55,
	2, 0xa1, 0x50,
	2, 0xa3, 0x58,
	2, 0xa4, 0x9c,
	2, 0xa7, 0x02,
	2, 0xa8, 0x01,
	2, 0xa9, 0x21,
	2, 0xaa, 0xfc,
	2, 0xab, 0x28,
	2, 0xac, 0x06,
	2, 0xad, 0x06,
	2, 0xae, 0x06,
	2, 0xaf, 0x03,
	2, 0xb0, 0x08,
	2, 0xb1, 0x26,
	2, 0xb2, 0x28,
	2, 0xb3, 0x28,
	2, 0xb4, 0x03,
	2, 0xb5, 0x08,
	2, 0xb6, 0x26,
	2, 0xb7, 0x08,
	2, 0xb8, 0x26,
	2, 0x2c, 0x22,
	2, 0x5c, 0x40,
	2, 0xc0, 0x00,
	2, 0xc1, 0x00,
	2, 0xc2, 0x00,
	2, 0xff, 0x30,
	2, 0xff, 0x52,
	2, 0xff, 0x02,
	2, 0xb0, 0x02,
	2, 0xd0, 0x02,
	2, 0xb1, 0x0f,
	2, 0xd1, 0x10,
	2, 0xb2, 0x11,
	2, 0xd2, 0x12,
	2, 0xb3, 0x32,
	2, 0xd3, 0x33,
	2, 0xb4, 0x36,
	2, 0xd4, 0x36,
	2, 0xb5, 0x3c,
	2, 0xd5, 0x3c,
	2, 0xb6, 0x20,
	2, 0xd6, 0x20,
	2, 0xb7, 0x3e,
	2, 0xd7, 0x3e,
	2, 0xb8, 0x0e,
	2, 0xd8, 0x0d,
	2, 0xb9, 0x05,
	2, 0xd9, 0x05,
	2, 0xba, 0x11,
	2, 0xda, 0x12,
	2, 0xbb, 0x11,
	2, 0xdb, 0x11,
	2, 0xbc, 0x13,
	2, 0xdc, 0x14,
	2, 0xbd, 0x14,
	2, 0xdd, 0x14,
	2, 0xbe, 0x16,
	2, 0xde, 0x18,
	2, 0xbf, 0x0e,
	2, 0xdf, 0x0f,
	2, 0xc0, 0x17,
	2, 0xe0, 0x17,
	2, 0xc1, 0x07,
	2, 0xe1, 0x08,
	2, 0xff, 0x30,
	2, 0xff, 0x52,
	2, 0xff, 0x03,
	2, 0x08, 0x8a,
	2, 0x09, 0x8b,
	2, 0x30, 0x00,
	2, 0x31, 0x00,
	2, 0x32, 0x00,
	2, 0x33, 0x00,
	2, 0x34, 0x61,
	2, 0x35, 0xd4,
	2, 0x36, 0x24,
	2, 0x37, 0x03,
	2, 0x40, 0x86,
	2, 0x41, 0x87,
	2, 0x42, 0x84,
	2, 0x43, 0x85,
	2, 0x44, 0x11,
	2, 0x45, 0xde,
	2, 0x46, 0xdd,
	2, 0x47, 0x11,
	2, 0x48, 0xe0,
	2, 0x49, 0xdf,
	2, 0x50, 0x82,
	2, 0x51, 0x83,
	2, 0x52, 0x80,
	2, 0x53, 0x81,
	2, 0x54, 0x11,
	2, 0x55, 0xe2,
	2, 0x56, 0xe1,
	2, 0x57, 0x11,
	2, 0x58, 0xe4,
	2, 0x59, 0xe3,
	2, 0x82, 0x0f,
	2, 0x83, 0x0f,
	2, 0x84, 0x00,
	2, 0x85, 0x0f,
	2, 0x86, 0x0f,
	2, 0x87, 0x0e,
	2, 0x88, 0x0e,
	2, 0x89, 0x06,
	2, 0x8a, 0x06,
	2, 0x8b, 0x07,
	2, 0x8c, 0x07,
	2, 0x8d, 0x04,
	2, 0x8e, 0x04,
	2, 0x8f, 0x05,
	2, 0x90, 0x05,
	2, 0x98, 0x0f,
	2, 0x99, 0x0f,
	2, 0x9a, 0x00,
	2, 0x9b, 0x0f,
	2, 0x9c, 0x0f,
	2, 0x9d, 0x0e,
	2, 0x9e, 0x0e,
	2, 0x9f, 0x06,
	2, 0xa0, 0x06,
	2, 0xa1, 0x07,
	2, 0xa2, 0x07,
	2, 0xa3, 0x04,
	2, 0xa4, 0x04,
	2, 0xa5, 0x05,
	2, 0xa6, 0x05,
	2, 0xe0, 0x02,
	2, 0xe1, 0x52,
	2, 0xff, 0x30,
	2, 0xff, 0x52,
	2, 0xff, 0x00,
	2, 0x36, 0x02,
	2, 0x11, 0x00,
	2, 0x29, 0x00,
	/* The final sleep-out and display-on are sent with their delays */
	0
};

/*
 * Delay after each power-on step before the next, in ms. The defaults are the
 * vendor BSP device tree values (prepare/reset/init/enable delays, and the
 * delays it attaches to the final sleep-out and display-on commands). Each
 * can be overridden from the environment, for testing from a cold power-on.
 * They are read when the step is due, so the card's environment applies once
 * it has been loaded. NV3051D_READY waits for enable_backlight() instead.
 */
static const struct {
	const char *env;
	ulong def_ms;
} nv3051d_delays[NV3051D_ON] = {
	[NV3051D_POWERED]	= { "panel_prepare_ms", 20 },
	[NV3051D_IN_RESET]	= { "panel_reset_ms", 150 },
	[NV3051D_RESET_DONE]	= { "panel_init_ms", 20 },
	[NV3051D_SLEEP_OUT]	= { "panel_slpout_ms", 200 },
	[NV3051D_DISPLAY_ON]	= { "panel_dispon_ms", 20 },
	[NV3051D_DISPON_DONE]	= { "panel_enable_ms", 120 },
};

static const struct nv3051d_panel_desc r36s_desc = {
	.timing = &r36s_timing,
	.init_seq = r36s_init_seq,
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
		      MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET,
};

static int nv3051d_send_init_seq(struct udevice *dev,
				 struct mipi_dsi_device *dsi)
{
	struct nv3051d_panel_priv *priv = dev_get_priv(dev);
	const u8 *cmd = priv->desc->init_seq;
	ssize_t ret;

	while (*cmd) {
		ret = mipi_dsi_dcs_write_buffer(dsi, cmd + 1, *cmd);
		if (ret < 0) {
			dev_err(dev, "init command 0x%02x failed: %zd\n",
				cmd[1], ret);
			return ret;
		}
		cmd += *cmd + 1;
	}

	return 0;
}

static void nv3051d_set_step(struct nv3051d_panel_priv *priv,
			     enum nv3051d_step step)
{
	priv->step = step;
	priv->step_us = timer_get_us();
}

/* Run from schedule() every millisecond: take the next step once it's due */
static void nv3051d_cyclic(struct cyclic_info *c)
{
	struct nv3051d_panel_priv *priv =
		container_of(c, struct nv3051d_panel_priv, cyclic);
	struct mipi_dsi_panel_plat *plat = dev_get_plat(priv->dev);
	enum nv3051d_step step = priv->step;
	ulong ms;
	int i, ret = 0;

	if (step >= NV3051D_ON || !nv3051d_delays[step].env)
		return;
	ms = env_get_ulong(nv3051d_delays[step].env, 10,
			   nv3051d_delays[step].def_ms);
	if (timer_get_us() - priv->step_us < ms * 1000)
		return;
	priv->delay_ms[step] = ms;

	switch (step) {
	case NV3051D_POWERED:
		/* Reset is active low: assert, hold, then release */
		dm_gpio_set_value(&priv->reset, 1);
		break;
	case NV3051D_IN_RESET:
		dm_gpio_set_value(&priv->reset, 0);
		break;
	case NV3051D_SLEEP_OUT:
		/* Still in command mode: the host can't send commands in video mode */
		ret = mipi_dsi_dcs_set_display_on(plat->device);
		if (!ret)
			ret = dsi_host_enable(priv->dsi_host);
		break;
	case NV3051D_DISPON_DONE:
		ret = backlight_enable(priv->backlight);
		break;
	default:
		break;
	}
	if (ret) {
		dev_err(priv->dev, "power-on step %d failed: %d\n", step, ret);
		cyclic_unregister(c);
		return;
	}
	nv3051d_set_step(priv, step + 1);

	if (priv->step == NV3051D_ON) {
		cyclic_unregister(c);
		printf("panel: on: ");
		for (i = 0; i < NV3051D_ON; i++) {
			if (nv3051d_delays[i].env)
				printf("%s=%lu ", nv3051d_delays[i].env,
				       priv->delay_ms[i]);
		}
		printf("\n");
	}
}

static int nv3051d_panel_enable_backlight(struct udevice *dev)
{
	struct nv3051d_panel_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	struct mipi_dsi_device *dsi = plat->device;
	int ret;

	ret = uclass_get_device(UCLASS_DSI_HOST, 0, &priv->dsi_host);
	if (ret)
		return ret;

	/* Wait out the rest of the reset: udelay() runs nv3051d_cyclic() */
	while (priv->step < NV3051D_READY)
		udelay(100);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		return ret;

	ret = nv3051d_send_init_seq(dev, dsi);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0)
		return ret;
	nv3051d_set_step(priv, NV3051D_SLEEP_OUT);

	/*
	 * nv3051d_cyclic() does the rest as the delays run out, including the
	 * switch to video mode that the DSI bridge would otherwise make now.
	 */
	return -EINPROGRESS;
}

static int nv3051d_panel_get_display_timing(struct udevice *dev,
					    struct display_timing *timings)
{
	struct nv3051d_panel_priv *priv = dev_get_priv(dev);

	memcpy(timings, priv->desc->timing, sizeof(*timings));

	return 0;
}

static int nv3051d_panel_of_to_plat(struct udevice *dev)
{
	struct nv3051d_panel_priv *priv = dev_get_priv(dev);
	int ret;

	ret = device_get_supply_regulator(dev, "vdd-supply", &priv->vdd);
	if (ret && ret != -ENOENT) {
		dev_err(dev, "failed to get vdd supply: %d\n", ret);
		return ret;
	}

	ret = gpio_request_by_name(dev, "reset-gpios", 0, &priv->reset,
				   GPIOD_IS_OUT);
	if (ret) {
		dev_err(dev, "failed to get reset gpio: %d\n", ret);
		return ret;
	}

	ret = uclass_get_device_by_phandle(UCLASS_PANEL_BACKLIGHT, dev,
					   "backlight", &priv->backlight);
	if (ret) {
		dev_err(dev, "failed to get backlight: %d\n", ret);
		return ret;
	}

	return 0;
}

static int nv3051d_panel_probe(struct udevice *dev)
{
	struct nv3051d_panel_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_panel_plat *plat = dev_get_plat(dev);
	int ret;

	priv->dev = dev;
	priv->desc = (const struct nv3051d_panel_desc *)dev_get_driver_data(dev);

	ret = regulator_set_enable_if_allowed(priv->vdd, true);
	if (ret) {
		dev_err(dev, "failed to enable vdd supply: %d\n", ret);
		return ret;
	}
	nv3051d_set_step(priv, NV3051D_POWERED);
	cyclic_register(&priv->cyclic, nv3051d_cyclic, 1000, dev->name);

	plat->lanes = 4;
	plat->format = MIPI_DSI_FMT_RGB888;
	plat->mode_flags = priv->desc->mode_flags;

	return 0;
}

/*
 * Before the OS starts, or when the display isn't wanted. If the panel isn't
 * on yet, hold it in reset and leave the rest to the OS.
 */
static int nv3051d_panel_remove(struct udevice *dev)
{
	struct nv3051d_panel_priv *priv = dev_get_priv(dev);

	cyclic_unregister(&priv->cyclic);
	if (priv->step != NV3051D_ON)
		dm_gpio_set_value(&priv->reset, 1);

	return 0;
}

static const struct panel_ops nv3051d_panel_ops = {
	.enable_backlight	= nv3051d_panel_enable_backlight,
	.get_display_timing	= nv3051d_panel_get_display_timing,
};

static const struct udevice_id nv3051d_panel_ids[] = {
	{ .compatible = "gameconsole,r36s-panel", .data = (ulong)&r36s_desc },
	{ }
};

U_BOOT_DRIVER(nv3051d_panel) = {
	.name		= "nv3051d_panel",
	.id		= UCLASS_PANEL,
	.of_match	= nv3051d_panel_ids,
	.ops		= &nv3051d_panel_ops,
	.of_to_plat	= nv3051d_panel_of_to_plat,
	.probe		= nv3051d_panel_probe,
	.remove		= nv3051d_panel_remove,
	.plat_auto	= sizeof(struct mipi_dsi_panel_plat),
	.priv_auto	= sizeof(struct nv3051d_panel_priv),
	.flags		= DM_FLAG_OS_PREPARE,
};
