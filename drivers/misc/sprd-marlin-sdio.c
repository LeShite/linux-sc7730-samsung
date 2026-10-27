// SPDX-License-Identifier: GPL-2.0
/*
 * SDIO transport of the Spreadtrum SC2331 Marlin WiFi/BT chip (vendor drivers/misc/sdiodev/sdio_dev.c).
 *
 * The chip sits on SDIO1 once FDL1 runs (sprd-sc2331-marlin.c). It exposes numbered channels:
 *   - function 0, byte 0x840/0x841: bit n set = channel n has something to read (0x841: channels 8-15)
 *   - function 0, 0x800 + 4n .. +2: bytes pending on channel n (24 bits, 0xff = bus error)
 *   - function 1: data, read or written with the channel number as the (fixed) address
 * Channels used by the vendor: 3 firmware download (write), 12 download replies (read), 0 calibration,
 * 8/9 WiFi, 10 sync, 11 AT, 13 assert info, 14 log, 15 loop check.
 *
 * Bring-up aid: /dev/marlin. write() = one byte of channel + the payload, read() = one byte of
 * channel + what that channel had (blocks up to the timeout), sysfs "status" lists the channels.
 */

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/etherdevice.h>
#include <linux/slab.h>
#include <linux/sprd-marlin-sdio.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define MARLIN_VENDOR		0x0000
#define MARLIN_DEVICE		0x2331
#define CHN_STATUS_LO		0x840
#define CHN_STATUS_HI		0x841
#define CHN_LEN(n)		(0x800 + 4 * (n))
#define MAX_XFER		(64 * 1024)
#define READ_TIMEOUT_MS		5000

struct marlin_sdio {
	struct sdio_func *func;
	struct gpio_desc *data_irq;
	struct gpio_desc *wake_out, *wake_ack;	/* CP wake request (46) and its answer (148) */
	int irq;
	struct mutex lock;
	wait_queue_head_t wait;
	bool irq_seen;
	void (*irq_cb)(void *);
	void *irq_cb_data;
	u8 *buf;
};

static struct marlin_sdio *marlin;

static int chn_status(struct marlin_sdio *m, u16 *status)
{
	int e0, e1;
	u8 lo, hi;

	sdio_claim_host(m->func);
	lo = sdio_f0_readb(m->func, CHN_STATUS_LO, &e0);
	hi = sdio_f0_readb(m->func, CHN_STATUS_HI, &e1);
	sdio_release_host(m->func);
	if (e0 || e1)
		return e0 ?: e1;
	*status = lo | hi << 8;
	return 0;
}

static int chn_len(struct marlin_sdio *m, unsigned int chn)
{
	int err, i;
	u8 b[3];

	sdio_claim_host(m->func);
	for (i = 0; i < 3; i++) {
		b[i] = sdio_f0_readb(m->func, CHN_LEN(chn) + i, &err);
		if (err) {
			sdio_release_host(m->func);
			return err;
		}
	}
	sdio_release_host(m->func);
	return b[0] | b[1] << 8 | b[2] << 16;
}

struct marlin_sdio *marlin_sdio_get(void)
{
	return marlin;
}

struct device *marlin_sdio_dev(struct marlin_sdio *m)
{
	return &m->func->dev;
}

int marlin_sdio_chn_status(struct marlin_sdio *m, u16 *status)
{
	return chn_status(m, status);
}

int marlin_sdio_chn_len(struct marlin_sdio *m, unsigned int chn)
{
	return chn_len(m, chn);
}

int marlin_sdio_chn_read(struct marlin_sdio *m, unsigned int chn, void *buf, int len)
{
	int ret;

	sdio_claim_host(m->func);
	ret = sdio_memcpy_fromio(m->func, buf, chn, len);
	sdio_release_host(m->func);
	return ret;
}

int marlin_sdio_chn_write(struct marlin_sdio *m, unsigned int chn, const void *buf, int len)
{
	int ret;

	sdio_claim_host(m->func);
	ret = sdio_memcpy_toio(m->func, chn, (void *)buf, len);
	sdio_release_host(m->func);
	return ret;
}

/*
 * Sleep handshake of the vendor's sdio_dev.c: the chip sleeps when idle. To talk to it the host
 * raises wake_out; the chip answers by raising wake_ack and stays awake while wake_out is high.
 * It raises data_irq itself when it has something to send.
 */
