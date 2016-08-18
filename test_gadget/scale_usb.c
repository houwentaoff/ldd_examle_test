/*
 * scale_usb.c -- scale_usb driver
 *
 * Copyright (C) 2003-2005 David Brownell
 * Copyright (C) 2006 Craig W. Nadler
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/hardirq.h>

//#include <linux/smp_lock.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/list.h>
#include <linux/interrupt.h>
#include <linux/utsname.h>
#include <linux/device.h>
#include <linux/moduleparam.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/types.h>
#include <linux/ctype.h>
#include <linux/cdev.h>

#include <asm/byteorder.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <asm/system.h>
#include <linux/uaccess.h>
#include <asm/unaligned.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
//#include <mach/regs-gpio.h>
#include "gadget_chips.h"
#include <linux/utsname.h>
#include <linux/device.h>
#include <linux/usb/composite.h>
extern int usb_composite_probe(struct usb_composite_driver *driver,
			       int (*bind)(struct usb_composite_dev *cdev));
#include "composite.c"
/*
 * Kbuild is not very cooperative with respect to linking separately
 * compiled library objects into one module.  So for now we won't use
 * separate compilation ... ensuring init/exit sections work to shrink
 * the runtime footprint, and giving us at least some parts of what
 * a "gcc --combine ... part1.c part2.c part3.c ... " build would.
 */
#include "usbstring.c"
#include "config.c"
#include "epautoconf.c"
#include "scale_usb.h"

/*-------------------------------------------------------------------------*/
#define NEW_STYLE

#define DRIVER_DESC		"Scale USB"
#define DRIVER_VERSION		"2016.05.10"

static const char shortname [] = "scale_usb";
static const char driver_desc [] = DRIVER_DESC;

static dev_t scale_devno;
//int is_connect = 0;
static struct class *usb_gadget_class;

/*-------------------------------------------------------------------------*/

struct scale_dev {
	spinlock_t		lock;		/* lock this structure */
	/* lock buffer lists during read/write calls */
	struct mutex		lock_scale_io;
	struct usb_gadget	*gadget;
	struct usb_request	*req;		/* for control responses */
	u8			config;
	s8			interface;
	struct usb_ep		*in_ep, *out_ep;
	const struct usb_endpoint_descriptor *in, *out;	

	struct list_head	rx_reqs;	/* List of free RX structs */
	struct list_head	rx_reqs_active;	/* List of Active RX xfers */
	struct list_head	rx_buffers;	/* List of completed xfers */


	/* wait until there is data to be read. */
	wait_queue_head_t	rx_wait;
	struct list_head	tx_reqs;	/* List of free TX structs */
	struct list_head	tx_reqs_active; /* List of Active TX xfers */
	/* Wait until there are write buffers available to use. */
	wait_queue_head_t	tx_wait;
	/* Wait until all write buffers have been sent. */
	wait_queue_head_t	tx_flush_wait;
	struct usb_request	*current_rx_req;
	size_t			current_rx_bytes;
	u8			*current_rx_buf;
	u8			scale_status;
	u8			reset_scale;
	struct cdev		scale_cdev;
	struct device		*pdev;
	u8			scale_cdev_open;
};

static struct scale_dev scale;

/*-------------------------------------------------------------------------*/

/* DO NOT REUSE THESE IDs with a protocol-incompatible driver!!  Ever!!
 * Instead:  allocate your own, using normal USB-IF procedures.
 */
#define PRINTER_VENDOR_NUM	0x10c4		/* NetChip */
#define PRINTER_PRODUCT_NUM	0x0000		/* Linux-USB Printer Gadget */

/* Number of requests to allocate per endpoint, not used for ep0. */
#define QLEN	10

#ifdef CONFIG_USB_GADGET_DUALSPEED
#define DEVSPEED	USB_SPEED_HIGH
#else   /* full speed (low speed doesn't do bulk) */
#define DEVSPEED        USB_SPEED_FULL
#endif

/*-------------------------------------------------------------------------*/

