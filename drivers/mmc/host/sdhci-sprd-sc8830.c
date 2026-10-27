// SPDX-License-Identifier: GPL-2.0
/*
 * SDHCI glue for the Spreadtrum SC8830 family (SC7730, ...) "sdhost 3.0"
 *
 * Mostly a standard SDHCI 3.0 register file. The differences, taken from
 * the vendor 3.10 driver (drivers/mmc/host/sdhost.c):
 *  - no usable capability registers: they are supplied here instead;
 *  - SDCLK = base / (2 * (div + 1)) with a 10-bit divider;
 *  - bit 3 of the software reset register is a vendor bit that the vendor
 *    driver always writes as 1 for the eMMC;
 *  - sample/drive delay registers at 0x80..0x88, lost on a full reset;
 *  - the power control register is unused: the bootloader powers the
 *    card, so it is left alone;
 *  - only SDMA was ever used (ADMA hits a BUG_ON in the vendor driver).
 *
 * The eMMC and SD hosts run from 384 MHz, set up by the bootloader. The SDIO host of the WiFi chip
 * (vendor "sdio1") runs from 96 MHz and nobody turns its clocks on, so a node may give
 *	sprd,base-clock = <Hz>;
 *	sprd,init-regs = <phys mask value>, ...;	(read-modify-write before the host is touched)
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mmc/host.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/mmc/sdhci-sprd-sc8830.h>

#include "sdhci-pltfm.h"

#define SC8830_SDHCI_RST_EMMC		BIT(3)
#define SC8830_SDHCI_WR_DELAY		0x80
#define SC8830_SDHCI_RD_POS_DELAY	0x84
#define SC8830_SDHCI_RD_NEG_DELAY	0x88

#define SC8830_SDHCI_BASE_CLK		384000000
#define SC8830_SDHCI_MAX_DIV		1023

/* 3.0 V VCC (the vendor OCR), SDMA, high speed, 8-bit, 1 MHz timeout clk */
#define SC8830_SDHCI_CAPS	(SDHCI_CAN_VDD_300 | SDHCI_CAN_DO_SDMA | \
				 SDHCI_CAN_DO_HISPD | SDHCI_CAN_DO_8BIT | \
				 SDHCI_TIMEOUT_CLK_UNIT | 1)

struct sc8830_sdhci {
	u32 delays[3];
	u32 base_clk;
	bool added;
};

/* Called by the Marlin UART loader after the FDL has started the SDIO core. */
int sdhci_sprd_sc8830_rescan_sdio(struct device_node *np)
{
	struct platform_device *pdev;
	struct sdhci_host *host;
	struct sdhci_pltfm_host *pltfm_host;
	struct sc8830_sdhci *priv;
	int ret = 0;

	pdev = of_find_device_by_node(np);
	if (!pdev)
		return -EPROBE_DEFER;

	host = platform_get_drvdata(pdev);
	if (!host) {
		ret = -EPROBE_DEFER;
		goto out_put;
	}

	pltfm_host = sdhci_priv(host);
	priv = sdhci_pltfm_priv(pltfm_host);
	if (!READ_ONCE(priv->added)) {
		ret = -EPROBE_DEFER;
		goto out_put;
	}

	WRITE_ONCE(host->mmc->caps2,
		   READ_ONCE(host->mmc->caps2) & ~MMC_CAP2_NO_SDIO);
	/* a non-removable card is scanned only once at boot (rescan_entered); allow one more scan */
	host->mmc->rescan_entered = 0;
	mmc_detect_change(host->mmc, msecs_to_jiffies(100));
out_put:
	put_device(&pdev->dev);
	return ret;
}
EXPORT_SYMBOL_GPL(sdhci_sprd_sc8830_rescan_sdio);

static void sc8830_sdhci_reset(struct sdhci_host *host, u8 mask)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sc8830_sdhci *priv = sdhci_pltfm_priv(pltfm_host);
	u8 val;

	sdhci_writeb(host, mask | SC8830_SDHCI_RST_EMMC, SDHCI_SOFTWARE_RESET);

	if (read_poll_timeout_atomic(sdhci_readb, val, !(val & mask), 10,
				     100000, false, host, SDHCI_SOFTWARE_RESET))
		dev_err(mmc_dev(host->mmc), "reset 0x%x never completed\n", mask);

	if (mask & SDHCI_RESET_ALL) {
		host->clock = 0;
		sdhci_writel(host, priv->delays[0], SC8830_SDHCI_WR_DELAY);
		sdhci_writel(host, priv->delays[1], SC8830_SDHCI_RD_POS_DELAY);
		sdhci_writel(host, priv->delays[2], SC8830_SDHCI_RD_NEG_DELAY);
	}
}

static void sc8830_sdhci_set_clock(struct sdhci_host *host, unsigned int clock)
{
	struct sc8830_sdhci *priv = sdhci_pltfm_priv(sdhci_priv(host));
	u32 div;

	host->mmc->actual_clock = 0;
	sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);
	if (!clock)
		return;

	/* smallest divider that does not overclock the card */
	div = DIV_ROUND_UP(priv->base_clk, 2 * clock);
	div = div ? div - 1 : 0;
	div = min_t(u32, div, SC8830_SDHCI_MAX_DIV);

	host->mmc->actual_clock = priv->base_clk / (2 * (div + 1));
	sdhci_enable_clk(host, ((div & 0x300) >> 2) | ((div & 0xff) << 8));
}

