// SPDX-License-Identifier: GPL-2.0-only
/*
 * UART driver for LSTP USB interface.
 *
 * Copyright (c) 2026, NVIDIA CORPORATION.  All rights reserved.
 */

#include "lstp-main.h"

#include <linux/delay.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_buffer.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>

enum lstp_uart_cmd {
	LSTP_UART_CMD_WRITE = 0x00,
};

/*
 * NACK retry policy for UART TX writes.
 *
 * At 115200 baud, transmitting 500 bytes takes ~43 ms.
 * 5 retries x 10 ms = 50 ms total retry window, covering one full
 * worst-case drain cycle.
 */
#define LSTP_UART_NAK_MAX_RETRIES 5
#define LSTP_UART_NAK_RETRY_DELAY_MS 10

/* This means we are limited to 256 UARTs across all attached LSTP devices. */
#define LSTP_UART_MINORS 256

/*
 * Per-channel UART context.
 *
 * TX write path uses a two-stage coalescing architecture:
 *
 *   write() --> [ coal_buf ] --drain--> [ tx_pkt ] --USB--> device
 *               (stage 1)               (stage 2)
 *
 * Stage 1: write() appends to coal_buf under write_lock and schedules
 *          write_work for immediate execution. No timer is used. The
 *          USB round-trip time acts as the natural coalescing window.
 * Stage 2: The work function drains coal_buf into the LSTP TX packet
 *          and sends it over USB with NACK retry. On completion, if
 *          coal_buf has new data, it reschedules immediately.
 */
struct lstp_uart_ctx {
	struct tty_port port;
	struct lstp_channel *ch; /* Backpointer to parent channel */
	int minor;

	/* All fields below protected by write_lock */
	spinlock_t write_lock;
	u8 *coal_buf; /* Coalescing buffer, max_payload bytes */
	size_t coal_len; /* Bytes currently buffered */
	bool tx_in_flight; /* TX packet currently on the wire */
	bool disconnected; /* Set once during cleanup, never cleared */
	struct work_struct write_work;
};

static struct tty_driver *lstp_tty_driver;

static DEFINE_IDR(lstp_minors);
static DEFINE_MUTEX(lstp_minors_lock);

/*****************************************************************************
 * Minor number management
 *****************************************************************************/

/* Allocate the next available minor number for a new UART channel. */
static int lstp_alloc_minor(struct lstp_channel *ch)
{
	int ret;

	mutex_lock(&lstp_minors_lock);
	ret = idr_alloc(&lstp_minors, ch, 0, LSTP_UART_MINORS, GFP_KERNEL);
	mutex_unlock(&lstp_minors_lock);

	return ret;
}

/*
 * Look up an active LSTP channel by its TTY minor number.
 * Returns NULL if the minor is unregistered or the channel has been
 * disconnected, preventing new opens on a device mid-teardown.
 */
static struct lstp_channel *lstp_get_by_minor(unsigned int minor)
{
	struct lstp_channel *ch;

	mutex_lock(&lstp_minors_lock);
	ch = idr_find(&lstp_minors, minor);
	if (ch) {
		struct lstp_uart_ctx *ctx = ch->priv;

		if (!ctx || READ_ONCE(ctx->disconnected))
			ch = NULL;
	}
	mutex_unlock(&lstp_minors_lock);

	return ch;
}

/*****************************************************************************
 * TX path
 *****************************************************************************/

/*
 * Send the TX packet and retry on NACK (ENXIO).
 *
 * The firmware NACKs when its UART TX FIFO is full. lstp_recv_resp_helper
 * maps LSTP_NACK to -ENXIO; any other result is returned immediately.
 * The resp_buf lock is released on success so the next transaction can proceed.
 */
static int lstp_uart_send_with_retry(struct lstp_uart_ctx *ctx, size_t len)
{
	struct lstp_channel *ch = ctx->ch;
	int retries = 0;
	int ret;

	do {
		if (READ_ONCE(ctx->disconnected))
			return -ENODEV;

		ret = lstp_recv_resp_helper(ch, LSTP_UART_CMD_WRITE, len, 0);
		if (!ret)
			lstp_unlock_resp_buffer(ch);

		if (ret != -ENXIO)
			break;

		if (++retries >= LSTP_UART_NAK_MAX_RETRIES) {
			dev_warn(&ch->usb->intf->dev,
				 "OBMF UART channel %d NAK retry limit reached (%d)\n",
				 ch->ch_id, retries);
			break;
		}

		msleep(LSTP_UART_NAK_RETRY_DELAY_MS);
	} while (true);

	if (ret)
		dev_warn(&ch->usb->intf->dev, "OBMF UART channel %d write failed (%d)\n",
			 ch->ch_id, ret);

	return ret;
}