#define xprintk(d, level, fmt, args...) \
	printk(level "%s: " fmt, DRIVER_DESC, ## args)

#define DEBUG	
#ifdef DEBUG
#define DBG(dev, fmt, args...) \
	xprintk(dev, KERN_INFO, fmt, ## args)
#else
#define DBG(dev, fmt, args...) \
	do { } while (0)
#endif /* DEBUG */

#ifdef VERBOSE
#define VDBG(dev, fmt, args...) \
	xprintk(dev, KERN_DEBUG, fmt, ## args)
#else
#define VDBG(dev, fmt, args...) \
	do { } while (0)
#endif /* VERBOSE */

#define ERROR(dev, fmt, args...) \
	xprintk(dev, KERN_ERR, fmt, ## args)
#define WARNING(dev, fmt, args...) \
	xprintk(dev, KERN_WARNING, fmt, ## args)
#define INFO(dev, fmt, args...) \
	xprintk(dev, KERN_INFO, fmt, ## args)

/*-------------------------------------------------------------------------*/

/* USB DRIVER HOOKUP (to the hardware driver, below us), mostly
 * ep0 implementation:  descriptors, config management, setup().
 * also optional class-specific notification interrupt transfer.
 */

/*
 * DESCRIPTORS ... most are static, but strings and (full) configuration
 * descriptors are built on demand.
 */

#define STRING_MANUFACTURER		1
#define STRING_PRODUCT			2
#define STRING_SERIALNUM		3

/* holds our biggest descriptor */
#define USB_DESC_BUFSIZE		256
#define USB_BUFSIZE			512

/* This device advertises one configuration. */
#define DEV_CONFIG_VALUE		1
#define	SCALE_INTERFACE		        0

static struct usb_device_descriptor device_desc = {
	.bLength =		0x12,
	.bDescriptorType =	0x01,
	.bcdUSB =		cpu_to_le16(0x0200),
	.bDeviceClass =		0x00,
	.bDeviceSubClass =	0x00,
	.bDeviceProtocol =	0x00,
	.idVendor =		cpu_to_le16(PRINTER_VENDOR_NUM),
	.idProduct =		cpu_to_le16(PRINTER_PRODUCT_NUM),
	.iManufacturer =	STRING_MANUFACTURER,
	.iProduct =		STRING_PRODUCT,
	.iSerialNumber =	STRING_SERIALNUM,
	.bNumConfigurations =	1
};

u8 report_desc[] = 
{
	0x05,0x01,
	0x09,0x02,
	0xa1,0x01,
	0x09,0x01,
	0xa1,0x00,
	0x05,0x09,
	0x19,0x01,
	0x29,0x03,
	0x15,0x00,
	0x25,0x01,
	0x95,0x03,
	0x75,0x01,
	0x81,0x02,
	0x95,0x01,
	0x75,0x05,
	0x81,0x01,
	0x05,0x01,
	0x09,0x30,
	0x09,0x31,
	0x09,0x38,
	0x15,0x81,
	0x25,0x7f,
	0x75,0x08,
	0x95,0x03,
	0x81,0x06,
	0xc0,0xc0
};

static struct usb_otg_descriptor otg_desc = {
	.bLength =		sizeof otg_desc,
	.bDescriptorType =	USB_DT_OTG,
	.bmAttributes =		USB_OTG_SRP
};

static struct usb_config_descriptor config_desc1 = {
	.bLength =		sizeof config_desc1,
	.bDescriptorType =	USB_DT_CONFIG,
	/* compute wTotalLength on the fly */
	.wTotalLength = cpu_to_le16(sizeof(config_desc1)),
	.bNumInterfaces =	0x01,
	.bConfigurationValue =	DEV_CONFIG_VALUE,
	.iConfiguration =	0x00,
	.bmAttributes =	USB_CONFIG_ATT_ONE ,
	.bMaxPower =		2 / 2,
};

static struct usb_interface_descriptor intf_desc = {
	.bLength =		0x09,
	.bDescriptorType =	USB_DT_INTERFACE,
	.bInterfaceNumber =	0x00,
	.bNumEndpoints =	0x02,
	.bInterfaceClass =	USB_CLASS_PER_INTERFACE,
	.bInterfaceSubClass =	0x00,	/* Sub-Class */
	.bInterfaceProtocol =	0x00,	/* Bi-Directional */
	.iInterface =		0x00
};


static struct usb_endpoint_descriptor fs_ep_in_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	__constant_cpu_to_le16(512),
	//.bInterval =		32,	// frames -> 32 ms
};

static struct usb_endpoint_descriptor fs_ep_out_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	__constant_cpu_to_le16(512),
	//.bInterval =		32,	// frames -> 32 ms
};

static const struct usb_descriptor_header *fs_scale_function [11] = {
	(struct usb_descriptor_header *) &config_desc1, //add 
	(struct usb_descriptor_header *) &intf_desc,
	(struct usb_descriptor_header *) &fs_ep_in_desc,
	(struct usb_descriptor_header *) &fs_ep_out_desc,
	NULL
};

#ifdef	CONFIG_USB_GADGET_DUALSPEED

/*
 * usb 2.0 devices need to expose both high speed and full speed
 * descriptors, unless they only run at full speed.
 */

static struct usb_endpoint_descriptor hs_ep_in_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	__constant_cpu_to_le16(512),
	//.bInterval =		9,	// 2**(9-1) = 256 uframes -> 32 ms
};

static struct usb_endpoint_descriptor hs_ep_out_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	__constant_cpu_to_le16(512),
	//.bInterval =		9,	// 2**(9-1) = 256 uframes -> 32 ms
};


static struct usb_qualifier_descriptor dev_qualifier = {
	.bLength =		sizeof dev_qualifier,
	.bDescriptorType =	USB_DT_DEVICE_QUALIFIER,
	.bcdUSB =		cpu_to_le16(0x0200),
	.bDeviceClass =		USB_CLASS_COMM,
	.bNumConfigurations =	1
};

static const struct usb_descriptor_header *hs_scale_function [11] = {
	(struct usb_descriptor_header *) &otg_desc,
	(struct usb_descriptor_header *) &intf_desc,
	(struct usb_descriptor_header *) &hs_ep_in_desc,
	(struct usb_descriptor_header *) &hs_ep_out_desc,
	NULL
};

/* maxpacket and other transfer characteristics vary by speed. */
#define ep_desc(g, hs, fs) (((g)->speed == USB_SPEED_HIGH)?(hs):(fs))

#else

/* if there's no high speed support, maxpacket doesn't change. */
#define ep_desc(g, hs, fs) (((void)(g)), (fs))

#endif	/* !CONFIG_USB_GADGET_DUALSPEED */

/*-------------------------------------------------------------------------*/

/* descriptors that are built on-demand */

static char				manufacturer [50] = "jiuzhou";
static char				product_desc [40] = DRIVER_DESC;
static char				serial_num [40] = "1";
static char				pnp_string [1024] =
	"XXMFG:linux;MDL:scale;CLS:COMM;SN:1;";

/* static strings, in UTF-8 */
static struct usb_string		strings [] = {
	{ STRING_MANUFACTURER,	manufacturer, },
	{ STRING_PRODUCT,	product_desc, },
	{ STRING_SERIALNUM,	serial_num, },
	{  }		/* end of list */
};

static struct usb_gadget_strings	stringtab = {
	.language	= 0x0409,	/* en-us */
	.strings	= strings,
};

/*-------------------------------------------------------------------------*/

static struct usb_request *
scale_req_alloc(struct usb_ep *ep, unsigned len, gfp_t gfp_flags)
{
	struct usb_request	*req;

	req = usb_ep_alloc_request(ep, gfp_flags);

	if (req != NULL) {
		req->length = len;
		req->buf = kmalloc(len, gfp_flags);
		if (req->buf == NULL) {
			usb_ep_free_request(ep, req);
			return NULL;
		}
	}

	return req;
}

static void
scale_req_free(struct usb_ep *ep, struct usb_request *req)
{
	if (ep != NULL && req != NULL) {
		kfree(req->buf);
		usb_ep_free_request(ep, req);
	}
}



/*-------------------------------------------------------------------------*/

static void rx_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct scale_dev	*dev = ep->driver_data;
	int			status = req->status;
	unsigned long		flags;

	spin_lock_irqsave(&dev->lock, flags);

	list_del_init(&req->list);	/* Remode from Active List */

	switch (status) {

	/* normal completion */
	case 0:
		if (req->actual > 0) {
			list_add_tail(&req->list, &dev->rx_buffers);
			DBG(dev, "rx length %d\n", req->actual);
		} else {
			list_add(&req->list, &dev->rx_reqs);
		}
		break;

	/* software-driven interface shutdown */
	case -ECONNRESET:		/* unlink */
	case -ESHUTDOWN:		/* disconnect etc */
		VDBG(dev, "rx shutdown, code %d\n", status);
		list_add(&req->list, &dev->rx_reqs);
		break;

	/* for hardware automagic (such as pxa) */
	case -ECONNABORTED:		/* endpoint reset */
		DBG(dev, "rx %s reset\n", ep->name);
		list_add(&req->list, &dev->rx_reqs);
		break;

	/* data overrun */
	case -EOVERFLOW:
		/* FALLTHROUGH */

	default:
		DBG(dev, "rx status %d\n", status);
		list_add(&req->list, &dev->rx_reqs);
		break;
	}

	wake_up_interruptible(&dev->rx_wait);
	spin_unlock_irqrestore(&dev->lock, flags);
}

static void tx_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct scale_dev	*dev = ep->driver_data;

	DBG(dev, "%s\n", __func__);

	switch (req->status) {
	default:
		VDBG(dev, "tx err %d\n", req->status);
		/* FALLTHROUGH */
	case -ECONNRESET:		/* unlink */
	case -ESHUTDOWN:		/* disconnect etc */
		break;
	case 0:
		break;
	}

	spin_lock(&dev->lock);
	/* Take the request struct off the active list and put it on the
	 * free list.
	 */
	list_del_init(&req->list);
	list_add(&req->list, &dev->tx_reqs);
	wake_up_interruptible(&dev->tx_wait);
	if (likely(list_empty(&dev->tx_reqs_active)))
		wake_up_interruptible(&dev->tx_flush_wait);

	spin_unlock(&dev->lock);
}