int marlin_sdio_wake(struct marlin_sdio *m)
{
	int i;

	if (!m->wake_out || !m->wake_ack)
		return 0;
	gpiod_set_value(m->wake_out, 1);
	for (i = 0; i < 400; i++) {			/* up to ~100 ms */
		if (gpiod_get_value(m->wake_ack))
			return 0;
		usleep_range(200, 300);
	}
	return -ETIMEDOUT;
}

void marlin_sdio_sleep(struct marlin_sdio *m)
{
	if (m->wake_out)
		gpiod_set_value(m->wake_out, 0);
}

void marlin_sdio_set_irq_cb(struct marlin_sdio *m, void (*cb)(void *), void *data)
{
	m->irq_cb_data = data;
	m->irq_cb = cb;
}

static irqreturn_t marlin_irq(int irq, void *data)
{
	struct marlin_sdio *m = data;

	m->irq_seen = true;
	wake_up(&m->wait);
	if (m->irq_cb)
		m->irq_cb(m->irq_cb_data);
	return IRQ_HANDLED;
}

static ssize_t marlin_write(struct file *f, const char __user *ubuf, size_t n, loff_t *pos)
{
	struct marlin_sdio *m = marlin;
	int ret;

	if (!m || n < 2 || n > MAX_XFER + 1)
		return -EINVAL;
	mutex_lock(&m->lock);
	if (copy_from_user(m->buf, ubuf, n)) {
		ret = -EFAULT;
		goto out;
	}
	sdio_claim_host(m->func);
	ret = sdio_memcpy_toio(m->func, m->buf[0], m->buf + 1, n - 1);
	sdio_release_host(m->func);
out:
	mutex_unlock(&m->lock);
	return ret ?: n;
}

static ssize_t marlin_read(struct file *f, char __user *ubuf, size_t n, loff_t *pos)
{
	struct marlin_sdio *m = marlin;
	unsigned long end = jiffies + msecs_to_jiffies(READ_TIMEOUT_MS);
	int ret, chn, len;
	u16 st;

	if (!m || n < 2)
		return -EINVAL;
	mutex_lock(&m->lock);
	for (;;) {
		ret = chn_status(m, &st);
		if (ret)
			goto out;
		if (st & 0xff00)		/* the vendor reads channels 8-15 */
			break;
		if (time_after(jiffies, end)) {
			ret = -ETIMEDOUT;
			goto out;
		}
		m->irq_seen = false;
		wait_event_timeout(m->wait, m->irq_seen, msecs_to_jiffies(5));
	}
	chn = __ffs(st >> 8) + 8;
	len = chn_len(m, chn);
	if (len <= 0 || len > MAX_XFER) {
		ret = len < 0 ? len : -EMSGSIZE;
		goto out;
	}
	if (len > n - 1)
		len = n - 1;
	m->buf[0] = chn;
	sdio_claim_host(m->func);
	ret = sdio_memcpy_fromio(m->func, m->buf + 1, chn, len);
	sdio_release_host(m->func);
	if (!ret && copy_to_user(ubuf, m->buf, len + 1))
		ret = -EFAULT;
	if (!ret)
		ret = len + 1;
out:
	mutex_unlock(&m->lock);
	return ret;
}

static const struct file_operations marlin_fops = {
	.owner = THIS_MODULE,
	.write = marlin_write,
	.read = marlin_read,
};

static struct miscdevice marlin_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "marlin",
	.fops = &marlin_fops,
};

static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct marlin_sdio *m = marlin;
	int n, len = 0;
	u16 st;
	int ret;

	if (!m)
		return -ENODEV;
	mutex_lock(&m->lock);
	ret = chn_status(m, &st);
	if (!ret) {
		len = sysfs_emit(buf, "status %#06x irq %d\n", st, gpiod_get_value(m->data_irq));
		for (n = 0; n < 16; n++)
			len += sysfs_emit_at(buf, len, "chn %2d: %d bytes\n", n, chn_len(m, n));
	}
	mutex_unlock(&m->lock);
	return ret ?: len;
}
static DEVICE_ATTR_RO(status);

