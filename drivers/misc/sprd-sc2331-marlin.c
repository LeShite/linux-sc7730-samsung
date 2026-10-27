// SPDX-License-Identifier: GPL-2.0
/*
 * First-stage loader for the Spreadtrum SC2331 Marlin WiFi/BT controller.
 * The chip's BootROM accepts the Spreadtrum BSL protocol on UART0. FDL1 is
 * loaded from firmware, executed in the chip, and then SDIO1 is rescanned.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdhci-sprd-sc8830.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/serdev.h>
#include <linux/sprd-marlin-sdio.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/wait.h>

#if IS_ENABLED(CONFIG_BT_HCIUART_SERDEV)
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#include "../bluetooth/hci_uart.h"
#endif

#define MARLIN_FLAG		0x7e
#define MARLIN_ESCAPE		0x7d
#define MARLIN_ESCAPE_XOR	0x20

#define MARLIN_CMD_CONNECT	0x0000
#define MARLIN_CMD_START_DATA	0x0001
#define MARLIN_CMD_MIDST_DATA	0x0002
#define MARLIN_CMD_END_DATA	0x0003
#define MARLIN_CMD_EXEC_DATA	0x0004

#define MARLIN_REP_ACK		0x0080
#define MARLIN_REP_VER		0x0081

#define MARLIN_MAX_DATA		512
#define MARLIN_RAW_SIZE		(4 + MARLIN_MAX_DATA + 2)
#define MARLIN_TX_SIZE		(2 * MARLIN_RAW_SIZE + 2)
#define MARLIN_FDL_MAX_SIZE	(256 * 1024)
#define MARLIN_FDL_CHUNK	512
#define MARLIN_BAUD		115200
#define MARLIN_SYNC_RETRIES	300
#define MARLIN_ACK_TIMEOUT_MS	3000

struct sprd_marlin {
	struct serdev_device *serdev;
	struct gpio_desc *reset;
	struct regulator_bulk_data supplies[2];
	struct device_node *sdio_host;
	struct work_struct work;
	bool serdev_open, bt_up, have_bd_addr;
	u8 bd_addr[6];
#if IS_ENABLED(CONFIG_BT_HCIUART_SERDEV)
	struct hci_uart hu;
#endif
	const char *firmware_name;
	u32 fdl_load_address;
	spinlock_t rx_lock;
	wait_queue_head_t rx_wait;
	u8 rx_buf[MARLIN_RAW_SIZE];
	unsigned int rx_len;
	bool rx_collecting;
	bool rx_escaped;
	bool rx_ready;
	u8 tx_raw[MARLIN_RAW_SIZE];
	u8 tx_buf[MARLIN_TX_SIZE];
};

static struct sprd_marlin *g_marlin;


static u16 sprd_marlin_crc16(const u8 *data, size_t len)
{
	u16 crc = 0;
	size_t i;
	int bit;

	for (i = 0; i < len; i++) {
		crc ^= (u16)data[i] << 8;
		for (bit = 0; bit < 8; bit++)
			crc = crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1;
	}

	return crc;
}

static size_t sprd_marlin_encode(struct sprd_marlin *marlin, u16 type,
				 const u8 *data, u16 data_len)
{
	unsigned int raw_len = 4 + data_len;
	u16 crc;
	size_t tx_len = 0;
	unsigned int i;

	marlin->tx_raw[0] = type >> 8;
	marlin->tx_raw[1] = type;
	marlin->tx_raw[2] = data_len >> 8;
	marlin->tx_raw[3] = data_len;
	if (data_len)
		memcpy(marlin->tx_raw + 4, data, data_len);

	crc = sprd_marlin_crc16(marlin->tx_raw, raw_len);
	marlin->tx_raw[raw_len++] = crc >> 8;
	marlin->tx_raw[raw_len++] = crc;

	marlin->tx_buf[tx_len++] = MARLIN_FLAG;
	for (i = 0; i < raw_len; i++) {
		u8 byte = marlin->tx_raw[i];

		if (byte == MARLIN_FLAG || byte == MARLIN_ESCAPE) {
			marlin->tx_buf[tx_len++] = MARLIN_ESCAPE;
			marlin->tx_buf[tx_len++] = byte ^ MARLIN_ESCAPE_XOR;
		} else {
			marlin->tx_buf[tx_len++] = byte;
		}
	}
	marlin->tx_buf[tx_len++] = MARLIN_FLAG;

	return tx_len;
}

static size_t sprd_marlin_receive_buf(struct serdev_device *serdev,
				      const u8 *data, size_t count)
{
	struct sprd_marlin *marlin = serdev_device_get_drvdata(serdev);
	unsigned long flags;
	bool wake = false;
	size_t i;

	spin_lock_irqsave(&marlin->rx_lock, flags);
	for (i = 0; i < count; i++) {
		u8 byte = data[i];

		if (byte == MARLIN_FLAG) {
			if (marlin->rx_collecting && marlin->rx_len &&
			    !marlin->rx_ready) {
				marlin->rx_collecting = false;
				marlin->rx_ready = true;
				wake = true;
			} else {
				marlin->rx_collecting = true;
				marlin->rx_escaped = false;
				marlin->rx_len = 0;
			}
			continue;
		}

		if (!marlin->rx_collecting || marlin->rx_ready)
			continue;

		if (marlin->rx_escaped) {
			byte ^= MARLIN_ESCAPE_XOR;
			marlin->rx_escaped = false;
		} else if (byte == MARLIN_ESCAPE) {
			marlin->rx_escaped = true;
			continue;
		}

		if (marlin->rx_len >= sizeof(marlin->rx_buf)) {
			marlin->rx_collecting = false;
			marlin->rx_len = 0;
			continue;
		}
		marlin->rx_buf[marlin->rx_len++] = byte;
	}
	spin_unlock_irqrestore(&marlin->rx_lock, flags);

	if (wake)
		wake_up_interruptible(&marlin->rx_wait);

	return count;
}

static void sprd_marlin_write_wakeup(struct serdev_device *serdev)
{
	serdev_device_write_wakeup(serdev);
}

static const struct serdev_device_ops sprd_marlin_serdev_ops = {
	.receive_buf = sprd_marlin_receive_buf,
	.write_wakeup = sprd_marlin_write_wakeup,
};

static void sprd_marlin_clear_rx(struct sprd_marlin *marlin)
{
	unsigned long flags;

	spin_lock_irqsave(&marlin->rx_lock, flags);
	marlin->rx_len = 0;
	marlin->rx_collecting = false;
	marlin->rx_escaped = false;
	marlin->rx_ready = false;
	spin_unlock_irqrestore(&marlin->rx_lock, flags);
}

static int sprd_marlin_read_frame(struct sprd_marlin *marlin, u16 *type,
				  u8 *payload, u16 *payload_len,
				  unsigned int timeout_ms)
{
	u8 frame[MARLIN_RAW_SIZE];
	unsigned long flags;
	unsigned int frame_len;
	unsigned int data_len;
	u16 crc, received_crc;
	long waited;

	waited = wait_event_timeout(marlin->rx_wait,
				    READ_ONCE(marlin->rx_ready),
				    msecs_to_jiffies(timeout_ms));
	if (!waited)
		return -ETIMEDOUT;

	spin_lock_irqsave(&marlin->rx_lock, flags);
	frame_len = marlin->rx_len;
	memcpy(frame, marlin->rx_buf, frame_len);
	marlin->rx_ready = false;
	marlin->rx_len = 0;
	spin_unlock_irqrestore(&marlin->rx_lock, flags);

	if (frame_len < 6)
		return -EBADMSG;

	data_len = ((unsigned int)frame[2] << 8) | frame[3];
	if (data_len > MARLIN_MAX_DATA || frame_len != data_len + 6)
		return -EMSGSIZE;

	crc = sprd_marlin_crc16(frame, frame_len - 2);
	received_crc = ((u16)frame[frame_len - 2] << 8) |
		       frame[frame_len - 1];
	if (crc != received_crc)
		return -EBADMSG;

	*type = ((u16)frame[0] << 8) | frame[1];
	*payload_len = data_len;
	if (data_len)
		memcpy(payload, frame + 4, data_len);

	return 0;
}

static int sprd_marlin_write(struct sprd_marlin *marlin,
			     const u8 *data, size_t len)
{
	size_t offset = 0;
	ssize_t written;

	while (offset < len) {
		written = serdev_device_write(marlin->serdev, data + offset,
					      len - offset,
					      msecs_to_jiffies(3000));
		if (written < 0)
			return written;
		if (!written)
			return -EIO;
		offset += written;
	}

	serdev_device_wait_until_sent(marlin->serdev,
				      msecs_to_jiffies(1000));
	return 0;
}

static int sprd_marlin_send_frame(struct sprd_marlin *marlin, u16 type,
				  const u8 *payload, u16 payload_len,
				  u16 *reply_type, u8 *reply,
				  u16 *reply_len,
				  unsigned int timeout_ms)
{
	size_t tx_len;
	int ret;

	if (payload_len > MARLIN_MAX_DATA)
		return -EMSGSIZE;

	sprd_marlin_clear_rx(marlin);
	tx_len = sprd_marlin_encode(marlin, type, payload, payload_len);
	ret = sprd_marlin_write(marlin, marlin->tx_buf, tx_len);
	if (ret)
		return ret;

	ret = sprd_marlin_read_frame(marlin, reply_type, reply, reply_len,
				     timeout_ms);
	if (ret)
		return ret;
	if (*reply_type != MARLIN_REP_ACK) {
		dev_err(&marlin->serdev->dev,
			"BSL command %04x returned %04x (%u bytes)\n",
			type, *reply_type, *reply_len);
		return -EREMOTEIO;
	}

	return 0;
}

static int sprd_marlin_sync(struct sprd_marlin *marlin)
{
	const u8 sync = MARLIN_FLAG;
	u8 reply[MARLIN_MAX_DATA];
	u16 type, reply_len;
	int i, ret;

	for (i = 0; i < MARLIN_SYNC_RETRIES; i++) {
		ret = sprd_marlin_write(marlin, &sync, sizeof(sync));
		if (ret)
			return ret;

		ret = sprd_marlin_read_frame(marlin, &type, reply, &reply_len, 10);
		if (!ret && type == MARLIN_REP_VER) {
			dev_info(&marlin->serdev->dev,
				 "BootROM version response (%u bytes): %*phN\n",
				 reply_len, reply_len, reply);
			return 0;
		}
		if (ret && ret != -ETIMEDOUT && ret != -EBADMSG)
			return ret;
	}

	return -ETIMEDOUT;
}

static int sprd_marlin_expect_ack(struct sprd_marlin *marlin, u16 command,
				  const u8 *payload, u16 payload_len)
{
	u8 reply[MARLIN_MAX_DATA];
	u16 reply_type, reply_len;

	return sprd_marlin_send_frame(marlin, command, payload, payload_len,
				      &reply_type, reply, &reply_len,
				      MARLIN_ACK_TIMEOUT_MS);
}

static int sprd_marlin_load_fdl(struct sprd_marlin *marlin)
{
	const struct firmware *fw;
	u8 start[8];
	size_t offset;
	ktime_t t0;
	int ret;

	ret = request_firmware(&fw, marlin->firmware_name,
			       &marlin->serdev->dev);
	if (ret)
		return dev_err_probe(&marlin->serdev->dev, ret,
				     "failed to load %s\n",
				     marlin->firmware_name);

	if (!fw->size || fw->size > MARLIN_FDL_MAX_SIZE) {
		dev_err(&marlin->serdev->dev, "invalid FDL1 size %zu\n", fw->size);
		ret = -EINVAL;
		goto out_release;
	}

	t0 = ktime_get();
	ret = sprd_marlin_sync(marlin);
	if (ret)
		goto out_release;
	dev_info(&marlin->serdev->dev, "sync %lld ms\n", ktime_to_ms(ktime_sub(ktime_get(), t0)));

	ret = sprd_marlin_expect_ack(marlin, MARLIN_CMD_CONNECT, NULL, 0);
	if (ret)
		goto out_release;
	dev_info(&marlin->serdev->dev, "connect acked %lld ms\n", ktime_to_ms(ktime_sub(ktime_get(), t0)));

	start[0] = marlin->fdl_load_address >> 24;
	start[1] = marlin->fdl_load_address >> 16;
	start[2] = marlin->fdl_load_address >> 8;
	start[3] = marlin->fdl_load_address;
	start[4] = fw->size >> 24;
	start[5] = fw->size >> 16;
	start[6] = fw->size >> 8;
	start[7] = fw->size;
	ret = sprd_marlin_expect_ack(marlin, MARLIN_CMD_START_DATA,
				     start, sizeof(start));
	if (ret)
		goto out_release;

	for (offset = 0; offset < fw->size; offset += MARLIN_FDL_CHUNK) {
		u16 chunk = min_t(size_t, MARLIN_FDL_CHUNK, fw->size - offset);

		ret = sprd_marlin_expect_ack(marlin, MARLIN_CMD_MIDST_DATA,
					     fw->data + offset, chunk);
		if (ret)
			goto out_release;
		if (!offset)
			dev_info(&marlin->serdev->dev, "first chunk acked %lld ms\n",
				 ktime_to_ms(ktime_sub(ktime_get(), t0)));
	}

	ret = sprd_marlin_expect_ack(marlin, MARLIN_CMD_END_DATA, NULL, 0);
	if (ret)
		goto out_release;

	ret = sprd_marlin_expect_ack(marlin, MARLIN_CMD_EXEC_DATA, NULL, 0);
	if (!ret)
		dev_info(&marlin->serdev->dev,
			 "uploaded and executed FDL1 (%zu bytes at %08x)\n",
			 fw->size, marlin->fdl_load_address);

out_release:
	release_firmware(fw);
	return ret;
}

static void sprd_marlin_disable_supplies(void *data)
{
	struct sprd_marlin *marlin = data;

	regulator_bulk_disable(ARRAY_SIZE(marlin->supplies), marlin->supplies);
}

static int sprd_marlin_power_on(struct sprd_marlin *marlin)
{
	struct device *dev = &marlin->serdev->dev;
	int ret;

	marlin->supplies[0].supply = "vddcon";
	marlin->supplies[1].supply = "vddwifipa";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(marlin->supplies),
				      marlin->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Marlin regulators not ready\n");

	ret = regulator_bulk_enable(ARRAY_SIZE(marlin->supplies),
				    marlin->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable Marlin supplies\n");

	ret = devm_add_action_or_reset(dev, sprd_marlin_disable_supplies, marlin);
	if (ret)
		return ret;

	marlin->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(marlin->reset))
		return dev_err_probe(dev, PTR_ERR(marlin->reset),
				     "Marlin reset GPIO not ready\n");

	/* Vendor sprd_download_rst(): high, low, high, with 1 ms per phase. */
	gpiod_set_value_cansleep(marlin->reset, 0);
	msleep(1);
	gpiod_set_value_cansleep(marlin->reset, 1);
	msleep(1);
	gpiod_set_value_cansleep(marlin->reset, 0);
	msleep(1);

	return 0;
}