/*
 * Workqueue handler: drain coal_buf into a single LSTP WRITE command,
 * send it over USB, and reschedule if new data arrived during the
 * round-trip.
 */
static void lstp_uart_write_work(struct work_struct *work)
{
	struct lstp_uart_ctx *ctx = container_of(work, struct lstp_uart_ctx, write_work);
	struct lstp_packet *tx_pkt = (struct lstp_packet *)ctx->ch->tx_buf;
	unsigned long flags;
	size_t len;

	/* Drain coalescing buffer into TX packet. */
	spin_lock_irqsave(&ctx->write_lock, flags);
	len = ctx->coal_len;
	if (len == 0) {
		spin_unlock_irqrestore(&ctx->write_lock, flags);
		return;
	}

	tx_pkt->hdr.ch_id = ctx->ch->ch_id;
	tx_pkt->hdr.cmd = SET_U8_BYTE(LSTP_UART_CMD_WRITE, 0);
	memcpy(tx_pkt->payload, ctx->coal_buf, len);
	tx_pkt->hdr.length = cpu_to_le16(len);
	ctx->coal_len = 0;
	ctx->tx_in_flight = true;
	spin_unlock_irqrestore(&ctx->write_lock, flags);

	/* coal_buf is now empty, wake TTY so write() can refill it. */
	tty_port_tty_wakeup(&ctx->port);

	lstp_uart_send_with_retry(ctx, len);

	/*
	 * Release in-flight state. If write() filled coal_buf during the
	 * USB round-trip, immediately schedule another drain cycle.
	 */
	spin_lock_irqsave(&ctx->write_lock, flags);
	ctx->tx_in_flight = false;
	if (ctx->coal_len > 0)
		schedule_work(&ctx->write_work);
	spin_unlock_irqrestore(&ctx->write_lock, flags);

	tty_port_tty_wakeup(&ctx->port);
}

/*****************************************************************************
 * RX path
 *****************************************************************************/

/*
 * Handle unsolicited RX data from the firmware (device -> host UART bytes).
 *
 * The firmware pushes received UART data as unsolicited LSTP packets.
 * We push it into the TTY flip buffer and ACK; if the flip buffer is
 * full we NACK, telling the firmware to hold off and retry.
 *
 * Runs in USB completion context, so use GFP_ATOMIC.
 */
static void lstp_uart_irq_callback(struct lstp_channel *ch)
{
	struct lstp_uart_ctx *ctx = ch->priv;
	struct lstp_packet *rx_pkt = (struct lstp_packet *)ch->irq_buf;
	struct lstp_packet *tx_pkt = (struct lstp_packet *)ch->tx_resp_buf;
	u16 rx_len = le16_to_cpu(rx_pkt->hdr.length);
	int room;
	int ret;

	/*
	 * No lock needed: lstp_usb_rx_callback holds irq_buffer_lock
	 * for the duration of this call and only resubmits the single
	 * RX URB after we return.
	 */
	room = tty_buffer_request_room(&ctx->port, rx_len);

	if (room < rx_len) {
		tx_pkt->hdr.status = SET_U8_BYTE(LSTP_NACK, 1);
	} else {
		tty_insert_flip_string(&ctx->port, rx_pkt->payload, rx_len);
		tty_flip_buffer_push(&ctx->port);
		tx_pkt->hdr.status = SET_U8_BYTE(LSTP_SUCCESS, 1);
	}

	tx_pkt->hdr.ch_id = ch->ch_id;
	tx_pkt->hdr.length = 0;

	usb_fill_bulk_urb(ch->bulk_tx_resp_urb, ch->usb->udev,
			  usb_sndbulkpipe(ch->usb->udev, ch->usb->bulk_out_ep), tx_pkt,
			  sizeof(struct lstp_header), lstp_usb_tx_callback, ch);
	ret = usb_submit_urb(ch->bulk_tx_resp_urb, GFP_ATOMIC);
	if (ret)
		dev_err(&ch->usb->intf->dev, "OBMF UART channel %d failed to submit TX URB (%d)\n",
			ch->ch_id, ret);
}

