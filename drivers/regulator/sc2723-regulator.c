// SPDX-License-Identifier: GPL-2.0
/*
 * LDOs of the Spreadtrum SC2723 PMIC, reached through the ADI bus of the
 * SC8830 family (SC7730, ...)
 *
 * ADI (version 1, as on the SC8830): PMIC registers appear in a window of
 * the ADI slave space. A write is a plain store of the 16-bit value there
 * (it goes through a FIFO); a read posts the register's physical address to
 * RD_CMD and polls RD_DATA, which echoes the low address bits. Taken from
 * the vendor 3.10 kernel (drivers/platform/sprd/adi.c).
 *
 * LDOs (vendor sc2723-regulators.dtsi and sc2723-regulator_dt.c): an
 * active-high power-down bit, and an 8-bit trim field selecting
 * 1.2 V + n * 10 mV. Each regulator is described in DT:
 *	sprd,pd = <offset bit>;
 *	sprd,trim = <offset mask>;
 * with offsets relative to the PMIC window. A DC-DC with another trim scale (vddcon: 0.6 V +
 * n * 3.125 mV) adds
 *	sprd,voltage = <min_uV step_uV>;
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/sc8830-adi.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#define ADI_GSSI_CFG0		0x1c
#define ADI_RD_CMD		0x24
#define ADI_RD_DATA		0x28
#define ADI_RD_DATA_BUSY	BIT(31)
#define ADI_RD_DATA_ADDR	GENMASK(30, 16)
#define ADI_RD_DATA_VAL		GENMASK(15, 0)
#define ADI_FIFO_STS		0x2c
#define ADI_FIFO_EMPTY		BIT(10)

/* power-off (vendor sc8830_power_off(), SC2723S): every LDO and DC-DC down */
#define ANA_GLB_LDO_DCDC_PD	0x0810
#define ANA_GLB_LDO_PD_CTRL	0x0814
#define ANA_GLB_PWR_WR_PROT	0x0950
#define  PWR_WR_PROT		BIT(15)
#define  PWR_WR_PROT_KEY	0x6e7f

/* restart (vendor sys_reset.c): the boot mode for the bootloader, then the PMIC watchdog */
#define ANA_GLB_ARM_MODULE_EN	0x0800
#define  ANA_WDG_EN		BIT(2)
#define ANA_GLB_RTC_CLK_EN	0x0808
#define  RTC_WDG_EN		BIT(2)
#define ANA_GLB_POR_RST_MONITOR	0x0924
#define WDG_LOAD_LOW		0x0040
#define WDG_LOAD_HIGH		0x0044
#define WDG_CTRL		0x0048
#define  WDG_CNT_EN		BIT(1)
#define  WDG_NEW_VER_EN		BIT(2)
#define  WDG_RST_EN		BIT(3)
#define WDG_LOCK		0x0060
#define  WDG_UNLOCK_KEY		0xe551

#define SC2723_LDO_MIN_UV	1200000
#define SC2723_LDO_STEP_UV	10000

struct sc2723 {
	struct device *dev;
	void __iomem *adi;		/* ADI controller */
	void __iomem *win;		/* PMIC register window */
	phys_addr_t win_phys;
	u32 addr_mask;			/* address bits echoed by reads */
	spinlock_t lock;
};

struct sc2723_ldo {
	struct regulator_desc desc;
	struct sc2723 *pmic;
	u32 pd_reg, pd_bit;
	u32 trim_reg, trim_mask;
};

/* the caller holds sc8830_adi_lock */
static int __sc2723_read(struct sc2723 *pmic, u32 reg, u16 *val)
{
	u32 phys = pmic->win_phys + reg, data;
	int ret;

	writel(phys, pmic->adi + ADI_RD_CMD);
	ret = readl_poll_timeout_atomic(pmic->adi + ADI_RD_DATA, data,
					!(data & ADI_RD_DATA_BUSY), 1, 2000);

	if (ret)
		return ret;
	if ((FIELD_GET(ADI_RD_DATA_ADDR, data) & pmic->addr_mask) !=
	    (phys & pmic->addr_mask))
		return -EIO;

	*val = FIELD_GET(ADI_RD_DATA_VAL, data);
	return 0;
}

