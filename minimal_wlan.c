// SPDX-License-Identifier: GPL-2.0
/*
 * minimal_wlan.c — Minimal Linux WiFi (mac80211) Driver for Beginners
 *
 * This is a VIRTUAL driver. It does NOT talk to real hardware.
 * It registers a fake WiFi interface with the kernel's mac80211 stack
 * so you can study the driver lifecycle without needing any hardware.
 *
 * Kernel subsystem flow:
 *
 *   [ iw / wpa_supplicant ]  ← userspace tools
 *          │  (Netlink)
 *          ▼
 *      [ nl80211 ]           ← kernel Netlink handler
 *          │
 *          ▼
 *      [ cfg80211 ]          ← wireless policy engine
 *          │
 *          ▼
 *      [ mac80211 ]          ← generic MAC layer (handles most 802.11 logic)
 *          │
 *          ▼
 *   [ THIS DRIVER ]          ← you are here
 *          │
 *          ▼
 *   [ (fake) hardware ]      ← simulated — frames are just dropped/printed
 *
 * BUILD:
 *   make
 *   sudo insmod minimal_wlan.ko
 *   ip link show               # look for wlan_min0
 *   sudo rmmod minimal_wlan
 *
 * AUTHOR: Educational example
 */

#include <linux/module.h>       /* MODULE_*, module_init/exit              */
#include <linux/kernel.h>       /* pr_info, pr_err                         */
#include <linux/init.h>         /* __init, __exit                          */
#include <linux/skbuff.h>       /* sk_buff — the universal packet container */
#include <linux/ieee80211.h>    /* 802.11 frame definitions                */
#include <net/mac80211.h>       /* mac80211 API — the main interface        */

/* -------------------------------------------------------------------------
 * Module metadata
 * ---------------------------------------------------------------------- */
MODULE_AUTHOR("Beginner WiFi Study");
MODULE_DESCRIPTION("Minimal virtual mac80211 WiFi driver (educational)");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.1");

/* -------------------------------------------------------------------------
 * 1. DRIVER PRIVATE DATA
 *
 * Every driver stores its own state in a private struct.
 * mac80211 allocates extra space after ieee80211_hw for us — we get a
 * pointer to it via hw->priv.
 * ---------------------------------------------------------------------- */
struct minimal_wlan_priv {
	struct ieee80211_hw *hw;       /* back-pointer to our hw handle    */
	bool                 started;  /* true after drv_start() is called */
	/* In a real driver: DMA rings, firmware state, spinlocks, etc.    */
};

/* -------------------------------------------------------------------------
 * 2. SUPPORTED CHANNELS (the "bands")
 *
 * We must tell mac80211 which frequencies we support.
 * A real driver reads this from EEPROM or firmware.
 * We declare the three standard non-overlapping 2.4 GHz channels:
 *   ch1 = 2412 MHz, ch6 = 2437 MHz, ch11 = 2462 MHz
 * ---------------------------------------------------------------------- */

static struct ieee80211_channel minimal_2ghz_channels[] = {
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2412, .hw_value =  1, .max_power = 20 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2437, .hw_value =  6, .max_power = 20 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2462, .hw_value = 11, .max_power = 20 },
};

/* Supported rates on 2.4 GHz (802.11b/g style) */
static struct ieee80211_rate minimal_2ghz_rates[] = {
	{ .bitrate = 10,  .hw_value = 0 },   /*  1 Mbps */
	{ .bitrate = 20,  .hw_value = 1 },   /*  2 Mbps */
	{ .bitrate = 55,  .hw_value = 2 },   /*  5.5 Mbps */
	{ .bitrate = 110, .hw_value = 3 },   /* 11 Mbps */
	{ .bitrate = 60,  .hw_value = 4 },   /*  6 Mbps (OFDM) */
	{ .bitrate = 120, .hw_value = 5 },   /* 12 Mbps */
	{ .bitrate = 240, .hw_value = 6 },   /* 24 Mbps */
	{ .bitrate = 540, .hw_value = 7 },   /* 54 Mbps */
};

/* Combine channels + rates into a "band" descriptor */
static struct ieee80211_supported_band minimal_band_2ghz = {
	.channels     = minimal_2ghz_channels,
	.n_channels   = ARRAY_SIZE(minimal_2ghz_channels),
	.bitrates     = minimal_2ghz_rates,
	.n_bitrates   = ARRAY_SIZE(minimal_2ghz_rates),
	/* No HT (802.11n) caps for simplicity */
};

