// SPDX-License-Identifier: GPL-2.0
/*
 * WiFi of the Spreadtrum SC2331 (Marlin) as a cfg80211 station driver, a rewrite of the vendor's
 * drivers/net/wireless/sc2331 (3.10) on top of sprd-marlin-sdio.c.
 *
 * The chip runs the whole MAC/SME in its firmware (loaded beforehand, see /boot/marlin): the host
 * sends it commands (scan, connect with the PMK, ...) and Ethernet frames, in "big frames" written
 * to SDIO channels 0-2, and reads events, command responses and received frames from channels 8/9:
 *
 *   TX big frame = 64-byte header { u8 vif, u8 msg_num, le16 len, 14 x msg header, le32 tx_cnt }
 *                  + messages { msg header, [34 bytes for frames: le32 seq + zeros], payload } padded to 4,
 *                  the whole thing padded to 1 KB (13312 bytes at most)
 *   RX frame     = 8 bytes + messages { msg header, payload } padded to 4, ending at type 0xff
 *   msg header   = { u8 type (7 bits: 0 command/event, 1 response, 2 frame) | vif << 7, u8 subtype, le16 len }
 */

#include <net/cfg80211.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/sprd-marlin-sdio.h>
#include <linux/unaligned.h>
#include <linux/wait.h>

#define HW_TX_SIZE		13312
#define HW_RX_SIZE		12288
#define RX_BUF_SIZE		(32 * 1024)
#define BIG_HDR_LEN		64
#define PKT_EXTRA		34
#define MAX_MSGS		12
#define TX_CHNS			3			/* channels 0, 1, 2 */
#define RX_MASK			0xeb00			/* channels 8, 9, 11, 13, 14, 15 */
#define CMD_TIMEOUT_MS		3000
#define TXQ_STOP		256

enum { MSG_CMD, MSG_RSP, MSG_PKT, MSG_WAPI };

enum {
	CMD_SET_SCAN = 4, CMD_SET_AUTH_TYPE, CMD_SET_WPA_VERSION, CMD_SET_PAIRWISE_CIPHER,
	CMD_SET_GROUP_CIPHER, CMD_SET_AKM_SUITE, CMD_SET_CHANNEL, CMD_SET_BSSID, CMD_SET_ESSID,
	CMD_KEY_ADD, CMD_KEY_DEL, CMD_KEY_SET, CMD_SET_DISCONNECT,
	CMD_SET_DEV_OPEN = 22, CMD_SET_DEV_CLOSE, CMD_SET_PSK,
	CMD_SET_WPS_IE = 26,
	EVT_CONNECT = 128, EVT_DISCONNECT, EVT_SCANDONE, EVT_MGMT_DEAUTH, EVT_MGMT_DISASSOC,
	EVT_REMAIN_ON_CHAN_EXPIRED, EVT_NEW_STATION, EVT_REPORT_FRAME, EVT_CONNECT_AP,
	EVT_SDIO_SEQ_NUM, EVT_REPORT_SCAN_FRAME,
};

enum { CIPHER_NONE, CIPHER_WEP40, CIPHER_WEP104, CIPHER_TKIP, CIPHER_CCMP };
enum { AKM_PSK = 1, AKM_8021X, AKM_FT_8021X, AKM_FT_PSK };
enum { WPS_REQ_IE = 1, WPS_ASSOC_IE };

struct msg_hdr {
	u8 type_vif;
	u8 subtype;
	__le16 len;
} __packed;

struct big_hdr {
	u8 vif;
	u8 msg_num;
	__le16 len;
	struct msg_hdr msg[14];
	__le32 tx_cnt;
} __packed;

struct tx_item {
	struct list_head node;
	struct msg_hdr hdr;
	u8 *data;			/* command payload, or the whole Ethernet frame */
	u32 len;
};

struct sc2331 {
	struct marlin_sdio *m;
	struct wiphy *wiphy;
	struct wireless_dev wdev;
	struct net_device *ndev;
	struct task_struct *task;
	wait_queue_head_t wq;
	bool kick;
	bool opened;

	spinlock_t txlock;
	struct list_head cmdq, pktq;
	int pktq_len;
	u8 *txbuf;
	u8 *rxbuf;
	u32 tx_cnt;
	int rx_dbg, tx_dbg;
	u32 pkt_seq;			/* running number of every frame sent, in its 34-byte gap */

	struct mutex cmd_lock;
	struct completion rsp_done;
	u8 rsp_subtype;
	s32 rsp_code;
	bool rsp_wait;