/*-------------------------------------------------------------------------*/
static int
scale_open(struct inode *inode, struct file *fd)
{
	struct scale_dev	*dev;
	unsigned long		flags;
	int			ret = -EBUSY;

	//lock_kernel();
	dev = container_of(inode->i_cdev, struct scale_dev, scale_cdev);

	spin_lock_irqsave(&dev->lock, flags);

	if (!dev->scale_cdev_open) {
		dev->scale_cdev_open = 1;
		fd->private_data = dev;
		ret = 0;
		/* Change the zebra_mouse status to show that it's on-line. */
		dev->scale_status |= SCALE_USB_SELECTED;
	}

	spin_unlock_irqrestore(&dev->lock, flags);

	DBG(dev, "scale_open returned %x\n", ret);
	//unlock_kernel();
	return ret;
}

static int
scale_close(struct inode *inode, struct file *fd)
{
	struct scale_dev	*dev = fd->private_data;
	unsigned long		flags;

	spin_lock_irqsave(&dev->lock, flags);
	dev->scale_cdev_open = 0;
	fd->private_data = NULL;
	/* Change scale_usb status to show that the scale_usb is off-line. */
	dev->scale_status &= ~SCALE_USB_SELECTED;
	spin_unlock_irqrestore(&dev->lock, flags);

	DBG(dev, "scale_close\n");
  	
	return 0;
}

/* This function must be called with interrupts turned off. */
static void
setup_rx_reqs(struct scale_dev *dev)
{
	struct usb_request              *req;

	while (likely(!list_empty(&dev->rx_reqs))) {
		int error;

		req = container_of(dev->rx_reqs.next,
				struct usb_request, list);
		list_del_init(&req->list);

		/* The USB Host sends us whatever amount of data it wants to
		 * so we always set the length field to the full USB_BUFSIZE.
		 * If the amount of data is more than the read() caller asked
		 * for it will be stored in the request buffer until it is
		 * asked for by read().
		 */
		req->length = USB_BUFSIZE;
		req->complete = rx_complete;

		error = usb_ep_queue(dev->out_ep, req, GFP_ATOMIC);
		if (error) {
			DBG(dev, "rx submit --> %d\n", error);
			list_add(&req->list, &dev->rx_reqs);
			break;
		} else {
			list_add(&req->list, &dev->rx_reqs_active);
		}
	}
}

static ssize_t
scale_read(struct file *fd, char __user *buf, size_t len, loff_t *ptr)
{
	struct scale_dev		*dev = fd->private_data;
	unsigned long			flags;
	size_t				size;
	size_t				bytes_copied;
	struct usb_request		*req;
	/* This is a pointer to the current USB rx request. */
	struct usb_request		*current_rx_req;
	/* This is the number of bytes in the current rx buffer. */
	size_t				current_rx_bytes;
	/* This is a pointer to the current rx buffer. */
	u8				*current_rx_buf;

	if (len == 0)
		return -EINVAL;

	DBG(dev, "scale_read trying to read %d bytes\n", (int)len);

	mutex_lock(&dev->lock_scale_io);
	spin_lock_irqsave(&dev->lock, flags);

	/* We will use this flag later to check if a zebra_mouse reset happened
	 * after we turn interrupts back on.
	 */
	dev->reset_scale = 0;

	setup_rx_reqs(dev);

	bytes_copied = 0;
	current_rx_req = dev->current_rx_req;
	current_rx_bytes = dev->current_rx_bytes;
	current_rx_buf = dev->current_rx_buf;
	dev->current_rx_req = NULL;
	dev->current_rx_bytes = 0;
	dev->current_rx_buf = NULL;

	/* Check if there is any data in the read buffers. Please note that
	 * current_rx_bytes is the number of bytes in the current rx buffer.
	 * If it is zero then check if there are any other rx_buffers that
	 * are on the completed list. We are only out of data if all rx
	 * buffers are empty.
	 */
	if ((current_rx_bytes == 0) &&
			(likely(list_empty(&dev->rx_buffers)))) {
		/* Turn interrupts back on before sleeping. */
		spin_unlock_irqrestore(&dev->lock, flags);

		/*
		 * If no data is available check if this is a NON-Blocking
		 * call or not.
		 */
		if (fd->f_flags & (O_NONBLOCK|O_NDELAY)) {
			mutex_unlock(&dev->lock_scale_io);
			return -EAGAIN;
		}

		/* Sleep until data is available */
		wait_event_interruptible(dev->rx_wait,
				(likely(!list_empty(&dev->rx_buffers))));
		spin_lock_irqsave(&dev->lock, flags);
	}

	/* We have data to return then copy it to the caller's buffer.*/
	while ((current_rx_bytes || likely(!list_empty(&dev->rx_buffers)))
			&& len) {
		if (current_rx_bytes == 0) {
			req = container_of(dev->rx_buffers.next,
					struct usb_request, list);
			list_del_init(&req->list);

			if (req->actual && req->buf) {
				current_rx_req = req;
				current_rx_bytes = req->actual;
				current_rx_buf = req->buf;
			} else {
				list_add(&req->list, &dev->rx_reqs);
				continue;
			}
		}

		/* Don't leave irqs off while doing memory copies */
		spin_unlock_irqrestore(&dev->lock, flags);

		if (len > current_rx_bytes)
			size = current_rx_bytes;
		else
			size = len;

		size -= copy_to_user(buf, current_rx_buf, size);
		bytes_copied += size;
		len -= size;
		buf += size;

		spin_lock_irqsave(&dev->lock, flags);

		/* We've disconnected or reset so return. */
		if (dev->reset_scale) {
			list_add(&current_rx_req->list, &dev->rx_reqs);
			spin_unlock_irqrestore(&dev->lock, flags);
			mutex_unlock(&dev->lock_scale_io);
			return -EAGAIN;
		}

		/* If we not returning all the data left in this RX request
		 * buffer then adjust the amount of data left in the buffer.
		 * Othewise if we are done with this RX request buffer then
		 * requeue it to get any incoming data from the USB host.
		 */
		if (size < current_rx_bytes) {
			current_rx_bytes -= size;
			current_rx_buf += size;
		} else {
			list_add(&current_rx_req->list, &dev->rx_reqs);
			current_rx_bytes = 0;
			current_rx_buf = NULL;
			current_rx_req = NULL;
		}
	}

	dev->current_rx_req = current_rx_req;
	dev->current_rx_bytes = current_rx_bytes;
	dev->current_rx_buf = current_rx_buf;

	spin_unlock_irqrestore(&dev->lock, flags);
	mutex_unlock(&dev->lock_scale_io);

	DBG(dev, "scale_read returned %d bytes\n", (int)bytes_copied);

	if (bytes_copied)
		return bytes_copied;
	else
		return -EAGAIN;
}


