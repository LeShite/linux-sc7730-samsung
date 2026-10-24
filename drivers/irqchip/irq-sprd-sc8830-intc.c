// SPDX-License-Identifier: GPL-2.0-only
/*
 * Spreadtrum SC8830 family (SC7730, ...) legacy interrupt controller
 *
 * Four INTC blocks sit next to the GIC, one per 32 SPIs. They are not in the
 * delivery path, but once all cores are in WFI the GIC clock is gated and
 * only an interrupt enabled in an INTC can wake the system. So every GIC
 * enable has to be mirrored here, much like the Tegra ICTLR
 * (drivers/irqchip/irq-tegra.c), which this driver is modelled on. Register
 * layout from the vendor 3.10 kernel (drivers/platform/sprd/irq.c).
 */

#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of_address.h>

#include <dt-bindings/interrupt-controller/arm-gic.h>

#define SC8830_INTC_IRQ_EN	0x08	/* write 1 to enable */
#define SC8830_INTC_IRQ_DIS	0x0c	/* write 1 to disable */
#define SC8830_INTC_FIQ_DIS	0x2c

#define SC8830_NUM_INTC		4

/* AP APB clock gates of INTC0..3, the optional "eb" register range */
#define SC8830_AP_APB_EB_INTC	GENMASK(22, 19)

static void __iomem *sc8830_intc_base[SC8830_NUM_INTC];

static void sc8830_intc_write(struct irq_data *d, unsigned int reg)
{
	void __iomem *base = irq_data_get_irq_chip_data(d);

	writel_relaxed(BIT(d->hwirq % 32), base + reg);
}

static void sc8830_intc_mask(struct irq_data *d)
{
	sc8830_intc_write(d, SC8830_INTC_IRQ_DIS);
	irq_chip_mask_parent(d);
}

static void sc8830_intc_unmask(struct irq_data *d)
{
	sc8830_intc_write(d, SC8830_INTC_IRQ_EN);
	irq_chip_unmask_parent(d);
}

static struct irq_chip sc8830_intc_chip = {
	.name			= "INTC",
	.irq_eoi		= irq_chip_eoi_parent,
	.irq_mask		= sc8830_intc_mask,
	.irq_unmask		= sc8830_intc_unmask,
	.irq_retrigger		= irq_chip_retrigger_hierarchy,
	.irq_set_type		= irq_chip_set_type_parent,
	.flags			= IRQCHIP_SKIP_SET_WAKE,
#ifdef CONFIG_SMP
	.irq_set_affinity	= irq_chip_set_affinity_parent,
#endif
};

static int sc8830_intc_translate(struct irq_domain *d,
				 struct irq_fwspec *fwspec,
				 unsigned long *hwirq, unsigned int *type)
{
	if (!is_of_node(fwspec->fwnode) || fwspec->param_count != 3)
		return -EINVAL;

	/* Only SPIs are mirrored, PPIs must target the GIC directly */
	if (fwspec->param[0] != GIC_SPI)
		return -EINVAL;

	*hwirq = fwspec->param[1];
	*type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;
	return 0;
}

static int sc8830_intc_alloc(struct irq_domain *domain, unsigned int virq,
			     unsigned int nr_irqs, void *data)
{
	struct irq_fwspec *fwspec = data;
	struct irq_fwspec parent_fwspec;
	irq_hw_number_t hwirq;
	unsigned int i;

	if (fwspec->param_count != 3 || fwspec->param[0] != GIC_SPI)
		return -EINVAL;

	hwirq = fwspec->param[1];
	if (hwirq + nr_irqs > SC8830_NUM_INTC * 32)
		return -EINVAL;

	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
					      &sc8830_intc_chip,
					      sc8830_intc_base[(hwirq + i) / 32]);

	parent_fwspec = *fwspec;
	parent_fwspec.fwnode = domain->parent->fwnode;
	return irq_domain_alloc_irqs_parent(domain, virq, nr_irqs,
					    &parent_fwspec);
}

static const struct irq_domain_ops sc8830_intc_domain_ops = {
	.translate	= sc8830_intc_translate,
	.alloc		= sc8830_intc_alloc,
	.free		= irq_domain_free_irqs_common,
};

static int __init sc8830_intc_init(struct device_node *node,
				   struct device_node *parent)
{
	struct irq_domain *parent_domain, *domain;
	void __iomem *eb;
	int i, idx;

	if (!parent)
		return -ENODEV;

	parent_domain = irq_find_host(parent);
	if (!parent_domain)
		return -ENXIO;

	idx = of_property_match_string(node, "reg-names", "eb");
	if (idx >= 0) {
		eb = of_iomap(node, idx);
		if (eb) {
			writel_relaxed(readl_relaxed(eb) | SC8830_AP_APB_EB_INTC, eb);
			iounmap(eb);
		}
	}

	for (i = 0; i < SC8830_NUM_INTC; i++) {
		sc8830_intc_base[i] = of_iomap(node, i);
		if (!sc8830_intc_base[i]) {
			pr_err("%pOF: missing INTC%d registers\n", node, i);
			return -ENOMEM;
		}

		/* Start with every source masked, like the vendor kernel */
		writel_relaxed(GENMASK(31, 0),
			       sc8830_intc_base[i] + SC8830_INTC_IRQ_DIS);
		writel_relaxed(GENMASK(31, 0),
			       sc8830_intc_base[i] + SC8830_INTC_FIQ_DIS);
	}

	domain = irq_domain_create_hierarchy(parent_domain, 0,
					     SC8830_NUM_INTC * 32,
					     of_fwnode_handle(node),
					     &sc8830_intc_domain_ops, NULL);
	if (!domain)
		return -ENOMEM;

	pr_info("%pOF: %d interrupts mirrored for wakeup from %pOF\n",
		node, SC8830_NUM_INTC * 32, parent);
	return 0;
}

IRQCHIP_DECLARE(sc8830_intc, "sprd,sc8830-intc", sc8830_intc_init);
