// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPU power domain of the Spreadtrum SC8830 family (SC7730, ...)
 *
 * Powers the Mali-400 up the way the vendor 3.10 clock tree does
 * (chip_x30g/__clock_tree.h: clk_gpu_top_i, clk_gpu_i, clk_gpu):
 *  - PMU APB PD_GPU_TOP_CFG: clear FORCE_SHUTDOWN (and AUTO_SHUTDOWN_EN);
 *  - AON APB APB_EB0: set GPU_EB;
 *  - GPU APB APB_CLK_CTRL: select the 384 MHz parent, divider 1.
 * The domain is kept always on for now.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>

#define SC8830_PD_GPU_TOP_FORCE_SHUTDOWN	BIT(25)
#define SC8830_PD_GPU_TOP_AUTO_SHUTDOWN_EN	BIT(24)
#define SC8830_AON_APB_EB0_GPU_EB		BIT(27)
#define SC8830_GPU_APB_CLK_CTRL			0x4
#define SC8830_GPU_CLK_SEL_MASK			GENMASK(2, 0)
#define SC8830_GPU_CLK_DIV_MASK			GENMASK(5, 4)
#define SC8830_GPU_CLK_SEL_384M			5

struct sc8830_gpu_pd {
	struct generic_pm_domain genpd;
	void __iomem *pmu;
	void __iomem *aon;
	void __iomem *gpu;
};

static void sc8830_gpu_pd_rmw(void __iomem *reg, u32 clr, u32 set)
{
	writel((readl(reg) & ~clr) | set, reg);
}

/*
 * Bring-up aid: announce each step and give the deferred fbcon flush time to
 * reach the panel, so that a bus hang leaves the failing step on screen.
 */
static void sc8830_gpu_pd_step(struct generic_pm_domain *genpd, const char *what)
{
	pr_info("sc8830-gpu-pd: %s\n", what);
	msleep(200);
}

static int sc8830_gpu_pd_power_on(struct generic_pm_domain *genpd)
{
	struct sc8830_gpu_pd *pd = container_of(genpd, struct sc8830_gpu_pd, genpd);

	sc8830_gpu_pd_step(genpd, "paso 1/3: energia (PD_GPU_TOP)");
	sc8830_gpu_pd_rmw(pd->pmu, SC8830_PD_GPU_TOP_FORCE_SHUTDOWN |
			  SC8830_PD_GPU_TOP_AUTO_SHUTDOWN_EN, 0);
	usleep_range(200, 300);
	sc8830_gpu_pd_step(genpd, "paso 2/3: habilitar (GPU_EB)");
	sc8830_gpu_pd_rmw(pd->aon, 0, SC8830_AON_APB_EB0_GPU_EB);
	usleep_range(10, 20);
	sc8830_gpu_pd_step(genpd, "paso 3/3: reloj 384 MHz (GPU APB)");
	sc8830_gpu_pd_rmw(pd->gpu + SC8830_GPU_APB_CLK_CTRL,
			  SC8830_GPU_CLK_SEL_MASK | SC8830_GPU_CLK_DIV_MASK,
			  SC8830_GPU_CLK_SEL_384M);
	usleep_range(10, 20);
	sc8830_gpu_pd_step(genpd, "GPU encendida");

	return 0;
}

static void __iomem *sc8830_gpu_pd_map(struct platform_device *pdev,
				       const char *name)
{
	struct resource *res;

	/* The AON APB register is shared with the timer: map, don't request */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res)
		return IOMEM_ERR_PTR(-EINVAL);

	return devm_ioremap(&pdev->dev, res->start, resource_size(res)) ?:
	       IOMEM_ERR_PTR(-ENOMEM);
}

static int sc8830_gpu_pd_probe(struct platform_device *pdev)
{
	struct sc8830_gpu_pd *pd;
	int ret;

	pd = devm_kzalloc(&pdev->dev, sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;

	pd->pmu = sc8830_gpu_pd_map(pdev, "pmu");
	pd->aon = sc8830_gpu_pd_map(pdev, "aon");
	pd->gpu = sc8830_gpu_pd_map(pdev, "gpu");
	if (IS_ERR(pd->pmu) || IS_ERR(pd->aon) || IS_ERR(pd->gpu))
		return -EINVAL;

	/*
	 * Only the always-on PMU and AON registers here: the GPU APB block
	 * sits inside the domain and must not be touched while it is off.
	 */
	dev_info(&pdev->dev, "before: pd_cfg %08x eb0 %08x\n",
		 readl(pd->pmu), readl(pd->aon));

	pd->genpd.name = "gpu";
	pd->genpd.power_on = sc8830_gpu_pd_power_on;
	pd->genpd.flags = GENPD_FLAG_ALWAYS_ON;
	sc8830_gpu_pd_power_on(&pd->genpd);

	dev_info(&pdev->dev, "after:  pd_cfg %08x eb0 %08x clk %08x rst %08x\n",
		 readl(pd->pmu), readl(pd->aon),
		 readl(pd->gpu + SC8830_GPU_APB_CLK_CTRL), readl(pd->gpu));

	ret = pm_genpd_init(&pd->genpd, NULL, false);
	if (ret)
		return ret;

	return of_genpd_add_provider_simple(pdev->dev.of_node, &pd->genpd);
}

static const struct of_device_id sc8830_gpu_pd_of_match[] = {
	{ .compatible = "sprd,sc8830-gpu-pd" },
	{ }
};

static struct platform_driver sc8830_gpu_pd_driver = {
	.probe = sc8830_gpu_pd_probe,
	.driver = {
		.name = "sc8830-gpu-pd",
		.of_match_table = sc8830_gpu_pd_of_match,
		.suppress_bind_attrs = true,
	},
};

/*
 * Late, so that the console is already on the panel if powering the GPU
 * goes wrong; Lima simply defers until this domain shows up.
 */
static int __init sc8830_gpu_pd_init(void)
{
	return platform_driver_register(&sc8830_gpu_pd_driver);
}
late_initcall(sc8830_gpu_pd_init);
