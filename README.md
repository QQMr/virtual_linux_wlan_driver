# Minimal WLAN Linux Driver (Educational)

A heavily-commented virtual WiFi driver built on top of Linux's **mac80211**
framework. No hardware required — runs entirely in software.

---

## Files

```
minimal_wlan_driver/
├── minimal_wlan.c   ← the driver (read this!)
├── Makefile         ← build + load helpers
└── README.md        ← this file
```

---

## Build & Run

### Prerequisites

```bash
# Debian/Ubuntu
sudo apt install linux-headers-$(uname -r) build-essential iw

# Fedora/RHEL
sudo dnf install kernel-devel iw
```

### Build

```bash
cd minimal_wlan_driver
make
```

You should see a `minimal_wlan.ko` file appear.

### Load the driver

```bash
sudo insmod minimal_wlan.ko
dmesg | tail -10          # see our pr_info() messages
ip link show              # a new wlan interface appears
iw dev                    # shows the phy + interface
```

### Bring up the interface

```bash
sudo ip link set wlanX up     # triggers start() + configure_filter()
sudo ip link set wlanX down   # triggers stop()
```

### Unload the driver

```bash
sudo rmmod minimal_wlan
dmesg | tail -5
```

---

## What Each Callback Does

| Callback             | When mac80211 calls it              | What a real driver does          |
|----------------------|-------------------------------------|----------------------------------|
| `start()`            | First `ip link set up`              | Power on HW, load firmware       |
| `stop()`             | Last `ip link set down`             | Flush TX, power off HW           |
| `add_interface()`    | `iw dev add` / netdev created       | Program MAC addr into HW regs    |
| `remove_interface()` | Interface destroyed                 | Free HW resources for that VIF   |
| `tx()`               | Every outgoing 802.11 frame         | Write to DMA TX ring, ring bell  |
| `config()`           | Channel or power change             | Retune RF PLL                    |
| `configure_filter()` | RX filter change (promisc, etc.)    | Program HW RX filter registers   |

---

## TX Path Explained

```
Application writes data
        │
        ▼
   TCP/IP stack
        │
        ▼
   net/mac80211       ← adds 802.11 header, selects rate
        │
        ▼
   minimal_tx()       ← OUR CODE — would DMA to hardware
        │
        ▼
   ieee80211_tx_status()  ← tell mac80211 "TX done, ACKed"
        │
        ▼
   mac80211 updates stats, frees skb
```

## RX Path Explained (for reference — we don't implement it here)

```
Hardware receives frame via antenna
        │
        ▼
   Driver ISR (interrupt handler)
        │  fills: ieee80211_rx_status (signal, rate, band)
        ▼
   ieee80211_rx_irqsafe(hw, skb)
        │
        ▼
   mac80211 decrypts, deaggregates, passes to netdev
        │
        ▼
   Application receives data
```

---

## Next Steps

1. Add a **simulated RX path** — inject a fake beacon frame:
   ```c
   ieee80211_rx_irqsafe(hw, skb);
   ```

2. Add **rate control** (`ieee80211_ops.sta_rc_update`)

3. Add **power save** (`IEEE80211_HW_SUPPORTS_PS`)

4. Study a real driver: `drivers/net/wireless/ralink/rt2x00/` (clean + commented)

5. Use `mac80211_hwsim` for full simulation:
   ```bash
   sudo modprobe mac80211_hwsim radios=2
   ```
