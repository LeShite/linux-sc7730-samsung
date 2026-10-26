// SPDX-License-Identifier: GPL-2.0
/*
 * CPU clock and core voltage of the Spreadtrum SC8830 family (SC7730) by hand, for overclocking
 * tests. Nothing changes at boot: the driver only reads and reports. Two sysfs files move things:
 *
 *	boost_mv	mV added to the boot voltage of vddarm (0..MAX_BOOST_MV), set first when going up
 *	freq_mhz	CPU clock, a multiple of 26 MHz: the MPLL runs 26 MHz * NINT, integer mode
 *
 * From the vendor 3.10 kernel (cpufreq-scx35.c, sc2723-regulator_dt.c): the CPU mux (AP AHB
 * CA7_CKG_CFG, bits 2:0: 3 = TDPLL 768 MHz, 6 = MPLL) is moved to TDPLL while the MPLL is
 * reprogrammed (AON APB MPLL_CFG1, bits 5:0 = NINT), then back. vddarm is the PMIC's DCDCARM: a
 * coarse step (CTRL0 bits 2:0 -> 1100/700/800/900/1000/650/1200/1300 mV) plus a fine trim
 * (CTRL_CAL bits 4:0, 100/32 mV each) added to it. Writing freq_mhz also sets what the speed
 * needs (tested on the tablet): up to 1300 MHz nothing extra, up to 1404 MHz +25 mV and AXI /3,
 * above that +50 mV and AXI /4. The PMIC sits behind the ADI bus: writes go
 * straight to its window, reads through the ADI read command.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define ADI_GSSI_CFG0		0x1c
#define ADI_RD_CMD		0x24
#define ADI_RD_DATA		0x28
#define  ADI_RD_DATA_BUSY	BIT(31)
#define  ADI_RD_DATA_ADDR	GENMASK(30, 16)
#define  ADI_RD_DATA_VAL	GENMASK(15, 0)
#define ADI_FIFO_STS		0x2c
#define  ADI_FIFO_EMPTY		BIT(10)

#define DCDCARM_CTRL0		0x08ac		/* bits 2:0: coarse step */
#define DCDCARM_CAL		0x08b8		/* bits 4:0: fine trim */
#define  COARSE_MASK		GENMASK(2, 0)
#define  TRIM_MASK		GENMASK(4, 0)
#define TRIM_UV_X32		100000		/* 100 mV over 32 steps */

#define AP_AHB_CA7_CKG_CFG	0x3000
#define  CA7_SEL_MASK		GENMASK(2, 0)
#define  CA7_SEL_TDPLL		3
#define  CA7_SEL_MPLL		6
#define  CA7_AXI_DIV_MASK	GENMASK(10, 8)	/* AXI clock = CPU / (field + 1), at most 500 MHz */
#define AON_MPLL_CFG1		0x3070
#define  MPLL_NINT_MASK		GENMASK(5, 0)
#define  MPLL_SDM_EN		BIT(6)

#define PLL_REF_MHZ		26
#define MIN_MHZ			(PLL_REF_MHZ * 29)	/* 754: just under TDPLL, never below the safe 768 */
#define MAX_MHZ			(PLL_REF_MHZ * 57)	/* 1482, tested stable */
#define MAX_BOOST_MV		100
#define MAX_UV			1200000

static const u32 coarse_uv[8] = { 1100000, 700000, 800000, 900000, 1000000, 650000, 1200000, 1300000 };

struct cpuclk {
	struct device *dev;
	void __iomem *adi, *pmic, *aon, *ahb;
	phys_addr_t pmic_phys;
	u32 addr_mask;
	struct mutex lock;
	int boot_uv;
	int boost_mv;
};

static int adi_read(struct cpuclk *c, u32 reg, u16 *val)
{
	u32 phys = c->pmic_phys + reg, data;
	int ret;

	writel(phys, c->adi + ADI_RD_CMD);
	ret = readl_poll_timeout_atomic(c->adi + ADI_RD_DATA, data, !(data & ADI_RD_DATA_BUSY), 1, 2000);
	if (ret)
		return ret;
	if ((FIELD_GET(ADI_RD_DATA_ADDR, data) & c->addr_mask) != (phys & c->addr_mask))
		return -EIO;
	*val = FIELD_GET(ADI_RD_DATA_VAL, data);
	return 0;
}

static int adi_update(struct cpuclk *c, u32 reg, u16 mask, u16 set)
{
	u16 v;
	u32 sts;
	int ret = adi_read(c, reg, &v);

	if (ret)
		return ret;
	writel((v & ~mask) | set, c->pmic + reg);
	return readl_poll_timeout_atomic(c->adi + ADI_FIFO_STS, sts, sts & ADI_FIFO_EMPTY, 1, 1000);
}