/*****************************************************************************
 * TTY port operations
 *****************************************************************************/

/*
 * Prevent N_TTY from splitting large write() syscalls into per-character
 * calls. Without this, one userspace write can degrade into many tiny
 * lstp_tty_write() calls.
 */
static int lstp_port_activate(struct tty_port *port, struct tty_struct *tty)
{
	set_bit(TTY_NO_WRITE_SPLIT, &tty->flags);
	return 0;
}

/* Drain any pending write work before the port is fully closed. */
static void lstp_port_shutdown(struct tty_port *port)
{
	struct lstp_uart_ctx *ctx = container_of(port, struct lstp_uart_ctx, port);

	cancel_work_sync(&ctx->write_work);
}

/*
 * Final tty_port destructor, called when the last port reference drops.
 * Guard on minor < LSTP_UART_MINORS to handle init failure before
 * lstp_alloc_minor() succeeds.
 */
static void lstp_port_destruct(struct tty_port *port)
{
	struct lstp_uart_ctx *ctx = container_of(port, struct lstp_uart_ctx, port);

	mutex_lock(&lstp_minors_lock);
	if (ctx->minor < LSTP_UART_MINORS)
		idr_remove(&lstp_minors, ctx->minor);
	mutex_unlock(&lstp_minors_lock);

	kfree(ctx->coal_buf);
	kfree(ctx);
}

static const struct tty_port_operations lstp_port_ops = {
	.activate = lstp_port_activate,
	.shutdown = lstp_port_shutdown,
	.destruct = lstp_port_destruct,
};

/*****************************************************************************
 * TTY operations
 *****************************************************************************/

/*
 * Acquire a tty_port reference for the lifetime of this TTY file.
 * Dropped in lstp_tty_cleanup() when the last fd closes.
 */
static int lstp_tty_install(struct tty_driver *driver, struct tty_struct *tty)
{
	struct lstp_channel *ch = lstp_get_by_minor(tty->index);
	struct lstp_uart_ctx *ctx;
	int ret;

	if (!ch)
		return -ENODEV;

	ctx = ch->priv;
	tty_port_get(&ctx->port);

	ret = tty_standard_install(driver, tty);
	if (ret) {
		tty_port_put(&ctx->port);
		return ret;
	}

	tty->driver_data = ch;
	return 0;
}

/* Delegate open/close/hangup to tty_port; triggers activate/shutdown. */
static int lstp_tty_open(struct tty_struct *tty, struct file *filp)
{
	return tty_port_open(tty->port, tty, filp);
}

static void lstp_tty_close(struct tty_struct *tty, struct file *filp)
{
	tty_port_close(tty->port, tty, filp);
}

static void lstp_tty_hangup(struct tty_struct *tty)
{
	tty_port_hangup(tty->port);
}

/* Drop the tty_port reference taken in lstp_tty_install(). */
static void lstp_tty_cleanup(struct tty_struct *tty)
{
	tty_port_put(tty->port);
}

/* Append userspace data to coal_buf and kick the write workqueue. */
static ssize_t lstp_tty_write(struct tty_struct *tty, const unsigned char *buf, size_t count)
{
	struct lstp_channel *ch = tty->driver_data;
	struct lstp_uart_ctx *ctx = ch->priv;
	size_t max_payload = ch->usb->bulk_tx_size - sizeof(struct lstp_header);
	unsigned long flags;
	size_t room;
	ssize_t ret;

	if (!count)
		return 0;

	spin_lock_irqsave(&ctx->write_lock, flags);

	if (ctx->disconnected) {
		ret = -EIO;
		goto out_unlock;
	}

	room = max_payload - ctx->coal_len;
	if (room == 0) {
		ret = 0;
		goto out_unlock;
	}

	ret = min_t(size_t, count, room);
	memcpy(ctx->coal_buf + ctx->coal_len, buf, ret);
	ctx->coal_len += ret;
	schedule_work(&ctx->write_work);

out_unlock:
	spin_unlock_irqrestore(&ctx->write_lock, flags);
	return ret;
}