/* the BootROM dance takes a long while: run it off the boot path */
static void sprd_marlin_work(struct work_struct *work)
{
	struct sprd_marlin *marlin = container_of(work, struct sprd_marlin, work);
	struct device *dev = &marlin->serdev->dev;
	int ret, i;

	ret = sprd_marlin_power_on(marlin);
	if (ret) {
		return;
	}

	ret = sprd_marlin_load_fdl(marlin);
	if (ret) {
		dev_err(dev, "FDL1 download failed: %d\n", ret);
		return;
	}

	/* The SDHCI node is registered separately and probes asynchronously. */
	for (i = 0; i < 50; i++) {
		ret = sdhci_sprd_sc8830_rescan_sdio(marlin->sdio_host);
		if (ret != -EPROBE_DEFER)
			break;
		msleep(100);
	}
	if (ret) {
		dev_err(dev, "SDIO1 host is not ready: %d\n", ret);
		return;
	}

	dev_info(dev, "SDIO1 rescan requested after FDL1 startup\n");
}

static int sprd_marlin_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct sprd_marlin *marlin;
	unsigned int baud;
	int ret;

	marlin = devm_kzalloc(dev, sizeof(*marlin), GFP_KERNEL);
	if (!marlin)
		return -ENOMEM;

	marlin->serdev = serdev;
	marlin->firmware_name = "sc2331_fdl.bin";
	if (of_property_read_string(dev->of_node, "firmware-name",
				    &marlin->firmware_name))
		marlin->firmware_name = "sc2331_fdl.bin";
	if (of_property_read_u32(dev->of_node, "sprd,fdl-load-address",
				 &marlin->fdl_load_address))
		return dev_err_probe(dev, -EINVAL,
				     "sprd,fdl-load-address is required\n");

	marlin->sdio_host = of_parse_phandle(dev->of_node, "sdio-host", 0);
	if (!marlin->sdio_host)
		return dev_err_probe(dev, -EINVAL, "sdio-host phandle is required\n");

	spin_lock_init(&marlin->rx_lock);
	init_waitqueue_head(&marlin->rx_wait);
	serdev_device_set_drvdata(serdev, marlin);
	serdev_device_set_client_ops(serdev, &sprd_marlin_serdev_ops);

	ret = serdev_device_open(serdev);
	if (!ret)
		marlin->serdev_open = true;
	if (ret) {
		of_node_put(marlin->sdio_host);
		return dev_err_probe(dev, ret, "failed to open UART0\n");
	}

	baud = serdev_device_set_baudrate(serdev, MARLIN_BAUD);
	serdev_device_set_flow_control(serdev, false);
	dev_info(dev, "UART0 configured for %u baud\n", baud);

	g_marlin = marlin;
	INIT_WORK(&marlin->work, sprd_marlin_work);
	schedule_work(&marlin->work);
	return 0;
}