static int sc2723_read(struct sc2723 *pmic, u32 reg, u16 *val)
{
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&sc8830_adi_lock, flags);
	ret = __sc2723_read(pmic, reg, val);
	raw_spin_unlock_irqrestore(&sc8830_adi_lock, flags);
	return ret;
}

static int sc2723_update(struct sc2723 *pmic, u32 reg, u16 mask, u16 val)
{
	unsigned long flags;
	u16 old;
	u32 sts;
	int ret;

	raw_spin_lock_irqsave(&sc8830_adi_lock, flags);
	ret = __sc2723_read(pmic, reg, &old);
	if (!ret) {
		writel((old & ~mask) | (val & mask), pmic->win + reg);
		ret = readl_poll_timeout_atomic(pmic->adi + ADI_FIFO_STS, sts,
						sts & ADI_FIFO_EMPTY, 1, 1000);
	}
	raw_spin_unlock_irqrestore(&sc8830_adi_lock, flags);

	return ret;
}

static void sc2723_write(struct sc2723 *pmic, u32 reg, u16 val)
{
	unsigned long flags;
	u32 sts;

	raw_spin_lock_irqsave(&sc8830_adi_lock, flags);
	writel(val, pmic->win + reg);
	readl_poll_timeout_atomic(pmic->adi + ADI_FIFO_STS, sts, sts & ADI_FIFO_EMPTY, 1, 1000);
	raw_spin_unlock_irqrestore(&sc8830_adi_lock, flags);
}

/* the vendor's sequence: LDOs off, unlock the power registers, then all LDOs and DC-DCs off */
static int sc2723_power_off(struct sys_off_data *data)
{
	struct sc2723 *pmic = data->cb_data;
	u16 prot = 0;
	int i;

	sc2723_write(pmic, ANA_GLB_LDO_PD_CTRL, 0x1fff);
	sc2723_write(pmic, ANA_GLB_PWR_WR_PROT, PWR_WR_PROT_KEY);
	for (i = 0; i < 1000 && !(prot & PWR_WR_PROT); i++)
		if (sc2723_read(pmic, ANA_GLB_PWR_WR_PROT, &prot))
			prot = 0;
	sc2723_write(pmic, ANA_GLB_LDO_PD_CTRL, 0xfff);
	sc2723_write(pmic, ANA_GLB_LDO_DCDC_PD, 0x7fff);
	mdelay(1000);
	return NOTIFY_DONE;
}

/* the reset reason the bootloader reads (vendor sprd_set_reboot_mode()); a plain reboot is "special" */
static u16 sc2723_reset_reason(const char *cmd)
{
	static const struct { const char *cmd; u16 code; } map[] = {
		{ "recovery", 0x20 }, { "alarm", 0x50 }, { "fastsleep", 0x60 }, { "bootloader", 0x30 },
		{ "panic", 0x80 }, { "special", 0x70 }, { "cftreboot", 0x90 }, { "autodloader", 0xa0 },
		{ "iqmode", 0xb0 },
	};
	int i;

	if (!cmd)
		return 0x70;
	for (i = 0; i < ARRAY_SIZE(map); i++)
		if (!strncmp(cmd, map[i].cmd, strlen(map[i].cmd)))
			return map[i].code;
	return 0x40;
}

/*
 * The vendor's arch_reset(): the reason, then sprd_turnon_watchdog(50): a count of
 * 50 * 1000 / 32768 = 1 tick. The watchdog then resets the whole board.
 */
