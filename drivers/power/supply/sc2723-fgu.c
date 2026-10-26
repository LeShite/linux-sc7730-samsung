// SPDX-License-Identifier: GPL-2.0
/*
 * Fuel gauge (FGU) of the Spreadtrum SC2723 PMIC, as on the Galaxy Tab A 7.0 (SM-T280).
 *
 * From the vendor 3.10 kernel (drivers/battery/sprd_2713_fgu.c, drivers/misc/sprd_otp): the FGU
 * sits in the PMIC's ADI window at 0x500 and is left running by the bootloader. It measures the
 * battery voltage and the current through a sense resistor, and integrates the current into a
 * coulomb counter (two samples a second). Its ADC is calibrated at the factory: the A-die efuse
 * holds the reading at 4.2 V (one-point calibration on the SC2723).
 *
 * Capacity: a starting point, then the coulomb counter. The starting point is the percentage the
 * vendor kernel (Android, the charging screen, TWRP) keeps in the FGU's battery-backed user area,
 * or, without one, the power-on open-circuit voltage through the battery's OCV table. The current
 * percentage goes back into that area every minute, so both kernels keep counting from it.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

/* FGU, offsets in the ADI window at 0x40038000 */
#define FGU_VOLT_VAL		0x0520
#define FGU_POCV_VAL		0x0528		/* open-circuit voltage measured at power-on */
#define FGU_CURT_VAL		0x052c
#define FGU_CLBCNT_VALH		0x0568
#define FGU_CLBCNT_VALL		0x056c
#define FGU_USER_AREA_SET	0x05a0		/* battery-backed: power-on type, the UI percentage */
#define FGU_USER_AREA_CLEAR	0x05a4
#define FGU_USER_AREA_STATUS	0x05a8
#define FGU_USER_AREA_SET1	0x05c0		/* ... and the raw percentage */
#define FGU_USER_AREA_CLEAR1	0x05c4
#define FGU_USER_AREA_STATUS1	0x05c8
#define  USER_AREA_SOC		GENMASK(11, 0)
#define FGU_VOLT_MASK		GENMASK(12, 0)
#define FGU_CURT_MASK		GENMASK(13, 0)
#define FGU_CUR_ZERO		8192		/* CUR_0ma_IDEA_ADC */
#define FGU_SAMPLE_HZ		2		/* FGU_CUR_SAMPLE_HZ: coulomb counter samples a second */

/* A-die efuse */
#define EFUSE_DATA_RD		0x0204
#define EFUSE_BLOCK_INDEX	0x020c
#define EFUSE_MODE_CTRL		0x0210
#define  EFUSE_RD_START		BIT(1)
#define EFUSE_STATUS		0x0214
#define  EFUSE_READ_BUSY	BIT(1)
#define ANA_GLB_ARM_MODULE_EN	0x0800
#define  ANA_EFS_EN		BIT(7)

/* ADI controller (0x40030000), as in sc2723-regulator.c */
#define ADI_GSSI_CFG0		0x1c
#define ADI_RD_CMD		0x24
#define ADI_RD_DATA		0x28
#define  ADI_RD_DATA_BUSY	BIT(31)
#define  ADI_RD_DATA_ADDR	GENMASK(30, 16)
#define  ADI_RD_DATA_VAL	GENMASK(15, 0)
#define ADI_FIFO_STS		0x2c
#define  ADI_FIFO_EMPTY		BIT(10)

#define SAVE_INTERVAL_S		60

struct sc2723_fgu {
	struct device *dev;
	void __iomem *adi, *pmic;
	phys_addr_t pmic_phys;
	u32 addr_mask;
	struct mutex lock;
	struct power_supply *psy;
	struct power_supply_battery_info *bat;
	int vol_1000mv_adc;		/* ADC counts per volt */
	int cur_1000ma_adc;		/* ADC counts per ampere */
	int init_cap;			/* percent at init_clbcnt */
	s32 init_clbcnt;
	int cnom_mah;
	struct delayed_work save;
};

