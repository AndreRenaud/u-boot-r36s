// SPDX-License-Identifier: GPL-2.0+
/*
 * Rockchip PX30/RK3326 VOP (big) driver
 *
 * Drives a single XRGB8888 framebuffer through window 0 out to a video
 * bridge (the Synopsys MIPI DSI host). The register layout differs from
 * the RK3288-style VOPs handled by rk_vop.c; it comes from Linux
 * drivers/gpu/drm/rockchip/rockchip_vop_reg.{c,h}.
 */

#include <clk.h>
#include <dm.h>
#include <log.h>
#include <panel.h>
#include <reset.h>
#include <video.h>
#include <video_bridge.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <linux/bitfield.h>
#include <linux/delay.h>

DECLARE_GLOBAL_DATA_PTR;

#define PX30_REG_CFG_DONE		0x000
#define PX30_SYS_CTRL2			0x018
#define  PX30_STANDBY			BIT(1)
#define PX30_DSP_CTRL0			0x020
#define  PX30_MIPI_EN			BIT(24)
#define  PX30_MIPI_DCLK_POL		BIT(25)
#define  PX30_MIPI_PIN_POL		GENMASK(28, 26)
#define PX30_DSP_CTRL2			0x028
#define  PX30_DSP_BLANK			BIT(14)
#define  PX30_OUT_MODE			GENMASK(19, 16)
#define PX30_WIN0_CTRL0			0x050
#define  PX30_WIN0_EN			BIT(0)
#define  PX30_WIN0_FORMAT		GENMASK(3, 1)
#define PX30_WIN0_VIR			0x05c
#define PX30_WIN0_YRGB_MST0		0x060
#define PX30_WIN0_ACT_INFO		0x068
#define PX30_WIN0_DSP_INFO		0x06c
#define PX30_WIN0_DSP_ST		0x070
#define PX30_WIN0_SCL_FACTOR_YRGB	0x074
#define PX30_WIN0_SCL_FACTOR_CBR	0x078
#define PX30_DSP_HTOTAL_HS_END		0x100
#define PX30_DSP_HACT_ST_END		0x104
#define PX30_DSP_VTOTAL_VS_END		0x108
#define PX30_DSP_VACT_ST_END		0x10c

/* Bits within PX30_MIPI_PIN_POL */
#define PX30_POL_HSYNC_POSITIVE		BIT(0)
#define PX30_POL_VSYNC_POSITIVE		BIT(1)

/* Scale factor of 1.0 in both directions */
#define PX30_SCL_FACTOR_1_1		0x10001000

struct px30_vop_priv {
	void __iomem *regs;
	struct clk aclk;
	struct clk hclk;
	struct clk dclk;
};

