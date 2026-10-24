// SPDX-License-Identifier: GPL-2.0
/*
 * SMP bring-up for the Spreadtrum SC8830 family (SC7730, ...)
 *
 * The boot ROM parks the secondary Cortex-A7 cores. A core jumps to the
 * address in its AP AHB jump register once its bit is set in the holding
 * pen register, its power domain is on and its reset is released. Register
 * layout from the vendor 3.10 kernel (drivers/platform/sprd/platsmp.c).
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/smp.h>
#include <linux/spinlock.h>

#include <asm/smp_plat.h>

#define AP_AHB_CA7_RST_SET		0x08
#define AP_AHB_HOLDING_PEN		0x14
#define AP_AHB_CPU_JUMP(cpu)		(0x18 + 4 * (cpu))

#define PMU_PD_CA7_C_CFG(cpu)		(0x04 + 4 * (cpu))	/* cores 1..3 */
#define PMU_PD_CA7_FORCE_SHUTDOWN	BIT(25)
#define PMU_PD_CA7_AUTO_SHUTDOWN_EN	BIT(24)
/* power-on, power-on sequence and isolation delays, as the vendor sets them */
#define PMU_PD_CA7_DELAYS		((4 << 16) | (4 << 8) | 4)

static void __iomem *ap_ahb_base;
static void __iomem *pmu_base;
static DEFINE_SPINLOCK(boot_lock);

static void __iomem *__init sc8830_smp_map(const char *compatible)
{
	struct device_node *node;
	void __iomem *base;

	node = of_find_compatible_node(NULL, NULL, compatible);
	if (!node) {
		pr_err("SMP: missing %s node\n", compatible);
		return NULL;
	}

	base = of_iomap(node, 0);
	of_node_put(node);
	if (!base)
		pr_err("SMP: couldn't map %s\n", compatible);

	return base;
}

static void __init sc8830_smp_prepare_cpus(unsigned int max_cpus)
{
	ap_ahb_base = sc8830_smp_map("sprd,sc8830-ap-ahb");
	pmu_base = sc8830_smp_map("sprd,sc8830-pmu-apb");
}

static void sc8830_rmw(void __iomem *reg, u32 clr, u32 set)
{
	writel((readl(reg) & ~clr) | set, reg);
}

static int sc8830_smp_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	unsigned int core = MPIDR_AFFINITY_LEVEL(cpu_logical_map(cpu), 0);
	void __iomem *pd;
	int tries;

	if (!ap_ahb_base || !pmu_base)
		return -EFAULT;
	if (core < 1 || core > 3)
		return -EINVAL;

	pd = pmu_base + PMU_PD_CA7_C_CFG(core);

	spin_lock(&boot_lock);

	writel(__pa_symbol(secondary_startup), ap_ahb_base + AP_AHB_CPU_JUMP(core));
	sc8830_rmw(ap_ahb_base + AP_AHB_HOLDING_PEN, 0, BIT(core));

	/* Power the core up while holding it in reset */
	sc8830_rmw(ap_ahb_base + AP_AHB_CA7_RST_SET, 0, BIT(core));
	writel(PMU_PD_CA7_DELAYS, pd);
	/*
	 * Unlike the vendor kernel, leave AUTO_SHUTDOWN_EN clear: it powers
	 * the core off in WFI, and plain WFI is what the idle loop uses here.
	 */
	sc8830_rmw(pd, PMU_PD_CA7_FORCE_SHUTDOWN | PMU_PD_CA7_AUTO_SHUTDOWN_EN, 0);
	dsb();
	mdelay(1);

	for (tries = 0; tries < 20 && (readl(pd) & PMU_PD_CA7_FORCE_SHUTDOWN); tries++) {
		sc8830_rmw(pd, PMU_PD_CA7_FORCE_SHUTDOWN, 0);
		dsb();
		udelay(500);
	}

	/* Release the reset: the core leaves the ROM pen for secondary_startup */
	sc8830_rmw(ap_ahb_base + AP_AHB_CA7_RST_SET, BIT(core), 0);
	dsb_sev();

	spin_unlock(&boot_lock);

	return 0;
}

static const struct smp_operations sc8830_smp_ops __initconst = {
	.smp_prepare_cpus	= sc8830_smp_prepare_cpus,
	.smp_boot_secondary	= sc8830_smp_boot_secondary,
};
CPU_METHOD_OF_DECLARE(sc8830_smp, "sprd,sc8830-smp", &sc8830_smp_ops);