static ssize_t
scale_write(struct file *fd, const char __user *buf, size_t len, loff_t *ptr)
{
	struct scale_dev	*dev = fd->private_data;
	unsigned long		flags;
	size_t			size;	/* Amount of data in a TX request. */
	size_t			bytes_copied = 0;
	struct usb_request	*req;

	if (dev->interface < 0)
	{
		ERROR(dev,"USB is not connected!\n");
		return -EINVAL;
	} 
	DBG(dev,"scale_write trying to send %d bytes\n", (int)len);

	if (len == 0)
		return -EINVAL;

	mutex_lock(&dev->lock_scale_io);
	spin_lock_irqsave(&dev->lock, flags);

	/* Check if a zebra_mouse reset happens while we have interrupts on */
	dev->reset_scale = 0;


	/* Check if there is any available write buffers */
	if (likely(list_empty(&dev->tx_reqs))) {
		/* Turn interrupts back on before sleeping. */
		spin_unlock_irqrestore(&dev->lock, flags);

		/*
		 * If write buffers are available check if this is
		 * a NON-Blocking call or not.
		 */
		if (fd->f_flags & (O_NONBLOCK|O_NDELAY)) {
			mutex_lock(&dev->lock_scale_io);
			return -EAGAIN;
		}

		/* Sleep until a write buffer is available */
		wait_event_interruptible(dev->tx_wait,
				(likely(!list_empty(&dev->tx_reqs))));
		spin_lock_irqsave(&dev->lock, flags);
	}

	while (likely(!list_empty(&dev->tx_reqs)) && len) {										

		if (len > USB_BUFSIZE)
			size = USB_BUFSIZE;
		else
			size = len;

		req = container_of(dev->tx_reqs.next, struct usb_request,
				list);
		list_del_init(&req->list);							

		req->complete = tx_complete;
		req->length = size;

		DBG(dev,"maxpacket size:%d,%d\r\n",len,dev->in_ep->maxpacket);
		/* Check if we need to send a zero length packet. */
		if (len > size)
			/* They will be more TX requests so no yet. */
			req->zero = 0;
		else
			/* If the data amount is not a multple of the
			 * maxpacket size then send a zero length packet.
			 */
			req->zero = ((len % dev->in_ep->maxpacket) == 0);

		/* Don't leave irqs off while doing memory copies */
		spin_unlock_irqrestore(&dev->lock, flags);

		if (copy_from_user(req->buf, buf, size)) {
			list_add(&req->list, &dev->tx_reqs);
			mutex_unlock(&dev->lock_scale_io);
			return bytes_copied;
		}

		bytes_copied += size;
		len -= size;
		buf += size;

		spin_lock_irqsave(&dev->lock, flags);

		/* We've disconnected or reset so free the req and buffer */
		if (dev->reset_scale) {
			list_add(&req->list, &dev->tx_reqs);
			spin_unlock_irqrestore(&dev->lock, flags);
			mutex_unlock(&dev->lock_scale_io);
			return -EAGAIN;
		}

		if (usb_ep_queue(dev->in_ep, req, GFP_ATOMIC)) {							
			list_add(&req->list, &dev->tx_reqs);
			spin_unlock_irqrestore(&dev->lock, flags);
			mutex_unlock(&dev->lock_scale_io);
			return -EAGAIN;
		}

		list_add(&req->list, &dev->tx_reqs_active);
	}


	spin_unlock_irqrestore(&dev->lock, flags);
	mutex_unlock(&dev->lock_scale_io);

	DBG(dev, "scale_write sent %d bytes\n", (int)bytes_copied);

	if (bytes_copied) {
		return bytes_copied;
	} else {
		return -EAGAIN;
	}
	
}

static int
scale_fsync(struct file *fd, struct dentry *dentry, int datasync)
{
	struct scale_dev	*dev = fd->private_data;
	unsigned long		flags;
	int			tx_list_empty;
	
	spin_lock_irqsave(&dev->lock, flags);
	tx_list_empty = (likely(list_empty(&dev->tx_reqs)));
	spin_unlock_irqrestore(&dev->lock, flags);

	if (!tx_list_empty) {
		/* Sleep until all data has been sent */
		printk("Sleep until all data has been sent!\n");
		wait_event_interruptible(dev->tx_flush_wait,
				(likely(list_empty(&dev->tx_reqs_active))));
	}
	
	return 0;
}