	bool scanning;
	struct cfg80211_scan_request *scan_req;
	bool connecting, connected;
	u8 bssid[ETH_ALEN];
};

static struct sc2331 *g_sc;

/* ---- message queues -------------------------------------------------------------------------------- */

static void kick(struct sc2331 *sc)
{
	sc->kick = true;
	wake_up(&sc->wq);
}

static void irq_cb(void *data)
{
	kick(data);
}

static int queue_msg(struct sc2331 *sc, u8 type, u8 subtype, const void *data, u32 len, bool is_cmd)
{
	struct tx_item *it = kzalloc(sizeof(*it), GFP_ATOMIC);
	unsigned long flags;

	if (!it)
		return -ENOMEM;
	if (len) {
		it->data = kmemdup(data, len, GFP_ATOMIC);
		if (!it->data) {
			kfree(it);
			return -ENOMEM;
		}
	}
	it->len = len;
	it->hdr.type_vif = type;			/* vif 0: bit 7 clear */
	it->hdr.subtype = subtype;
	it->hdr.len = cpu_to_le16(len);

	spin_lock_irqsave(&sc->txlock, flags);
	if (is_cmd) {
		list_add_tail(&it->node, &sc->cmdq);
	} else {
		list_add_tail(&it->node, &sc->pktq);
		if (++sc->pktq_len >= TXQ_STOP && sc->ndev)
			netif_stop_queue(sc->ndev);
	}
	spin_unlock_irqrestore(&sc->txlock, flags);
	kick(sc);
	return 0;
}

/* send a command and wait for its response; returns the firmware's state code or a negative errno */
static int sc_cmd(struct sc2331 *sc, u8 id, const void *data, u32 len, int timeout_ms)
{
	int ret;

	mutex_lock(&sc->cmd_lock);
	reinit_completion(&sc->rsp_done);
	sc->rsp_subtype = id;
	sc->rsp_wait = true;
	ret = queue_msg(sc, MSG_CMD, id, data, len, true);
	if (!ret && !wait_for_completion_timeout(&sc->rsp_done, msecs_to_jiffies(timeout_ms))) {
		dev_err(sc->wiphy->dev.parent, "command %u: no response\n", id);
		ret = -ETIMEDOUT;
	} else if (!ret) {
		ret = sc->rsp_code ? -EIO : 0;
		if (sc->rsp_code)
			dev_warn(sc->wiphy->dev.parent, "command %u: state code %d\n", id, sc->rsp_code);
	}
	sc->rsp_wait = false;
	mutex_unlock(&sc->cmd_lock);
	return ret;
}

static int sc_cmd_u32(struct sc2331 *sc, u8 id, u32 val)
{
	__le32 v = cpu_to_le32(val);

	return sc_cmd(sc, id, &v, sizeof(v), CMD_TIMEOUT_MS);
}

/* ---- events ----------------------------------------------------------------------------------------- */

static void scan_finish(struct sc2331 *sc, bool aborted)
{
	struct cfg80211_scan_info info = { .aborted = aborted };
	struct cfg80211_scan_request *req = sc->scan_req;

	if (!req)
		return;
	sc->scan_req = NULL;
	sc->scanning = false;
	cfg80211_scan_done(req, &info);
}

static void evt_scan_frame(struct sc2331 *sc, const u8 *p, int len)
{
	/* { u8 ops; (pad) le16 channel; le16 signal (dBm, signed); le16 frame_len } then the frame */
	u8 ops;
	u16 chan, flen;
	s16 sig;
	struct ieee80211_channel *ch;
	struct cfg80211_bss *bss;

	if (len < 8)
		return;
	ops = p[0];
	chan = get_unaligned_le16(p + 2);
	sig = (s16)get_unaligned_le16(p + 4);
	flen = get_unaligned_le16(p + 6);
	if (ops == 1) {				/* end of the scan */
		scan_finish(sc, false);
		return;
	}
	if (ops != 0 || flen < 37 || flen + 8 > len)
		return;
	ch = ieee80211_get_channel(sc->wiphy, ieee80211_channel_to_frequency(chan, NL80211_BAND_2GHZ));
	if (!ch)
		return;
	bss = cfg80211_inform_bss_frame(sc->wiphy, ch, (struct ieee80211_mgmt *)(p + 8), flen,
					sig * 100, GFP_KERNEL);
	if (bss)
		cfg80211_put_bss(sc->wiphy, bss);
}