/* -------------------------------------------------------------------------
 * 3. FAKE AP TABLE
 *
 * Each entry describes one simulated access point.
 * When the STA sends a Probe Request, we inject a Probe Response for
 * every entry in this table — just like multiple real APs responding.
 *
 * Fields:
 *   bssid      — fake MAC address for the AP (locally administered: 02:xx)
 *   ssid       — network name
 *   ssid_len   — length of ssid (no NUL)
 *   channel    — 802.11 channel number
 *   freq       — centre frequency in MHz (must match channel)
 *   signal     — simulated RSSI in dBm (higher = stronger)
 *   capability — 802.11 Capability Information field
 *                  bit 0 = ESS (infrastructure BSS)
 *                  bit 5 = short preamble
 *                  bit 10 = short slot time
 * ---------------------------------------------------------------------- */
struct fake_ap_info {
	u8   bssid[ETH_ALEN];
	char ssid[IEEE80211_MAX_SSID_LEN + 1];
	u8   ssid_len;
	u8   channel;
	u32  freq;     /* MHz */
	s8   signal;   /* dBm */
	u16  capability;
};

static const struct fake_ap_info fake_ap_table[] = {
	{
		/* Strong signal, ch1 */
		.bssid      = { 0x02, 0x00, 0x00, 0xAA, 0xBB, 0x01 },
		.ssid       = "MinimalNet-A",
		.ssid_len   = 12,
		.channel    = 1,
		.freq       = 2412,
		.signal     = -45,
		.capability = 0x0431,   /* ESS | short preamble | short slot */
	},
	{
		/* Medium signal, ch6 */
		.bssid      = { 0x02, 0x00, 0x00, 0xAA, 0xBB, 0x02 },
		.ssid       = "MinimalNet-B",
		.ssid_len   = 12,
		.channel    = 6,
		.freq       = 2437,
		.signal     = -65,
		.capability = 0x0431,
	},
	{
		/* Weak signal, ch11 */
		.bssid      = { 0x02, 0x00, 0x00, 0xAA, 0xBB, 0x03 },
		.ssid       = "MinimalNet-C",
		.ssid_len   = 12,
		.channel    = 11,
		.freq       = 2462,
		.signal     = -80,
		.capability = 0x0411,   /* ESS | short preamble */
	},
	{
		/* Hidden SSID simulation — empty SSID in Probe Response */
		.bssid      = { 0x02, 0x00, 0x00, 0xAA, 0xBB, 0x04 },
		.ssid       = "",
		.ssid_len   = 0,
		.channel    = 6,
		.freq       = 2437,
		.signal     = -70,
		.capability = 0x0411,
	},
};

/* -------------------------------------------------------------------------
 * 4. PROBE RESPONSE INJECTION
 *
 * Called from minimal_tx() when we see an outgoing Probe Request.
 * For each fake AP in fake_ap_table we craft a valid 802.11 Probe Response
 * frame and hand it to mac80211 via ieee80211_rx(), simulating what a real
 * AP would transmit over the air.
 *
 * Frame layout:
 *   [ 802.11 MAC header (24 B) ]
 *   [ Timestamp (8 B) | Beacon Interval (2 B) | Capability (2 B) ]
 *   [ IE: SSID (variable) ]
 *   [ IE: Supported Rates (8 B) ]
 *   [ IE: DS Parameter Set / channel (1 B) ]
 * ---------------------------------------------------------------------- */

/*
 * build_and_inject_probe_resp() — craft one Probe Response and inject it.
 *
 * @hw:     our ieee80211_hw
 * @req:    the Probe Request header (we copy the source address as dest)
 * @ap:     which fake AP is "responding"
 */
