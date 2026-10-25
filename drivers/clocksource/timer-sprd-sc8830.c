// SPDX-License-Identifier: GPL-2.0
/*
 * Spreadtrum SC8830 family (SC7730, SC7715, ...) timer driver
 *
 * Each timer block holds three 32-bit down counters spaced 0x20 apart.
 * Every CPU gets the first timer of its own block as local clockevent, as
 * in the vendor kernel: CPU0 the AON GPTIMER0, CPUs 1-3 the AP timers 0-2.
 * The second timer of GPTIMER0 is the free-running clocksource and
 * sched_clock. All of them run at 32768 Hz. The register layout follows the
 * vendor 3.10 kernel (drivers/platform/sprd/timer_sc8830.c).
 *
 * DT: reg = GPTIMER0, AON APB EB0/EB1 clock gates, then one block per
 * secondary CPU; interrupts = one per CPU.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>

#define SC8830_TIMER_LOAD(n)	(0x20 * (n) + 0x00)
#define SC8830_TIMER_CTL(n)	(0x20 * (n) + 0x08)
#define SC8830_TIMER_INT(n)	(0x20 * (n) + 0x0c)
#define SC8830_TIMER_CNT_RD(n)	(0x20 * (n) + 0x10)

#define SC8830_TIMER_CTL_PERIOD	BIT(6)
#define SC8830_TIMER_CTL_ENABLE	BIT(7)
#define SC8830_TIMER_CTL_NEW	BIT(8)

#define SC8830_TIMER_INT_EN	BIT(0)
#define SC8830_TIMER_INT_CLR	BIT(3)
#define SC8830_TIMER_INT_BUSY	BIT(4)

#define SC8830_EVENT_TIMER	0
#define SC8830_SOURCE_TIMER	1
#define SC8830_NUM_TIMERS	3
#define SC8830_MAX_CPUS		4

/* AON APB clock gates, the second register range */
#define SC8830_AON_APB_EB0	0x0
#define SC8830_AON_APB_EB0_TMR	(BIT(10) | BIT(11) | BIT(12))	/* AP_SYST, AON_TMR, AP_TMR0 */
#define SC8830_AON_APB_EB1	0x4
#define SC8830_AON_APB_EB1_TMR	(BIT(9) | BIT(10))		/* AP_TMR1, AP_TMR2 */

struct sc8830_timer {
	struct clock_event_device evt;
	void __iomem *base;
	int irq;
};

static struct sc8830_timer sc8830_timers[SC8830_MAX_CPUS];
static void __iomem *sc8830_source_base;
static unsigned long sc8830_timer_rate;

static inline void __iomem *sc8830_evt_base(struct clock_event_device *evt)
{
	return container_of(evt, struct sc8830_timer, evt)->base;
}

static void sc8830_timer_ctl(void __iomem *base, int n, u32 ctl)
{
	writel_relaxed(ctl, base + SC8830_TIMER_CTL(n));
}

/*
 * A load is synchronised into the 32k domain, which takes a few 32k cycles.
 * Wait for the previous one before touching the timer again, and do it while
 * the timer still runs: once disabled the sync never completes. The vendor
 * driver orders it the same way. The bound keeps a stuck bit from hanging
 * the CPU with interrupts off.
 */
static void sc8830_timer_wait_load(void __iomem *base, int n)
{
	unsigned int spins = 100000;

	while (readl_relaxed(base + SC8830_TIMER_INT(n)) & SC8830_TIMER_INT_BUSY) {
		if (!--spins) {
			pr_warn_once("sc8830-timer: load busy bit stuck\n");
			return;
		}
		cpu_relax();
	}
}

static int sc8830_timer_set_next_event(unsigned long cycles,
				       struct clock_event_device *evt)
{
	void __iomem *base = sc8830_evt_base(evt);

	sc8830_timer_wait_load(base, SC8830_EVENT_TIMER);
	sc8830_timer_ctl(base, SC8830_EVENT_TIMER, 0);
	writel_relaxed(cycles, base + SC8830_TIMER_LOAD(SC8830_EVENT_TIMER));
	sc8830_timer_ctl(base, SC8830_EVENT_TIMER, SC8830_TIMER_CTL_ENABLE);