static void evt_connect(struct sc2331 *sc, const u8 *p, int len)
{
	/* [0] = 1, [1] = reassociation, [2] = status, le16 bssid_len, bssid, u8 req_ie_len, req_ie,
	 * u8 + u8 = resp_ie_len, resp_ie */
	const u8 *bssid, *req, *resp;
	u8 status, req_len;
	u16 bssid_len, resp_len;
	int left = len;

	if (len < 3 || p[0] != 1)
		goto fail;
	status = p[2];
	if (status)
		goto fail;
	p += 3;
	left -= 3;
	if (left < 8)
		goto fail;
	bssid_len = get_unaligned_le16(p);
	bssid = p + 2;
	left -= 2 + bssid_len;
	if (bssid_len != ETH_ALEN || left <= 0)
		goto fail;
	req_len = bssid[bssid_len];
	req = bssid + bssid_len + 1;
	left -= 1 + req_len;
	if (left < 2)
		goto fail;
	resp_len = req[req_len] + req[req_len + 1];
	resp = req + req_len + 2;
	if (resp_len > left - 2)
		resp_len = left - 2;
	memcpy(sc->bssid, bssid, ETH_ALEN);
	sc->connecting = false;
	sc->connected = true;
	cfg80211_connect_result(sc->ndev, bssid, req, req_len, resp, resp_len, WLAN_STATUS_SUCCESS, GFP_KERNEL);
	netif_carrier_on(sc->ndev);
	return;
fail:
	if (sc->connecting) {
		sc->connecting = false;
		cfg80211_connect_result(sc->ndev, sc->bssid, NULL, 0, NULL, 0, WLAN_STATUS_UNSPECIFIED_FAILURE,
					GFP_KERNEL);
	}
}

static void evt_disconnect(struct sc2331 *sc, const u8 *p, int len)
{
	u16 reason = len >= 2 ? get_unaligned_le16(p) : 0;

	if (sc->connecting) {
		sc->connecting = false;
		cfg80211_connect_result(sc->ndev, sc->bssid, NULL, 0, NULL, 0, WLAN_STATUS_UNSPECIFIED_FAILURE,
					GFP_KERNEL);
	} else if (sc->connected) {
		cfg80211_disconnected(sc->ndev, reason, NULL, 0, false, GFP_KERNEL);
	}
	sc->connected = false;
	netif_carrier_off(sc->ndev);
}

static void rx_event(struct sc2331 *sc, u8 evt, const u8 *p, int len)
{
	if (evt != EVT_REPORT_SCAN_FRAME)
		dev_info(sc->wiphy->dev.parent, "event %u len %d: %*ph\n", evt, len, min(len, 32), p);
	switch (evt) {
	case EVT_CONNECT:
		evt_connect(sc, p, len);
		break;
	case EVT_DISCONNECT:
		evt_disconnect(sc, p, len);
		break;
	case EVT_SCANDONE:
		scan_finish(sc, false);
		break;
	case EVT_REPORT_SCAN_FRAME:
		evt_scan_frame(sc, p, len);
		break;
	default:
		dev_dbg(sc->wiphy->dev.parent, "event %u (%d bytes) ignored\n", evt, len);
	}
}

static void rx_frame(struct sc2331 *sc, const u8 *p, int len)
{
	struct sk_buff *skb;

	if (len < ETH_HLEN || !sc->ndev)
		return;
	skb = dev_alloc_skb(len + NET_IP_ALIGN);
	if (!skb)
		return;
	skb_reserve(skb, NET_IP_ALIGN);
	skb_put_data(skb, p, len);
	skb->dev = sc->ndev;
	skb->protocol = eth_type_trans(skb, sc->ndev);
	sc->ndev->stats.rx_packets++;
	sc->ndev->stats.rx_bytes += len;
	netif_rx(skb);
}