static unsigned int
scale_poll(struct file *fd, poll_table *wait)
{
	struct scale_dev	*dev = fd->private_data;
	unsigned long		flags;
	int			status = 0;


	mutex_lock(&dev->lock_scale_io);
	spin_lock_irqsave(&dev->lock, flags);
	setup_rx_reqs(dev);
	spin_unlock_irqrestore(&dev->lock, flags);
	mutex_unlock(&dev->lock_scale_io);

	poll_wait(fd, &dev->rx_wait, wait);
	poll_wait(fd, &dev->tx_wait, wait);

	spin_lock_irqsave(&dev->lock, flags);
	if (likely(!list_empty(&dev->tx_reqs)))
		status |= POLLOUT | POLLWRNORM;

	if (likely(dev->current_rx_bytes) ||
			likely(!list_empty(&dev->rx_buffers)))
		status |= POLLIN | POLLRDNORM;

	spin_unlock_irqrestore(&dev->lock, flags);

	return status;
}

static long
scale_ioctl(struct file *fd, unsigned int code, unsigned int *arg)
{
	struct scale_dev	*dev = fd->private_data;
	unsigned long		flags;
	unsigned int			status = 0;
	u8                      is_connected;

	DBG(dev, "scale_ioctl: cmd=0x%4.4x, arg=%d\n", code, *arg);

	/* handle ioctls */

	spin_lock_irqsave(&dev->lock, flags);

	switch (code) {
	case GET_SCALE_USB_STATUS:
		if(dev->interface >= 0)
			is_connected = 1;
		else
			is_connected = 0;	
		status = (int)dev->scale_status;
		status += (is_connected<<8);
		*arg = status;
		break;
	case SET_SCALE_USB_STATUS:
		dev->scale_status = (u8)*arg;
		break;
	default:
		/* could not handle ioctl */
		DBG(dev, "scale_ioctl: ERROR cmd=0x%4.4xis not supported\n",
				code);
		status = -ENOTTY;
	}

	spin_unlock_irqrestore(&dev->lock, flags);

	return status;
}


/* used after endpoint configuration */
static struct file_operations scale_io_operations = {
	.owner =	THIS_MODULE,
	.open =		scale_open,
	.read =		scale_read,
	.write =	scale_write,
	.fsync =	scale_fsync,
	.poll =		scale_poll,
	.unlocked_ioctl = scale_ioctl,
	.release =	scale_close
};

/*-------------------------------------------------------------------------*/

static int
set_scale_interface(struct scale_dev *dev)
{
	int			result = 0;
	
	dev->in = ep_desc(dev->gadget, &hs_ep_in_desc, &fs_ep_in_desc);
	dev->in_ep->driver_data = dev;
	
	dev->out = ep_desc(dev->gadget, &hs_ep_out_desc, &fs_ep_out_desc);
	dev->out_ep->driver_data = dev;
	
	result = usb_ep_enable(dev->in_ep, dev->in);
	if (result != 0) {
		DBG(dev,"enable %s --> %d\n", dev->in_ep->name, result);
		goto done;
	}
	
	result = usb_ep_enable(dev->out_ep, dev->out);
	if (result != 0) {
		DBG(dev,"enable %s --> %d\n", dev->in_ep->name, result);
		goto done;
	}

done:
	/* on error, disable any endpoints  */
	if (result != 0) {
		(void) usb_ep_disable(dev->in_ep);
		(void) usb_ep_disable(dev->out_ep);
		dev->in = NULL;
		dev->out = NULL;	
	}
	
	
	/* caller is responsible for cleanup on error */
	return result;
}

static void scale_reset_interface(struct scale_dev *dev)
{
	if (dev->interface < 0)
		return;

	DBG(dev, "%s\n", __func__);

	if (dev->in)
		usb_ep_disable(dev->in_ep);	

	if (dev->out)
		usb_ep_disable(dev->out_ep);

	dev->interface = -1;
	
}

/* change our operational config.  must agree with the code
 * that returns config descriptors, and altsetting code.
 */
static int
scale_set_config(struct scale_dev *dev, unsigned number)
{
	int			result = 0;
	struct usb_gadget	*gadget = dev->gadget;

	switch (number) {
	case DEV_CONFIG_VALUE:
		result = 0;
		break;
	default:
		result = -EINVAL;
		/* FALL THROUGH */
	case 0:
		break;
	}

	if (result) {
		usb_gadget_vbus_draw(dev->gadget,
				dev->gadget->is_otg ? 8 : 100);
	} else {
		char *speed;
		unsigned power;

		power = 2 * config_desc1.bMaxPower;
		usb_gadget_vbus_draw(dev->gadget, power);

		switch (gadget->speed) {
		case USB_SPEED_FULL:	speed = "full"; break;
#ifdef CONFIG_USB_GADGET_DUALSPEED
		case USB_SPEED_HIGH:	speed = "high"; break;
#endif
		default:		speed = "?"; break;
		}

		dev->config = number;
		INFO(dev, "%s speed config #%d: %d mA, %s\n",
				speed, number, power, driver_desc);
	}
	
	set_scale_interface(dev);
	
	
	return result;
}

static int
config_buf1(enum usb_device_speed speed, u8 *buf, u8 type, unsigned index,
		int is_otg)
{
	int					len;
	const struct usb_descriptor_header	**function;
#ifdef CONFIG_USB_GADGET_DUALSPEED
	int					hs = (speed == USB_SPEED_HIGH);
	
	
	if (type == USB_DT_OTHER_SPEED_CONFIG)
		hs = !hs;

	if (hs) {
		function = hs_scale_function;
	} else {
		function = fs_scale_function;
	}
#else
	function = fs_scale_function;
#endif

	if (index >= device_desc.bNumConfigurations)
		return -EINVAL;

	/* for now, don't advertise srp-only devices */
	if (!is_otg)
		function++;

//int usb_gadget_config_buf(const struct usb_config_descriptor *config,
//	void *buf, unsigned buflen, const struct usb_descriptor_header **desc);

	len = usb_gadget_config_buf(&config_desc1, buf, USB_DESC_BUFSIZE,
			function);
	if (len < 0)
		return len;
	((struct usb_config_descriptor *) buf)->bDescriptorType = type;
	
	
	return len;
}