static int get_uv(struct cpuclk *c, int *uv, u16 *coarse, u16 *trim)
{
	u16 ctl, cal;
	int ret = adi_read(c, DCDCARM_CTRL0, &ctl);

	if (!ret)
		ret = adi_read(c, DCDCARM_CAL, &cal);
	if (ret)
		return ret;
	*coarse = ctl & COARSE_MASK;
	*trim = cal & TRIM_MASK;
	*uv = coarse_uv[*coarse] + *trim * TRIM_UV_X32 / 32;
	return 0;
}

/* the coarse step and trim for a voltage: the highest step not above it, then trim up to it */
static int uv_to_regs(int uv, u16 *coarse, u16 *trim)
{
	int i, best = -1;

	for (i = 0; i < 8; i++)
		if (uv >= coarse_uv[i] && uv - coarse_uv[i] < 100000 &&
		    (best < 0 || coarse_uv[i] > coarse_uv[best]))
			best = i;
	if (best < 0)
		return -EINVAL;
	*coarse = best;
	*trim = DIV_ROUND_UP((uv - coarse_uv[best]) * 32, TRIM_UV_X32);
	return *trim > TRIM_MASK ? -EINVAL : 0;
}

/* never below the lower of the old and new voltage on the way: raise the step first, trim first to lower */
static int set_uv(struct cpuclk *c, int uv)
{
	u16 oc, ot, nc, nt;
	int old, ret;

	ret = get_uv(c, &old, &oc, &ot);
	if (!ret)
		ret = uv_to_regs(uv, &nc, &nt);
	if (ret)
		return ret;
	if (coarse_uv[nc] > coarse_uv[oc]) {
		ret = adi_update(c, DCDCARM_CTRL0, COARSE_MASK, nc);
		if (!ret)
			ret = adi_update(c, DCDCARM_CAL, TRIM_MASK, nt);
	} else {
		ret = adi_update(c, DCDCARM_CAL, TRIM_MASK, nt);
		if (!ret)
			ret = adi_update(c, DCDCARM_CTRL0, COARSE_MASK, nc);
	}
	udelay(100 + abs(uv - old) / 1000 * 10);	/* the vendor budgets 10 us per 50 mV; this is generous */
	return ret;
}

static unsigned int get_mhz(struct cpuclk *c)
{
	u32 sel = readl(c->ahb + AP_AHB_CA7_CKG_CFG) & CA7_SEL_MASK;

	if (sel == CA7_SEL_TDPLL)
		return 768;
	if (sel != CA7_SEL_MPLL)
		return 0;
	return PLL_REF_MHZ * FIELD_GET(MPLL_NINT_MASK, readl(c->aon + AON_MPLL_CFG1));
}

static void cpu_sel(struct cpuclk *c, u32 sel)
{
	u32 v = readl(c->ahb + AP_AHB_CA7_CKG_CFG);

	writel((v & ~CA7_SEL_MASK) | sel, c->ahb + AP_AHB_CA7_CKG_CFG);
	readl(c->ahb + AP_AHB_CA7_CKG_CFG);
	udelay(10);
}

static void set_mhz(struct cpuclk *c, unsigned int mhz)
{
	unsigned long flags;
	u32 v;

	local_irq_save(flags);
	cpu_sel(c, CA7_SEL_TDPLL);
	v = readl(c->aon + AON_MPLL_CFG1);
	v = (v & ~(MPLL_NINT_MASK | MPLL_SDM_EN)) | (mhz / PLL_REF_MHZ);
	writel(v, c->aon + AON_MPLL_CFG1);
	readl(c->aon + AON_MPLL_CFG1);
	udelay(500);			/* lock; this chip has no lock flag in this register */
	cpu_sel(c, CA7_SEL_MPLL);
	local_irq_restore(flags);
}

static unsigned int mhz_to_axi_div(unsigned int mhz)
{
	return mhz > 1404 ? 4 : 3;
}

static int mhz_to_boost_mv(unsigned int mhz)
{
	return mhz > 1404 ? 50 : mhz > 1300 ? 25 : 0;
}

static ssize_t freq_mhz_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cpuclk *c = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", get_mhz(c));
}

static void set_axi_div(struct cpuclk *c, unsigned int div)
{
	u32 v = readl(c->ahb + AP_AHB_CA7_CKG_CFG);

	writel((v & ~CA7_AXI_DIV_MASK) | FIELD_PREP(CA7_AXI_DIV_MASK, div - 1), c->ahb + AP_AHB_CA7_CKG_CFG);
	readl(c->ahb + AP_AHB_CA7_CKG_CFG);
}