/* Report free space in coal_buf, or 0 if disconnected. */
static unsigned int lstp_tty_write_room(struct tty_struct *tty)
{
	struct lstp_channel *ch = tty->driver_data;
	struct lstp_uart_ctx *ctx = ch->priv;
	size_t max_payload = ch->usb->bulk_tx_size - sizeof(struct lstp_header);
	unsigned long flags;
	size_t room;

	spin_lock_irqsave(&ctx->write_lock, flags);
	if (ctx->disconnected)
		room = 0;
	else
		room = max_payload - ctx->coal_len;
	spin_unlock_irqrestore(&ctx->write_lock, flags);

	return room;
}

/* Report total bytes not yet delivered: pending + in-flight. */
static unsigned int lstp_tty_chars_in_buffer(struct tty_struct *tty)
{
	struct lstp_channel *ch = tty->driver_data;
	struct lstp_uart_ctx *ctx = ch->priv;
	unsigned long flags;
	unsigned int count;

	spin_lock_irqsave(&ctx->write_lock, flags);
	count = ctx->coal_len;
	if (ctx->tx_in_flight)
		count += le16_to_cpu(((struct lstp_packet *)ch->tx_buf)->hdr.length);
	spin_unlock_irqrestore(&ctx->write_lock, flags);

	return count;
}

/*
 * Discard pending data in the coalescing buffer. Does not cancel an
 * in-flight TX, because that data is already on the wire.
 */
static void lstp_tty_flush_buffer(struct tty_struct *tty)
{
	struct lstp_channel *ch = tty->driver_data;
	struct lstp_uart_ctx *ctx = ch->priv;
	unsigned long flags;

	spin_lock_irqsave(&ctx->write_lock, flags);
	ctx->coal_len = 0;
	spin_unlock_irqrestore(&ctx->write_lock, flags);

	cancel_work(&ctx->write_work);
}

/*
 * The firmware exposes a fixed UART configuration; no baud or framing
 * changes are supported. Preserve the hardware-set bits so userspace
 * reads back the actual parameters.
 */
static void lstp_tty_set_termios(struct tty_struct *tty, const struct ktermios *old_termios)
{
	tty_termios_copy_hw(&tty->termios, old_termios);
}

static const struct tty_operations lstp_ops = {
	.install = lstp_tty_install,
	.open = lstp_tty_open,
	.close = lstp_tty_close,
	.cleanup = lstp_tty_cleanup,
	.hangup = lstp_tty_hangup,
	.write = lstp_tty_write,
	.write_room = lstp_tty_write_room,
	.chars_in_buffer = lstp_tty_chars_in_buffer,
	.flush_buffer = lstp_tty_flush_buffer,
	.set_termios = lstp_tty_set_termios,
};

/*****************************************************************************
 * Device lifecycle (per-channel init / cleanup)
 *****************************************************************************/

/*
 * Teardown for a single UART channel. Called via devm on USB disconnect.
 *
 * Ordering: mark disconnected first, drain in-flight work, hang up any
 * open TTY, then unregister. The final tty_port_put() triggers
 * lstp_port_destruct() when the last reference drops.
 */
static void lstp_uart_cleanup(void *data)
{
	struct lstp_channel *ch = data;
	struct lstp_uart_ctx *ctx = ch->priv;
	struct tty_struct *tty;
	unsigned long flags;

	spin_lock_irqsave(&ctx->write_lock, flags);
	ctx->disconnected = true;
	spin_unlock_irqrestore(&ctx->write_lock, flags);

	cancel_work_sync(&ctx->write_work);

	/* TODO: replace with tty_port_tty_vhangup() when available */
	tty = tty_port_tty_get(&ctx->port);
	if (tty) {
		tty_vhangup(tty);
		tty_kref_put(tty);
	}

	tty_unregister_device(lstp_tty_driver, ctx->minor);
	tty_port_put(&ctx->port);
}

/**
 * lstp_uart_init() - Initialize an LSTP UART channel.
 * @ch: LSTP channel to initialize as UART
 *
 * Context: Process context. Called during probe before RX URB is active.
 *
 * Return: 0 on success, negative errno on failure
 */