/* Change our operational Interface. */
static int
set_interface(struct scale_dev *dev, unsigned number)
{
	int			result = 0;

	/* Free the current interface */
	switch (dev->interface) {
	case SCALE_INTERFACE:
		scale_reset_interface(dev);
		break;
	}

	switch (number) {
	case SCALE_INTERFACE:
		result = set_scale_interface(dev);
		if (result) {
			scale_reset_interface(dev);
		} else {
			dev->interface = SCALE_INTERFACE;
		}
		break;
	default:
		result = -EINVAL;
		/* FALL THROUGH */
	}

	if (!result)
		INFO(dev, "Using interface %x\n", number);
		

	return result;
}

static void scale_setup_complete(struct usb_ep *ep, struct usb_request *req)
{
	
	if (req->status || req->actual != req->length)
		DBG((struct scale_dev *) ep->driver_data,
				"setup complete --> %d, %d/%d\n",
				req->status, req->actual, req->length);
							
}

static void scale_soft_reset(struct scale_dev *dev)
{
	struct usb_request	*req;
	
	INFO(dev, "Received scale Reset Request\n");

	if (usb_ep_disable(dev->in_ep))
		DBG(dev, "Failed to disable USB in_ep\n");
	if (usb_ep_disable(dev->out_ep))
		DBG(dev, "Failed to disable USB out_ep\n");

	if (dev->current_rx_req != NULL) {
		list_add(&dev->current_rx_req->list, &dev->rx_reqs);
		dev->current_rx_req = NULL;
	}
	dev->current_rx_bytes = 0;
	dev->current_rx_buf = NULL;
	dev->reset_scale = 1;

	while (likely(!(list_empty(&dev->tx_reqs_active)))) {
		req = container_of(dev->tx_reqs_active.next,
				struct usb_request, list);
		list_del_init(&req->list);
		list_add(&req->list, &dev->tx_reqs);
	}
	
	while (likely(!(list_empty(&dev->rx_reqs_active)))) {
		req = container_of(dev->rx_buffers.next, struct usb_request,
				list);
		list_del_init(&req->list);
		list_add(&req->list, &dev->rx_reqs);
	}

	while (likely(!(list_empty(&dev->tx_reqs_active)))) {
		req = container_of(dev->tx_reqs_active.next,
				struct usb_request, list);
		list_del_init(&req->list);
		list_add(&req->list, &dev->tx_reqs);
	}
	
	if (usb_ep_enable(dev->in_ep, dev->in))
		DBG(dev, "Failed to enable USB in_ep\n");
	if (usb_ep_enable(dev->out_ep, dev->out))
		DBG(dev, "Failed to enable USB out_ep\n");

	wake_up_interruptible(&dev->rx_wait);
	wake_up_interruptible(&dev->tx_wait);
	wake_up_interruptible(&dev->tx_flush_wait);
	
}

/*-------------------------------------------------------------------------*/

/*
 * The setup() callback implements all the ep0 functionality that's not
 * handled lower down.
 */
static int
scale_setup(struct usb_gadget *gadget, const struct usb_ctrlrequest *ctrl)
{

	struct scale_dev	*dev = get_gadget_data(gadget);
	struct usb_request	*req = dev->req;
	int			value = -EOPNOTSUPP;
	u16			wIndex = le16_to_cpu(ctrl->wIndex);
	u16			wValue = le16_to_cpu(ctrl->wValue);
	u16			wLength = le16_to_cpu(ctrl->wLength);
	

	DBG(dev, "ctrl req%02x.%02x v%04x i%04x l%d\n",
		ctrl->bRequestType, ctrl->bRequest, wValue, wIndex, wLength);
//  printk("ctrl req%02x.%02x v%04x i%04x l%04x\n",
//		ctrl->bRequestType, ctrl->bRequest, wValue, wIndex, wLength);
		
	req->complete = scale_setup_complete;

	switch (ctrl->bRequestType&USB_TYPE_MASK) {

	case USB_TYPE_STANDARD:
		DBG(dev,"USB_TYPE_STANDARD\n");
		switch (ctrl->bRequest) {
		case USB_REQ_GET_DESCRIPTOR:
			DBG(dev,"USB_REQ_GET_DESCRIPTOR\n");
			if (ctrl->bRequestType != USB_DIR_IN)
				{
					value = min(wLength, (u16) sizeof report_desc);
					DBG(dev,"ctrl->bRequestType != USB_DIR_IN\n");
					memcpy(req->buf, &report_desc, (u16)sizeof report_desc);
					break;
				}
			switch (wValue >> 8) {
	
			case USB_DT_DEVICE:
				DBG(dev,"USB_DT_DEVICE\n");
				value = min(wLength, (u16) sizeof device_desc);
				DBG(dev,"wLength %d,sizeof(device_desc) %d,value %d\n",wLength,(u16) sizeof device_desc,value);
				memcpy(req->buf, &device_desc, value);
				break;

			case USB_DT_CONFIG:
				DBG(dev,"USB_DT_CONFIG,wValue is %d\n",wValue);
				value = config_buf1(gadget->speed, req->buf,
						wValue >> 8,
						wValue & 0xff,
						gadget->is_otg);
				if (value >= 0)
					value = min(wLength, (u16) value);
				DBG(dev,"value %d\n",value);
				break;

			case USB_DT_STRING:
				DBG(dev,"USB_DT_STRING,%d\n",wValue);
				value = usb_gadget_get_string(&stringtab,
						wValue & 0xff, req->buf);
				if (value >= 0)
					value = min(wLength, (u16) value);
				DBG(dev,"value %d\n",value);
				break;
			}
			break;
		case USB_REQ_SET_CONFIGURATION:
			DBG(dev,"USB_REQ_SET_CONFIGURATION\n");
			if (ctrl->bRequestType != 0)
				break;
			if (gadget->a_hnp_support)
				DBG(dev, "HNP available\n");
			else if (gadget->a_alt_hnp_support)
				DBG(dev, "HNP needs a different root port\n");
			value = scale_set_config(dev, wValue);
			break;
		case USB_REQ_GET_CONFIGURATION:
			DBG(dev,"USB_REQ_GET_CONFIGURATION\n");
			if (ctrl->bRequestType != USB_DIR_IN)
				break;
			*(u8 *)req->buf = dev->config;
			value = min(wLength, (u16) 1);
			break;

		case USB_REQ_SET_INTERFACE:
			DBG(dev,"USB_REQ_SET_INTERFACE\n");
			if (ctrl->bRequestType != USB_RECIP_INTERFACE ||
					!dev->config)
				break;

			value = set_interface(dev, SCALE_INTERFACE);
			break;
		case USB_REQ_GET_INTERFACE:
			DBG(dev,"USB_REQ_GET_INTERFACE\n");
			if (ctrl->bRequestType !=
					(USB_DIR_IN|USB_RECIP_INTERFACE)
					|| !dev->config)
				break;

			*(u8 *)req->buf = dev->interface;
			value = min(wLength, (u16) 1);
			break;

		default:
			DBG(dev,"unknown ---------------\n");
			goto unknown;
		}
		break;

	case USB_TYPE_CLASS:
		DBG(dev,"USB_TYPE_CLASS\n");
		switch (ctrl->bRequest) {
		case 0: /* Get the IEEE-1284 PNP String */
			/* Only one zebra_mouse interface is supported. */
			DBG(dev,"USB_TYPE_CLASS 0 ---------------\n");
			if ((wIndex>>8) != SCALE_INTERFACE)
				break;

			value = (pnp_string[0]<<8)|pnp_string[1];
			memcpy(req->buf, pnp_string, value);
			DBG(dev, "1284 PNP String: %x %s\n", value,
					&pnp_string[2]);
			break;

		case 1: /* Get Port Status */
			/* Only one zebra_mouse interface is supported. */
			DBG(dev,"USB_TYPE_CLASS 1 ---------------\n");				
			if (wIndex != SCALE_INTERFACE)
				break;

			*(u8 *)req->buf = dev->scale_status;
			value = min(wLength, (u16) 1);
			break;

		case 2: /* Soft Reset */
			/* Only one zebra_mouse interface is supported. */
			DBG(dev,"USB_TYPE_CLASS 2 ---------------\n");
			if (wIndex != SCALE_INTERFACE)
				break;

			scale_soft_reset(dev);

			value = 0;
			break;

		default:
			goto unknown;
		}
		break;

	default:
unknown:
		VDBG(dev,
			"unknown ctrl req%02x.%02x v%04x i%04x l%d\n",
			ctrl->bRequestType, ctrl->bRequest,
			wValue, wIndex, wLength);
		break;
	}

	/* respond with data transfer before status phase? */
	if (value >= 0) {
		req->length = value;
		req->zero = value < wLength;
		value = usb_ep_queue(gadget->ep0, req, GFP_ATOMIC);
		if (value < 0) {
			DBG(dev, "ep_queue --> %d\n", value);
			req->status = 0;
			scale_setup_complete(gadget->ep0, req);
		}
	}
	
	DBG(dev,"scale_setup end---------------\n");
	////is_connect = 1;
	dev->interface = SCALE_INTERFACE;
	/* host either stalls (value < 0) or reports success */
	return value;
}

