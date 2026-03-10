# V4L2 (Video4Linux2) API — Study Notes

Video4Linux2 is the kernel subsystem and userspace API for video capture, output,
and overlay devices on Linux. Every webcam, TV tuner, and encoder card exposes
itself through V4L2.

---

## 1. Where V4L2 Lives in the Kernel

```
drivers/media/v4l2-core/   ← core framework (videodev2.h, v4l2-dev.c, …)
drivers/media/usb/uvc/     ← USB Video Class (most webcams)
drivers/media/platform/    ← SoC / platform camera drivers
include/uapi/linux/videodev2.h  ← the userspace-visible ABI
```

A V4L2 driver registers a `struct video_device` with the core. The core
provides `/dev/videoN`, handles `open()`/`close()`, and routes `ioctl()` calls
to driver-supplied handler functions (via `struct v4l2_ioctl_ops`).

---

## 2. Key Data Structures

### 2.1 `struct v4l2_device`
Top-level object representing the hardware. Drivers embed this in their private
structure and call `v4l2_device_register()`.

```c
struct my_dev {
    struct v4l2_device v4l2_dev;   /* must be first (or at least registered) */
    struct video_device vdev;
    /* ... hardware-specific fields ... */
};
```

### 2.2 `struct video_device`
Represents one `/dev/videoN` node. Key fields:

| Field | Purpose |
|-------|---------|
| `name` | Shown in `v4l2-ctl --info` |
| `fops` | `file_operations` (open, release, mmap, poll, unlocked_ioctl) |
| `ioctl_ops` | Pointer to `struct v4l2_ioctl_ops` |
| `release` | Called when last reference dropped |
| `device_caps` | Combination of `V4L2_CAP_*` flags |

### 2.3 `struct v4l2_format` (`VIDIOC_G/S_FMT`)
Describes pixel format, resolution, bytes-per-line, etc.

```c
struct v4l2_format {
    __u32 type;           /* V4L2_BUF_TYPE_VIDEO_CAPTURE, etc. */
    union {
        struct v4l2_pix_format        pix;     /* single-plane */
        struct v4l2_pix_format_mplane pix_mp;  /* multi-plane */
        struct v4l2_window            win;
        /* … */
    } fmt;
};
```

Common pixel formats:

| FourCC | Meaning |
|--------|---------|
| `V4L2_PIX_FMT_YUYV` | Packed YUV 4:2:2 |
| `V4L2_PIX_FMT_NV12` | Semi-planar YUV 4:2:0 |
| `V4L2_PIX_FMT_RGB24` | 24-bit RGB |
| `V4L2_PIX_FMT_MJPEG` | Motion JPEG |
| `V4L2_PIX_FMT_H264`  | H.264 compressed |

### 2.4 `struct v4l2_requestbuffers` (`VIDIOC_REQBUFS`)
Allocates a queue of DMA buffers. Driver communicates with the `videobuf2`
(vb2) framework, which handles the details.

### 2.5 `struct v4l2_buffer` (`VIDIOC_QBUF` / `VIDIOC_DQBUF`)
Describes a single buffer in the queue: index, memory type, timestamp, flags.

---

## 3. Buffer Memory Models

V4L2 supports three memory models, selected at `VIDIOC_REQBUFS` time:

| `memory` value | Who owns the memory | Typical use |
|----------------|---------------------|-------------|
| `V4L2_MEMORY_MMAP` | Kernel allocates; userspace `mmap()`s | Webcams, most drivers |
| `V4L2_MEMORY_USERPTR` | Userspace allocates, passes pointer to kernel | Legacy; avoid |
| `V4L2_MEMORY_DMABUF` | Third-party DMA-buf fd (e.g. GPU buffer) | Zero-copy GPU pipelines |

**MMAP flow (most common):**

```
VIDIOC_REQBUFS  → driver allocates N kernel buffers
VIDIOC_QUERYBUF → get offset for each buffer
mmap()          → map each buffer into userspace VA
VIDIOC_QBUF     → give buffer to driver ("fill this")
VIDIOC_STREAMON → start streaming
poll() / select → wait until a buffer is ready
VIDIOC_DQBUF    → take filled buffer back
  … process frame …
VIDIOC_QBUF     → recycle buffer back to driver
VIDIOC_STREAMOFF → stop streaming
```

---

## 4. The videobuf2 (vb2) Framework

Modern V4L2 drivers use `videobuf2` instead of managing buffers manually.
The driver fills a `struct vb2_queue` and provides a small set of callbacks:

```c
static const struct vb2_ops my_vb2_ops = {
    .queue_setup     = my_queue_setup,   /* how many buffers / planes? */
    .buf_prepare     = my_buf_prepare,   /* validate / init a buffer */
    .buf_queue       = my_buf_queue,     /* driver receives a queued buffer */
    .start_streaming = my_start_streaming,
    .stop_streaming  = my_stop_streaming,
};
```

`vb2_queue_init()`, `vb2_ioctl_reqbufs()`, `vb2_ioctl_qbuf()`, etc. then
handle all the ioctl boilerplate automatically.

---

## 5. Controls (`VIDIOC_G/S_CTRL`)

V4L2 exposes device knobs (brightness, contrast, gain, exposure …) as
**controls**. The `v4l2-ctrl` framework manages them:

```c
/* In driver probe: */
v4l2_ctrl_handler_init(&dev->ctrl_handler, 4);
dev->brightness = v4l2_ctrl_new_std(&dev->ctrl_handler,
                      &my_ctrl_ops,
                      V4L2_CID_BRIGHTNESS, 0, 255, 1, 128);
```