/* echo 1 > wlan, once the firmware runs (marlin-up.sh): registers wlan0; 0 removes it */
static ssize_t wlan_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t n)
{
	struct marlin_sdio *m = marlin;
	u8 mac[ETH_ALEN];
	bool on, have_mac = false;
	char word[4] = { 0 };
	int ret;

	if (sscanf(buf, "%3s", word) != 1 || kstrtobool(word, &on))
		return -EINVAL;
	if (on && strchr(buf, ' ')) {
		const char *p = strchr(buf, ' ') + 1;

		if (sscanf(p, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6)
			have_mac = true;
	}
	if (!m)
		return -ENODEV;
	if (on) {
		ret = sc2331_wlan_start(m, have_mac ? mac : NULL);
		return ret ?: n;
	}
	sc2331_wlan_stop();
	return n;
}
static DEVICE_ATTR_WO(wlan);

static int marlin_probe(struct sdio_func *func, const struct sdio_device_id *id)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, "sprd,sc2331-marlin");
	struct marlin_sdio *m;
	int ret;

	if (func->num != 1)
		return -ENODEV;
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->buf = kmalloc(MAX_XFER + 1, GFP_KERNEL);
	if (!m->buf) {
		kfree(m);
		return -ENOMEM;
	}
	m->func = func;
	mutex_init(&m->lock);
	init_waitqueue_head(&m->wait);

	sdio_claim_host(func);
	ret = sdio_enable_func(func);
	if (!ret)
		ret = sdio_set_block_size(func, 512);
	sdio_release_host(func);
	if (ret) {
		dev_err(&func->dev, "cannot enable function 1: %d\n", ret);
		goto err;
	}

	if (np) {
		m->data_irq = fwnode_gpiod_get_index(of_fwnode_handle(np), "data-irq", 0, GPIOD_IN, "marlin-irq");
		if (IS_ERR(m->data_irq))
			m->data_irq = NULL;
		/* held high (chip awake) until the WiFi driver takes over the sleep policy */
		m->wake_out = fwnode_gpiod_get_index(of_fwnode_handle(np), "wake-out", 0, GPIOD_OUT_HIGH, "marlin-wake-out");
		m->wake_ack = fwnode_gpiod_get_index(of_fwnode_handle(np), "wake-ack", 0, GPIOD_IN, "marlin-wake-ack");
		if (IS_ERR(m->wake_out) || IS_ERR(m->wake_ack))
			m->wake_out = m->wake_ack = NULL;
		of_node_put(np);
	}
	if (m->data_irq) {
		m->irq = gpiod_to_irq(m->data_irq);
		if (m->irq > 0)
			ret = request_irq(m->irq, marlin_irq, IRQF_TRIGGER_RISING | IRQF_NO_SUSPEND, "marlin-data", m);
		if (m->irq <= 0 || ret) {
			dev_warn(&func->dev, "no data interrupt (%d): polling\n", m->irq);
			m->irq = 0;
		}
	} else {
		dev_warn(&func->dev, "no data-irq-gpios: polling\n");
	}

	marlin = m;
	sdio_set_drvdata(func, m);
	ret = misc_register(&marlin_misc);
	if (!ret)
		ret = device_create_file(marlin_misc.this_device, &dev_attr_status) ?:
		      device_create_file(marlin_misc.this_device, &dev_attr_wlan);
	dev_info(&func->dev, "Marlin SDIO function 1 enabled, /dev/marlin %s, irq %d\n", ret ? "FAILED" : "ready", m->irq);
	return ret;
err:
	kfree(m->buf);
	kfree(m);
	return ret;
}

static void marlin_remove(struct sdio_func *func)
{
	struct marlin_sdio *m = sdio_get_drvdata(func);

	marlin = NULL;
	sc2331_wlan_stop();
	device_remove_file(marlin_misc.this_device, &dev_attr_wlan);
	device_remove_file(marlin_misc.this_device, &dev_attr_status);
	misc_deregister(&marlin_misc);
	if (m->irq)
		free_irq(m->irq, m);
	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);
	kfree(m->buf);
	kfree(m);
}

static const struct sdio_device_id marlin_ids[] = {
	{ SDIO_DEVICE(MARLIN_VENDOR, MARLIN_DEVICE) },
	{ }
};
MODULE_DEVICE_TABLE(sdio, marlin_ids);

static struct sdio_driver marlin_driver = {
	.name = "sprd-marlin-sdio",
	.id_table = marlin_ids,
	.probe = marlin_probe,
	.remove = marlin_remove,
};
module_sdio_driver(marlin_driver);

MODULE_DESCRIPTION("Spreadtrum SC2331 Marlin SDIO transport (bring-up)");
MODULE_LICENSE("GPL");