static int sc2723_restart(struct sys_off_data *data)
{
	struct sc2723 *pmic = data->cb_data;
	u32 cnt = 50 * 1000 / 32768;

	sc2723_write(pmic, ANA_GLB_POR_RST_MONITOR, sc2723_reset_reason(data->cmd));
	sc2723_update(pmic, ANA_GLB_ARM_MODULE_EN, ANA_WDG_EN, ANA_WDG_EN);
	sc2723_update(pmic, ANA_GLB_RTC_CLK_EN, RTC_WDG_EN, RTC_WDG_EN);
	sc2723_write(pmic, WDG_LOCK, WDG_UNLOCK_KEY);
	sc2723_update(pmic, WDG_CTRL, WDG_NEW_VER_EN, WDG_NEW_VER_EN);
	sc2723_write(pmic, WDG_LOAD_HIGH, cnt >> 16);
	sc2723_write(pmic, WDG_LOAD_LOW, cnt & 0xffff);
	sc2723_update(pmic, WDG_CTRL, WDG_CNT_EN | WDG_RST_EN, WDG_CNT_EN | WDG_RST_EN);
	sc2723_write(pmic, WDG_LOCK, (u16)~WDG_UNLOCK_KEY);
	mdelay(1000);
	return NOTIFY_DONE;
}

static struct sc2723_ldo *rdev_to_ldo(struct regulator_dev *rdev)
{
	return container_of(rdev->desc, struct sc2723_ldo, desc);
}

static int sc2723_ldo_enable(struct regulator_dev *rdev)
{
	struct sc2723_ldo *ldo = rdev_to_ldo(rdev);

	return sc2723_update(ldo->pmic, ldo->pd_reg, ldo->pd_bit, 0);
}

static int sc2723_ldo_disable(struct regulator_dev *rdev)
{
	struct sc2723_ldo *ldo = rdev_to_ldo(rdev);

	return sc2723_update(ldo->pmic, ldo->pd_reg, ldo->pd_bit, ldo->pd_bit);
}

static int sc2723_ldo_is_enabled(struct regulator_dev *rdev)
{
	struct sc2723_ldo *ldo = rdev_to_ldo(rdev);
	u16 val;
	int ret;

	ret = sc2723_read(ldo->pmic, ldo->pd_reg, &val);
	return ret ? ret : !(val & ldo->pd_bit);
}

static int sc2723_ldo_set_voltage_sel(struct regulator_dev *rdev,
				      unsigned int sel)
{
	struct sc2723_ldo *ldo = rdev_to_ldo(rdev);

	return sc2723_update(ldo->pmic, ldo->trim_reg, ldo->trim_mask,
			     sel << __ffs(ldo->trim_mask));
}

static int sc2723_ldo_get_voltage_sel(struct regulator_dev *rdev)
{
	struct sc2723_ldo *ldo = rdev_to_ldo(rdev);
	u16 val;
	int ret;

	ret = sc2723_read(ldo->pmic, ldo->trim_reg, &val);
	return ret ? ret : (val & ldo->trim_mask) >> __ffs(ldo->trim_mask);
}

static const struct regulator_ops sc2723_ldo_ops = {
	.enable = sc2723_ldo_enable,
	.disable = sc2723_ldo_disable,
	.is_enabled = sc2723_ldo_is_enabled,
	.set_voltage_sel = sc2723_ldo_set_voltage_sel,
	.get_voltage_sel = sc2723_ldo_get_voltage_sel,
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
};