int lstp_uart_init(struct lstp_channel *ch)
{
	struct lstp_uart_ctx *ctx;
	size_t max_payload;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	tty_port_init(&ctx->port);
	ctx->port.ops = &lstp_port_ops;
	ctx->minor = LSTP_UART_MINORS; /* Invalid until lstp_alloc_minor() succeeds */
	spin_lock_init(&ctx->write_lock);

	max_payload = ch->usb->bulk_tx_size - sizeof(struct lstp_header);
	ctx->coal_buf = kzalloc(max_payload, GFP_KERNEL);
	if (!ctx->coal_buf) {
		ret = -ENOMEM;
		goto err_put_port;
	}

	ch->tx_resp_buf = devm_kzalloc(&ch->usb->intf->dev, ch->usb->bulk_rx_size, GFP_KERNEL);
	if (!ch->tx_resp_buf) {
		ret = -ENOMEM;
		goto err_put_port;
	}

	ch->resp_buf = devm_kzalloc(&ch->usb->intf->dev, ch->usb->bulk_rx_size, GFP_KERNEL);
	if (!ch->resp_buf) {
		ret = -ENOMEM;
		goto err_put_port;
	}

	ch->irq_buf = devm_kzalloc(&ch->usb->intf->dev, ch->usb->bulk_rx_size, GFP_KERNEL);
	if (!ch->irq_buf) {
		ret = -ENOMEM;
		goto err_put_port;
	}

	ret = lstp_alloc_minor(ch);
	if (ret < 0) {
		dev_err(&ch->usb->intf->dev, "OBMF UART channel %d failed to allocate minor (%d)\n",
			ch->ch_id, ret);
		goto err_put_port;
	}

	ctx->minor = ret;
	INIT_WORK(&ctx->write_work, lstp_uart_write_work);
	ctx->ch = ch;
	ch->priv = ctx;
	ch->irq_callback = lstp_uart_irq_callback;

	ret = devm_add_action_or_reset(&ch->usb->intf->dev, lstp_uart_cleanup, ch);
	if (ret)
		return ret;

	dev_info(&ch->usb->intf->dev, "OBMF UART channel %d registered\n", ch->ch_id);
	return 0;

err_put_port:
	tty_port_put(&ctx->port);
	return ret;
}

/**
 * lstp_uart_start() - Start an LSTP UART channel.
 * @ch: LSTP channel to start
 *
 * Context: Process context. Called during probe after RX URB is active.
 *
 * Return: 0 on success, negative errno on failure
 */
int lstp_uart_start(struct lstp_channel *ch)
{
	struct lstp_uart_ctx *ctx = ch->priv;
	struct device *tty_dev;

	tty_dev = tty_port_register_device(&ctx->port, lstp_tty_driver, ctx->minor,
					   &ch->usb->intf->dev);
	if (IS_ERR(tty_dev)) {
		dev_err(&ch->usb->intf->dev, "OBMF UART channel %d failed to register TTY (%ld)\n",
			ch->ch_id, PTR_ERR(tty_dev));
		return PTR_ERR(tty_dev);
	}

	ch->child_dev = tty_dev;

	dev_info(&ch->usb->intf->dev, "OBMF UART channel %d started as /dev/ttyOBMF%d\n",
		 ch->ch_id, ctx->minor);
	return 0;
}

/*****************************************************************************
 * Module-level TTY driver registration
 *****************************************************************************/

/*
 * Register the global ttyOBMF driver. Called once at module load;
 * individual channels register their devices later via lstp_uart_start().
 */
int __init lstp_uart_driver_init(void)
{
	int ret;

	lstp_tty_driver =
		tty_alloc_driver(LSTP_UART_MINORS, TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(lstp_tty_driver))
		return PTR_ERR(lstp_tty_driver);

	lstp_tty_driver->driver_name = "obmf";
	lstp_tty_driver->name = "ttyOBMF";
	lstp_tty_driver->major = 0;
	lstp_tty_driver->minor_start = 0;
	lstp_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	lstp_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	lstp_tty_driver->init_termios = tty_std_termios;
	lstp_tty_driver->init_termios.c_cflag = B115200 | CS8 | CREAD | HUPCL | CLOCAL;
	tty_set_operations(lstp_tty_driver, &lstp_ops);

	ret = tty_register_driver(lstp_tty_driver);
	if (ret) {
		tty_driver_kref_put(lstp_tty_driver);
		lstp_tty_driver = NULL;
		return ret;
	}

	return 0;
}

/* Unregister the global ttyOBMF driver and free minor number space. */
void lstp_uart_driver_exit(void)
{
	if (!lstp_tty_driver)
		return;

	tty_unregister_driver(lstp_tty_driver);
	tty_driver_kref_put(lstp_tty_driver);
	lstp_tty_driver = NULL;
	idr_destroy(&lstp_minors);
}