static ssize_t freq_mhz_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t n)
{
	struct cpuclk *c = dev_get_drvdata(dev);
	unsigned int mhz, cur, axi;
	int mv, ret = kstrtouint(buf, 0, &mhz);

	if (ret)
		return ret;
	mhz = mhz / PLL_REF_MHZ * PLL_REF_MHZ;
	if (mhz < MIN_MHZ || mhz > MAX_MHZ)
		return -EINVAL;
	axi = mhz_to_axi_div(mhz);
	mv = mhz_to_boost_mv(mhz);
	mutex_lock(&c->lock);
	cur = get_mhz(c);
	if (mhz > cur) {			/* up: voltage and the slower bus first */
		ret = mv > c->boost_mv ? set_uv(c, c->boot_uv + mv * 1000) : 0;
		if (!ret)
			c->boost_mv = max(c->boost_mv, mv);
		if (!ret && axi > mhz_to_axi_div(cur))
			set_axi_div(c, axi);
		if (!ret)
			set_mhz(c, mhz);
	} else {				/* down: clock first */
		set_mhz(c, mhz);
		set_axi_div(c, axi);
		ret = set_uv(c, c->boot_uv + mv * 1000);
		if (!ret)
			c->boost_mv = mv;
	}
	mutex_unlock(&c->lock);
	dev_info(dev, "CPU now %u MHz, +%d mV\n", get_mhz(c), c->boost_mv);
	return ret ?: n;
}

static ssize_t boost_mv_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cpuclk *c = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", c->boost_mv);
}

static ssize_t boost_mv_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t n)
{
	struct cpuclk *c = dev_get_drvdata(dev);
	int mv, ret = kstrtoint(buf, 0, &mv);

	if (ret)
		return ret;
	if (mv < 0 || mv > MAX_BOOST_MV || c->boot_uv + mv * 1000 > MAX_UV)
		return -EINVAL;
	mutex_lock(&c->lock);
	if (mv < c->boost_mv && get_mhz(c) > 1300) {	/* lower the clock before the voltage */
		mutex_unlock(&c->lock);
		return -EBUSY;
	}
	ret = set_uv(c, c->boot_uv + mv * 1000);
	if (!ret)
		c->boost_mv = mv;
	mutex_unlock(&c->lock);
	return ret ?: n;
}

static ssize_t vddarm_uv_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cpuclk *c = dev_get_drvdata(dev);
	u16 co, tr;
	int uv, ret;

	mutex_lock(&c->lock);
	ret = get_uv(c, &uv, &co, &tr);
	mutex_unlock(&c->lock);
	return ret ?: sysfs_emit(buf, "%d coarse %u trim %u\n", uv, co, tr);
}

static DEVICE_ATTR_RW(freq_mhz);
static DEVICE_ATTR_RW(boost_mv);
static DEVICE_ATTR_RO(vddarm_uv);

static struct attribute *cpuclk_attrs[] = {
	&dev_attr_freq_mhz.attr, &dev_attr_boost_mv.attr, &dev_attr_vddarm_uv.attr, NULL
};
ATTRIBUTE_GROUPS(cpuclk);

static void __iomem *map(struct platform_device *pdev, const char *name, phys_addr_t *phys)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

	if (!res)
		return IOMEM_ERR_PTR(-EINVAL);
	if (phys)
		*phys = res->start;
	/* not exclusive: other drivers share these blocks */
	return devm_ioremap(&pdev->dev, res->start, resource_size(res)) ?: IOMEM_ERR_PTR(-ENOMEM);
}

static int cpuclk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cpuclk *c;
	u16 co, tr;
	u32 cfg0, bits;
	int ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = dev;
	mutex_init(&c->lock);
	c->adi = map(pdev, "adi", NULL);
	c->pmic = map(pdev, "pmic", &c->pmic_phys);
	c->aon = map(pdev, "aon", NULL);
	c->ahb = map(pdev, "ahb", NULL);
	if (IS_ERR(c->adi) || IS_ERR(c->pmic) || IS_ERR(c->aon) || IS_ERR(c->ahb))
		return -ENOMEM;
	cfg0 = readl(c->adi + ADI_GSSI_CFG0);
	bits = (cfg0 & 0x3f) - ((cfg0 >> 11) & 0x1f) - 1;
	if (bits > 13)
		bits = 9;
	c->addr_mask = GENMASK(bits + 1, 0);
	platform_set_drvdata(pdev, c);

	ret = get_uv(c, &c->boot_uv, &co, &tr);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read vddarm\n");
	dev_info(dev, "vddarm %d uV (coarse %u trim %u), CPU %u MHz (CA7_CKG_CFG %#x, MPLL_CFG1 %#x)\n",
		 c->boot_uv, co, tr, get_mhz(c), readl(c->ahb + AP_AHB_CA7_CKG_CFG),
		 readl(c->aon + AON_MPLL_CFG1));
	return 0;
}

static const struct of_device_id cpuclk_of_match[] = {
	{ .compatible = "sprd,sc8830-cpuclk" },
	{ }
};
MODULE_DEVICE_TABLE(of, cpuclk_of_match);

static struct platform_driver cpuclk_driver = {
	.probe = cpuclk_probe,
	.driver = {
		.name = "sc8830-cpuclk",
		.of_match_table = cpuclk_of_match,
		.dev_groups = cpuclk_groups,
	},
};
module_platform_driver(cpuclk_driver);

MODULE_DESCRIPTION("Spreadtrum SC8830 CPU clock and vddarm, by hand");
MODULE_LICENSE("GPL");