static void
scale_disconnect(struct usb_gadget *gadget)
{
	struct scale_dev	*dev = get_gadget_data(gadget);
	unsigned long		flags;

	DBG(dev, "%s\n", __func__);

	spin_lock_irqsave(&dev->lock, flags);

	scale_reset_interface(dev);
	////is_connect = 0;
	spin_unlock_irqrestore(&dev->lock, flags);
}

static void
scale_unbind(struct usb_gadget *gadget)
{
	struct scale_dev	*dev = get_gadget_data(gadget);
	struct usb_request	*req;

	DBG(dev, "%s\n", __func__);

	/* Remove sysfs files */
	device_destroy(usb_gadget_class, scale_devno);

	/* Remove Character Device */
	cdev_del(&dev->scale_cdev);

	/* we must already have been disconnected ... no i/o may be active */
	WARN_ON(!list_empty(&dev->tx_reqs_active));
	WARN_ON(!list_empty(&dev->rx_reqs_active));

	/* Free all memory for this driver. */
	while (!list_empty(&dev->tx_reqs)) {
		req = container_of(dev->tx_reqs.next, struct usb_request,
				list);
		list_del(&req->list);
		scale_req_free(dev->in_ep, req);
	}

	if (dev->req) {
		scale_req_free(gadget->ep0, dev->req);
		dev->req = NULL;
	}
	
	
	if (dev->current_rx_req != NULL)
		scale_req_free(dev->out_ep, dev->current_rx_req);

	while (!list_empty(&dev->rx_reqs)) {
		req = container_of(dev->rx_reqs.next,
				struct usb_request, list);
		list_del(&req->list);
		scale_req_free(dev->out_ep, req);
	}

	while (!list_empty(&dev->rx_buffers)) {
		req = container_of(dev->rx_buffers.next,
				struct usb_request, list);
		list_del(&req->list);
		scale_req_free(dev->out_ep, req);
	}

	set_gadget_data(gadget, NULL);
	DBG(dev, "%s end\n", __func__);
}