static int adi_read(struct sc2723_fgu *f, u32 reg, u16 *val)
{
	u32 phys = f->pmic_phys + reg, data;
	int ret;

	writel(phys, f->adi + ADI_RD_CMD);
	ret = readl_poll_timeout_atomic(f->adi + ADI_RD_DATA, data,
					!(data & ADI_RD_DATA_BUSY), 1, 2000);
	if (ret)
		return ret;
	if ((FIELD_GET(ADI_RD_DATA_ADDR, data) & f->addr_mask) != (phys & f->addr_mask))
		return -EIO;
	*val = FIELD_GET(ADI_RD_DATA_VAL, data);
	return 0;
}

static void adi_write(struct sc2723_fgu *f, u32 reg, u16 val)
{
	u32 sts;

	writel(val, f->pmic + reg);
	readl_poll_timeout_atomic(f->adi + ADI_FIFO_STS, sts, sts & ADI_FIFO_EMPTY, 1, 1000);
}

/* the FGU updates its registers on its own: read until two reads agree */
static int fgu_get(struct sc2723_fgu *f, u32 reg)
{
	u16 a = 0, b = 1;
	int i;

	for (i = 0; i < 10 && a != b; i++)
		if (adi_read(f, reg, &a) || adi_read(f, reg, &b))
			return -EIO;
	return a;
}

static s32 clbcnt_get(struct sc2723_fgu *f)
{
	int l1, h1, l2, h2;

	do {
		l1 = fgu_get(f, FGU_CLBCNT_VALL);
		h1 = fgu_get(f, FGU_CLBCNT_VALH);
		l2 = fgu_get(f, FGU_CLBCNT_VALL);
		h2 = fgu_get(f, FGU_CLBCNT_VALH);
	} while (l1 != l2 || h1 != h2);
	return (s32)((h1 << 16) | l1);
}

static int adc2mv(struct sc2723_fgu *f, int adc)
{
	return adc * 1000 / f->vol_1000mv_adc;
}

static int adc2ma(struct sc2723_fgu *f, int adc)
{
	return adc * 1000 / f->cur_1000ma_adc;
}

static int read_mv(struct sc2723_fgu *f)
{
	int v = fgu_get(f, FGU_VOLT_VAL);

	return v < 0 ? v : adc2mv(f, v & FGU_VOLT_MASK);
}

/* positive while charging */
static int read_ma(struct sc2723_fgu *f)
{
	int c = fgu_get(f, FGU_CURT_VAL);

	return c < 0 ? 0 : adc2ma(f, (c & FGU_CURT_MASK) - FGU_CUR_ZERO);
}

static int ocv2cap(struct sc2723_fgu *f, int mv)
{
	return power_supply_batinfo_ocv2cap(f->bat, mv * 1000, 20);
}

static int capacity(struct sc2723_fgu *f)
{
	s32 delta = clbcnt_get(f) - f->init_clbcnt;
	int mah = adc2ma(f, DIV_ROUND_CLOSEST(delta, 3600 * FGU_SAMPLE_HZ));

	return clamp(f->init_cap + DIV_ROUND_CLOSEST(mah * 100, f->cnom_mah), 0, 100);
}

/* one 8-bit efuse block, inverted as the vendor's adie_efuse_read() returns it */
static int efuse_read(struct sc2723_fgu *f, int blk)
{
	u16 v = 0, sts, mod;
	int i;

	if (adi_read(f, ANA_GLB_ARM_MODULE_EN, &mod))
		return -EIO;
	adi_write(f, ANA_GLB_ARM_MODULE_EN, mod | ANA_EFS_EN);
	adi_write(f, EFUSE_BLOCK_INDEX, blk & 0x1f);
	adi_write(f, EFUSE_MODE_CTRL, EFUSE_RD_START);
	for (i = 0; i < 1000; i++) {
		if (adi_read(f, EFUSE_STATUS, &sts) || !(sts & EFUSE_READ_BUSY))
			break;
		udelay(10);
	}
	adi_read(f, EFUSE_DATA_RD, &v);
	adi_write(f, ANA_GLB_ARM_MODULE_EN, mod);
	return (~v) & 0xff;
}

