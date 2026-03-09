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
#include <linux/atomic.h>       /* atomic64_t — lock-free 64-bit counter   */
#include <linux/ktime.h>        /* ktime_get_boottime_ns()                 */
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
	struct ieee80211_hw *hw;        /* back-pointer to our hw handle     */
	bool                 started;   /* true after drv_start() is called  */

	/*
	 * TSF (Timing Synchronization Function) counter.
	 *
	 * Real hardware has a 64-bit free-running microsecond counter used
	 * to timestamp every received frame (mactime in ieee80211_rx_status).
	 * We simulate it with an atomic so it is safe to read from any
	 * context (timer, softirq, process context) without a spinlock.
	 */
	atomic64_t           tsf_us;    /* simulated TSF, in microseconds    */

	/*
	 * Current RX filter flags (set by configure_filter).
	 * Stored here so other callbacks (e.g. a future scan helper) can
	 * check what frame types mac80211 wants to receive.
	 */
	unsigned int         rx_filter_flags;

	/*
	 * Last known channel frequency (MHz).
	 * Updated in config() so rx_status_fill() can always report the
	 * correct operating frequency without re-reading hw->conf.
	 */
	u32                  cur_freq;
};

/* -------------------------------------------------------------------------
 * 2. SUPPORTED CHANNELS (the "bands")
 *
 * We must tell mac80211 which frequencies we support.
 * A real driver reads this from EEPROM or firmware.
 * We declare just one 2.4 GHz channel (channel 1 = 2412 MHz).
 * ---------------------------------------------------------------------- */

