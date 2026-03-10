// SPDX-License-Identifier: GPL-2.0-only
/*
 * v4l2_virtual_cam.c — Minimal Educational Virtual V4L2 Camera Driver
 *
 * Registers a /dev/videoN device that produces a simple colour-bar test
 * pattern in YUYV format.  No hardware required.
 *
 * Concepts demonstrated:
 *   • v4l2_device / video_device registration
 *   • v4l2_ioctl_ops  (querycap, enum_fmt, g/s_fmt)
 *   • videobuf2 (vb2) queue setup and buffer callbacks
 *   • A kernel timer that "fills" buffers periodically (simulates hardware DMA)
 *   • v4l2_ctrl_handler for a brightness control
 *
 * NOT demonstrated (out of scope for a minimal driver):
 *   • Multi-plane formats
 *   • DMABUF / USERPTR memory models
 *   • Hardware interrupt handling
 *   • Power management
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/videobuf2-vmalloc.h>

/* -------------------------------------------------------------------------
 * Module parameters
 * ---------------------------------------------------------------------- */

static unsigned int width  = 640;
static unsigned int height = 480;
static unsigned int fps    = 30;

module_param(width,  uint, 0444);
MODULE_PARM_DESC(width,  "Frame width  (default 640)");
module_param(height, uint, 0444);
MODULE_PARM_DESC(height, "Frame height (default 480)");
module_param(fps,    uint, 0444);
MODULE_PARM_DESC(fps,    "Frames per second (default 30)");

/* -------------------------------------------------------------------------
 * Private structures
 * ---------------------------------------------------------------------- */

/*
 * Per-buffer state.  vb2 allocates one of these for every buffer in the
 * queue.  We just keep a back-pointer to our device.
 */
struct vcam_buffer {
	struct vb2_v4l2_buffer vb;   /* must be first */
	struct list_head       list; /* links into vcam_dev.buf_list */
};

/*
 * Main driver state.  One instance per /dev/videoN node.
 */
struct vcam_dev {
	/* V4L2 framework objects */
	struct v4l2_device      v4l2_dev;
	struct video_device     vdev;
	struct v4l2_ctrl_handler ctrl_handler;

	/* videobuf2 queue */
	struct vb2_queue        queue;
	struct mutex            lock;      /* serialises ioctl + streaming */

	/* Currently negotiated format */
	struct v4l2_pix_format  fmt;

	/* List of buffers queued by the driver (waiting to be "filled") */
	struct list_head        buf_list;
	spinlock_t              buf_lock;

	/* Timer that fires at 'fps' Hz to simulate a hardware frame interrupt */
	struct timer_list       frame_timer;
	unsigned int            sequence;   /* monotonic frame counter */

	/* Controls */
	struct v4l2_ctrl       *brightness;
};

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Write a simple 8-colour vertical bar pattern into a YUYV buffer. */
static void fill_yuyv_bars(void *buf, unsigned int w, unsigned int h)
{
	/*
	 * YUYV packs two pixels into 4 bytes: [Y0 U Y1 V].
	 * Eight bars of equal width, each a different primary/secondary colour.
	 */
	static const u8 bars[8][3] = { /* Y, Cb, Cr */
		{235, 128, 128}, /* white   */
		{210,  16, 146}, /* yellow  */
		{170, 166,  16}, /* cyan    */
		{145,  54,  34}, /* green   */
		{ 91, 202, 222}, /* magenta */
		{ 65,  90, 240}, /* red     */
		{ 35, 240, 110}, /* blue    */
		{ 16, 128, 128}, /* black   */
	};
	u8 *p = (u8 *)buf;
	unsigned int bar_w = w / 8;

	for (unsigned int y = 0; y < h; y++) {
		for (unsigned int x = 0; x < w; x += 2) {
			unsigned int b = (x / bar_w) < 8 ? (x / bar_w) : 7;
			*p++ = bars[b][0]; /* Y0 */
			*p++ = bars[b][1]; /* Cb */
			*p++ = bars[b][0]; /* Y1 */
			*p++ = bars[b][2]; /* Cr */
		}
	}
}

/* -------------------------------------------------------------------------
 * Timer callback — simulates a hardware "frame ready" interrupt
 * ---------------------------------------------------------------------- */