/* sci_efuse_fgu_cal_get() for the SC2723: the ADC reading at 4.2 V, 9 bits from bit 96 */
static void calibrate(struct sc2723_fgu *f, u32 rsense_real, u32 rsense_spec)
{
	int b0 = efuse_read(f, 0), b12 = efuse_read(f, 12), b13 = efuse_read(f, 13);
	int adc4200 = 0;

	if (b0 >= 0 && !(b0 & BIT(7)) && b12 >= 0 && b13 >= 0)
		adc4200 = ((b12 | (b13 << 8)) & 0x1ff) + 6963 - 4096 - 256;
	if (adc4200 > 2000)
		f->vol_1000mv_adc = DIV_ROUND_CLOSEST(adc4200 * 10, 42);
	else
		f->vol_1000mv_adc = 678;	/* the vendor's uncalibrated default */
	f->cur_1000ma_adc = DIV_ROUND_CLOSEST(f->vol_1000mv_adc * 4 * rsense_real, rsense_spec);
	dev_info(f->dev, "efuse %02x %02x %02x: 4.2 V = %d counts, %d counts/V, %d counts/A\n",
		 b0, b12, b13, adc4200, f->vol_1000mv_adc, f->cur_1000ma_adc);
}

static void save_soc(struct sc2723_fgu *f, int cap)
{
	adi_write(f, FGU_USER_AREA_CLEAR, USER_AREA_SOC);
	adi_write(f, FGU_USER_AREA_SET, cap & USER_AREA_SOC);
	adi_write(f, FGU_USER_AREA_CLEAR1, USER_AREA_SOC);
	adi_write(f, FGU_USER_AREA_SET1, cap & USER_AREA_SOC);
}

static void save_work(struct work_struct *work)
{
	struct sc2723_fgu *f = container_of(work, struct sc2723_fgu, save.work);

	mutex_lock(&f->lock);
	save_soc(f, capacity(f));
	mutex_unlock(&f->lock);
	schedule_delayed_work(&f->save, SAVE_INTERVAL_S * HZ);
}

static const enum power_supply_property sc2723_fgu_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
};

static int sc2723_fgu_get(struct power_supply *psy, enum power_supply_property psp,
			  union power_supply_propval *val)
{
	struct sc2723_fgu *f = power_supply_get_drvdata(psy);
	int ret = 0, ma, cap;

	mutex_lock(&f->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		ma = read_ma(f);
		cap = capacity(f);
		/* with a charger driver (sm5701-charger supplies "battery"), trust its cable sense */
		ret = power_supply_am_i_supplied(psy);
		if (ret >= 0) {
			if (ret && cap >= 100)
				val->intval = POWER_SUPPLY_STATUS_FULL;
			else if (ret && ma > -50)
				val->intval = POWER_SUPPLY_STATUS_CHARGING;
			else
				val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			ret = 0;
			break;
		}
		ret = 0;
		if (ma > 20)
			val->intval = cap >= 100 ? POWER_SUPPLY_STATUS_FULL : POWER_SUPPLY_STATUS_CHARGING;
		else if (ma > -20 && cap >= 100)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = read_mv(f);
		val->intval = ret * 1000;
		ret = ret < 0 ? ret : 0;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = read_ma(f) * 1000;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = capacity(f);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = f->cnom_mah * 1000;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&f->lock);
	return ret;
}

static const struct power_supply_desc sc2723_fgu_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sc2723_fgu_props,
	.num_properties = ARRAY_SIZE(sc2723_fgu_props),
	.get_property = sc2723_fgu_get,
};