	return 0;
}

static int sc8830_timer_set_periodic(struct clock_event_device *evt)
{
	void __iomem *base = sc8830_evt_base(evt);

	sc8830_timer_ctl(base, SC8830_EVENT_TIMER, SC8830_TIMER_CTL_PERIOD);
	writel_relaxed(DIV_ROUND_UP(sc8830_timer_rate, HZ),
		       base + SC8830_TIMER_LOAD(SC8830_EVENT_TIMER));
	sc8830_timer_ctl(base, SC8830_EVENT_TIMER,
			 SC8830_TIMER_CTL_ENABLE | SC8830_TIMER_CTL_PERIOD);
	writel_relaxed(SC8830_TIMER_INT_EN,
		       base + SC8830_TIMER_INT(SC8830_EVENT_TIMER));

	return 0;
}

static int sc8830_timer_set_oneshot(struct clock_event_device *evt)
{
	void __iomem *base = sc8830_evt_base(evt);

	/* Like the vendor driver, keep it running until the first event */
	writel_relaxed(DIV_ROUND_UP(sc8830_timer_rate, HZ),
		       base + SC8830_TIMER_LOAD(SC8830_EVENT_TIMER));
	sc8830_timer_ctl(base, SC8830_EVENT_TIMER, SC8830_TIMER_CTL_ENABLE);
	writel_relaxed(SC8830_TIMER_INT_EN,
		       base + SC8830_TIMER_INT(SC8830_EVENT_TIMER));

	return 0;
}

static int sc8830_timer_shutdown(struct clock_event_device *evt)
{
	void __iomem *base = sc8830_evt_base(evt);

	/* Writing only the clear bit also masks the interrupt */
	writel_relaxed(SC8830_TIMER_INT_CLR,
		       base + SC8830_TIMER_INT(SC8830_EVENT_TIMER));
	sc8830_timer_ctl(base, SC8830_EVENT_TIMER,
			 readl_relaxed(base + SC8830_TIMER_CTL(SC8830_EVENT_TIMER)) &
			 SC8830_TIMER_CTL_PERIOD);

	return 0;
}

static irqreturn_t sc8830_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;
	void __iomem *base = sc8830_evt_base(evt);
	u32 val;

	val = readl_relaxed(base + SC8830_TIMER_INT(SC8830_EVENT_TIMER));
	writel_relaxed(val | SC8830_TIMER_INT_CLR,
		       base + SC8830_TIMER_INT(SC8830_EVENT_TIMER));

	if (evt->event_handler)
		evt->event_handler(evt);

	return IRQ_HANDLED;
}

/* Runs on each CPU as it comes up: install its own block as tick device */
static int sc8830_timer_starting_cpu(unsigned int cpu)
{
	struct sc8830_timer *t = &sc8830_timers[cpu];

	if (cpu >= SC8830_MAX_CPUS || !t->base)
		return 0;

	t->evt.name = "sc8830-timer";
	t->evt.rating = 300;
	t->evt.features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT;
	t->evt.set_next_event = sc8830_timer_set_next_event;
	t->evt.set_state_periodic = sc8830_timer_set_periodic;
	t->evt.set_state_oneshot = sc8830_timer_set_oneshot;
	t->evt.set_state_shutdown = sc8830_timer_shutdown;
	t->evt.cpumask = cpumask_of(cpu);
	t->evt.irq = t->irq;

	irq_force_affinity(t->irq, cpumask_of(cpu));
	enable_irq(t->irq);
	clockevents_config_and_register(&t->evt, sc8830_timer_rate, 2, 0xf0000000);

	return 0;
}

static int sc8830_timer_dying_cpu(unsigned int cpu)
{
	struct sc8830_timer *t = &sc8830_timers[cpu];

	if (cpu < SC8830_MAX_CPUS && t->base) {
		t->evt.set_state_shutdown(&t->evt);
		disable_irq_nosync(t->irq);
	}

	return 0;
}

/* The counters run down, so invert them for a monotonic up-count */
static u64 notrace sc8830_timer_read_count(void)
{
	return ~readl_relaxed(sc8830_source_base +
			      SC8830_TIMER_CNT_RD(SC8830_SOURCE_TIMER));
}