static void vcam_frame_timer(struct timer_list *t)
{
	struct vcam_dev     *dev = from_timer(dev, t, frame_timer);
	struct vcam_buffer  *buf = NULL;
	unsigned long        flags;

	spin_lock_irqsave(&dev->buf_lock, flags);
	if (!list_empty(&dev->buf_list)) {
		buf = list_first_entry(&dev->buf_list, struct vcam_buffer, list);
		list_del(&buf->list);
	}
	spin_unlock_irqrestore(&dev->buf_lock, flags);

	if (buf) {
		void *vaddr = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);

		/* Produce the test pattern into the mapped kernel buffer */
		fill_yuyv_bars(vaddr, dev->fmt.width, dev->fmt.height);

		/* Stamp the buffer with a timestamp and sequence number */
		buf->vb.vb2_buf.timestamp = ktime_get_ns();
		buf->vb.sequence          = dev->sequence++;
		buf->vb.field             = V4L2_FIELD_NONE;

		/*
		 * Mark the buffer DONE — vb2 will wake any poll()/DQBUF
		 * caller in userspace.
		 */
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
	}

	/* Re-arm the timer for the next frame */
	mod_timer(&dev->frame_timer, jiffies + HZ / fps);
}

/* -------------------------------------------------------------------------
 * vb2_ops callbacks
 * ---------------------------------------------------------------------- */

/*
 * queue_setup — called by VIDIOC_REQBUFS to decide buffer geometry.
 * We return: 1 plane, each plane = (width * height * 2) bytes for YUYV.
 */
static int vcam_queue_setup(struct vb2_queue *q,
			    unsigned int *num_buffers,
			    unsigned int *num_planes,
			    unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct vcam_dev *dev = vb2_get_drv_priv(q);

	if (*num_buffers < 2)
		*num_buffers = 2;  /* need at least 2 to pipeline */

	*num_planes = 1;
	sizes[0]    = dev->fmt.sizeimage;
	return 0;
}

/*
 * buf_prepare — validate / initialise a buffer before it can be queued.
 */
static int vcam_buf_prepare(struct vb2_buffer *vb)
{
	struct vcam_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long sz = dev->fmt.sizeimage;

	if (vb2_plane_size(vb, 0) < sz) {
		pr_err("vcam: buffer too small (%lu < %lu)\n",
		       vb2_plane_size(vb, 0), sz);
		return -EINVAL;
	}
	/* Tell vb2 how many bytes will actually be written */
	vb2_set_plane_payload(vb, 0, sz);
	return 0;
}

/*
 * buf_queue — driver receives a buffer that userspace queued via VIDIOC_QBUF.
 * We simply add it to our pending list; the timer will fill it.
 */
static void vcam_buf_queue(struct vb2_buffer *vb)
{
	struct vcam_dev   *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vcam_buffer *buf  = container_of(vbuf, struct vcam_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&dev->buf_lock, flags);
	list_add_tail(&buf->list, &dev->buf_list);
	spin_unlock_irqrestore(&dev->buf_lock, flags);
}

/*
 * start_streaming — called by VIDIOC_STREAMON once all buffers are queued.
 * We arm the timer here.
 */
static int vcam_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct vcam_dev *dev = vb2_get_drv_priv(q);

	dev->sequence = 0;
	mod_timer(&dev->frame_timer, jiffies + HZ / fps);
	pr_info("vcam: streaming started (%ux%u @ %u fps)\n",
		dev->fmt.width, dev->fmt.height, fps);
	return 0;
}

/*
 * stop_streaming — called by VIDIOC_STREAMOFF.
 * Cancel the timer and return all queued buffers to vb2 as QUEUED→ERROR.
 */
static void vcam_stop_streaming(struct vb2_queue *q)
{
	struct vcam_dev    *dev = vb2_get_drv_priv(q);
	struct vcam_buffer *buf, *tmp;
	unsigned long       flags;

	del_timer_sync(&dev->frame_timer);

	/* Return every pending buffer with an error so vb2 can free them */
	spin_lock_irqsave(&dev->buf_lock, flags);
	list_for_each_entry_safe(buf, tmp, &dev->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&dev->buf_lock, flags);

	pr_info("vcam: streaming stopped\n");
}

static const struct vb2_ops vcam_vb2_ops = {
	.queue_setup      = vcam_queue_setup,
	.buf_prepare      = vcam_buf_prepare,
	.buf_queue        = vcam_buf_queue,
	.start_streaming  = vcam_start_streaming,
	.stop_streaming   = vcam_stop_streaming,
	/*
	 * vb2_vmalloc_memops provides mmap/get_userptr/put_userptr for
	 * vmalloc-backed buffers — we don't need to implement those ourselves.
	 */
};

/* -------------------------------------------------------------------------
 * v4l2_ioctl_ops callbacks
 * ---------------------------------------------------------------------- */

/*
 * VIDIOC_QUERYCAP — tell userspace what this device can do.
 */