static void __iomem *map(struct platform_device *pdev, const char *name, phys_addr_t *phys)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

	if (!res)
		return IOMEM_ERR_PTR(-EINVAL);
	if (phys)
		*phys = res->start;
	/* not exclusive: the regulator, audio and keys drivers map the ADI and PMIC windows too */
	return devm_ioremap(&pdev->dev, res->start, resource_size(res)) ?: IOMEM_ERR_PTR(-ENOMEM);
}

static void sc2723_fgu_stop(void *data)
{
	cancel_delayed_work_sync(data);
}

static int sc2723_fgu_probe(struct platform_device *pdev)
{
	struct power_supply_config cfg = { };
	struct device *dev = &pdev->dev;
	u32 rsense[2] = { 200, 200 }, cfg0, bits;
	struct sc2723_fgu *f;
	int ret, saved, pocv_mv, mv, ma;

	f = devm_kzalloc(dev, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->dev = dev;
	mutex_init(&f->lock);
	f->adi = map(pdev, "adi", NULL);
	f->pmic = map(pdev, "pmic", &f->pmic_phys);
	if (IS_ERR(f->adi) || IS_ERR(f->pmic))
		return -ENOMEM;

	cfg0 = readl(f->adi + ADI_GSSI_CFG0);
	bits = (cfg0 & 0x3f) - ((cfg0 >> 11) & 0x1f) - 1;
	if (bits > 13)
		bits = 9;
	f->addr_mask = GENMASK(bits + 1, 0);

	of_property_read_u32_array(dev->of_node, "sprd,rsense", rsense, 2);	/* real, spec */
	calibrate(f, rsense[0], rsense[1]);

	f->cnom_mah = 3950;	/* until the battery info is read: the property callbacks may run */
	cfg.drv_data = f;
	cfg.fwnode = dev_fwnode(dev);
	f->psy = devm_power_supply_register(dev, &sc2723_fgu_desc, &cfg);
	if (IS_ERR(f->psy))
		return dev_err_probe(dev, PTR_ERR(f->psy), "register failed\n");
	ret = power_supply_get_battery_info(f->psy, &f->bat);
	if (ret)
		return dev_err_probe(dev, ret, "no monitored-battery\n");
	if (f->bat->charge_full_design_uah > 0)
		f->cnom_mah = f->bat->charge_full_design_uah / 1000;

	/* starting point: the percentage the vendor kernel left, else the power-on OCV */
	saved = fgu_get(f, FGU_USER_AREA_STATUS);
	saved = saved < 0 ? 0 : FIELD_GET(USER_AREA_SOC, saved);
	pocv_mv = adc2mv(f, fgu_get(f, FGU_POCV_VAL) & FGU_VOLT_MASK);
	f->init_clbcnt = clbcnt_get(f);
	f->init_cap = saved >= 1 && saved <= 100 ? saved : ocv2cap(f, pocv_mv);
	mv = read_mv(f);
	ma = read_ma(f);
	dev_info(dev, "%d mV, %d mA, power-on OCV %d mV (%d%%), saved %d%% -> starting at %d%%\n",
		 mv, ma, pocv_mv, ocv2cap(f, pocv_mv), saved, f->init_cap);

	INIT_DELAYED_WORK(&f->save, save_work);
	ret = devm_add_action_or_reset(dev, sc2723_fgu_stop, &f->save);
	if (ret)
		return ret;
	schedule_delayed_work(&f->save, SAVE_INTERVAL_S * HZ);
	return 0;
}

static const struct of_device_id sc2723_fgu_of_match[] = {
	{ .compatible = "sprd,sc2723-fgu" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc2723_fgu_of_match);

static struct platform_driver sc2723_fgu_driver = {
	.probe = sc2723_fgu_probe,
	.driver = {
		.name = "sc2723-fgu",
		.of_match_table = sc2723_fgu_of_match,
	},
};
module_platform_driver(sc2723_fgu_driver);

MODULE_DESCRIPTION("Spreadtrum SC2723 PMIC fuel gauge");
MODULE_LICENSE("GPL");
