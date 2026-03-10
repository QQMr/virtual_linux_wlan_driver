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

---

## V4L2 Virtual Camera Driver

This repo also includes `v4l2_virtual_cam.c`, a minimal virtual V4L2 camera
driver that produces a colour-bar test pattern in YUYV format.

### Files

```
├── v4l2_virtual_cam.c   ← the V4L2 driver (read this!)
└── doc/V4L2_Study.md    ← V4L2 API study notes
```

### Build & Load

```bash
make                         # builds both minimal_wlan.ko and v4l2_virtual_cam.ko
sudo make load-cam           # inserts the module → /dev/videoN appears
sudo make show-cam           # v4l2-ctl --all output
sudo make unload-cam         # remove the module
```

### Module Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `width`   | 640     | Frame width in pixels |
| `height`  | 480     | Frame height in pixels |
| `fps`     | 30      | Frames per second (timer-driven) |

```bash
sudo insmod v4l2_virtual_cam.ko width=1280 height=720 fps=60
```

### What Each Piece Does

| Component | Role |
|-----------|------|
| `vcam_probe()` | Allocates state, registers `v4l2_device` + `video_device` |
| `vcam_ioctl_ops` | Handles `QUERYCAP`, `ENUM_FMT`, `G/S_FMT` and delegates buffer ioctls to vb2 helpers |
| `vcam_vb2_ops` | vb2 callbacks: queue setup, buffer prepare/queue, start/stop streaming |
| `vcam_frame_timer()` | Kernel timer that fires at `fps` Hz, fills the next queued buffer with colour bars, marks it `DONE` |
| `fill_yuyv_bars()` | Writes an 8-colour vertical bar pattern in YUYV packed format |

### Capture a Frame from Userspace

```bash
# Capture 10 frames to stdout (requires ffmpeg)
ffmpeg -f v4l2 -i /dev/video0 -vframes 10 frame%04d.png
# Or with v4l2-ctl
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=5 --stream-to=out.yuv
```

### Key V4L2 Concepts Illustrated

- `v4l2_device_register` / `video_register_device` lifecycle
- `struct v4l2_ioctl_ops` dispatch table
- `videobuf2` (vb2) queue with `vb2_vmalloc_memops`
- `VIDIOC_REQBUFS` → `mmap()` → `VIDIOC_QBUF` → `VIDIOC_STREAMON` flow
- Simulated hardware interrupt via `timer_list`
- `v4l2_ctrl_handler` for a brightness control

See `doc/V4L2_Study.md` for a full API walkthrough and comparison with mac80211.
