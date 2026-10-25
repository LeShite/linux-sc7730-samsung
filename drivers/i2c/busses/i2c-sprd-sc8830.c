// SPDX-License-Identifier: GPL-2.0
/*
 * I2C controller of the Spreadtrum SC8830 family (SC7730, ...)
 *
 * The older Spreadtrum I2C block (not the one i2c-sprd.c drives): every
 * byte is a command written to I2C_CMD (data in bits 15:8 with START,
 * WRITE, READ, STOP and TX_ACK flags), completion shows up as the INT bit in
 * I2C_CTL and the slave's ACK in I2C_CMD. Polled, like the vendor 3.10
 * driver (drivers/i2c/busses/i2c-sprd.c), which this follows.
 */

#include <linux/bitfield.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define I2C_CTL			0x00
#define I2C_CTL_INT		BIT(0)
#define I2C_CTL_IE		BIT(3)
#define I2C_CTL_EN		BIT(4)
#define I2C_CTL_PULLUP_MODE	BIT(10)
#define I2C_CTL_DUTY		BIT(11)

#define I2C_CMD			0x04
#define I2C_CMD_INT_ACK		BIT(0)
#define I2C_CMD_TX_ACK		BIT(1)
#define I2C_CMD_WRITE		BIT(2)
#define I2C_CMD_READ		BIT(3)
#define I2C_CMD_STOP		BIT(4)
#define I2C_CMD_START		BIT(5)
#define I2C_CMD_ACK		BIT(6)	/* 1: the slave did not acknowledge */
#define I2C_CMD_BUSY		BIT(7)
#define I2C_CMD_DATA		GENMASK(15, 8)

#define I2C_CLKD0		0x08
#define I2C_CLKD1		0x0c

#define SC8830_I2C_SRC_CLK	26000000
#define SC8830_I2C_TIMEOUT_US	5000

/* AP APB clock gates of I2C0..I2C4, the optional "eb" register range */
#define SC8830_AP_APB_EB_I2C(n)	BIT(8 + (n))

struct sc8830_i2c {
	struct i2c_adapter adap;
	void __iomem *base;
	struct device *dev;
};

static int sc8830_i2c_wait_done(struct sc8830_i2c *i2c)
{
	u32 val;
	int ret;

	ret = readl_poll_timeout_atomic(i2c->base + I2C_CTL, val,
					val & I2C_CTL_INT, 1,
					SC8830_I2C_TIMEOUT_US);
	if (ret)
		return ret;

	/* acknowledge the interrupt once the command engine is idle */
	readl_poll_timeout_atomic(i2c->base + I2C_CMD, val, !(val & I2C_CMD_BUSY),
				  1, SC8830_I2C_TIMEOUT_US);
	writel((readl(i2c->base + I2C_CMD) & I2C_CMD_DATA) | I2C_CMD_INT_ACK,
	       i2c->base + I2C_CMD);

	ret = readl_poll_timeout_atomic(i2c->base + I2C_CMD, val,
					!(val & I2C_CMD_ACK), 1,
					SC8830_I2C_TIMEOUT_US);
	return ret ? -ENXIO : 0;
}

static int sc8830_i2c_cmd(struct sc8830_i2c *i2c, u8 data, u32 flags)
{
	writel(FIELD_PREP(I2C_CMD_DATA, data) | flags, i2c->base + I2C_CMD);

	return sc8830_i2c_wait_done(i2c);
}

