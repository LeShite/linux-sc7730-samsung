// SPDX-License-Identifier: GPL-2.0
/*
 * Temperature sensors of the Spreadtrum SC8830 family (SC7730): one next to the CPU cores, one next
 * to the GPU, in a block at 0x402f0000 that the bootloader leaves off.
 *
 * From the vendor 3.10 kernel (drivers/thermal/sprd_ddie_thm.c): the block's clocks and reset sit in
 * AON APB (EB1 bit 1, RTC_EB bits 10-14, RST1 bit 1); each sensor has 0x100 bytes of registers, its
 * reading is a 15-bit raw value at +0x60 and the conversion is linear,
 *	T [m°C] = -40000 + (raw - 623) * 160000 / (1030 - 623)
 * (the efuse calibration only shifts it by a few thousandths of a degree).
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

#define THM_CTRL		0x0000
#define THM_INT_CTRL		0x0004
#define SENSOR_OFF(n)		((n) * 0x100)
#define SENSOR_CTRL		0x0020
#define SENSOR_DET_PERI		0x0024
#define SENSOR_INT_CTRL		0x0028
#define SENSOR_INT_CLR		0x0034
#define SENSOR_MON_PERI		0x0058
#define SENSOR_MON_CTL		0x005c
#define SENSOR_TEMPER0_READ	0x0060
#define  RAW_MASK		0x7fff

/* AON APB (0x402e0000) */
#define AON_APB_EB1		0x0004
#define  AON_THM_EB		BIT(1)
#define AON_APB_RST1		0x000c
#define  AON_THM_SOFT_RST	BIT(1)
#define AON_APB_RTC_EB		0x0010
#define  AON_THM_RTC_EB_ALL	GENMASK(14, 10)	/* THM, ARM/GPU THMA and their auto enables */

#define RAW_LOW			623
#define RAW_HIGH		1030
#define RAW_VALID_MIN		550
#define RAW_VALID_MAX		1200
#define TEMP_LOW_MC		(-40000)
#define TEMP_HIGH_MC		120000

struct sc8830_thm {
	void __iomem *thm, *aon;
};

struct sc8830_thm_sensor {
	struct sc8830_thm *thm;
	int id;
};

static void upd(void __iomem *reg, u32 clear, u32 set)
{
	writel((readl(reg) & ~clear) | set, reg);
}

static int sc8830_thm_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct sc8830_thm_sensor *s = thermal_zone_device_priv(tz);
	u32 raw = readl(s->thm->thm + SENSOR_OFF(s->id) + SENSOR_TEMPER0_READ) & RAW_MASK;

	if (raw < RAW_VALID_MIN || raw > RAW_VALID_MAX)
		return -EAGAIN;			/* not measured yet */
	*temp = TEMP_LOW_MC + (int)(raw - RAW_LOW) * (TEMP_HIGH_MC - TEMP_LOW_MC) / (RAW_HIGH - RAW_LOW);
	return 0;
}

static const struct thermal_zone_device_ops sc8830_thm_ops = {
	.get_temp = sc8830_thm_get_temp,
};

static void sc8830_thm_sensor_init(struct sc8830_thm *t, int id)
{
	void __iomem *s = t->thm + SENSOR_OFF(id);

	upd(s + SENSOR_INT_CTRL, 0xffff, 0);		/* no interrupts */
	upd(s + SENSOR_INT_CLR, 0, 0xffff);
	upd(s + SENSOR_DET_PERI, 0x4000, 0x4000);
	upd(s + SENSOR_MON_CTL, 0x21, 0x21);
	upd(s + SENSOR_MON_PERI, 0x100, 0x400);
	if (id == 0)		/* the vendor also sets the GPU sensor's control from the CPU one */
		upd(t->thm + SENSOR_OFF(1) + SENSOR_CTRL, 0x30, 0x30);
	upd(s + SENSOR_CTRL, 0x30, 0x39);
}

static void __iomem *map(struct platform_device *pdev, const char *name)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

	if (!res)
		return IOMEM_ERR_PTR(-EINVAL);
	/* not exclusive: other drivers use the AON APB enables */
	return devm_ioremap(&pdev->dev, res->start, resource_size(res)) ?: IOMEM_ERR_PTR(-ENOMEM);
}

static int sc8830_thm_probe(struct platform_device *pdev)
{
	static const char * const names[] = { "cpu_thermal", "gpu_thermal" };
	struct device *dev = &pdev->dev;
	struct sc8830_thm *t;
	int i;

	t = devm_kzalloc(dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	t->thm = map(pdev, "thm");
	t->aon = map(pdev, "aon");
	if (IS_ERR(t->thm) || IS_ERR(t->aon))
		return -ENOMEM;

	upd(t->aon + AON_APB_EB1, 0, AON_THM_EB);
	upd(t->aon + AON_APB_RTC_EB, 0, AON_THM_RTC_EB_ALL);
	upd(t->aon + AON_APB_RST1, 0, AON_THM_SOFT_RST);
	udelay(100);
	upd(t->aon + AON_APB_RST1, AON_THM_SOFT_RST, 0);
	upd(t->thm + THM_CTRL, 0, 0x3);
	upd(t->thm + THM_INT_CTRL, 0, 0x1);

	for (i = 0; i < 2; i++) {
		struct thermal_zone_device *tz;
		struct sc8830_thm_sensor *s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);

		if (!s)
			return -ENOMEM;
		s->thm = t;
		s->id = i;
		sc8830_thm_sensor_init(t, i);
		/* no trips: this only reports the temperature (built in, never unregistered) */
		tz = thermal_zone_device_register_with_trips(names[i], NULL, 0, s, &sc8830_thm_ops, NULL, 0, 0);
		if (IS_ERR(tz))
			return dev_err_probe(dev, PTR_ERR(tz), "zone %d\n", i);
		thermal_zone_device_enable(tz);
	}
	msleep(200);
	dev_info(dev, "sensors on, raw %u (CPU) and %u (GPU)\n",
		 readl(t->thm + SENSOR_TEMPER0_READ) & RAW_MASK,
		 readl(t->thm + SENSOR_OFF(1) + SENSOR_TEMPER0_READ) & RAW_MASK);
	return 0;
}

static const struct of_device_id sc8830_thm_of_match[] = {
	{ .compatible = "sprd,sc8830-thermal" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc8830_thm_of_match);

static struct platform_driver sc8830_thm_driver = {
	.probe = sc8830_thm_probe,
	.driver = {
		.name = "sc8830-thermal",
		.of_match_table = sc8830_thm_of_match,
	},
};
module_platform_driver(sc8830_thm_driver);

MODULE_DESCRIPTION("Spreadtrum SC8830 CPU and GPU temperature sensors");
MODULE_LICENSE("GPL");