Userspace queries with `VIDIOC_QUERYCTRL` and sets with `VIDIOC_S_CTRL`.

---

## 6. ioctl Dispatch

The kernel routes every `ioctl()` on `/dev/videoN` through
`video_ioctl2()` → `__video_do_ioctl()`, which calls the appropriate
`v4l2_ioctl_ops` function pointer. Drivers that use vb2 can re-use the
`vb2_ioctl_*` wrappers for buffer-management ioctls:

```c
static const struct v4l2_ioctl_ops my_ioctl_ops = {
    .vidioc_querycap      = my_querycap,
    .vidioc_enum_fmt_vid_cap = my_enum_fmt,
    .vidioc_g_fmt_vid_cap = my_g_fmt,
    .vidioc_s_fmt_vid_cap = my_s_fmt,
    .vidioc_reqbufs       = vb2_ioctl_reqbufs,    /* vb2 helper */
    .vidioc_querybuf      = vb2_ioctl_querybuf,
    .vidioc_qbuf          = vb2_ioctl_qbuf,
    .vidioc_dqbuf         = vb2_ioctl_dqbuf,
    .vidioc_streamon      = vb2_ioctl_streamon,
    .vidioc_streamoff     = vb2_ioctl_streamoff,
};
```

---

## 7. Driver Lifecycle

```
platform_driver_probe() or usb_probe()
  │
  ├─ alloc priv struct
  ├─ v4l2_device_register(dev, &priv->v4l2_dev)
  ├─ vb2_queue_init(…)
  ├─ v4l2_ctrl_handler_init(…)
  ├─ video_register_device(&priv->vdev, VFL_TYPE_VIDEO, -1)
  │      → /dev/video0 appears
  └─ return 0

open() → v4l2_fh_open() → driver's .open callback
ioctl() → video_ioctl2 → ops table
mmap() → vb2_mmap
poll() → vb2_poll
release() → v4l2_fh_release → driver's .release callback

remove():
  video_unregister_device(&priv->vdev)
  v4l2_ctrl_handler_free(…)
  v4l2_device_unregister(…)
```

---

## 8. Minimal Userspace Capture Loop

```c
#include <linux/videodev2.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

int fd = open("/dev/video0", O_RDWR);

/* 1. Check capabilities */
struct v4l2_capability cap;
ioctl(fd, VIDIOC_QUERYCAP, &cap);
assert(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE);

/* 2. Set format */
struct v4l2_format fmt = {
    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .fmt.pix = { .width=640, .height=480,
                 .pixelformat=V4L2_PIX_FMT_YUYV,
                 .field=V4L2_FIELD_NONE }
};
ioctl(fd, VIDIOC_S_FMT, &fmt);

/* 3. Allocate buffers */
struct v4l2_requestbuffers req = {
    .count=4, .type=V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .memory=V4L2_MEMORY_MMAP
};
ioctl(fd, VIDIOC_REQBUFS, &req);

/* 4. mmap each buffer and queue it */
void *bufs[4]; size_t sizes[4];
for (int i = 0; i < 4; i++) {
    struct v4l2_buffer buf = { .type=req.type, .memory=req.memory, .index=i };
    ioctl(fd, VIDIOC_QUERYBUF, &buf);
    sizes[i] = buf.length;
    bufs[i]  = mmap(NULL, buf.length, PROT_READ|PROT_WRITE,
                    MAP_SHARED, fd, buf.m.offset);
    ioctl(fd, VIDIOC_QBUF, &buf);
}

/* 5. Stream on */
int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
ioctl(fd, VIDIOC_STREAMON, &type);

/* 6. Capture loop */
for (int frame = 0; frame < 100; frame++) {
    struct v4l2_buffer buf = { .type=req.type, .memory=req.memory };
    ioctl(fd, VIDIOC_DQBUF, &buf);          /* wait + dequeue */
    process_frame(bufs[buf.index], buf.bytesused);
    ioctl(fd, VIDIOC_QBUF, &buf);           /* recycle */
}

/* 7. Stream off */
ioctl(fd, VIDIOC_STREAMOFF, &type);
```

---

## 9. Comparison: V4L2 vs mac80211

| Aspect | mac80211 (WLAN) | V4L2 (Video) |
|--------|-----------------|--------------|
| Driver registers via | `ieee80211_alloc_hw()` + `ieee80211_register_hw()` | `v4l2_device_register()` + `video_register_device()` |
| Ops table | `struct ieee80211_ops` | `struct v4l2_ioctl_ops` + `struct v4l2_file_operations` |
| Buffer mgmt | skb ring (TX) / `ieee80211_rx_irqsafe` (RX) | videobuf2 queue |
| Userspace node | Socket / `nl80211` netlink | `/dev/videoN` char device |
| Control plane | `iw` / `wpa_supplicant` | `v4l2-ctl` / libv4l2 |
| Frame callback | `ieee80211_ops.tx()` | `vb2_ops.buf_queue()` |

---

## 10. Further Reading

- `include/uapi/linux/videodev2.h` — the entire V4L2 ABI in one header
- `Documentation/userspace-api/media/v4l/` — official kernel docs
- `drivers/media/platform/vivid/` — the kernel's own virtual V4L2 driver (feature-complete reference)
- `drivers/media/usb/uvc/` — production USB webcam driver
- `v4l2-ctl --all -d /dev/video0` — inspect a live device from userspace
- Hans Verkuil's V4L2 talks at ELCE / LPC