static int sc8830_i2c_xfer_msg(struct sc8830_i2c *i2c, struct i2c_msg *msg,
			       bool last)
{
	bool rd = msg->flags & I2C_M_RD;
	int i, ret;

	writel(readl(i2c->base + I2C_CTL) | I2C_CTL_EN | I2C_CTL_IE,
	       i2c->base + I2C_CTL);

	ret = sc8830_i2c_cmd(i2c, i2c_8bit_addr_from_msg(msg),
			     I2C_CMD_START | I2C_CMD_WRITE);
	if (ret)
		return ret;

	for (i = 0; i < msg->len; i++) {
		bool final = i == msg->len - 1;

		if (rd) {
			/* NACK and STOP after the last byte of a read */
			ret = sc8830_i2c_cmd(i2c, 0, I2C_CMD_READ |
					     (final ? I2C_CMD_STOP | I2C_CMD_TX_ACK : 0));
			if (ret)
				return ret;
			msg->buf[i] = FIELD_GET(I2C_CMD_DATA,
						readl(i2c->base + I2C_CMD));
		} else {
			ret = sc8830_i2c_cmd(i2c, msg->buf[i], I2C_CMD_WRITE |
					     (final && last ? I2C_CMD_STOP : 0));
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int sc8830_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			   int num)
{
	struct sc8830_i2c *i2c = i2c_get_adapdata(adap);
	int i, ret;

	for (i = 0; i < num; i++) {
		ret = sc8830_i2c_xfer_msg(i2c, &msgs[i], i == num - 1);
		if (ret) {
			/* free the bus for the next transfer */
			writel(I2C_CMD_STOP, i2c->base + I2C_CMD);
			sc8830_i2c_wait_done(i2c);
			return ret;
		}
	}

	return num;
}

static u32 sc8830_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm sc8830_i2c_algo = {
	.xfer = sc8830_i2c_xfer,
	.functionality = sc8830_i2c_func,
};

static void sc8830_i2c_hw_init(struct sc8830_i2c *i2c, u32 freq, bool pullup)
{
	u32 div = SC8830_I2C_SRC_CLK / (4 * freq) - 1;
	u32 ctl;

	ctl = readl(i2c->base + I2C_CTL) & ~(I2C_CTL_EN | I2C_CTL_IE);
	writel(ctl, i2c->base + I2C_CTL);

	/* r6p0 "duty" layout: both halves of the period in CLKD0 */
	writel((div << 16) | div, i2c->base + I2C_CLKD0);
	writel(0, i2c->base + I2C_CLKD1);

	ctl |= I2C_CTL_EN | I2C_CTL_IE | I2C_CTL_DUTY;
	if (pullup)
		ctl |= I2C_CTL_PULLUP_MODE;
	writel(ctl, i2c->base + I2C_CTL);
	writel(I2C_CMD_INT_ACK, i2c->base + I2C_CMD);
}

static int sc8830_i2c_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sc8830_i2c *i2c;
	struct resource *res;
	u32 freq = I2C_MAX_STANDARD_MODE_FREQ, bus;
	int ret;

	i2c = devm_kzalloc(dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	i2c->dev = dev;
	i2c->base = devm_platform_ioremap_resource_byname(pdev, "i2c");
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);

	/* The clock gate register is shared with other blocks: map only */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "eb");
	if (res && !of_property_read_u32(dev->of_node, "sprd,gate-bit", &bus)) {
		void __iomem *eb = devm_ioremap(dev, res->start, resource_size(res));

		if (eb)
			writel(readl(eb) | BIT(bus), eb);
	}

	of_property_read_u32(dev->of_node, "clock-frequency", &freq);
	sc8830_i2c_hw_init(i2c, freq,
			   of_property_read_bool(dev->of_node, "sprd,pull-up"));

	i2c->adap.owner = THIS_MODULE;
	i2c->adap.algo = &sc8830_i2c_algo;
	i2c->adap.retries = 3;
	i2c->adap.dev.parent = dev;
	i2c->adap.dev.of_node = dev->of_node;
	strscpy(i2c->adap.name, dev_name(dev), sizeof(i2c->adap.name));
	i2c_set_adapdata(&i2c->adap, i2c);

	ret = devm_i2c_add_adapter(dev, &i2c->adap);
	if (ret)
		return ret;

	dev_info(dev, "%u Hz, version %04x\n", freq,
		 readl(i2c->base + I2C_CTL) >> 16);
	return 0;
}

static const struct of_device_id sc8830_i2c_of_match[] = {
	{ .compatible = "sprd,sc8830-i2c" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc8830_i2c_of_match);

static struct platform_driver sc8830_i2c_driver = {
	.probe = sc8830_i2c_probe,
	.driver = {
		.name = "sc8830-i2c",
		.of_match_table = sc8830_i2c_of_match,
	},
};
module_platform_driver(sc8830_i2c_driver);

MODULE_DESCRIPTION("Spreadtrum SC8830 family I2C controller");
MODULE_LICENSE("GPL");