static void build_and_inject_probe_resp(struct ieee80211_hw *hw,
					struct ieee80211_hdr *req,
					const struct fake_ap_info *ap)
{
	/*
	 * IE sizes:
	 *   SSID IE          = 2 + ssid_len
	 *   Supported Rates  = 2 + 8
	 *   DS Parameter Set = 2 + 1
	 */
	const int ie_len = (2 + ap->ssid_len) + (2 + 8) + (2 + 1);
	const int hdr_len = offsetof(struct ieee80211_mgmt,
				     u.probe_resp.variable);

	struct ieee80211_rx_status rx_status = {};
	struct ieee80211_mgmt *mgmt;
	struct sk_buff *skb;
	u8 *pos;

	skb = dev_alloc_skb(hdr_len + ie_len);
	if (!skb)
		return;

	skb_put(skb, hdr_len + ie_len);
	mgmt = (struct ieee80211_mgmt *)skb->data;
	memset(mgmt, 0, hdr_len + ie_len);

	/* ── 802.11 MAC header ────────────────────────────────────────── */
	mgmt->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_PROBE_RESP);
	/* DA = whoever sent the Probe Request */
	memcpy(mgmt->da,    req->addr2,   ETH_ALEN);
	/* SA = BSSID = our fake AP */
	memcpy(mgmt->sa,    ap->bssid,    ETH_ALEN);
	memcpy(mgmt->bssid, ap->bssid,    ETH_ALEN);
	mgmt->seq_ctrl = 0;

	/* ── Fixed parameters ─────────────────────────────────────────── */
	mgmt->u.probe_resp.timestamp  = cpu_to_le64(0);
	mgmt->u.probe_resp.beacon_int = cpu_to_le16(100);   /* 100 TU */
	mgmt->u.probe_resp.capab_info = cpu_to_le16(ap->capability);

	/* ── Information Elements ─────────────────────────────────────── */
	pos = mgmt->u.probe_resp.variable;

	/* SSID IE (tag 0) — empty for hidden networks */
	*pos++ = WLAN_EID_SSID;
	*pos++ = ap->ssid_len;
	if (ap->ssid_len)
		memcpy(pos, ap->ssid, ap->ssid_len);
	pos += ap->ssid_len;

	/* Supported Rates IE (tag 1) — 802.11b/g rates */
	*pos++ = WLAN_EID_SUPP_RATES;
	*pos++ = 8;
	*pos++ = 0x82;   /*  1 Mbps, basic */
	*pos++ = 0x84;   /*  2 Mbps, basic */
	*pos++ = 0x8B;   /*  5.5 Mbps, basic */
	*pos++ = 0x96;   /* 11 Mbps, basic */
	*pos++ = 0x24;   /* 18 Mbps */
	*pos++ = 0x30;   /* 24 Mbps */
	*pos++ = 0x48;   /* 36 Mbps */
	*pos++ = 0x6C;   /* 54 Mbps */

	/* DS Parameter Set IE (tag 3) — tells STA which channel we are on */
	*pos++ = WLAN_EID_DS_PARAMS;
	*pos++ = 1;
	*pos++ = ap->channel;

	/* ── RX status ────────────────────────────────────────────────── */
	/*
	 * ieee80211_rx_status is stored in the skb's control buffer (cb[]).
	 * mac80211 reads it to know on which channel/band the frame arrived
	 * and what the signal strength was.
	 */
	rx_status.band     = NL80211_BAND_2GHZ;
	rx_status.freq     = ap->freq;
	rx_status.signal   = ap->signal;
	rx_status.encoding = RX_ENC_LEGACY;
	rx_status.rate_idx = 0;   /* 1 Mbps */

	memcpy(IEEE80211_SKB_RXCB(skb), &rx_status, sizeof(rx_status));

	pr_info("minimal_wlan: injecting Probe Response: SSID='%s' BSSID=%pM ch%d signal=%d dBm\n",
		ap->ssid_len ? ap->ssid : "(hidden)",
		ap->bssid, ap->channel, ap->signal);

	/*
	 * Hand the frame to mac80211 as if the hardware just received it.
	 * ieee80211_rx() takes ownership of the skb — do NOT touch it after.
	 */
	ieee80211_rx(hw, skb);
}

/*
 * send_fake_probe_responses() — iterate all fake APs and inject one
 * Probe Response per AP.
 */
static void send_fake_probe_responses(struct ieee80211_hw *hw,
				      struct ieee80211_hdr *req)
{
	int i;

	pr_info("minimal_wlan: Probe Request detected — responding with %zu fake AP(s)\n",
		ARRAY_SIZE(fake_ap_table));

	for (i = 0; i < ARRAY_SIZE(fake_ap_table); i++)
		build_and_inject_probe_resp(hw, req, &fake_ap_table[i]);
}

/* -------------------------------------------------------------------------
 * 5. DRIVER CALLBACKS (ieee80211_ops)
 *
 * mac80211 calls these functions to control our "hardware".
 * Think of them as the hardware abstraction layer.
 * ---------------------------------------------------------------------- */

/*
 * tx() — called by mac80211 for every outgoing frame.
 *
 * In a real driver you would:
 *   1. Map the skb into DMA memory
 *   2. Write a TX descriptor to a hardware ring buffer
 *   3. Ring a doorbell register to wake up the hardware
 *   4. Return — the hardware sends it asynchronously
 *   5. Handle TX-done interrupt → call ieee80211_tx_status()
 *
 * Here we log the frame type, intercept Probe Requests, and free it.
 */