static void rx_parse(struct sc2331 *sc, const u8 *buf, int len)
{
	int pos = 8;				/* big frame header */

	while (pos + (int)sizeof(struct msg_hdr) <= len) {
		const struct msg_hdr *h = (const void *)(buf + pos);
		const u8 *data = buf + pos + sizeof(*h);
		int dlen = le16_to_cpu(h->len), type = h->type_vif & 0x7f;

		if (type == 0x7f || h->subtype == 0xff || pos + (int)sizeof(*h) + dlen > len)
			break;
		switch (type) {
		case MSG_PKT:
			if (sc->rx_dbg++ < 20)
				dev_info(sc->wiphy->dev.parent, "rx frame off %u len %d: %*ph\n", h->subtype, dlen,
					 min(dlen, 24), data + h->subtype);
			if (h->subtype <= dlen)
				rx_frame(sc, data + h->subtype, dlen - h->subtype);
			break;
		case MSG_RSP:
			if (sc->rsp_wait && h->subtype == sc->rsp_subtype) {
				sc->rsp_code = dlen >= 4 ? (s32)get_unaligned_le32(data) : 0;
				dev_dbg(sc->wiphy->dev.parent, "response %u: code %d (%d bytes)\n", h->subtype, sc->rsp_code, dlen);
				complete(&sc->rsp_done);
			}
			break;
		case MSG_CMD:
			rx_event(sc, h->subtype, data, dlen);
			break;
		default:
			dev_dbg(sc->wiphy->dev.parent, "unknown message type %d\n", type);
			return;
		}
		pos += sizeof(*h) + ALIGN(dlen, 4);
	}
}

/* ---- the I/O thread ----------------------------------------------------------------------------------- */

static bool sc_rx(struct sc2331 *sc)
{
	bool did = false;
	u16 st;
	int chn, len, ret;

	if (marlin_sdio_chn_status(sc->m, &st))
		return false;
	st &= RX_MASK;
	while (st) {
		chn = __ffs(st);
		st &= ~BIT(chn);
		len = marlin_sdio_chn_len(sc->m, chn);
		if (len <= 0)
			continue;
		if (len > RX_BUF_SIZE)
			len = RX_BUF_SIZE;
		ret = marlin_sdio_chn_read(sc->m, chn, sc->rxbuf, len);
		if (ret) {
			dev_warn(sc->wiphy->dev.parent, "read channel %d: %d\n", chn, ret);
			continue;
		}
		did = true;
		if (chn == 8 || chn == 9)
			rx_parse(sc, sc->rxbuf, len);
		else if (chn == 13)
			dev_err(sc->wiphy->dev.parent, "firmware assert: %.*s\n", min(len, 120), sc->rxbuf);
	}
	return did;
}

static u32 unit_len(const struct tx_item *it)
{
	return ALIGN(sizeof(struct msg_hdr) + ((it->hdr.type_vif & 0x7f) == MSG_PKT ? PKT_EXTRA : 0) + it->len, 4);
}

static bool sc_tx(struct sc2331 *sc)
{
	struct big_hdr *bh = (struct big_hdr *)sc->txbuf;
	unsigned long flags;
	struct tx_item *it;
	LIST_HEAD(done);
	u16 st = 0;
	int num = 0, pos = BIG_HDR_LEN, chn = -1, i, ret;
	bool took_pkt = false;

	spin_lock_irqsave(&sc->txlock, flags);
	if (list_empty(&sc->cmdq) && list_empty(&sc->pktq)) {
		spin_unlock_irqrestore(&sc->txlock, flags);
		return false;
	}
	spin_unlock_irqrestore(&sc->txlock, flags);

	if (marlin_sdio_chn_status(sc->m, &st))
		return false;
	for (i = 0; i < TX_CHNS; i++)
		if (!(st & BIT(i))) {
			chn = i;
			break;
		}
	if (chn < 0)
		return false;				/* all busy: try again shortly */

	memset(sc->txbuf, 0, BIG_HDR_LEN);
	spin_lock_irqsave(&sc->txlock, flags);
	while (num < MAX_MSGS) {
		struct list_head *src = !list_empty(&sc->cmdq) ? &sc->cmdq : &sc->pktq;
		u32 ul;

		if (list_empty(src))
			break;
		it = list_first_entry(src, struct tx_item, node);
		ul = unit_len(it);
		if (pos + ul >= HW_TX_SIZE)
			break;
		list_del(&it->node);
		if (src == &sc->pktq) {
			sc->pktq_len--;
			took_pkt = true;
		}
		if ((it->hdr.type_vif & 0x7f) == MSG_PKT && sc->tx_dbg++ < 20)
			dev_info(sc->wiphy->dev.parent, "tx frame %u bytes: %*ph\n", it->len, min_t(int, it->len, 24), it->data);
		memcpy(sc->txbuf + pos, &it->hdr, sizeof(it->hdr));
		memcpy(&bh->msg[num], &it->hdr, sizeof(it->hdr));
		if ((it->hdr.type_vif & 0x7f) == MSG_PKT) {
			put_unaligned_le32(sc->pkt_seq++, sc->txbuf + pos + sizeof(it->hdr));
			memset(sc->txbuf + pos + sizeof(it->hdr) + 4, 0, PKT_EXTRA - 4);
			memcpy(sc->txbuf + pos + sizeof(it->hdr) + PKT_EXTRA, it->data, it->len);
		} else if (it->len) {
			memcpy(sc->txbuf + pos + sizeof(it->hdr), it->data, it->len);
		}
		memset(sc->txbuf + pos + sizeof(it->hdr) + ((it->hdr.type_vif & 0x7f) == MSG_PKT ? PKT_EXTRA : 0) + it->len,
		       0, ul - (sizeof(it->hdr) + ((it->hdr.type_vif & 0x7f) == MSG_PKT ? PKT_EXTRA : 0) + it->len));
		pos += ul;
		num++;
		list_add_tail(&it->node, &done);
	}
	if (took_pkt && sc->pktq_len < TXQ_STOP / 2 && sc->ndev && netif_queue_stopped(sc->ndev))
		netif_wake_queue(sc->ndev);
	spin_unlock_irqrestore(&sc->txlock, flags);
	if (!num)
		return false;

	bh->vif = 0;
	bh->msg_num = num;
	bh->len = cpu_to_le16(pos);
	bh->tx_cnt = cpu_to_le32(sc->tx_cnt);
	i = ALIGN(pos, 1024);
	memset(sc->txbuf + pos, 0, i - pos);
	ret = marlin_sdio_chn_write(sc->m, chn, sc->txbuf, i);
	if (ret)
		dev_warn(sc->wiphy->dev.parent, "write channel %d (%d bytes): %d\n", chn, i, ret);
	else
		sc->tx_cnt++;

	while (!list_empty(&done)) {
		it = list_first_entry(&done, struct tx_item, node);
		list_del(&it->node);
		kfree(it->data);
		kfree(it);
	}
	return true;
}