/* Single 2.4 GHz channel */
static struct ieee80211_channel minimal_2ghz_channels[] = {
	{
		.band           = NL80211_BAND_2GHZ,
		.center_freq    = 2412,   /* Channel 1 */
		.hw_value       = 1,
		.max_power      = 20,     /* dBm */
	},
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
 * 3. RX STATUS HELPERS
 *
 * ieee80211_rx_status is the metadata struct that accompanies every frame
 * passed UP to mac80211 via ieee80211_rx() / ieee80211_rx_irqsafe().
 *
 * It tells mac80211:
 *   - On which channel/band the frame arrived
 *   - The signal strength (RSSI) in dBm
 *   - Which rate it was received at
 *   - A hardware timestamp (mactime / TSF)
 *   - A wall-clock/boot timestamp
 *   - Various flags (decrypted? FCS stripped? AMPDU? …)
 *
 * Getting these fields right is critical:
 *   • Wrong band/freq   → frame is silently discarded by cfg80211
 *   • Missing mactime   → IBSS/AP beacon timing breaks
 *   • Wrong rate_idx    → radiotap headers shown in Wireshark are wrong
 * ---------------------------------------------------------------------- */

/*
 * minimal_rx_status_fill() — populate an ieee80211_rx_status for a
 * virtually received frame.
 *
 * @priv:     driver private data (for TSF, cur_freq)
 * @rx_status: the status struct to fill (already zeroed by caller)
 * @signal_dbm: simulated RSSI, e.g. -50 for a strong local AP
 * @rate_idx:  index into minimal_2ghz_rates[] (0=1Mbps … 7=54Mbps)
 *
 * Fields explained inline below.
 */
static void minimal_rx_status_fill(struct minimal_wlan_priv *priv,
				   struct ieee80211_rx_status *rx_status,
				   s8 signal_dbm, u8 rate_idx)
{
	/*
	 * ── Band & Frequency ─────────────────────────────────────────────
	 *
	 * band:  NL80211_BAND_2GHZ or _5GHZ.
	 *        mac80211 uses this to look up the correct rate table.
	 *
	 * freq:  Center frequency of the channel in MHz.
	 *        cfg80211 checks this against the BSS entry; a mismatch
	 *        causes the frame to be silently dropped.
	 *        We read cur_freq that was updated in config().
	 */
	rx_status->band = NL80211_BAND_2GHZ;
	rx_status->freq = priv->cur_freq ? priv->cur_freq : 2412;

	/*
	 * ── Signal Strength ──────────────────────────────────────────────
	 *
	 * signal: RSSI in dBm (signed byte, range −128…+127).
	 *         Valid only when IEEE80211_HW_SIGNAL_DBM is set in hw flags.
	 *         Typical values: −30 (very strong) … −90 (weak).
	 *
	 * chains / chain_signal[]:
	 *         Per-antenna RSSI, used by rate control and beamforming.
	 *         chains is a bitmask (bit 0 = antenna A, bit 1 = B, …).
	 *         Here we simulate a single-antenna device (chain 0 only).
	 */
	rx_status->signal         = signal_dbm;
	rx_status->chains         = BIT(0);           /* antenna 0 only   */
	rx_status->chain_signal[0] = signal_dbm;

	/*
	 * ── Rate ─────────────────────────────────────────────────────────
	 *
	 * encoding:  How the frame was modulated.
	 *   RX_ENC_LEGACY  → 802.11a/b/g (DSSS or OFDM)
	 *   RX_ENC_HT      → 802.11n (MCS index in rate_idx)
	 *   RX_ENC_VHT     → 802.11ac
	 *   RX_ENC_HE      → 802.11ax (Wi-Fi 6)
	 *
	 * bw:  Channel bandwidth.
	 *   RATE_INFO_BW_20 → 20 MHz (standard for b/g)
	 *
	 * rate_idx:
	 *   For LEGACY: index into the band's rate table (minimal_2ghz_rates).
	 *   For HT:     MCS index (0–7 for single-stream 802.11n).
	 *
	 * nss:  Number of spatial streams (1 for our single-antenna sim).
	 */
	rx_status->encoding = RX_ENC_LEGACY;
	rx_status->bw       = RATE_INFO_BW_20;
	rx_status->rate_idx = min_t(u8, rate_idx,
				    (u8)(ARRAY_SIZE(minimal_2ghz_rates) - 1));
	rx_status->nss      = 1;

	/*
	 * ── Timestamps ───────────────────────────────────────────────────
	 *
	 * mactime (TSF timestamp):
	 *   64-bit microsecond counter from the hardware's TSF clock.
	 *   mac80211 uses this for:
	 *     • IBSS/Ad-hoc TSF synchronisation
	 *     • Beacon timing (TBTT calculations)
	 *     • ADDBA / BlockAck session timeouts
	 *
	 *   RX_FLAG_MACTIME_START: mactime is the TSF at the start of the
	 *     first symbol of the MPDU preamble (most accurate, preferred).
	 *   RX_FLAG_MACTIME_END:   mactime is the TSF at the end of the
	 *     last symbol.  Easier for some hardware to provide.
	 *
	 *   We advance our simulated TSF by 1000 µs per call to model
	 *   a stream of frames arriving ~1 ms apart.
	 *
	 * boottime_ns:
	 *   Wall-clock time (since boot) in nanoseconds.
	 *   Used by cfg80211 to age-out BSS entries in scan results.
	 *   Without this, old BSSes never expire.
	 */
	rx_status->mactime    = (u64)atomic64_add_return(1000, &priv->tsf_us);
	rx_status->boottime_ns = ktime_get_boottime_ns();

	/*
	 * ── RX Flags ─────────────────────────────────────────────────────
	 *
	 * flag is a bitmask of RX_FLAG_* values.  Key ones:
	 *
	 * RX_FLAG_MACTIME_START
	 *   Tells mac80211 our mactime is at the start of the preamble.
	 *
	 * RX_FLAG_DECRYPTED
	 *   The frame's payload was decrypted by hardware.
	 *   mac80211 will skip its software decryption path.
	 *   (We don't set this — our frames are never encrypted.)
	 *
	 * RX_FLAG_MMIC_STRIPPED
	 *   The TKIP Michael MIC was verified and stripped by hardware.
	 *   Must be set together with RX_FLAG_DECRYPTED for TKIP.
	 *
	 * RX_FLAG_NO_SIGNAL_VAL
	 *   Signal field is not valid.  We do NOT set this because we
	 *   fill in a meaningful signal value above.
	 *
	 * RX_FLAG_AMPDU_DETAILS / RX_FLAG_AMPDU_LAST_KNOWN
	 *   For A-MPDU aggregation metadata.  Not needed for legacy frames.
	 */
	rx_status->flag |= RX_FLAG_MACTIME_START;
}

/*
 * minimal_rx_inject() — build a minimal 802.11 frame and feed it to
 * mac80211's RX path as if the hardware had just received it.
 *
 * In a real driver this is NOT a function you write — instead the hardware
 * raises an interrupt, your ISR reads the DMA ring, and calls ieee80211_rx().
 * Here we synthesise the frame entirely in software for demonstration.
 *
 * @hw:  our ieee80211_hw
 * @fc:  Frame Control word (decides frame type/subtype)
 *
 * Returns 0 on success, negative errno on allocation failure.
 */
static int minimal_rx_inject(struct ieee80211_hw *hw, __le16 fc)
{
	struct minimal_wlan_priv *priv = hw->priv;
	struct ieee80211_rx_status *rx_status;
	struct ieee80211_hdr_3addr *hdr;
	struct sk_buff *skb;

	/*
	 * Allocate an skb large enough for a minimal 802.11 header.
	 * NET_IP_ALIGN (2) is the standard headroom offset that keeps IP
	 * headers aligned to 4 bytes after the 802.11 + LLC/SNAP overhead.
	 */
	skb = dev_alloc_skb(sizeof(*hdr) + NET_IP_ALIGN);
	if (!skb)
		return -ENOMEM;

	skb_reserve(skb, NET_IP_ALIGN);

	/* Fill in a minimal 802.11 three-address header */
	hdr = skb_put_zero(skb, sizeof(*hdr));
	hdr->frame_control = fc;
	/* addr1 (DA) = broadcast, addr2 (SA) = zero, addr3 (BSSID) = zero */
	eth_broadcast_addr(hdr->addr1);

	/*
	 * IEEE80211_SKB_RXCB(skb) returns a pointer into skb->cb[].
	 * cb[] is 48 bytes of per-packet scratch space that rides with
	 * every sk_buff.  mac80211 reserves it for ieee80211_rx_status.
	 *
	 * We must memset it before use because skb->cb is not zeroed
	 * on allocation (it may contain garbage from a previous sk_buff
	 * reuse in the allocator's pool).
	 */
	rx_status = IEEE80211_SKB_RXCB(skb);
	memset(rx_status, 0, sizeof(*rx_status));

	/*
	 * Fill all RX status fields using our helper.
	 * −55 dBm = "good signal", rate_idx=4 = 6 Mbps OFDM.
	 */
	minimal_rx_status_fill(priv, rx_status, -55, 4);

	pr_debug("minimal_wlan: rx_inject fc=0x%04x freq=%u signal=%d rate_idx=%u\n",
		 le16_to_cpu(fc), rx_status->freq,
		 rx_status->signal, rx_status->rate_idx);

	/*
	 * ieee80211_rx_irqsafe() — hand the skb to mac80211.
	 *
	 * "_irqsafe" means it can be called from interrupt context (or
	 * softirq / timer callback).  It queues the skb onto a per-CPU
	 * tasklet rather than processing it inline.
	 *
	 * After this call the skb belongs to mac80211; do not touch it.
	 */
	ieee80211_rx_irqsafe(hw, skb);
	return 0;
}

/* -------------------------------------------------------------------------
 * 4. DRIVER CALLBACKS (ieee80211_ops)
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
 * Here we just log the frame type and free it.
 */
static void minimal_tx(struct ieee80211_hw *hw,
			struct ieee80211_tx_control *control,
			struct sk_buff *skb)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_hdr    *hdr  = (struct ieee80211_hdr *)skb->data;
	__le16 fc = hdr->frame_control;