static void sprd_marlin_remove(struct serdev_device *serdev)
{
	struct sprd_marlin *marlin = g_marlin;

	sprd_marlin_bt_stop();
	cancel_work_sync(&marlin->work);
	if (marlin->serdev_open)
		serdev_device_close(serdev);
	g_marlin = NULL;
	if (marlin->reset)
		gpiod_set_value_cansleep(marlin->reset, 1);
	of_node_put(marlin->sdio_host);
}


/* ---- Bluetooth over the same UART0 -------------------------------------------------------------------
 *
 * Once the firmware runs, UART0 carries plain H4 HCI at 3 Mbaud with hardware flow control, after a
 * vendor command (opcode 0xfca0) with the controller's configuration: the pskey_bt.txt of Samsung's
 * libbt-vendor, packed as its hw_pskey_send() does (196 bytes: the struct without its first word, with
 * the alignment padding). The BD address (offset 76, little endian) is replaced by our own.
 */
#if IS_ENABLED(CONFIG_BT_HCIUART_SERDEV)
#define MARLIN_PSKEY_OP		0xfca0
#define MARLIN_PSKEY_ADDR_OFF	76
#define MARLIN_BT_BAUD		3000000

static const u8 marlin_pskey[196] = {
	0x00, 0x01, 0x01, 0x01, 0x80, 0xba, 0x8c, 0x01, 0x00, 0x1f, 0x00, 0x00,
	0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x05, 0x00, 0x00, 0x00, 0xfe, 0x8c, 0x41, 0xd0, 0xfe, 0x8c, 0x43, 0xd2,
	0xfe, 0x8c, 0x43, 0xd4, 0xfe, 0x8c, 0x43, 0xd6, 0xfe, 0x8c, 0x04, 0xd8,
	0xff, 0xff, 0x8d, 0xfe, 0x9b, 0x3f, 0x79, 0x83, 0xff, 0xa7, 0xff, 0x7f,
	0x00, 0xe0, 0xf7, 0x3e, 0x11, 0xff, 0x00, 0x22, 0x2d, 0xae, 0x00, 0x01,
	0x05, 0x0e, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x30, 0x00, 0x3f, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
};