static int sc_thread(void *arg)
{
	struct sc2331 *sc = arg;

	while (!kthread_should_stop()) {
		bool did = false;

		sc->kick = false;
		did |= sc_rx(sc);
		did |= sc_tx(sc);
		if (!did)
			wait_event_interruptible_timeout(sc->wq, sc->kick || kthread_should_stop(),
							 msecs_to_jiffies(sc->opened ? 10 : 100));
	}
	return 0;
}

/* ---- net_device -------------------------------------------------------------------------------------- */

static int sc_open(struct net_device *ndev)
{
	struct sc2331 *sc = *(struct sc2331 **)netdev_priv(ndev);
	struct {
		__le16 mode;
		u8 mac[ETH_ALEN];
	} __packed open = { .mode = cpu_to_le16(1) };		/* station */
	int ret;

	memcpy(open.mac, ndev->dev_addr, ETH_ALEN);
	ret = sc_cmd(sc, CMD_SET_DEV_OPEN, &open, sizeof(open), 8000);
	if (ret)
		return ret;
	sc->opened = true;
	netif_carrier_off(ndev);
	netif_start_queue(ndev);
	return 0;
}

static int sc_stop(struct net_device *ndev)
{
	struct sc2331 *sc = *(struct sc2331 **)netdev_priv(ndev);
	u8 mode = 1;

	netif_stop_queue(ndev);
	sc_cmd(sc, CMD_SET_DEV_CLOSE, &mode, 1, CMD_TIMEOUT_MS);
	sc->opened = false;
	scan_finish(sc, true);
	return 0;
}

static netdev_tx_t sc_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct sc2331 *sc = *(struct sc2331 **)netdev_priv(ndev);

	if (!sc->connected || queue_msg(sc, MSG_PKT, 0, skb->data, skb->len, false)) {
		ndev->stats.tx_dropped++;
	} else {
		ndev->stats.tx_packets++;
		ndev->stats.tx_bytes += skb->len;
	}
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops sc_netdev_ops = {
	.ndo_open = sc_open,
	.ndo_stop = sc_stop,
	.ndo_start_xmit = sc_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
};

/* ---- cfg80211 ---------------------------------------------------------------------------------------- */