static void px30_vop_mode_set(struct udevice *dev,
			      const struct display_timing *t, ulong fbbase)
{
	struct px30_vop_priv *priv = dev_get_priv(dev);
	void __iomem *regs = priv->regs;
	u32 hactive = t->hactive.typ;
	u32 vactive = t->vactive.typ;
	u32 hsync_len = t->hsync_len.typ;
	u32 vsync_len = t->vsync_len.typ;
	u32 hact_st = hsync_len + t->hback_porch.typ;
	u32 vact_st = vsync_len + t->vback_porch.typ;
	u32 htotal = hact_st + hactive + t->hfront_porch.typ;
	u32 vtotal = vact_st + vactive + t->vfront_porch.typ;
	u32 pin_pol = 0;
	u32 dsp_ctrl0;

	writel(htotal << 16 | hsync_len, regs + PX30_DSP_HTOTAL_HS_END);
	writel(hact_st << 16 | (hact_st + hactive), regs + PX30_DSP_HACT_ST_END);
	writel(vtotal << 16 | vsync_len, regs + PX30_DSP_VTOTAL_VS_END);
	writel(vact_st << 16 | (vact_st + vactive), regs + PX30_DSP_VACT_ST_END);

	/* Window 0: full screen, unscaled ARGB8888 (format 0) */
	writel((vactive - 1) << 16 | (hactive - 1), regs + PX30_WIN0_ACT_INFO);
	writel((vactive - 1) << 16 | (hactive - 1), regs + PX30_WIN0_DSP_INFO);
	writel(vact_st << 16 | hact_st, regs + PX30_WIN0_DSP_ST);
	writel(PX30_SCL_FACTOR_1_1, regs + PX30_WIN0_SCL_FACTOR_YRGB);
	writel(PX30_SCL_FACTOR_1_1, regs + PX30_WIN0_SCL_FACTOR_CBR);
	writel(hactive, regs + PX30_WIN0_VIR);	/* stride in 32-bit words */
	writel(fbbase, regs + PX30_WIN0_YRGB_MST0);
	clrsetbits_le32(regs + PX30_WIN0_CTRL0, PX30_WIN0_FORMAT | PX30_WIN0_EN,
			PX30_WIN0_EN);

	if (t->flags & DISPLAY_FLAGS_HSYNC_HIGH)
		pin_pol |= PX30_POL_HSYNC_POSITIVE;
	if (t->flags & DISPLAY_FLAGS_VSYNC_HIGH)
		pin_pol |= PX30_POL_VSYNC_POSITIVE;
	dsp_ctrl0 = PX30_MIPI_EN | FIELD_PREP(PX30_MIPI_PIN_POL, pin_pol);
	if (t->flags & DISPLAY_FLAGS_PIXDATA_NEGEDGE)
		dsp_ctrl0 |= PX30_MIPI_DCLK_POL;
	clrsetbits_le32(regs + PX30_DSP_CTRL0,
			PX30_MIPI_EN | PX30_MIPI_DCLK_POL | PX30_MIPI_PIN_POL,
			dsp_ctrl0);

	/* P888 output, not blanked, out of standby */
	clrbits_le32(regs + PX30_DSP_CTRL2, PX30_OUT_MODE | PX30_DSP_BLANK);
	clrbits_le32(regs + PX30_SYS_CTRL2, PX30_STANDBY);

	writel(1, regs + PX30_REG_CFG_DONE);
}

static int px30_vop_clk_enable(struct udevice *dev, const char *name,
			       struct clk *clk)
{
	int ret;

	ret = clk_get_by_name(dev, name, clk);
	if (ret) {
		dev_err(dev, "failed to get %s clock: %d\n", name, ret);
		return ret;
	}

	ret = clk_enable(clk);
	if (ret && ret != -ENOSYS) {
		dev_err(dev, "failed to enable %s clock: %d\n", name, ret);
		return ret;
	}

	return 0;
}

static int px30_vop_power_up(struct udevice *dev, ulong pixelclock)
{
	struct px30_vop_priv *priv = dev_get_priv(dev);
	struct reset_ctl ahb_rst;
	long rate;
	int ret;

	ret = px30_vop_clk_enable(dev, "aclk_vop", &priv->aclk);
	if (!ret)
		ret = px30_vop_clk_enable(dev, "hclk_vop", &priv->hclk);
	if (!ret)
		ret = px30_vop_clk_enable(dev, "dclk_vop", &priv->dclk);
	if (ret)
		return ret;

	rate = clk_set_rate(&priv->dclk, pixelclock);
	if (rate < 0) {
		dev_err(dev, "failed to set dclk to %lu Hz: %ld\n",
			pixelclock, rate);
		return rate;
	}
	dev_dbg(dev, "dclk %ld Hz (wanted %lu Hz)\n", rate, pixelclock);

	/* Put the VOP registers back to their defaults */
	ret = reset_get_by_name(dev, "ahb", &ahb_rst);
	if (ret) {
		dev_err(dev, "failed to get ahb reset: %d\n", ret);
		return ret;
	}
	reset_assert(&ahb_rst);
	udelay(20);
	reset_deassert(&ahb_rst);

	return 0;
}

