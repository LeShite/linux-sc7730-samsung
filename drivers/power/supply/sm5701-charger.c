// SPDX-License-Identifier: GPL-2.0
/*
 * Charger of the Silicon Mitus SM5701 (charger, flash LED driver and OTG boost in one chip), as on
 * the Galaxy Tab A 7.0 (SM-T280), with the cable type from the SM5504 MUIC next to it.
 *
 * From the vendor 3.10 kernel (drivers/battery/sm5701_charger.c, drivers/mfd/sm5504.c and the
 * board's DTS): SM5701 at 0x49 and SM5504 at 0x14, each on a GPIO-bit-banged I2C bus; the charge
 * enable line (active low) on GPIO 215. Per cable, the vendor's battery DTS sets the input current
 * limit and the fast charge current: a PC port (SDP) 460/500 mA, a wall charger (DCP) 1000/1550 mA.
 * The float voltage is 4.34 V and charging stops when the current falls to the top-off level.
 *
 * Polled every two seconds: VBUS, the charge state and the cable type.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

/* SM5701 */
#define SM5701_STATUS1		0x06
#define  STATUS1_VBUSOK		BIT(2)
#define  STATUS1_VBUSUVLO	BIT(3)
#define  STATUS1_VBUSOVP	BIT(4)
#define  STATUS1_NOBAT		BIT(7)
#define SM5701_STATUS2		0x07
#define  STATUS2_TOPOFF		BIT(0)
#define  STATUS2_DONE		BIT(1)
#define  STATUS2_CHGON		BIT(3)
#define SM5701_CNTL		0x09
#define  CNTL_OPMODE		0x07
#define  OPMODE_CHARGER_ON	0x04
#define SM5701_VBUSCNTL		0x0a
#define  VBUSCNTL_LIMIT		0x07		/* 0: 100 mA, 1: 500 mA, n: (n + 5) * 100 mA */
#define  VBUSCNTL_AICLEN	BIT(6)
#define SM5701_CHGCNTL1		0x0b
#define  CHGCNTL1_TOPOFF	0x0f		/* 100 mA + n * 25 mA */
#define  CHGCNTL1_AUTOSTOP	BIT(4)
#define SM5701_CHGCNTL2		0x0c		/* fast charge: 100 mA + n * 25 mA */
#define SM5701_CHGCNTL3		0x0d		/* float voltage */
#define  CHGCNTL3_BATREG	0x1f
#define SM5701_DEVICE_ID	0x15

/* SM5504 MUIC */
#define SM5504_DEVICE1		0x0a
#define  DEV1_OTG		BIT(0)
#define  DEV1_SDP		BIT(2)
#define  DEV1_CDP		BIT(5)
#define  DEV1_DCP		BIT(6)

#define FLOAT_UV		4340000		/* the vendor's chg_float_voltage */
#define TOPOFF_MA		150		/* full_check_current_2nd */
#define POLL_MS			2000

enum cable { CABLE_NONE, CABLE_SDP, CABLE_CDP, CABLE_DCP, CABLE_OTHER };

static const char * const cable_names[] = { "none", "PC (SDP)", "CDP", "wall (DCP)", "other" };

/* input current limit, fast charge current (mA): the vendor battery DTS for the SDP and DCP cables */
static const int cable_ma[][2] = {
	[CABLE_NONE]  = { 500, 500 },
	[CABLE_SDP]   = { 500, 500 },	/* 460 in the DTS; the register has 100 mA steps above 500 */
	[CABLE_CDP]   = { 1000, 1000 },
	[CABLE_DCP]   = { 1000, 1550 },
	[CABLE_OTHER] = { 500, 500 },
};

struct sm5701 {
	struct i2c_client *client, *muic;
	struct gpio_desc *chgen;
	struct power_supply *psy;
	struct delayed_work poll;
	struct mutex lock;
	u8 dev_id;
	bool online;
	enum cable cable;
	u8 status1, status2;
};

static int rd(struct i2c_client *c, u8 reg)
{
	return c ? i2c_smbus_read_byte_data(c, reg) : -ENODEV;
}