static int sc_scan(struct wiphy *wiphy, struct cfg80211_scan_request *req)
{
	struct sc2331 *sc = wiphy_priv(wiphy);
	u8 chans[16] = { 0 }, *buf, *p;
	int i, j = 0, ssids_len = 0, total, ret;

	if (!sc->opened)
		return -ENETDOWN;
	if (sc->scan_req)
		return -EBUSY;
	if (req->ie_len > 0 && req->ie_len <= 255) {
		u8 *ie = kmalloc(req->ie_len + 2, GFP_KERNEL);

		if (ie) {
			ie[0] = WPS_REQ_IE;
			ie[1] = req->ie_len;
			memcpy(ie + 2, req->ie, req->ie_len);
			sc_cmd(sc, CMD_SET_WPS_IE, ie, req->ie_len + 2, CMD_TIMEOUT_MS);
			kfree(ie);
		}
	}
	for (i = 0; i < req->n_channels && j < 14; i++) {
		int hw = req->channels[i]->hw_value;

		if (hw)
			chans[++j] = hw;
	}
	chans[0] = j;
	buf = kzalloc(j + 1 + 4 + 512, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memcpy(buf, chans, j + 1);
	p = buf + j + 1 + 4;
	for (i = 0; i < min(req->n_ssids, 9); i++) {
		if (!req->ssids[i].ssid_len)
			continue;
		*p++ = req->ssids[i].ssid_len;
		memcpy(p, req->ssids[i].ssid, req->ssids[i].ssid_len);
		p += req->ssids[i].ssid_len;
		ssids_len += 1 + req->ssids[i].ssid_len;
	}
	put_unaligned_le32(ssids_len, buf + j + 1);
	total = j + 1 + 4 + ssids_len;
	sc->scan_req = req;
	sc->scanning = true;
	ret = sc_cmd(sc, CMD_SET_SCAN, buf, total, CMD_TIMEOUT_MS);
	kfree(buf);
	if (ret) {
		sc->scan_req = NULL;
		sc->scanning = false;
	}
	return ret;
}

static u32 cipher_to_fw(u32 c)
{
	switch (c) {
	case WLAN_CIPHER_SUITE_WEP40:
		return CIPHER_WEP40;
	case WLAN_CIPHER_SUITE_WEP104:
		return CIPHER_WEP104;
	case WLAN_CIPHER_SUITE_TKIP:
		return CIPHER_TKIP;
	case WLAN_CIPHER_SUITE_CCMP:
		return CIPHER_CCMP;
	default:
		return CIPHER_NONE;
	}
}

static int sc_connect(struct wiphy *wiphy, struct net_device *ndev, struct cfg80211_connect_params *sme)
{
	struct sc2331 *sc = wiphy_priv(wiphy);
	bool wep = sme->crypto.cipher_group == WLAN_CIPHER_SUITE_WEP40 ||
		   sme->crypto.cipher_group == WLAN_CIPHER_SUITE_WEP104;
	u32 auth = (sme->auth_type == NL80211_AUTHTYPE_SHARED_KEY ||
		    (sme->auth_type == NL80211_AUTHTYPE_AUTOMATIC && wep)) ? 1 : 0;
	u8 psk[2 + 64] = { 0 };
	int ret, keylen = 0;

	if (!sc->opened)
		return -ENETDOWN;
	ret = sc_cmd_u32(sc, CMD_SET_WPA_VERSION, sme->crypto.wpa_versions);
	if (!ret)
		ret = sc_cmd_u32(sc, CMD_SET_AUTH_TYPE, auth);
	if (!ret && sme->crypto.n_ciphers_pairwise)
		ret = sc_cmd_u32(sc, CMD_SET_PAIRWISE_CIPHER, cipher_to_fw(sme->crypto.ciphers_pairwise[0]));
	if (!ret)
		ret = sc_cmd_u32(sc, CMD_SET_GROUP_CIPHER, cipher_to_fw(sme->crypto.cipher_group));
	if (!ret)
		ret = sc_cmd_u32(sc, CMD_SET_AUTH_TYPE, auth);
	if (!ret && sme->crypto.n_akm_suites) {
		u32 akm = AKM_PSK;

		if (sme->crypto.akm_suites[0] == WLAN_AKM_SUITE_8021X)
			akm = AKM_8021X;
		else if (sme->crypto.akm_suites[0] == WLAN_AKM_SUITE_FT_PSK)
			akm = AKM_FT_PSK;
		ret = sc_cmd_u32(sc, CMD_SET_AKM_SUITE, akm);
	}
	if (!ret) {
		/* the firmware does the four-way handshake itself, from the PMK */
		if (sme->crypto.psk) {
			keylen = 32;
			memcpy(psk + 2, sme->crypto.psk, keylen);
		} else if (sme->key_len && sme->key_len <= 32) {
			keylen = sme->key_len;
			memcpy(psk + 2, sme->key, keylen);
		}
		put_unaligned_le16(keylen, psk);
		ret = sc_cmd(sc, CMD_SET_PSK, psk, 2 + keylen, CMD_TIMEOUT_MS);
	}
	if (!ret && sme->channel)
		ret = sc_cmd_u32(sc, CMD_SET_CHANNEL, ieee80211_frequency_to_channel(sme->channel->center_freq));
	if (!ret && sme->bssid) {
		ret = sc_cmd(sc, CMD_SET_BSSID, sme->bssid, ETH_ALEN, CMD_TIMEOUT_MS);
		memcpy(sc->bssid, sme->bssid, ETH_ALEN);
	}
	if (!ret) {
		u8 *e = kzalloc(2 + sme->ssid_len, GFP_KERNEL);

		if (!e)
			return -ENOMEM;
		put_unaligned_le16(sme->ssid_len, e);
		memcpy(e + 2, sme->ssid, sme->ssid_len);
		sc->connecting = true;
		ret = sc_cmd(sc, CMD_SET_ESSID, e, 2 + sme->ssid_len, CMD_TIMEOUT_MS);
		kfree(e);
		if (ret)
			sc->connecting = false;
	}
	return ret;
}

static int sc_disconnect(struct wiphy *wiphy, struct net_device *ndev, u16 reason)
{
	struct sc2331 *sc = wiphy_priv(wiphy);
	__le16 r = cpu_to_le16(reason);

	return sc_cmd(sc, CMD_SET_DISCONNECT, &r, sizeof(r), CMD_TIMEOUT_MS);
}

/* the firmware keeps its own keys; only WEP would need these */
static int sc_add_key(struct wiphy *w, struct net_device *d, int link_id, u8 idx, bool pairwise,
		      const u8 *mac, struct key_params *params)
{
	return 0;
}

static int sc_del_key(struct wiphy *w, struct net_device *d, int link_id, u8 idx, bool pairwise, const u8 *mac)
{
	return 0;
}

static int sc_set_default_key(struct wiphy *w, struct net_device *d, int link_id, u8 idx, bool unicast,
			      bool multicast)
{
	return 0;
}

static int sc_change_iface(struct wiphy *w, struct net_device *d, enum nl80211_iftype type,
			   struct vif_params *params)
{
	return type == NL80211_IFTYPE_STATION ? 0 : -EOPNOTSUPP;
}

static const struct cfg80211_ops sc_cfg_ops = {
	.scan = sc_scan,
	.connect = sc_connect,
	.disconnect = sc_disconnect,
	.add_key = sc_add_key,
	.del_key = sc_del_key,
	.set_default_key = sc_set_default_key,
	.change_virtual_intf = sc_change_iface,
};

#define CHAN2G(_ch, _freq) { .band = NL80211_BAND_2GHZ, .center_freq = (_freq), .hw_value = (_ch), .max_power = 20 }
static struct ieee80211_channel sc_channels[] = {
	CHAN2G(1, 2412), CHAN2G(2, 2417), CHAN2G(3, 2422), CHAN2G(4, 2427), CHAN2G(5, 2432),
	CHAN2G(6, 2437), CHAN2G(7, 2442), CHAN2G(8, 2447), CHAN2G(9, 2452), CHAN2G(10, 2457),
	CHAN2G(11, 2462), CHAN2G(12, 2467), CHAN2G(13, 2472),
};

#define RATE(_bitrate, _hw) { .bitrate = (_bitrate), .hw_value = (_hw) }
static struct ieee80211_rate sc_rates[] = {
	RATE(10, 0x1), RATE(20, 0x2), RATE(55, 0x5), RATE(110, 0xb), RATE(60, 0x6), RATE(90, 0x9),
	RATE(120, 0xc), RATE(180, 0x12), RATE(240, 0x18), RATE(360, 0x24), RATE(480, 0x30), RATE(540, 0x36),
};

static struct ieee80211_supported_band sc_band_2ghz = {
	.band = NL80211_BAND_2GHZ,
	.channels = sc_channels,
	.n_channels = ARRAY_SIZE(sc_channels),
	.bitrates = sc_rates,
	.n_bitrates = ARRAY_SIZE(sc_rates),
};

static const u32 sc_ciphers[] = {
	WLAN_CIPHER_SUITE_WEP40, WLAN_CIPHER_SUITE_WEP104, WLAN_CIPHER_SUITE_TKIP, WLAN_CIPHER_SUITE_CCMP,
};

/* ---- start / stop -------------------------------------------------------------------------------------- */

void sc2331_wlan_stop(void)
{
	struct sc2331 *sc = g_sc;

	if (!sc)
		return;
	g_sc = NULL;
	marlin_sdio_set_irq_cb(sc->m, NULL, NULL);
	if (sc->ndev)
		unregister_netdev(sc->ndev);
	kthread_stop(sc->task);
	wiphy_unregister(sc->wiphy);
	if (sc->ndev)
		free_netdev(sc->ndev);
	kfree(sc->txbuf);
	kfree(sc->rxbuf);
	wiphy_free(sc->wiphy);
}
EXPORT_SYMBOL_GPL(sc2331_wlan_stop);

int sc2331_wlan_start(struct marlin_sdio *m)
{
	struct device *dev = marlin_sdio_dev(m);
	struct wiphy *wiphy;
	struct sc2331 *sc;
	struct net_device *ndev;
	int ret;

	if (g_sc)
		return -EBUSY;
	wiphy = wiphy_new(&sc_cfg_ops, sizeof(*sc));
	if (!wiphy)
		return -ENOMEM;
	sc = wiphy_priv(wiphy);
	sc->wiphy = wiphy;
	sc->m = m;
	init_waitqueue_head(&sc->wq);
	spin_lock_init(&sc->txlock);
	INIT_LIST_HEAD(&sc->cmdq);
	INIT_LIST_HEAD(&sc->pktq);
	mutex_init(&sc->cmd_lock);
	init_completion(&sc->rsp_done);
	sc->txbuf = kzalloc(HW_TX_SIZE + 1024, GFP_KERNEL);
	sc->rxbuf = kmalloc(RX_BUF_SIZE, GFP_KERNEL);
	if (!sc->txbuf || !sc->rxbuf) {
		ret = -ENOMEM;
		goto err_free;
	}

	set_wiphy_dev(wiphy, dev);
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->max_scan_ssids = 9;
	wiphy->max_scan_ie_len = 2304;
	wiphy->cipher_suites = sc_ciphers;
	wiphy->n_cipher_suites = ARRAY_SIZE(sc_ciphers);
	wiphy->bands[NL80211_BAND_2GHZ] = &sc_band_2ghz;
	wiphy->flags &= ~WIPHY_FLAG_PS_ON_BY_DEFAULT;
	wiphy_ext_feature_set(wiphy, NL80211_EXT_FEATURE_4WAY_HANDSHAKE_STA_PSK);
	ret = wiphy_register(wiphy);
	if (ret)
		goto err_free;

	ndev = alloc_netdev(sizeof(struct sc2331 *), "wlan%d", NET_NAME_ENUM, ether_setup);
	if (!ndev) {
		ret = -ENOMEM;
		goto err_unreg;
	}
	*(struct sc2331 **)netdev_priv(ndev) = sc;
	sc->ndev = ndev;
	ndev->netdev_ops = &sc_netdev_ops;
	ndev->watchdog_timeo = HZ;
	ndev->needs_free_netdev = false;
	sc->wdev.wiphy = wiphy;
	sc->wdev.netdev = ndev;
	sc->wdev.iftype = NL80211_IFTYPE_STATION;
	ndev->ieee80211_ptr = &sc->wdev;
	SET_NETDEV_DEV(ndev, wiphy_dev(wiphy));
	eth_hw_addr_random(ndev);
	ret = register_netdev(ndev);
	if (ret)
		goto err_netdev;

	sc->task = kthread_run(sc_thread, sc, "sc2331");
	if (IS_ERR(sc->task)) {
		ret = PTR_ERR(sc->task);
		goto err_reg;
	}
	g_sc = sc;
	marlin_sdio_set_irq_cb(m, irq_cb, sc);
	dev_info(dev, "WiFi interface %s (%pM) registered\n", ndev->name, ndev->dev_addr);
	return 0;
err_reg:
	unregister_netdev(ndev);
err_netdev:
	free_netdev(ndev);
	sc->ndev = NULL;
err_unreg:
	wiphy_unregister(wiphy);
err_free:
	kfree(sc->txbuf);
	kfree(sc->rxbuf);
	wiphy_free(wiphy);
	return ret;
}
EXPORT_SYMBOL_GPL(sc2331_wlan_start);

MODULE_DESCRIPTION("Spreadtrum SC2331 (Marlin) WiFi, cfg80211 station driver");
MODULE_LICENSE("GPL");
