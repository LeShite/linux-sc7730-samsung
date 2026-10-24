// SPDX-License-Identifier: GPL-2.0
/*
 * Spreadtrum SC8830 family (SC7730, SC7715, ...) timer driver
 *
 * Each timer block holds three 32-bit down counters spaced 0x20 apart.
 * Timer 0 is used for clockevents and timer 1 as free-running clocksource
 * and sched_clock. The register layout follows the vendor 3.10 kernel
 * (drivers/platform/sprd/timer_sc8830.c).
 */

#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/of_address.h>
#include <linux/sched_clock.h>

#include "timer-of.h"

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

/* AON APB clock gates, the optional second register range */
#define SC8830_AON_APB_EB0	0x0
#define SC8830_AON_APB_EB0_TMR	(BIT(10) | BIT(11) | BIT(12))

static void __iomem *sc8830_source_base;

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
	void __iomem *base = timer_of_base(to_timer_of(evt));

	sc8830_timer_wait_load(base, SC8830_EVENT_TIMER);
	sc8830_timer_ctl(base, SC8830_EVENT_TIMER, 0);
	writel_relaxed(cycles, base + SC8830_TIMER_LOAD(SC8830_EVENT_TIMER));
	sc8830_timer_ctl(base, SC8830_EVENT_TIMER, SC8830_TIMER_CTL_ENABLE);

	return 0;
}

static int sc8830_timer_set_periodic(struct clock_event_device *evt)
{
	struct timer_of *to = to_timer_of(evt);
	void __iomem *base = timer_of_base(to);

	sc8830_timer_ctl(base, SC8830_EVENT_TIMER, SC8830_TIMER_CTL_PERIOD);
	writel_relaxed(timer_of_period(to),
		       base + SC8830_TIMER_LOAD(SC8830_EVENT_TIMER));
	sc8830_timer_ctl(base, SC8830_EVENT_TIMER,
			 SC8830_TIMER_CTL_ENABLE | SC8830_TIMER_CTL_PERIOD);
	writel_relaxed(SC8830_TIMER_INT_EN,
		       base + SC8830_TIMER_INT(SC8830_EVENT_TIMER));

	return 0;
}

static int sc8830_timer_set_oneshot(struct clock_event_device *evt)
{
	struct timer_of *to = to_timer_of(evt);
	void __iomem *base = timer_of_base(to);

	/* Like the vendor driver, keep it running until the first event */
	writel_relaxed(timer_of_period(to),
		       base + SC8830_TIMER_LOAD(SC8830_EVENT_TIMER));
	sc8830_timer_ctl(base, SC8830_EVENT_TIMER, SC8830_TIMER_CTL_ENABLE);
	writel_relaxed(SC8830_TIMER_INT_EN,
		       base + SC8830_TIMER_INT(SC8830_EVENT_TIMER));

	return 0;
}

static int sc8830_timer_shutdown(struct clock_event_device *evt)
{
	void __iomem *base = timer_of_base(to_timer_of(evt));

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
	void __iomem *base = timer_of_base(to_timer_of(evt));
	u32 val;

	val = readl_relaxed(base + SC8830_TIMER_INT(SC8830_EVENT_TIMER));
	writel_relaxed(val | SC8830_TIMER_INT_CLR,
		       base + SC8830_TIMER_INT(SC8830_EVENT_TIMER));

	if (evt->event_handler)
		evt->event_handler(evt);

	return IRQ_HANDLED;
}

static struct timer_of sc8830_timer_of = {
	.flags = TIMER_OF_IRQ | TIMER_OF_BASE | TIMER_OF_CLOCK,

	.clkevt = {
		.name = "sc8830-timer",
		.rating = 300,
		.features = CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT,
		.set_next_event = sc8830_timer_set_next_event,
		.set_state_periodic = sc8830_timer_set_periodic,
		.set_state_oneshot = sc8830_timer_set_oneshot,
		.set_state_shutdown = sc8830_timer_shutdown,
	},

	.of_irq = {
		.handler = sc8830_timer_interrupt,
		.flags = IRQF_TIMER | IRQF_IRQPOLL,
	},
};

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
	iounmap(eb);
}

static int __init sc8830_timer_init(struct device_node *np)
{
	struct timer_of *to = &sc8830_timer_of;
	void __iomem *base;
	unsigned long rate;
	int n, ret;

	sc8830_timer_enable_gates(np);

	ret = timer_of_init(np, to);
	if (ret)
		return ret;

	base = timer_of_base(to);
	rate = timer_of_rate(to);

	/* The bootloader may leave timers running; start from a clean state */
	for (n = 0; n < SC8830_NUM_TIMERS; n++) {
		sc8830_timer_ctl(base, n, 0);
		writel_relaxed(SC8830_TIMER_INT_CLR, base + SC8830_TIMER_INT(n));
	}

	writel_relaxed(0, base + SC8830_TIMER_INT(SC8830_SOURCE_TIMER));
	sc8830_timer_ctl(base, SC8830_SOURCE_TIMER,
			 SC8830_TIMER_CTL_NEW | SC8830_TIMER_CTL_PERIOD);
	writel_relaxed(U32_MAX, base + SC8830_TIMER_LOAD(SC8830_SOURCE_TIMER));
	sc8830_timer_ctl(base, SC8830_SOURCE_TIMER, SC8830_TIMER_CTL_ENABLE |
			 SC8830_TIMER_CTL_NEW | SC8830_TIMER_CTL_PERIOD);

	sc8830_source_base = base;
	sched_clock_register(sc8830_timer_read_count, 32, rate);
	clocksource_register_hz(&sc8830_clocksource, rate);

	/*
	 * Register as the boot CPU's local tick device. With a wider cpumask
	 * the per-CPU dummy_timer is preferred and this timer is demoted to
	 * broadcast duty, which rules out oneshot mode and hrtimers.
	 */
	to->clkevt.cpumask = cpumask_of(0);
	clockevents_config_and_register(&to->clkevt, rate, 2, 0xf0000000);

	return 0;
}

TIMER_OF_DECLARE(sc8830, "sprd,sc8830-timer", sc8830_timer_init);