static unsigned int sc8830_sdhci_get_max_clock(struct sdhci_host *host)
{
	return ((struct sc8830_sdhci *)sdhci_pltfm_priv(sdhci_priv(host)))->base_clk;
}

static unsigned int sc8830_sdhci_get_min_clock(struct sdhci_host *host)
{
	return sc8830_sdhci_get_max_clock(host) / (2 * (SC8830_SDHCI_MAX_DIV + 1));
}

/* clocks and gates the bootloader left off: "sprd,init-regs" triples */
static int sc8830_sdhci_init_regs(struct device *dev)
{
	int n = of_property_count_u32_elems(dev->of_node, "sprd,init-regs"), i;
	u32 t[3];

	for (i = 0; n > 0 && i + 3 <= n; i += 3) {
		void __iomem *r;

		if (of_property_read_u32_index(dev->of_node, "sprd,init-regs", i, &t[0]) ||
		    of_property_read_u32_index(dev->of_node, "sprd,init-regs", i + 1, &t[1]) ||
		    of_property_read_u32_index(dev->of_node, "sprd,init-regs", i + 2, &t[2]))
			return -EINVAL;
		r = ioremap(t[0], 4);
		if (!r)
			return -ENOMEM;
		writel((readl(r) & ~t[1]) | (t[2] & t[1]), r);
		dev_info(dev, "init reg %08x = %08x\n", t[0], readl(r));
		iounmap(r);
	}
	return 0;
}

/* The bootloader powers the card; the power control register is unused */
static void sc8830_sdhci_set_power(struct sdhci_host *host, unsigned char mode,
				   unsigned short vdd)
{
}

static const struct sdhci_ops sc8830_sdhci_ops = {
	.reset			= sc8830_sdhci_reset,
	.set_clock		= sc8830_sdhci_set_clock,
	.get_max_clock		= sc8830_sdhci_get_max_clock,
	.get_min_clock		= sc8830_sdhci_get_min_clock,
	.set_power		= sc8830_sdhci_set_power,
	.set_bus_width		= sdhci_set_bus_width,
	.set_uhs_signaling	= sdhci_set_uhs_signaling,
};

static const struct sdhci_pltfm_data sc8830_sdhci_pdata = {
	.ops = &sc8830_sdhci_ops,
	.quirks = SDHCI_QUIRK_BROKEN_ADMA | SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
		  SDHCI_QUIRK_NO_HISPD_BIT | SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.quirks2 = SDHCI_QUIRK2_NO_1_8_V | SDHCI_QUIRK2_BROKEN_HS200 |
		   SDHCI_QUIRK2_BROKEN_DDR50,
};

static int sc8830_sdhci_probe(struct platform_device *pdev)
{
	const u16 version = SDHCI_SPEC_300 << SDHCI_SPEC_VER_SHIFT;
	const u32 caps = SC8830_SDHCI_CAPS, caps1 = 0;
	struct sdhci_pltfm_host *pltfm_host;
	struct sc8830_sdhci *priv;
	struct sdhci_host *host;
	int ret;

	ret = sc8830_sdhci_init_regs(&pdev->dev);
	if (ret)
		return ret;

	host = sdhci_pltfm_init(pdev, &sc8830_sdhci_pdata, sizeof(*priv));
	if (IS_ERR(host))
		return PTR_ERR(host);

	pltfm_host = sdhci_priv(host);
	priv = sdhci_pltfm_priv(pltfm_host);
	of_property_read_u32_array(pdev->dev.of_node, "sprd,delays",
				   priv->delays, ARRAY_SIZE(priv->delays));
	priv->base_clk = SC8830_SDHCI_BASE_CLK;
	of_property_read_u32(pdev->dev.of_node, "sprd,base-clock", &priv->base_clk);

	/* What the bootloader left behind, before the first reset */
	dev_info(&pdev->dev,
		 "boot state: caps %08x/%08x ver %04x clk %04x hctl %02x pres %08x\n",
		 sdhci_readl(host, SDHCI_CAPABILITIES),
		 sdhci_readl(host, SDHCI_CAPABILITIES_1),
		 sdhci_readw(host, SDHCI_HOST_VERSION),
		 sdhci_readw(host, SDHCI_CLOCK_CONTROL),
		 sdhci_readb(host, SDHCI_HOST_CONTROL),
		 sdhci_readl(host, SDHCI_PRESENT_STATE));

	ret = mmc_of_parse(host->mmc);
	if (ret)
		return ret;

	__sdhci_read_caps(host, &version, &caps, &caps1);

	ret = sdhci_add_host(host);
	if (!ret)
		WRITE_ONCE(priv->added, true);
	return ret;
}

static const struct of_device_id sc8830_sdhci_of_match[] = {
	{ .compatible = "sprd,sc8830-sdhci" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc8830_sdhci_of_match);

static struct platform_driver sc8830_sdhci_driver = {
	.probe = sc8830_sdhci_probe,
	.remove = sdhci_pltfm_remove,
	.driver = {
		.name = "sdhci-sprd-sc8830",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = sc8830_sdhci_of_match,
	},
};
module_platform_driver(sc8830_sdhci_driver);

MODULE_DESCRIPTION("SDHCI glue for Spreadtrum SC8830 family SoCs");
MODULE_LICENSE("GPL");