struct marlin_h4 {
	struct sk_buff *rx_skb;
	struct sk_buff_head txq;
};

static int marlin_h4_open(struct hci_uart *hu)
{
	struct marlin_h4 *h4 = kzalloc(sizeof(*h4), GFP_KERNEL);

	if (!h4)
		return -ENOMEM;
	skb_queue_head_init(&h4->txq);
	hu->priv = h4;
	return 0;
}

static int marlin_h4_flush(struct hci_uart *hu)
{
	struct marlin_h4 *h4 = hu->priv;

	skb_queue_purge(&h4->txq);
	return 0;
}

static int marlin_h4_close(struct hci_uart *hu)
{
	struct marlin_h4 *h4 = hu->priv;

	skb_queue_purge(&h4->txq);
	kfree_skb(h4->rx_skb);
	hu->priv = NULL;
	kfree(h4);
	return 0;
}

static int marlin_h4_enqueue(struct hci_uart *hu, struct sk_buff *skb)
{
	struct marlin_h4 *h4 = hu->priv;

	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);
	skb_queue_tail(&h4->txq, skb);
	return 0;
}

static const struct h4_recv_pkt marlin_h4_recv_pkts[] = {
	{ H4_RECV_ACL,   .recv = hci_recv_frame },
	{ H4_RECV_SCO,   .recv = hci_recv_frame },
	{ H4_RECV_EVENT, .recv = hci_recv_frame },
	{ H4_RECV_ISO,   .recv = hci_recv_frame },
};