static int vcam_querycap(struct file *file, void *priv,
			 struct v4l2_capability *cap)
{
	strscpy(cap->driver,   "vcam",              sizeof(cap->driver));
	strscpy(cap->card,     "Virtual Camera",    sizeof(cap->card));
	strscpy(cap->bus_info, "platform:vcam",     sizeof(cap->bus_info));
	cap->device_caps  = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

/*
 * VIDIOC_ENUM_FMT — enumerate supported pixel formats.
 * We only support YUYV.
 */
static int vcam_enum_fmt(struct file *file, void *priv,
			 struct v4l2_fmtdesc *f)
{
	if (f->index != 0)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_YUYV;
	strscpy(f->description, "YUYV 4:2:2", sizeof(f->description));
	return 0;
}

/*
 * VIDIOC_G_FMT — return the current format.
 */
static int vcam_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct vcam_dev *dev = video_drvdata(file);

	f->fmt.pix = dev->fmt;
	return 0;
}

/*
 * VIDIOC_TRY_FMT / VIDIOC_S_FMT — attempt to set a format.
 *
 * A real driver would accept a range of sizes; we snap to our fixed
 * resolution so the logic stays easy to read.
 */
static int vcam_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;

	pix->pixelformat = V4L2_PIX_FMT_YUYV;  /* only format we support */
	pix->width       = clamp_t(u32, pix->width,  48, 4096) & ~1u; /* must be even */
	pix->height      = clamp_t(u32, pix->height, 32, 2160) & ~1u;
	pix->bytesperline = pix->width * 2;     /* 2 bytes per pixel for YUYV */
	pix->sizeimage    = pix->bytesperline * pix->height;
	pix->field        = V4L2_FIELD_NONE;
	pix->colorspace   = V4L2_COLORSPACE_SRGB;
	return 0;
}

static int vcam_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct vcam_dev *dev = video_drvdata(file);
	int ret;

	/* Cannot change format while streaming */
	if (vb2_is_busy(&dev->queue))
		return -EBUSY;

	ret = vcam_try_fmt(file, priv, f);
	if (ret)
		return ret;

	dev->fmt = f->fmt.pix;
	pr_info("vcam: format set to %ux%u YUYV\n",
		dev->fmt.width, dev->fmt.height);
	return 0;
}

/* Convenience: also fill in the frame interval for VIDIOC_G_PARM */
static int vcam_g_parm(struct file *file, void *priv,
		       struct v4l2_streamparm *sp)
{
	struct v4l2_captureparm *cp = &sp->parm.capture;