static int update(struct i2c_client *c, u8 reg, u8 mask, u8 val)
{
	int old = rd(c, reg);

	if (old < 0)
		return old;
	return i2c_smbus_write_byte_data(c, reg, (old & ~mask) | (val & mask));
}

static u8 batreg_code(int uv)
{
	int mv = uv / 1000;

	/* SM5701_set_batreg_voltage(): both chip revisions agree at 4.34 V (26) */
	if (mv <= 3725)
		return 0;
	if (mv >= 4400)
		return 0x1f;
	if (mv <= 4300)
		return (mv - 3725) / 25;
	return (mv - 4300) * 10 / 125 + 23;
}

static enum cable read_cable(struct sm5701 *c)
{
	int d = rd(c->muic, SM5504_DEVICE1);

	if (d < 0)
		return CABLE_OTHER;
	if (d & DEV1_DCP)
		return CABLE_DCP;
	if (d & DEV1_CDP)
		return CABLE_CDP;
	if (d & DEV1_SDP)
		return CABLE_SDP;
	return d & DEV1_OTG ? CABLE_NONE : CABLE_OTHER;
}

static void set_currents(struct sm5701 *c, enum cable cable)
{
	int in = cable_ma[cable][0], fast = cable_ma[cable][1];
	u8 lim = in <= 100 ? 0 : in <= 500 ? 1 : min(in / 100 - 5, 7);

	/* fast charge first, then the input limit (the vendor's order) */
	i2c_smbus_write_byte_data(c->client, SM5701_CHGCNTL2, (clamp(fast, 100, 1600) - 100) / 25);
	update(c->client, SM5701_VBUSCNTL, VBUSCNTL_LIMIT, lim);
	dev_info(&c->client->dev, "cable %s: input limit %d mA, charge %d mA (VBUSCNTL %02x CHGCNTL2 %02x)\n",
		 cable_names[cable], in, fast, rd(c->client, SM5701_VBUSCNTL),
		 rd(c->client, SM5701_CHGCNTL2));
}

static void poll_work(struct work_struct *work)
{
	struct sm5701 *c = container_of(work, struct sm5701, poll.work);
	int s1, s2;
	bool online, changed = false;
	enum cable cable;

	mutex_lock(&c->lock);
	s1 = rd(c->client, SM5701_STATUS1);
	s2 = rd(c->client, SM5701_STATUS2);
	if (s1 >= 0 && s2 >= 0) {
		online = s1 & STATUS1_VBUSOK;
		cable = online ? read_cable(c) : CABLE_NONE;
		if (online != c->online || cable != c->cable) {
			if (online)
				set_currents(c, cable);
			else
				dev_info(&c->client->dev, "cable out\n");
			c->online = online;
			c->cable = cable;
			changed = true;
		}
		if (s2 != c->status2)
			changed = true;
		c->status1 = s1;
		c->status2 = s2;
	}
	mutex_unlock(&c->lock);
	if (changed)
		power_supply_changed(c->psy);
	schedule_delayed_work(&c->poll, msecs_to_jiffies(POLL_MS));
}

static const enum power_supply_property sm5701_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE,
};

static int sm5701_get(struct power_supply *psy, enum power_supply_property psp,
		      union power_supply_propval *val)
{
	struct sm5701 *c = power_supply_get_drvdata(psy);
	int ret = 0, r;

	mutex_lock(&c->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = c->online;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (!c->online)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (c->status2 & (STATUS2_DONE | STATUS2_TOPOFF))
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else if (c->status2 & STATUS2_CHGON)
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = c->status1 & STATUS1_VBUSOVP ? POWER_SUPPLY_HEALTH_OVERVOLTAGE :
			      POWER_SUPPLY_HEALTH_GOOD;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		r = rd(c->client, SM5701_VBUSCNTL);
		r = r < 0 ? 0 : r & VBUSCNTL_LIMIT;
		val->intval = (r == 0 ? 100 : r == 1 ? 500 : (r + 5) * 100) * 1000;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		r = rd(c->client, SM5701_CHGCNTL2);
		val->intval = (100 + (r < 0 ? 0 : (r & 0x3f) * 25)) * 1000;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE:
		val->intval = FLOAT_UV;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&c->lock);
	return ret;
}