static int marlin_h4_recv(struct hci_uart *hu, const void *data, int count)
{
	struct marlin_h4 *h4 = hu->priv;

	h4->rx_skb = h4_recv_buf(hu, h4->rx_skb, data, count, marlin_h4_recv_pkts, ARRAY_SIZE(marlin_h4_recv_pkts));
	if (IS_ERR(h4->rx_skb)) {
		int err = PTR_ERR(h4->rx_skb);

		bt_dev_err(hu->hdev, "Frame reassembly failed (%d)", err);
		h4->rx_skb = NULL;
		return err;
	}
	return count;
}

static struct sk_buff *marlin_h4_dequeue(struct hci_uart *hu)
{
	struct marlin_h4 *h4 = hu->priv;

	return skb_dequeue(&h4->txq);
}

static int marlin_bt_setup(struct hci_uart *hu)
{
	struct sprd_marlin *m = g_marlin;
	u8 pskey[sizeof(marlin_pskey)];
	struct sk_buff *skb;

	serdev_device_set_flow_control(hu->serdev, true);
	memcpy(pskey, marlin_pskey, sizeof(pskey));
	if (m->have_bd_addr)
		memcpy(pskey + MARLIN_PSKEY_ADDR_OFF, m->bd_addr, 6);
	skb = __hci_cmd_sync(hu->hdev, MARLIN_PSKEY_OP, sizeof(pskey), pskey, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		bt_dev_err(hu->hdev, "pskey configuration failed (%ld)", PTR_ERR(skb));
		return PTR_ERR(skb);
	}
	bt_dev_info(hu->hdev, "controller configured (%d bytes of reply)", skb->len);
	kfree_skb(skb);
	return 0;
}