static int sc2723_register_ldo(struct sc2723 *pmic, struct device_node *np)
{
	struct regulator_config cfg = { };
	struct regulator_dev *rdev;
	struct sc2723_ldo *ldo;
	u32 pd[2], trim[2], volt[2] = { SC2723_LDO_MIN_UV, SC2723_LDO_STEP_UV };
	int ret;

	if (of_property_read_u32_array(np, "sprd,pd", pd, 2) ||
	    of_property_read_u32_array(np, "sprd,trim", trim, 2) || !trim[1]) {
		dev_err(pmic->dev, "%pOF: sprd,pd and sprd,trim are required\n", np);
		return -EINVAL;
	}

	of_property_read_u32_array(np, "sprd,voltage", volt, 2);

	ldo = devm_kzalloc(pmic->dev, sizeof(*ldo), GFP_KERNEL);
	if (!ldo)
		return -ENOMEM;

	ldo->pmic = pmic;
	ldo->pd_reg = pd[0];
	ldo->pd_bit = pd[1];
	ldo->trim_reg = trim[0];
	ldo->trim_mask = trim[1];

	ldo->desc.name = np->name;
	ldo->desc.of_match = np->name;
	ldo->desc.type = REGULATOR_VOLTAGE;
	ldo->desc.owner = THIS_MODULE;
	ldo->desc.ops = &sc2723_ldo_ops;
	ldo->desc.min_uV = volt[0];
	ldo->desc.uV_step = volt[1];
	ldo->desc.n_voltages = (trim[1] >> __ffs(trim[1])) + 1;
	ldo->desc.enable_time = 1000;

	cfg.dev = pmic->dev;
	cfg.of_node = np;
	cfg.init_data = of_get_regulator_init_data(pmic->dev, np, &ldo->desc);

	rdev = devm_regulator_register(pmic->dev, &ldo->desc, &cfg);
	if (IS_ERR(rdev))
		return dev_err_probe(pmic->dev, PTR_ERR(rdev),
				     "%pOF: register failed\n", np);

	ret = sc2723_ldo_get_voltage_sel(rdev);
	dev_info(pmic->dev, "%s: %s, %d uV\n", np->name,
		 sc2723_ldo_is_enabled(rdev) > 0 ? "on" : "off",
		 ret < 0 ? ret : regulator_list_voltage_linear(rdev, ret));
	return 0;
}

static int sc2723_probe(struct platform_device *pdev)
{
	struct device_node *child;
	struct sc2723 *pmic;
	struct resource *res;
	u32 cfg0, bits;
	int ret;

	pmic = devm_kzalloc(&pdev->dev, sizeof(*pmic), GFP_KERNEL);
	if (!pmic)
		return -ENOMEM;

	pmic->dev = &pdev->dev;
	spin_lock_init(&pmic->lock);

	pmic->adi = devm_platform_ioremap_resource_byname(pdev, "adi");
	if (IS_ERR(pmic->adi))
		return PTR_ERR(pmic->adi);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pmic");
	if (!res)
		return -EINVAL;
	pmic->win_phys = res->start;
	pmic->win = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pmic->win))
		return PTR_ERR(pmic->win);

	/* The bootloader set the ADI up; derive the read-back address width */
	cfg0 = readl(pmic->adi + ADI_GSSI_CFG0);
	bits = (cfg0 & 0x3f) - ((cfg0 >> 11) & 0x1f) - 1;
	if (bits > 13)		/* unexpected config: the version 0 width */
		bits = 9;
	pmic->addr_mask = GENMASK(bits + 1, 0);

	for_each_available_child_of_node(pdev->dev.of_node, child) {
		ret = sc2723_register_ldo(pmic, child);
		if (ret) {
			of_node_put(child);
			return ret;
		}
	}

	ret = devm_register_power_off_handler(&pdev->dev, sc2723_power_off, pmic);
	if (ret)
		dev_warn(&pdev->dev, "no power-off handler: %d\n", ret);
	ret = devm_register_restart_handler(&pdev->dev, sc2723_restart, pmic);
	if (ret)
		dev_warn(&pdev->dev, "no restart handler: %d\n", ret);
	return 0;
}

static const struct of_device_id sc2723_of_match[] = {
	{ .compatible = "sprd,sc2723-regulator" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc2723_of_match);

static struct platform_driver sc2723_driver = {
	.probe = sc2723_probe,
	.driver = {
		.name = "sc2723-regulator",
		.of_match_table = sc2723_of_match,
	},
};
module_platform_driver(sc2723_driver);

MODULE_DESCRIPTION("Spreadtrum SC2723 PMIC LDOs over ADI");
MODULE_LICENSE("GPL");