	/* Print what kind of 802.11 frame this is */
	if (ieee80211_is_data(fc))
		pr_info("minimal_wlan: TX data frame (%d bytes)\n", skb->len);
	else if (ieee80211_is_mgmt(fc))
		pr_info("minimal_wlan: TX mgmt frame (%d bytes)\n", skb->len);
	else
		pr_info("minimal_wlan: TX ctrl frame (%d bytes)\n", skb->len);

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

	/* Reset TSF to 0 each time the hardware is "powered on" */
	atomic64_set(&priv->tsf_us, 0);

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
	struct minimal_wlan_priv *priv = hw->priv;
	struct ieee80211_conf    *conf = &hw->conf;

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
		priv->cur_freq = conf->chandef.chan->center_freq;
		pr_info("minimal_wlan: config() — tuning to %d MHz\n",
			priv->cur_freq);
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
 *
 * We now also save the accepted flags in priv->rx_filter_flags so that
 * any future code (e.g. a scan helper) can check what mac80211 wants.
 */
static void minimal_configure_filter(struct ieee80211_hw *hw,
				      unsigned int changed_flags,
				      unsigned int *total_flags,
				      u64 multicast)
{
	struct minimal_wlan_priv *priv = hw->priv;

	/*
	 * We are virtual so we accept everything.
	 * A real driver would mask out flags it doesn't support.
	 */
	*total_flags &= (FIF_ALLMULTI | FIF_BCN_PRBRESP_PROMISC |
			 FIF_PROBE_REQ | FIF_OTHER_BSS);

	priv->rx_filter_flags = *total_flags;

	pr_info("minimal_wlan: configure_filter() accepted_flags=0x%x\n",
		priv->rx_filter_flags);
}

/*
 * get_tsf() / set_tsf() — read and write the TSF counter.
 *
 * The TSF (Timing Synchronization Function) is a 64-bit free-running
 * microsecond counter mandated by the 802.11 spec.  mac80211 calls
 * get_tsf() when it needs to:
 *   • Compute the next TBTT (Target Beacon Transmission Time)
 *   • Build a Beacon or Probe Response (Timestamp field in the body)
 *   • Synchronise IBSS TSF with a peer
 *
 * set_tsf() is called when joining an IBSS whose TSF is ahead of ours,
 * so that our beacons are aligned with the rest of the network.
 *
 * reset_tsf() zeros the counter, used when starting a new IBSS.
 *
 * link_id (added in kernel 5.19 for Multi-Link Operation) identifies
 * which 802.11 link the TSF belongs to.  For a classic single-link
 * device, always pass 0.
 */
static u64 minimal_get_tsf(struct ieee80211_hw *hw,
			    struct ieee80211_vif *vif)
{
	struct minimal_wlan_priv *priv = hw->priv;