	if (sp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	cp->capability       = V4L2_CAP_TIMEPERFRAME;
	cp->timeperframe.numerator   = 1;
	cp->timeperframe.denominator = fps;
	return 0;
}

static const struct v4l2_ioctl_ops vcam_ioctl_ops = {
	.vidioc_querycap         = vcam_querycap,
	.vidioc_enum_fmt_vid_cap = vcam_enum_fmt,
	.vidioc_g_fmt_vid_cap    = vcam_g_fmt,
	.vidioc_s_fmt_vid_cap    = vcam_s_fmt,
	.vidioc_try_fmt_vid_cap  = vcam_try_fmt,
	.vidioc_g_parm           = vcam_g_parm,

	/* All buffer-management ioctls handled by vb2 helpers */
	.vidioc_reqbufs          = vb2_ioctl_reqbufs,
	.vidioc_querybuf         = vb2_ioctl_querybuf,
	.vidioc_qbuf             = vb2_ioctl_qbuf,
	.vidioc_dqbuf            = vb2_ioctl_dqbuf,
	.vidioc_expbuf           = vb2_ioctl_expbuf,
	.vidioc_streamon         = vb2_ioctl_streamon,
	.vidioc_streamoff        = vb2_ioctl_streamoff,

	/* Subscribe to events (required for V4L2_EVENT_CTRL to work) */
	.vidioc_subscribe_event   = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* -------------------------------------------------------------------------
 * File operations
 * ---------------------------------------------------------------------- */

static int vcam_open(struct file *file)
{
	struct vcam_dev *dev = video_drvdata(file);

	return v4l2_fh_open(file);  /* sets file->private_data */
	(void)dev;                   /* suppress unused-variable warning */
}

static const struct v4l2_file_operations vcam_fops = {
	.owner          = THIS_MODULE,
	.open           = vcam_open,
	.release        = vb2_fop_release,
	.read           = vb2_fop_read,
	.poll           = vb2_fop_poll,
	.mmap           = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,     /* core dispatcher */
};

/* -------------------------------------------------------------------------
 * Platform driver probe / remove
 * ---------------------------------------------------------------------- */

static int vcam_probe(struct platform_device *pdev)
{
	struct vcam_dev *dev;
	struct video_device *vdev;
	struct vb2_queue *q;
	int ret;

	/* 1. Allocate private structure */
	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	/* 2. Register v4l2_device — top-level V4L2 object */
	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		return ret;

	/* 3. Set up the mutex and buffer list */
	mutex_init(&dev->lock);
	spin_lock_init(&dev->buf_lock);
	INIT_LIST_HEAD(&dev->buf_list);

	/* 4. Default format: 640×480 YUYV */
	dev->fmt.width        = width;
	dev->fmt.height       = height;
	dev->fmt.pixelformat  = V4L2_PIX_FMT_YUYV;
	dev->fmt.bytesperline = width * 2;
	dev->fmt.sizeimage    = dev->fmt.bytesperline * height;
	dev->fmt.field        = V4L2_FIELD_NONE;
	dev->fmt.colorspace   = V4L2_COLORSPACE_SRGB;

	/* 5. Initialise the vb2 queue */
	q             = &dev->queue;
	q->type       = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes   = VB2_MMAP | VB2_READ;
	q->drv_priv   = dev;
	q->buf_struct_size = sizeof(struct vcam_buffer);
	q->ops        = &vcam_vb2_ops;
	q->mem_ops    = &vb2_vmalloc_memops; /* vmalloc — no IOMMU needed */
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock       = &dev->lock;

	ret = vb2_queue_init(q);
	if (ret) {
		pr_err("vcam: vb2_queue_init failed: %d\n", ret);
		goto err_v4l2;
	}

	/* 6. Set up the control handler (brightness knob) */
	v4l2_ctrl_handler_init(&dev->ctrl_handler, 1);
	dev->brightness = v4l2_ctrl_new_std(&dev->ctrl_handler, NULL,
					    V4L2_CID_BRIGHTNESS,
					    0, 255, 1, 128);
	if (dev->ctrl_handler.error) {
		ret = dev->ctrl_handler.error;
		goto err_vb2;
	}
	dev->v4l2_dev.ctrl_handler = &dev->ctrl_handler;

	/* 7. Initialise the frame timer */
	timer_setup(&dev->frame_timer, vcam_frame_timer, 0);

	/* 8. Fill in and register the video_device node */
	vdev                = &dev->vdev;
	strscpy(vdev->name, "vcam", sizeof(vdev->name));
	vdev->v4l2_dev      = &dev->v4l2_dev;
	vdev->fops          = &vcam_fops;
	vdev->ioctl_ops     = &vcam_ioctl_ops;
	vdev->release       = video_device_release_empty; /* stack-allocated */
	vdev->queue         = q;
	vdev->lock          = &dev->lock;
	vdev->device_caps   = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;

	video_set_drvdata(vdev, dev);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		pr_err("vcam: video_register_device failed: %d\n", ret);
		goto err_ctrl;
	}

	platform_set_drvdata(pdev, dev);
	pr_info("vcam: registered as /dev/video%d (%ux%u YUYV @ %u fps)\n",
		vdev->num, width, height, fps);
	return 0;

err_ctrl:
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
err_vb2:
	vb2_queue_release(q);
err_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);
	return ret;
}

static int vcam_remove(struct platform_device *pdev)
{
	struct vcam_dev *dev = platform_get_drvdata(pdev);

	del_timer_sync(&dev->frame_timer);
	video_unregister_device(&dev->vdev);
	vb2_queue_release(&dev->queue);
	v4l2_ctrl_handler_free(&dev->ctrl_handler);
	v4l2_device_unregister(&dev->v4l2_dev);
	pr_info("vcam: removed\n");
	return 0;
}

/* -------------------------------------------------------------------------
 * Platform device / driver boilerplate
 *
 * Because we have no real hardware, we self-register a platform_device
 * in module_init so the driver probes immediately.
 * ---------------------------------------------------------------------- */

static struct platform_device *vcam_pdev;

static struct platform_driver vcam_driver = {
	.probe  = vcam_probe,
	.remove = vcam_remove,
	.driver = {
		.name = "vcam",
	},
};

static int __init vcam_init(void)
{
	int ret;

	/* Register the driver first, then the device — probe fires synchronously */
	ret = platform_driver_register(&vcam_driver);
	if (ret)
		return ret;

	vcam_pdev = platform_device_register_simple("vcam", -1, NULL, 0);
	if (IS_ERR(vcam_pdev)) {
		platform_driver_unregister(&vcam_driver);
		return PTR_ERR(vcam_pdev);
	}
	return 0;
}

static void __exit vcam_exit(void)
{
	platform_device_unregister(vcam_pdev);
	platform_driver_unregister(&vcam_driver);
}

module_init(vcam_init);
module_exit(vcam_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Educational");
MODULE_DESCRIPTION("Minimal virtual V4L2 camera driver (study aid)");
