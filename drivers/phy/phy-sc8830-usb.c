// SPDX-License-Identifier: GPL-2.0
/*
 * USB 2.0 PHY of the Spreadtrum SC8830 family (SC7730), in front of its DesignWare HS OTG (dwc2)
 * controller at 0x20200000.
 *
 * From the vendor 3.10 kernel (drivers/usb/gadget/dwc_otg/usb_hw.c, usb_startup() for SCX35): the
 * vddusb LDO at 3.3 V, the PHY out of power-down (AON APB PWR_CTRL bit 0), the controller's AHB
 * clock (AP AHB EB bit 4), a pulse on the controller, UTMI and PHY resets (AP AHB RST bits 5-7),
 * then the board's tuning value in AP APB USB_PHY_TUNE.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#define AON_PWR_CTRL		0x24
#define  AON_USB_PHY_PD		BIT(0)
#define AHB_EB			0x00
#define  AHB_USB_EB		BIT(4)
#define AHB_RST			0x04
#define  AHB_USB_RSTS		(BIT(5) | BIT(6) | BIT(7))	/* controller, UTMI, PHY */

struct sc8830_usb_phy {
	void __iomem *aon, *ahb, *tune;
	struct regulator *vdd;
	u32 tune_value;
};

static void upd(void __iomem *reg, u32 mask, u32 val)
{
	writel((readl(reg) & ~mask) | (val & mask), reg);
}

static int sc8830_usb_phy_power_on(struct phy *phy)
{
	struct sc8830_usb_phy *p = phy_get_drvdata(phy);
	int ret;

	ret = regulator_enable(p->vdd);
	if (ret)
		return ret;
	mdelay(10);
	upd(p->aon + AON_PWR_CTRL, AON_USB_PHY_PD, 0);
	upd(p->ahb + AHB_EB, AHB_USB_EB, AHB_USB_EB);
	mdelay(2);
	upd(p->ahb + AHB_RST, AHB_USB_RSTS, AHB_USB_RSTS);
	mdelay(5);
	upd(p->ahb + AHB_RST, AHB_USB_RSTS, 0);
	writel(p->tune_value, p->tune);
	dev_info(&phy->dev, "on, tune %08x\n", readl(p->tune));
	return 0;
}

static int sc8830_usb_phy_power_off(struct phy *phy)
{
	struct sc8830_usb_phy *p = phy_get_drvdata(phy);

	upd(p->ahb + AHB_EB, AHB_USB_EB, 0);
	upd(p->aon + AON_PWR_CTRL, AON_USB_PHY_PD, AON_USB_PHY_PD);
	return regulator_disable(p->vdd);
}

static const struct phy_ops sc8830_usb_phy_ops = {
	.power_on = sc8830_usb_phy_power_on,
	.power_off = sc8830_usb_phy_power_off,
	.owner = THIS_MODULE,
};

static void __iomem *map(struct platform_device *pdev, const char *name)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

	if (!res)
		return IOMEM_ERR_PTR(-EINVAL);
	/* not exclusive: the AON and AP AHB registers are shared with other blocks' drivers */
	return devm_ioremap(&pdev->dev, res->start, resource_size(res)) ?: IOMEM_ERR_PTR(-ENOMEM);
}

static int sc8830_usb_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;
	struct sc8830_usb_phy *p;
	struct phy *phy;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->aon = map(pdev, "aon");
	p->ahb = map(pdev, "ahb");
	p->tune = map(pdev, "tune");
	if (IS_ERR(p->aon) || IS_ERR(p->ahb) || IS_ERR(p->tune))
		return -ENOMEM;
	p->tune_value = 0x44073e33;	/* the vendor's default */
	of_property_read_u32(dev->of_node, "sprd,tune-value", &p->tune_value);

	p->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(p->vdd))
		return dev_err_probe(dev, PTR_ERR(p->vdd), "no vdd\n");

	phy = devm_phy_create(dev, NULL, &sc8830_usb_phy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	phy_set_drvdata(phy, p);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id sc8830_usb_phy_of_match[] = {
	{ .compatible = "sprd,sc8830-usb-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc8830_usb_phy_of_match);

static struct platform_driver sc8830_usb_phy_driver = {
	.probe = sc8830_usb_phy_probe,
	.driver = {
		.name = "sc8830-usb-phy",
		.of_match_table = sc8830_usb_phy_of_match,
	},
};
module_platform_driver(sc8830_usb_phy_driver);

MODULE_DESCRIPTION("Spreadtrum SC8830 USB 2.0 PHY");
MODULE_LICENSE("GPL");