static void minimal_tx(struct ieee80211_hw *hw,
		       struct ieee80211_tx_control *control,
		       struct sk_buff *skb)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_hdr    *hdr  = (struct ieee80211_hdr *)skb->data;
	__le16 fc = hdr->frame_control;

	if (ieee80211_is_data(fc)) {
		pr_info("minimal_wlan: TX data frame (%d bytes)\n", skb->len);
	} else if (ieee80211_is_mgmt(fc)) {
		pr_info("minimal_wlan: TX mgmt frame (%d bytes)\n", skb->len);

		/*
		 * Intercept outgoing Probe Requests.
		 * Simulate what real APs on the air would reply.
		 */
		if (ieee80211_is_probe_req(fc))
			send_fake_probe_responses(hw, hdr);
	} else {
		pr_info("minimal_wlan: TX ctrl frame (%d bytes)\n", skb->len);
	}

	/*
	 * We must tell mac80211 the TX completed (successfully or not).
	 * Set the ACK flag to pretend the remote side acknowledged it.
	 * A real driver does this in its TX-done interrupt handler.
	 */
	info->flags |= IEEE80211_TX_STAT_ACK;

	/* ieee80211_tx_status() consumes (frees) the skb */
	ieee80211_tx_status(hw, skb);
}

/*
 * start() — called when the first interface is brought up (e.g. "ip link set wlan0 up").
 *
 * In a real driver: power on hardware, load firmware, init DMA rings.
 */
static int minimal_start(struct ieee80211_hw *hw)
{
	struct minimal_wlan_priv *priv = hw->priv;

	pr_info("minimal_wlan: start() — hardware powered on (simulated)\n");
	priv->started = true;
	return 0;  /* 0 = success */
}

/*
 * stop() — called when the last interface goes down.
 *
 * In a real driver: flush TX queues, power off hardware.
 */
static void minimal_stop(struct ieee80211_hw *hw)
{
	struct minimal_wlan_priv *priv = hw->priv;

	pr_info("minimal_wlan: stop() — hardware powered off (simulated)\n");
	priv->started = false;
}

/*
 * add_interface() — called when a new virtual interface is created.
 *
 * Examples: station (STA), access point (AP), monitor, IBSS (ad-hoc).
 * mac80211 passes a vif (virtual interface) struct we can annotate.
 *
 * In a real driver: program the MAC address into hardware registers.
 */
static int minimal_add_interface(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif)
{
	pr_info("minimal_wlan: add_interface() type=%d addr=%pM\n",
		vif->type, vif->addr);
	return 0;
}

/*
 * remove_interface() — called when a virtual interface is destroyed.
 */
static void minimal_remove_interface(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif)
{
	pr_info("minimal_wlan: remove_interface() addr=%pM\n", vif->addr);
}

/*
 * config() — called when the channel or TX power changes.
 *
 * In a real driver: tune the RF to the new frequency.
 */
static int minimal_config(struct ieee80211_hw *hw, u32 changed)
{
	struct ieee80211_conf *conf = &hw->conf;

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
		pr_info("minimal_wlan: config() — tuning to %d MHz\n",
			conf->chandef.chan->center_freq);
	}
	if (changed & IEEE80211_CONF_CHANGE_POWER) {
		pr_info("minimal_wlan: config() — TX power %d dBm\n",
			conf->power_level);
	}
	return 0;
}

/*
 * configure_filter() — tells driver which frames to pass up to mac80211.
 *
 * Flags like FIF_BCN_PRBRESP_PROMISC, FIF_ALLMULTI, etc.
 * In a real driver: program RX filter registers in hardware.
 */
static void minimal_configure_filter(struct ieee80211_hw *hw,
				     unsigned int changed_flags,
				     unsigned int *total_flags,
				     u64 multicast)
{
	pr_info("minimal_wlan: configure_filter() flags=0x%x\n", *total_flags);

	/*
	 * We are virtual so we accept everything.
	 * A real driver would mask out flags it doesn't support.
	 */
	*total_flags &= (FIF_ALLMULTI | FIF_BCN_PRBRESP_PROMISC |
			 FIF_PROBE_REQ | FIF_OTHER_BSS);
}

/*
 * The ops table — the "vtable" that mac80211 uses to call into our driver.
 * Only the fields we fill in are active; others default to no-op.
 */
static const struct ieee80211_ops minimal_ops = {
	.tx               = minimal_tx,
	.start            = minimal_start,
	.stop             = minimal_stop,
	.add_interface    = minimal_add_interface,
	.remove_interface = minimal_remove_interface,
	.config           = minimal_config,
	.configure_filter = minimal_configure_filter,
};