static char *sm5701_supplied_to[] = { "battery" };

static const struct power_supply_desc sm5701_desc = {
	.name = "charger",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = sm5701_props,
	.num_properties = ARRAY_SIZE(sm5701_props),
	.get_property = sm5701_get,
};

static void sm5701_stop(void *data)
{
	cancel_delayed_work_sync(data);
}

static void sm5701_put_muic(void *data)
{
	put_device(&((struct i2c_client *)data)->dev);
}

static int sm5701_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct power_supply_config cfg = { };
	struct device_node *np;
	struct sm5701 *c;
	int id, ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->client = client;
	mutex_init(&c->lock);

	id = rd(client, SM5701_DEVICE_ID);
	if (id < 0)
		return dev_err_probe(dev, id, "no answer at 0x%02x\n", client->addr);
	c->dev_id = id;

	np = of_parse_phandle(dev->of_node, "sm,muic", 0);
	if (np) {
		c->muic = of_find_i2c_device_by_node(np);
		of_node_put(np);
		if (!c->muic)
			return -EPROBE_DEFER;
		ret = devm_add_action_or_reset(dev, sm5701_put_muic, c->muic);
		if (ret)
			return ret;
	}

	/* the vendor's SM5701_charger_initialize(): AICL off, auto-stop on, float voltage, top-off */
	update(client, SM5701_VBUSCNTL, VBUSCNTL_AICLEN, 0);
	update(client, SM5701_CHGCNTL3, CHGCNTL3_BATREG, batreg_code(FLOAT_UV));
	update(client, SM5701_CHGCNTL1, CHGCNTL1_AUTOSTOP | CHGCNTL1_TOPOFF,
	       CHGCNTL1_AUTOSTOP | (TOPOFF_MA - 100) / 25);
	update(client, SM5701_CNTL, CNTL_OPMODE, OPMODE_CHARGER_ON);

	/* charging enabled: the line is active low, the descriptor says so */
	c->chgen = devm_gpiod_get_optional(dev, "chgen", GPIOD_OUT_HIGH);
	if (IS_ERR(c->chgen))
		return dev_err_probe(dev, PTR_ERR(c->chgen), "chgen gpio\n");

	cfg.drv_data = c;
	cfg.fwnode = dev_fwnode(dev);
	cfg.supplied_to = sm5701_supplied_to;
	cfg.num_supplicants = ARRAY_SIZE(sm5701_supplied_to);
	c->psy = devm_power_supply_register(dev, &sm5701_desc, &cfg);
	if (IS_ERR(c->psy))
		return PTR_ERR(c->psy);

	dev_info(dev, "rev %d, STATUS1 %02x STATUS2 %02x CNTL %02x VBUSCNTL %02x CHGCNTL1-3 %02x %02x %02x, MUIC %s\n",
		 c->dev_id, rd(client, SM5701_STATUS1), rd(client, SM5701_STATUS2),
		 rd(client, SM5701_CNTL), rd(client, SM5701_VBUSCNTL), rd(client, SM5701_CHGCNTL1),
		 rd(client, SM5701_CHGCNTL2), rd(client, SM5701_CHGCNTL3),
		 c->muic ? "found" : "none");

	INIT_DELAYED_WORK(&c->poll, poll_work);
	ret = devm_add_action_or_reset(dev, sm5701_stop, &c->poll);
	if (ret)
		return ret;
	schedule_delayed_work(&c->poll, 0);
	return 0;
}

static const struct of_device_id sm5701_of_match[] = {
	{ .compatible = "siliconmitus,sm5701-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, sm5701_of_match);

static struct i2c_driver sm5701_driver = {
	.probe = sm5701_probe,
	.driver = {
		.name = "sm5701-charger",
		.of_match_table = sm5701_of_match,
	},
};
module_i2c_driver(sm5701_driver);

MODULE_DESCRIPTION("Silicon Mitus SM5701 charger, cable type from an SM5504 MUIC");
MODULE_LICENSE("GPL");