/* Follow the VOP's output endpoints to the first bound video bridge */
static int px30_vop_find_bridge(struct udevice *dev, struct udevice **bridgep)
{
	ofnode port, ep, node;
	u32 phandle;
	int depth;

	port = dev_read_subnode(dev, "port");
	if (!ofnode_valid(port))
		return -EINVAL;

	ofnode_for_each_subnode(ep, port) {
		if (ofnode_read_u32(ep, "remote-endpoint", &phandle))
			continue;
		node = ofnode_get_by_phandle(phandle);

		/* endpoint -> port -> ports -> encoder */
		for (depth = 0; depth < 3 && ofnode_valid(node); depth++) {
			node = ofnode_get_parent(node);
			if (!uclass_get_device_by_ofnode(UCLASS_VIDEO_BRIDGE,
							 node, bridgep))
				return 0;
		}
	}

	return -ENODEV;
}

static int px30_vop_probe(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct px30_vop_priv *priv = dev_get_priv(dev);
	struct display_timing timing;
	struct udevice *bridge, *panel;
	int ret;

	/* Before relocation only the framebuffer size from bind matters */
	if (!(gd->flags & GD_FLG_RELOC))
		return 0;

	priv->regs = dev_read_addr_ptr(dev);
	if (!priv->regs)
		return -EINVAL;

	ret = px30_vop_find_bridge(dev, &bridge);
	if (ret) {
		dev_err(dev, "no video bridge found: %d\n", ret);
		return ret;
	}

	ret = uclass_first_device_err(UCLASS_PANEL, &panel);
	if (ret) {
		dev_err(dev, "no panel found: %d\n", ret);
		return ret;
	}

	ret = panel_get_display_timing(panel, &timing);
	if (ret) {
		dev_err(dev, "failed to get panel timing: %d\n", ret);
		return ret;
	}

	ret = px30_vop_power_up(dev, timing.pixelclock.typ);
	if (ret)
		return ret;

	uc_priv->xsize = timing.hactive.typ;
	uc_priv->ysize = timing.vactive.typ;
	uc_priv->bpix = VIDEO_BPP32;

	/* Start scanning out a black screen rather than whatever is in RAM */
	memset((void *)plat->base, 0, uc_priv->xsize * uc_priv->ysize * 4);
	flush_dcache_range(plat->base,
			   plat->base + uc_priv->xsize * uc_priv->ysize * 4);
	px30_vop_mode_set(dev, &timing, plat->base);

	ret = video_bridge_attach(bridge);
	if (ret) {
		dev_err(dev, "failed to attach bridge: %d\n", ret);
		return ret;
	}

	ret = video_bridge_set_backlight(bridge, 80);
	if (ret) {
		dev_err(dev, "failed to enable bridge and panel: %d\n", ret);
		return ret;
	}

	video_set_flush_dcache(dev, true);

	return 0;
}

static int px30_vop_bind(struct udevice *dev)
{
	struct video_uc_plat *plat = dev_get_uclass_plat(dev);

	plat->size = 4 * (CONFIG_VIDEO_ROCKCHIP_MAX_XRES *
			  CONFIG_VIDEO_ROCKCHIP_MAX_YRES);

	return 0;
}

static int px30_vop_remove(struct udevice *dev)
{
	struct px30_vop_priv *priv = dev_get_priv(dev);

	if (!priv->regs)
		return 0;

	/*
	 * Stop scanning out of U-Boot's framebuffer before the OS starts, so
	 * Linux can attach the VOP's IOMMU without page faults.
	 */
	clrbits_le32(priv->regs + PX30_WIN0_CTRL0, PX30_WIN0_EN);
	writel(1, priv->regs + PX30_REG_CFG_DONE);

	return 0;
}

static const struct udevice_id px30_vop_ids[] = {
	{ .compatible = "rockchip,px30-vop-big" },
	{ }
};

U_BOOT_DRIVER(px30_vop) = {
	.name		= "px30_vop",
	.id		= UCLASS_VIDEO,
	.of_match	= px30_vop_ids,
	.bind		= px30_vop_bind,
	.probe		= px30_vop_probe,
	.remove		= px30_vop_remove,
	.priv_auto	= sizeof(struct px30_vop_priv),
	.flags		= DM_FLAG_PRE_RELOC | DM_FLAG_OS_PREPARE,
};
