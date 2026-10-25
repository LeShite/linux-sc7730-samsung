// SPDX-License-Identifier: GPL-2.0
/*
 * Backlight of the Galaxy Tab A 7.0 (SM-T280): the S6D7AA0X11 panel dims itself on the DCS command
 * "write display brightness" (0x51) sent over MIPI DSI. The bootloader leaves the SC7730's DSI host
 * (Synopsys DesignWare 1.21a at 0x21800000) running in video mode and simpledrm scans out its
 * framebuffer; this only queues DCS short writes through the host's generic interface, which the
 * host sends in the blanking periods.
 *
 * From the vendor 3.10 kernel: sprd-dsi-panel-dsi_s6d7aa0x11_gtexswifi_video.dtsi
 * (backlight-on 53 2C, set-brightness 51 xx), the board's gen-panel-backlight curve, and
 * drivers/video/sprdfb/dsi_1_21a (GEN_HDR, CMD_PKT_STATUS).
 */

#include <linux/backlight.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define DSI_VERSION		0x00
#define DSI_CMD_MODE_CFG	0x68
#define DSI_GEN_HDR		0x6c
#define DSI_CMD_PKT_STATUS	0x74
#define  GEN_CMD_EMPTY		BIT(0)
#define  GEN_CMD_FULL		BIT(1)

#define DCS_SHORT_WRITE_PARAM	0x15
#define DCS_WRITE_BRIGHTNESS	0x51
#define DCS_WRITE_CTRL_DISPLAY	0x53
#define  CTRL_BCTRL_DD_BL	0x2c	/* the vendor's backlight-on value */

#define BRIGHTNESS_DEFAULT	143	/* the vendor's DEF point */

struct sc8830_bl {
	struct device *dev;
	void __iomem *dsi;
};

/* the vendor's backlight-brt-range: user level -> panel value, linear between the points */
static const u8 brt_curve[][2] = {
	{ 0, 0 }, { 10, 3 }, { 20, 11 }, { 143, 95 }, { 255, 202 },
};

static u8 panel_level(int level)
{
	int i;

	for (i = 1; i < ARRAY_SIZE(brt_curve); i++)
		if (level <= brt_curve[i][0]) {
			int x0 = brt_curve[i - 1][0], y0 = brt_curve[i - 1][1];
			int x1 = brt_curve[i][0], y1 = brt_curve[i][1];

			return y0 + (level - x0) * (y1 - y0) / (x1 - x0);
		}
	return brt_curve[ARRAY_SIZE(brt_curve) - 1][1];
}

static int dcs_write1(struct sc8830_bl *b, u8 cmd, u8 param)
{
	u32 sts;
	int ret;

	ret = readl_poll_timeout(b->dsi + DSI_CMD_PKT_STATUS, sts, !(sts & GEN_CMD_FULL), 20, 20000);
	if (ret)
		return ret;
	writel((param << 16) | (cmd << 8) | DCS_SHORT_WRITE_PARAM, b->dsi + DSI_GEN_HDR);
	/* sent once the queue drains (the next blanking period) */
	return readl_poll_timeout(b->dsi + DSI_CMD_PKT_STATUS, sts, sts & GEN_CMD_EMPTY, 20, 50000);
}

static int sc8830_bl_update(struct backlight_device *bd)
{
	struct sc8830_bl *b = bl_get_data(bd);
	int level = backlight_get_brightness(bd);	/* 0 when blanked or powered down */
	int ret = dcs_write1(b, DCS_WRITE_BRIGHTNESS, panel_level(level));

	if (ret)
		dev_warn(b->dev, "brightness %d not sent: %d\n", level, ret);
	return ret;
}

static const struct backlight_ops sc8830_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = sc8830_bl_update,
};

static int sc8830_bl_probe(struct platform_device *pdev)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = 255,
		.brightness = BRIGHTNESS_DEFAULT,
		.scale = BACKLIGHT_SCALE_NON_LINEAR,
	};
	struct device *dev = &pdev->dev;
	struct backlight_device *bd;
	struct sc8830_bl *b;
	u32 ver;

	b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;
	b->dev = dev;
	b->dsi = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(b->dsi))
		return PTR_ERR(b->dsi);

	ver = readl(b->dsi + DSI_VERSION);
	dev_info(dev, "DSI host version %08x, cmd mode cfg %08x, pkt status %08x\n", ver,
		 readl(b->dsi + DSI_CMD_MODE_CFG), readl(b->dsi + DSI_CMD_PKT_STATUS));
	if (!ver || ver == ~0u)
		return dev_err_probe(dev, -ENODEV, "DSI host not running\n");

	if (dcs_write1(b, DCS_WRITE_CTRL_DISPLAY, CTRL_BCTRL_DD_BL))
		dev_warn(dev, "backlight-on command not sent\n");

	bd = devm_backlight_device_register(dev, "panel", dev, b, &sc8830_bl_ops, &props);
	if (IS_ERR(bd))
		return PTR_ERR(bd);
	return backlight_update_status(bd);
}

static const struct of_device_id sc8830_bl_of_match[] = {
	{ .compatible = "sprd,sc8830-dsi-backlight" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc8830_bl_of_match);

static struct platform_driver sc8830_bl_driver = {
	.probe = sc8830_bl_probe,
	.driver = {
		.name = "sc8830-dsi-backlight",
		.of_match_table = sc8830_bl_of_match,
	},
};
module_platform_driver(sc8830_bl_driver);

MODULE_DESCRIPTION("SM-T280 panel backlight through DSI DCS commands");
MODULE_LICENSE("GPL");