static const struct hci_uart_proto marlin_h4p = {
	.id		= HCI_UART_H4,
	.name		= "sc2331",
	.manufacturer	= 65535,
	.init_speed	= MARLIN_BT_BAUD,
	.open		= marlin_h4_open,
	.close		= marlin_h4_close,
	.flush		= marlin_h4_flush,
	.setup		= marlin_bt_setup,
	.recv		= marlin_h4_recv,
	.enqueue	= marlin_h4_enqueue,
	.dequeue	= marlin_h4_dequeue,
};

int sprd_marlin_bt_start(const u8 *bd_addr)
{
	struct sprd_marlin *m = g_marlin;
	int ret;

	if (!m)
		return -ENODEV;
	if (m->bt_up)
		return -EBUSY;
	if (bd_addr) {
		memcpy(m->bd_addr, bd_addr, 6);
		m->have_bd_addr = true;
	}
	if (m->serdev_open) {			/* the loader is done with the port */
		serdev_device_close(m->serdev);
		m->serdev_open = false;
	}
	m->hu.serdev = m->serdev;
	serdev_device_set_drvdata(m->serdev, &m->hu);
	ret = hci_uart_register_device(&m->hu, &marlin_h4p);
	if (ret) {
		dev_err(&m->serdev->dev, "cannot register the Bluetooth controller: %d\n", ret);
		return ret;
	}
	m->bt_up = true;
	dev_info(&m->serdev->dev, "Bluetooth controller registered\n");
	return 0;
}
EXPORT_SYMBOL_GPL(sprd_marlin_bt_start);

void sprd_marlin_bt_stop(void)
{
	struct sprd_marlin *m = g_marlin;

	if (m && m->bt_up) {
		hci_uart_unregister_device(&m->hu);
		m->bt_up = false;
	}
}
EXPORT_SYMBOL_GPL(sprd_marlin_bt_stop);
#endif /* CONFIG_BT_HCIUART_SERDEV */

static const struct of_device_id sprd_marlin_of_match[] = {
	{ .compatible = "sprd,sc2331-marlin" },
	{}
};
MODULE_DEVICE_TABLE(of, sprd_marlin_of_match);

static struct serdev_device_driver sprd_marlin_driver = {
	.probe = sprd_marlin_probe,
	.remove = sprd_marlin_remove,
	.driver = {
		.name = "sprd-sc2331-marlin",
		.of_match_table = sprd_marlin_of_match,
	},
};
module_serdev_device_driver(sprd_marlin_driver);

MODULE_DESCRIPTION("Spreadtrum SC2331 Marlin FDL loader");
MODULE_LICENSE("GPL");