static u64 sc8830_clocksource_read(struct clocksource *cs)
{
	return sc8830_timer_read_count();
}

static struct clocksource sc8830_clocksource = {
	.name = "sc8830-timer",
	.rating = 300,
	.read = sc8830_clocksource_read,
	.mask = CLOCKSOURCE_MASK(32),
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
};

static void __init sc8830_timer_enable_gates(struct device_node *np)
{
	void __iomem *eb = of_iomap(np, 1);

	if (!eb)
		return;

	writel_relaxed(readl_relaxed(eb + SC8830_AON_APB_EB0) |
		       SC8830_AON_APB_EB0_TMR, eb + SC8830_AON_APB_EB0);
	writel_relaxed(readl_relaxed(eb + SC8830_AON_APB_EB1) |
		       SC8830_AON_APB_EB1_TMR, eb + SC8830_AON_APB_EB1);
	iounmap(eb);
}

static int __init sc8830_timer_init(struct device_node *np)
{
	struct clk *clk;
	int cpu, n, ret;

	sc8830_timer_enable_gates(np);

	sc8830_timer_rate = 32768;
	clk = of_clk_get(np, 0);
	if (!IS_ERR(clk)) {
		clk_prepare_enable(clk);
		sc8830_timer_rate = clk_get_rate(clk);
	}

	for (cpu = 0; cpu < SC8830_MAX_CPUS; cpu++) {
		struct sc8830_timer *t = &sc8830_timers[cpu];

		/* reg 0 is CPU0's block, reg 1 the gates, then CPUs 1-3 */
		t->base = of_iomap(np, cpu ? cpu + 1 : 0);
		if (!t->base) {
			if (!cpu)
				return -ENXIO;
			break;
		}

		/* The bootloader may leave timers running; start clean */
		for (n = 0; n < SC8830_NUM_TIMERS; n++) {
			sc8830_timer_ctl(t->base, n, 0);
			writel_relaxed(SC8830_TIMER_INT_CLR,
				       t->base + SC8830_TIMER_INT(n));
		}

		t->irq = irq_of_parse_and_map(np, cpu);
		if (!t->irq) {
			pr_err("sc8830-timer: no interrupt for CPU%d\n", cpu);
			iounmap(t->base);
			t->base = NULL;
			if (!cpu)
				return -EINVAL;
			break;
		}

		/* Enabled from the starting callback, on the right CPU */
		irq_set_status_flags(t->irq, IRQ_NOAUTOEN);
		ret = request_irq(t->irq, sc8830_timer_interrupt,
				  IRQF_TIMER | IRQF_NOBALANCING,
				  "sc8830-timer", &t->evt);
		if (ret) {
			pr_err("sc8830-timer: can't request irq for CPU%d\n", cpu);
			return ret;
		}
	}

	sc8830_source_base = sc8830_timers[0].base;
	writel_relaxed(0, sc8830_source_base + SC8830_TIMER_INT(SC8830_SOURCE_TIMER));
	sc8830_timer_ctl(sc8830_source_base, SC8830_SOURCE_TIMER,
			 SC8830_TIMER_CTL_NEW | SC8830_TIMER_CTL_PERIOD);
	writel_relaxed(U32_MAX,
		       sc8830_source_base + SC8830_TIMER_LOAD(SC8830_SOURCE_TIMER));
	sc8830_timer_ctl(sc8830_source_base, SC8830_SOURCE_TIMER,
			 SC8830_TIMER_CTL_ENABLE | SC8830_TIMER_CTL_NEW |
			 SC8830_TIMER_CTL_PERIOD);

	sched_clock_register(sc8830_timer_read_count, 32, sc8830_timer_rate);
	clocksource_register_hz(&sc8830_clocksource, sc8830_timer_rate);

	return cpuhp_setup_state(CPUHP_AP_SPRD_SC8830_TIMER_STARTING,
				 "clockevents/sc8830:starting",
				 sc8830_timer_starting_cpu,
				 sc8830_timer_dying_cpu);
}

TIMER_OF_DECLARE(sc8830, "sprd,sc8830-timer", sc8830_timer_init);