	return (u64)atomic64_read(&priv->tsf_us);
}

static void minimal_set_tsf(struct ieee80211_hw *hw,
			     struct ieee80211_vif *vif, u64 tsf)
{
	struct minimal_wlan_priv *priv = hw->priv;

	atomic64_set(&priv->tsf_us, (s64)tsf);
	pr_info("minimal_wlan: set_tsf() tsf=%llu µs\n", tsf);
}

static void minimal_reset_tsf(struct ieee80211_hw *hw,
			       struct ieee80211_vif *vif)
{
	struct minimal_wlan_priv *priv = hw->priv;

	atomic64_set(&priv->tsf_us, 0);
	pr_info("minimal_wlan: reset_tsf()\n");
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
	.get_tsf          = minimal_get_tsf,
	.set_tsf          = minimal_set_tsf,
	.reset_tsf        = minimal_reset_tsf,
};

/* -------------------------------------------------------------------------
 * 5. MODULE INIT / EXIT
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
	priv->hw      = g_hw;
	priv->cur_freq = 2412;             /* default: channel 1 */
	atomic64_set(&priv->tsf_us, 0);

	/* ------------------------------------------------------------------
	 * Step B: Set hardware capabilities
	 *
	 * These flags tell mac80211 what our hardware supports so it knows
	 * what it needs to handle in software vs. what we do in hardware.
	 * ---------------------------------------------------------------- */

	/*
	 * IEEE80211_HW_SIGNAL_DBM — we report signal strength in dBm.
	 * Without this, mac80211 uses arbitrary units and the signal field
	 * in ieee80211_rx_status is ignored.
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
	 * We tell it we support 2.4 GHz only.
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
	pr_info("minimal_wlan: try: iw dev   OR   ip link show\n");

	/* ------------------------------------------------------------------
	 * Step E: Inject a test RX frame
	 *
	 * Call minimal_rx_inject() once to demonstrate the full RX path.
	 * We inject a Null-Data management frame (a benign, payload-less
	 * frame type that mac80211 happily accepts).
	 *
	 * In a real driver you would NEVER call this from init; frames
	 * arrive asynchronously from hardware interrupts.
	 * ---------------------------------------------------------------- */
	ret = minimal_rx_inject(g_hw,
				cpu_to_le16(IEEE80211_FTYPE_MGMT |
					    IEEE80211_STYPE_ACTION));
	if (ret)
		pr_warn("minimal_wlan: test rx_inject failed: %d\n", ret);

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