/* -------------------------------------------------------------------------
 * 6. MODULE INIT / EXIT
 *
 * init  = insmod  → allocate hw, fill capabilities, register with mac80211
 * exit  = rmmod   → unregister, free
 * ---------------------------------------------------------------------- */

/* Global pointer so exit() can clean up */
static struct ieee80211_hw *g_hw;

static int __init minimal_wlan_init(void)
{
	struct minimal_wlan_priv *priv;
	int ret;

	pr_info("minimal_wlan: loading module\n");

	/* ------------------------------------------------------------------
	 * Step A: Allocate ieee80211_hw
	 *
	 * ieee80211_alloc_hw() creates the hw struct AND appends
	 * sizeof(struct minimal_wlan_priv) bytes for our private data.
	 * We get to our private data via hw->priv.
	 * ---------------------------------------------------------------- */
	g_hw = ieee80211_alloc_hw(sizeof(struct minimal_wlan_priv),
				  &minimal_ops);
	if (!g_hw) {
		pr_err("minimal_wlan: ieee80211_alloc_hw() failed\n");
		return -ENOMEM;
	}

	priv = g_hw->priv;
	priv->hw = g_hw;

	/* ------------------------------------------------------------------
	 * Step B: Set hardware capabilities
	 *
	 * These flags tell mac80211 what our hardware supports so it knows
	 * what it needs to handle in software vs. what we do in hardware.
	 * ---------------------------------------------------------------- */

	/*
	 * IEEE80211_HW_SIGNAL_DBM — we report signal strength in dBm.
	 * Without this, mac80211 uses arbitrary units.
	 */
	ieee80211_hw_set(g_hw, SIGNAL_DBM);

	/*
	 * IEEE80211_HW_RX_INCLUDES_FCS — our hardware keeps the 4-byte
	 * Frame Check Sequence at the end of received frames.
	 * (We're virtual so it doesn't matter, but good to know about.)
	 */
	/* ieee80211_hw_set(g_hw, RX_INCLUDES_FCS); */  /* uncomment if needed */

	/* Number of TX hardware queues (one per AC: VO, VI, BE, BK) */
	g_hw->queues = 4;

	/* Maximum number of rates to try per frame before giving up */
	g_hw->max_rates = 4;
	g_hw->max_rate_tries = 11;

	/* ------------------------------------------------------------------
	 * Step C: Register supported frequency bands
	 *
	 * wiphy is the "wireless physical device" descriptor inside hw.
	 * We tell it we support 2.4 GHz only (ch1, ch6, ch11).
	 * ---------------------------------------------------------------- */
	g_hw->wiphy->bands[NL80211_BAND_2GHZ] = &minimal_band_2ghz;

	/*
	 * Interface types we support.
	 * BIT(NL80211_IFTYPE_STATION) = client (STA) mode
	 * BIT(NL80211_IFTYPE_AP)      = access point mode
	 * BIT(NL80211_IFTYPE_MONITOR) = monitor/sniff mode
	 */
	g_hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION) |
				       BIT(NL80211_IFTYPE_AP)      |
				       BIT(NL80211_IFTYPE_MONITOR);

	/* ------------------------------------------------------------------
	 * Step D: Register with mac80211
	 *
	 * After this call, a new wireless interface appears in the system
	 * (visible via "iw dev" or "ip link").
	 * mac80211 will start calling our ops functions.
	 * ---------------------------------------------------------------- */
	ret = ieee80211_register_hw(g_hw);
	if (ret) {
		pr_err("minimal_wlan: ieee80211_register_hw() failed: %d\n", ret);
		ieee80211_free_hw(g_hw);
		return ret;
	}

	pr_info("minimal_wlan: registered — interface is now visible\n");
	pr_info("minimal_wlan: %zu fake AP(s) ready to respond to scans\n",
		ARRAY_SIZE(fake_ap_table));
	pr_info("minimal_wlan: try: iw dev   OR   ip link show\n");
	return 0;
}

static void __exit minimal_wlan_exit(void)
{
	pr_info("minimal_wlan: unloading module\n");

	/*
	 * Unregister first (mac80211 will call remove_interface/stop if needed),
	 * then free the hw struct.
	 */
	ieee80211_unregister_hw(g_hw);
	ieee80211_free_hw(g_hw);

	pr_info("minimal_wlan: unloaded\n");
}

module_init(minimal_wlan_init);
module_exit(minimal_wlan_exit);