static int __init
scale_bind(struct usb_gadget *gadget)
{
	struct scale_dev *dev;
	struct usb_ep    *in_ep, *out_ep;
	int		 status = -ENOMEM;
	int		 gcnum;
	size_t		 len;
	u32		 i;
	struct usb_request	*req;

	dev = &scale;

	DBG(dev, "%s\n", __func__);

	/* Setup the sysfs files for the zebra_mouse gadget. */
	dev->pdev = device_create(usb_gadget_class, NULL, scale_devno,NULL,shortname);		  
	if (IS_ERR(dev->pdev)) {
		ERROR(dev, "Failed to create device: scale\n");
		goto fail;
	}

	/*
	 * Register a character device as an interface to a user mode
	 * program that handles the zebra_mouse specific functionality.
	 */
	cdev_init(&dev->scale_cdev, &scale_io_operations);
	dev->scale_cdev.owner = THIS_MODULE;
	status = cdev_add(&dev->scale_cdev, scale_devno, 1);
	if (status) {
		ERROR(dev, "Failed to open char device\n");
		goto fail;
	}

	gcnum = usb_gadget_controller_number(gadget);
	if (gcnum >= 0) {
		device_desc.bcdDevice = cpu_to_le16(0x0200 + gcnum);
	} else {
		dev_warn(&gadget->dev, "controller '%s' not recognized\n",
			gadget->name);
		/* unrecognized, but safe unless bulk is REALLY quirky */
		device_desc.bcdDevice =
			cpu_to_le16(0xFFFF);
	}
	snprintf(manufacturer, sizeof(manufacturer), "%s %s with %s",
		init_utsname()->sysname, init_utsname()->release,
		gadget->name);

	device_desc.idVendor =
		cpu_to_le16(PRINTER_VENDOR_NUM);
	device_desc.idProduct =
		cpu_to_le16(PRINTER_PRODUCT_NUM);

	len = strlen(pnp_string);
	pnp_string[0] = (len >> 8) & 0xFF;
	pnp_string[1] = len & 0xFF;

	/* all we really need is bulk IN/OUT */
	usb_ep_autoconfig_reset(gadget);
	in_ep = usb_ep_autoconfig(gadget, &fs_ep_in_desc);
	if (!in_ep) {
autoconf_fail:
		dev_err(&gadget->dev, "can't autoconfigure on %s\n",
			gadget->name);
		return -ENODEV;
	}
	in_ep->driver_data = in_ep;	/* claim */
	
	out_ep = usb_ep_autoconfig(gadget, &fs_ep_out_desc);
	if (!out_ep)
		goto autoconf_fail;
	out_ep->driver_data = out_ep;	/* claim */

#ifdef	CONFIG_USB_GADGET_DUALSPEED
	/* assumes ep0 uses the same value for both speeds ... */
	dev_qualifier.bMaxPacketSize0 = device_desc.bMaxPacketSize0;

	/* and that all endpoints are dual-speed */
	hs_ep_in_desc.bEndpointAddress = fs_ep_in_desc.bEndpointAddress;
	hs_ep_out_desc.bEndpointAddress = fs_ep_out_desc.bEndpointAddress;
#endif	/* DUALSPEED */

	device_desc.bMaxPacketSize0 = gadget->ep0->maxpacket;
	usb_gadget_set_selfpowered(gadget);

	//if (gadget->is_otg) {
	//	otg_desc.bmAttributes |= USB_OTG_HNP,
	//	config_desc.bmAttributes |= USB_CONFIG_ATT_WAKEUP;
	//}

	spin_lock_init(&dev->lock);
	mutex_init(&dev->lock_scale_io);
	INIT_LIST_HEAD(&dev->tx_reqs);
	INIT_LIST_HEAD(&dev->tx_reqs_active);
	INIT_LIST_HEAD(&dev->rx_reqs);
	INIT_LIST_HEAD(&dev->rx_reqs_active);
	INIT_LIST_HEAD(&dev->rx_buffers);
	init_waitqueue_head(&dev->rx_wait);
	init_waitqueue_head(&dev->tx_wait);
	init_waitqueue_head(&dev->tx_flush_wait);

	dev->config = 0;
	dev->interface = -1;
	dev->scale_cdev_open = 0;
	dev->scale_status = SCALE_USB_NOT_ERROR;
	dev->current_rx_req = NULL;
	dev->current_rx_bytes = 0;
	dev->current_rx_buf = NULL;

	dev->in_ep = in_ep;
	dev->out_ep = out_ep;
	
	/* preallocate control message data and buffer */
	dev->req = scale_req_alloc(gadget->ep0, USB_DESC_BUFSIZE,
			GFP_KERNEL);
	if (!dev->req) {
		status = -ENOMEM;
		goto fail;
	}

	for (i = 0; i < QLEN; i++) {
		req = scale_req_alloc(dev->in_ep, USB_BUFSIZE, GFP_KERNEL);
		if (!req) {
			while (!list_empty(&dev->tx_reqs)) {
				req = container_of(dev->tx_reqs.next,
						struct usb_request, list);
				list_del(&req->list);
				scale_req_free(dev->in_ep, req);
			}
			return -ENOMEM;
		}
		list_add(&req->list, &dev->tx_reqs);
	}
	
	for (i = 0; i < QLEN; i++) {
		req = scale_req_alloc(dev->out_ep, USB_BUFSIZE, GFP_KERNEL);
		if (!req) {
			while (!list_empty(&dev->rx_reqs)) {
				req = container_of(dev->rx_reqs.next,
						struct usb_request, list);
				list_del(&req->list);
				scale_req_free(dev->out_ep, req);
			}
			return -ENOMEM;
		}
		list_add(&req->list, &dev->rx_reqs);
	}
	
	dev->req->complete = scale_setup_complete;

	/* finish hookup to lower layer ... */
	dev->gadget = gadget;
	set_gadget_data(gadget, dev);
	gadget->ep0->driver_data = dev;

	INFO(dev, "%s, version: " DRIVER_VERSION "\n", driver_desc);
	INFO(dev, "using %s, IN %s OUT: %s \n", gadget->name, in_ep->name, out_ep->name);
	
	return 0;

fail:
	scale_unbind(gadget);
	return status;
}

/*-------------------------------------------------------------------------*/

static struct usb_gadget_driver scale_driver = {
	.speed		= DEVSPEED,

	.function	= (char *) driver_desc,
	//.bind		= scale_bind,
	.unbind		= scale_unbind,

	.setup		= scale_setup,
	.disconnect	= scale_disconnect,

	.driver		= {
		.name		= (char *) shortname,
		.owner		= THIS_MODULE,
	},
};

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_AUTHOR("Touch Down");
MODULE_LICENSE("GPL");

static int __init
init(void)
{
	int status;
	printk("[scale usb]:==>%s\n", __func__);
	usb_gadget_class = class_create(THIS_MODULE, shortname);
	if (IS_ERR(usb_gadget_class)) {
		status = PTR_ERR(usb_gadget_class);
		ERROR(dev, "unable to create scale class %d\n", status);
		return status;
	}

	status = alloc_chrdev_region(&scale_devno, 0, 1,
			shortname);
	if (status) {
		ERROR(dev, "alloc_chrdev_region %d\n", status);
		class_destroy(usb_gadget_class);
		return status;
	}
#ifdef NEW_STYLE    
    usb_composite_probe(&scale_driver, scale_bind);
    status = 0;
#else
	status = usb_gadget_register_driver(&scale_driver);
#endif
	if (status) {
		class_destroy(usb_gadget_class);
		unregister_chrdev_region(scale_devno, 1);
		DBG(dev, "scale_register_driver %x\n", status);
	}

	return status;
}
module_init(init);

static void __exit
cleanup(void)
{
	int status;

	mutex_lock(&scale.lock_scale_io);
	
	unregister_chrdev_region(scale_devno, 2);

	status = usb_gadget_unregister_driver(&scale_driver);
	if (status)
		ERROR(dev, "scale_unregister_driver %x\n", status);
	class_destroy(usb_gadget_class);
	mutex_unlock(&scale.lock_scale_io);
}
module_exit(cleanup);

