// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPIO driver for LSTP USB interface.
 *
 * Copyright (c) 2026, NVIDIA CORPORATION.  All rights reserved.
 */

#include <linux/gpio/driver.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "lstp-main.h"

#define LSTP_GPIO_NAME_MAX_LEN 32U
#define LSTP_GPIO_MAX_CFGS_PER_READ 10U

enum lstp_gpio_cmd {
	LSTP_GPIO_CMD_GET_VALUE = 0x00,
	LSTP_GPIO_CMD_SET_VALUE = 0x01,
	LSTP_GPIO_CMD_GET_IRQ_CONFIG = 0x02,
	LSTP_GPIO_CMD_SET_IRQ_CONFIG = 0x03,
	LSTP_GPIO_CMD_IRQ_EVENT = 0x04,
};

enum lstp_gpio_direction {
	LSTP_GPIO_DIRECTION_OUTPUT = 0x00,
	LSTP_GPIO_DIRECTION_INPUT = 0x01,
};

enum lstp_gpio_state {
	LSTP_GPIO_STATE_LOW = 0x00,
	LSTP_GPIO_STATE_HIGH = 0x01,
};

static const char *lstp_gpio_cmd_name(u8 cmd)
{
	switch (cmd) {
	case LSTP_GPIO_CMD_GET_VALUE:
		return "get_value";
	case LSTP_GPIO_CMD_SET_VALUE:
		return "set_value";
	case LSTP_GPIO_CMD_GET_IRQ_CONFIG:
		return "get_irq_config";
	case LSTP_GPIO_CMD_SET_IRQ_CONFIG:
		return "set_irq_config";
	case LSTP_GPIO_CMD_IRQ_EVENT:
		return "irq_event";
	default:
		return "unknown";
	}
}

static const char *lstp_gpio_fw_direction_name(u8 direction)
{
	switch (direction) {
	case LSTP_GPIO_DIRECTION_OUTPUT:
		return "output";
	case LSTP_GPIO_DIRECTION_INPUT:
		return "input";
	default:
		return "invalid";
	}
}

struct lstp_gpio_channel_config {
	u8 channel_num_gpio;
} __packed;

struct lstp_gpio_line_config {
	u8 gpio_name[LSTP_GPIO_NAME_MAX_LEN];
	u8 direction;
	u8 default_output;
	u8 output_drive_config;
	u8 output_persist_state;
	u8 bias_pull_config;
	u16 bias_pull_strength;
	u16 output_drive_strength;
	u16 slew_rate;
	u16 input_debounce_time;
	u8 rsvd_0;
	u8 rsvd_1;
	u8 rsvd_2;
} __packed;

struct lstp_gpio_get_value_request {
	__le16 gpio_index;
} __packed;

struct lstp_gpio_get_value_response {
	u8 value;
} __packed;

struct lstp_gpio_set_value_request {
	__le16 gpio_index;
	u8 value;
} __packed;

struct lstp_gpio_priv {
	struct gpio_chip gc;
	struct lstp_channel *ch;
	const char **names;
	struct lstp_gpio_line_config *line_cfgs;
	unsigned int num_line_cfgs;
};

static void lstp_gpio_dump_line_configs(struct lstp_channel *ch,
					const struct lstp_gpio_line_config *line_cfgs,
					unsigned int num_line_cfgs)
{
	struct device *dev = &ch->usb->intf->dev;
	unsigned int dump_count = min_t(unsigned int, num_line_cfgs, 64U);
	unsigned int i;

	if (!line_cfgs || !num_line_cfgs)
		return;

	if (dump_count < num_line_cfgs)
		dev_dbg(dev, "%s: ch_%d: dumping first %u of %u GPIO line configs\n", __func__,
			ch->ch_id, dump_count, num_line_cfgs);

	for (i = 0; i < dump_count; i++) {
		char name[LSTP_GPIO_NAME_MAX_LEN + 1];
		size_t name_len;

		name_len = strnlen(line_cfgs[i].gpio_name, LSTP_GPIO_NAME_MAX_LEN);
		memcpy(name, line_cfgs[i].gpio_name, name_len);
		name[name_len] = '\0';

		dev_dbg(dev,
			"%s: ch_%d: line[%u] name=%s dir=%s(%u) default=%u persist=%u drive_cfg=%u bias_cfg=%u bias_strength=%u drive_strength=%u slew=%u debounce=%u\n",
			__func__, ch->ch_id, i, name[0] ? name : "<unnamed>",
			lstp_gpio_fw_direction_name(line_cfgs[i].direction), line_cfgs[i].direction,
			line_cfgs[i].default_output, line_cfgs[i].output_persist_state,
			line_cfgs[i].output_drive_config, line_cfgs[i].bias_pull_config,
			le16_to_cpu(line_cfgs[i].bias_pull_strength),
			le16_to_cpu(line_cfgs[i].output_drive_strength),
			le16_to_cpu(line_cfgs[i].slew_rate),
			le16_to_cpu(line_cfgs[i].input_debounce_time));
	}
}

/**
 * lstp_gpio_check_offset() - Validate a GPIO line offset.
 * @priv: GPIO channel private data
 * @offset: GPIO line offset within the chip
 *
 * Return: 0 on success, negative errno on failure
 */
static int lstp_gpio_check_offset(struct lstp_gpio_priv *priv, unsigned int offset)
{
	if (offset < priv->gc.ngpio)
		return 0;

	dev_err(&priv->ch->usb->intf->dev, "%s: ch_%d: Invalid GPIO offset %u (max %u)\n", __func__,
		priv->ch->ch_id, offset, priv->gc.ngpio - 1);
	return -EINVAL;
}

static bool lstp_gpio_has_line_cfg(const struct lstp_gpio_priv *priv, unsigned int offset)
{
	return offset < priv->num_line_cfgs;
}

/**
 * lstp_gpio_get_cached_direction() - Return the firmware-configured direction.
 * @priv: GPIO channel private data
 * @offset: GPIO line offset
 *
 * The obmf-demo firmware exposes fixed GPIO direction in ReadConfig and does
 * not provide a runtime direction command. If the firmware omitted per-line
 * metadata, default to output so gpioset remains usable.
 *
 * Return: GPIO_LINE_DIRECTION_* on success, negative errno on error
 */
static int lstp_gpio_get_cached_direction(struct lstp_gpio_priv *priv, unsigned int offset)
{
	u8 direction;

	if (!lstp_gpio_has_line_cfg(priv, offset))
		return GPIO_LINE_DIRECTION_OUT;

	direction = priv->line_cfgs[offset].direction;
	switch (direction) {
	case LSTP_GPIO_DIRECTION_INPUT:
		return GPIO_LINE_DIRECTION_IN;
	case LSTP_GPIO_DIRECTION_OUTPUT:
		return GPIO_LINE_DIRECTION_OUT;
	default:
		dev_err(&priv->ch->usb->intf->dev,
			"%s: ch_%d: Invalid cached direction %u for GPIO %u\n", __func__,
			priv->ch->ch_id, direction, offset);
		return -EIO;
	}
}

/**
 * lstp_gpio_check_output_capable() - Verify a GPIO can be driven.
 * @priv: GPIO channel private data
 * @offset: GPIO line offset
 *
 * The current firmware only supports SetValue. Direction is fixed by the
 * remote-side GPIO configuration, so reject writes to GPIOs declared as input.
 *
 * Return: 0 on success, negative errno on failure
 */
static int lstp_gpio_check_output_capable(struct lstp_gpio_priv *priv, unsigned int offset)
{
	struct device *dev = &priv->ch->usb->intf->dev;
	int direction;

	direction = lstp_gpio_get_cached_direction(priv, offset);
	if (direction < 0)
		return direction;

	if (lstp_gpio_has_line_cfg(priv, offset)) {
		const struct lstp_gpio_line_config *cfg = &priv->line_cfgs[offset];

		dev_dbg(dev, "%s: ch_%d: GPIO %u cached dir=%s(%u) default=%u persist=%u\n",
			__func__, priv->ch->ch_id, offset,
			lstp_gpio_fw_direction_name(cfg->direction), cfg->direction,
			cfg->default_output, cfg->output_persist_state);
	} else {
		dev_dbg(dev, "%s: ch_%d: GPIO %u has no cached line config, assuming output\n",
			__func__, priv->ch->ch_id, offset);
	}

	if (direction == GPIO_LINE_DIRECTION_OUT)
		return 0;

	dev_err(&priv->ch->usb->intf->dev,
		"%s: ch_%d: GPIO %u is configured as input by firmware\n", __func__,
		priv->ch->ch_id, offset);
	return -EOPNOTSUPP;
}

/**
 * lstp_gpio_xfer_locked() - Send an LSTP GPIO request while ch->tx_mutex is held.
 * @priv: GPIO channel private data
 * @cmd: GPIO command opcode
 * @request: Request payload to send (may be NULL if @request_len is 0)
 * @request_len: Request payload length in bytes
 * @response: Optional response buffer
 * @response_len: Expected response payload length
 *
 * Caller must hold priv->ch->tx_mutex.
 *
 * Return: 0 on success, negative errno on failure
 */
static int lstp_gpio_xfer_locked(struct lstp_gpio_priv *priv, u8 cmd, const void *request,
				 u16 request_len, void *response, u16 response_len)
{
	struct lstp_channel *ch = priv->ch;
	struct device *dev = &ch->usb->intf->dev;
	struct lstp_packet *tx_pkt = (struct lstp_packet *)ch->tx_buf;
	struct lstp_packet *rx_pkt = (struct lstp_packet *)ch->resp_buf;
	int ret;

	if (request_len && request)
		memcpy(tx_pkt->payload, request, request_len);

	if (request_len) {
		dev_dbg(dev, "%s: ch_%d: cmd=%s(0x%02x) tx_len=%u req=%*ph\n", __func__, ch->ch_id,
			lstp_gpio_cmd_name(cmd), cmd, request_len,
			min_t(unsigned int, request_len, 32U), request);
	} else {
		dev_dbg(dev, "%s: ch_%d: cmd=%s(0x%02x) tx_len=0\n", __func__, ch->ch_id,
			lstp_gpio_cmd_name(cmd), cmd);
	}

	ret = lstp_recv_resp_helper(ch, cmd, request_len, response_len);
	if (ret) {
		dev_err(dev, "%s: ch_%d: cmd=%s(0x%02x) transfer failed (%d)\n", __func__,
			ch->ch_id, lstp_gpio_cmd_name(cmd), cmd, ret);
		return ret;
	}

	if (le16_to_cpu(rx_pkt->hdr.length)) {
		dev_dbg(dev, "%s: ch_%d: cmd=%s(0x%02x) rx_status=0x%02x rx_len=%u payload=%*ph\n",
			__func__, ch->ch_id, lstp_gpio_cmd_name(cmd), cmd, rx_pkt->hdr.status,
			le16_to_cpu(rx_pkt->hdr.length),
			min_t(unsigned int, le16_to_cpu(rx_pkt->hdr.length), 32U), rx_pkt->payload);
	} else {
		dev_dbg(dev, "%s: ch_%d: cmd=%s(0x%02x) rx_status=0x%02x rx_len=0\n", __func__,
			ch->ch_id, lstp_gpio_cmd_name(cmd), cmd, rx_pkt->hdr.status);
	}

	if (response && response_len)
		memcpy(response, rx_pkt->payload, response_len);

	lstp_unlock_resp_buffer(ch);
	return 0;
}

/**
 * lstp_gpio_get_response_value_locked() - Read a single GPIO value response.
 * @priv: GPIO channel private data
 * @offset: GPIO line offset
 * @value: Output value from the remote endpoint
 *
 * Caller must hold priv->ch->tx_mutex.
 *
 * Return: 0 on success, negative errno on failure
 */
static int lstp_gpio_get_response_value_locked(struct lstp_gpio_priv *priv, unsigned int offset,
					       int *value)
{
	struct device *dev = &priv->ch->usb->intf->dev;
	struct lstp_gpio_get_value_request req = {
		.gpio_index = cpu_to_le16(offset),
	};
	struct lstp_gpio_get_value_response resp;
	int ret;

	ret = lstp_gpio_xfer_locked(priv, LSTP_GPIO_CMD_GET_VALUE, &req, sizeof(req), &resp,
				    sizeof(resp));
	if (ret)
		return ret;

	*value = (resp.value == LSTP_GPIO_STATE_HIGH);
	dev_dbg(dev, "%s: ch_%d: GPIO %u get raw_value=0x%02x parsed=%d\n", __func__,
		priv->ch->ch_id, offset, resp.value, *value);
	return 0;
}

/**
 * lstp_gpio_set_value_locked() - Send a single GPIO set request.
 * @priv: GPIO channel private data
 * @offset: GPIO line offset
 * @value: GPIO line value
 *
 * Caller must hold priv->ch->tx_mutex.
 *
 * Return: 0 on success, negative errno on failure
 */
static int lstp_gpio_set_value_locked(struct lstp_gpio_priv *priv, unsigned int offset, int value)
{
	struct device *dev = &priv->ch->usb->intf->dev;
	struct lstp_gpio_set_value_request req = {
		.gpio_index = cpu_to_le16(offset),
		.value = value ? LSTP_GPIO_STATE_HIGH : LSTP_GPIO_STATE_LOW,
	};
	int ret;

	dev_dbg(dev, "%s: ch_%d: GPIO %u set value=%d raw=0x%02x\n", __func__, priv->ch->ch_id,
		offset, value, req.value);

	ret = lstp_gpio_xfer_locked(priv, LSTP_GPIO_CMD_SET_VALUE, &req, sizeof(req), NULL, 0);
	if (ret)
		dev_err(dev, "%s: ch_%d: GPIO %u set failed (%d)\n", __func__, priv->ch->ch_id,
			offset, ret);
	else
		dev_dbg(dev, "%s: ch_%d: GPIO %u set completed\n", __func__, priv->ch->ch_id,
			offset);

	return ret;
}

/**
 * lstp_gpio_get_direction() - Report firmware-configured direction for a GPIO line.
 * @gc: GPIO chip
 * @offset: GPIO line offset
 *
 * Return: GPIO_LINE_DIRECTION_* on success, negative errno on failure
 */
static int lstp_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct lstp_gpio_priv *priv = gpiochip_get_data(gc);
	int ret;

	ret = lstp_gpio_check_offset(priv, offset);
	if (ret)
		return ret;

	return lstp_gpio_get_cached_direction(priv, offset);
}

/**
 * lstp_gpio_direction_input() - Accept an input request without sending a packet.
 * @gc: GPIO chip
 * @offset: GPIO line offset
 *
 * The current firmware does not expose a runtime direction command. Keep this
 * as a no-op so gpioget can read back lines regardless of their fixed remote
 * direction.
 *
 * Return: 0 on success, negative errno on failure
 */
static int lstp_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct lstp_gpio_priv *priv = gpiochip_get_data(gc);

	return lstp_gpio_check_offset(priv, offset);
}

/**
 * lstp_gpio_direction_output() - Drive a GPIO line on the remote endpoint.
 * @gc: GPIO chip
 * @offset: GPIO line offset
 * @value: Initial output level
 *
 * The obmf-demo firmware models output drive through SetValue only. Enforce the
 * direction cached from ReadConfig before sending the write request.
 *
 * Return: 0 on success, negative errno on failure
 */
static int lstp_gpio_direction_output(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct lstp_gpio_priv *priv = gpiochip_get_data(gc);
	struct device *dev = &priv->ch->usb->intf->dev;
	int ret;

	dev_dbg(dev, "%s: ch_%d: request offset=%u value=%d\n", __func__, priv->ch->ch_id, offset,
		value);

	ret = lstp_gpio_check_offset(priv, offset);
	if (ret)
		return ret;

	ret = lstp_gpio_check_output_capable(priv, offset);
	if (ret)
		return ret;

	mutex_lock(&priv->ch->tx_mutex);
	ret = lstp_gpio_set_value_locked(priv, offset, value);
	mutex_unlock(&priv->ch->tx_mutex);

	if (ret)
		dev_err(dev, "%s: ch_%d: offset=%u value=%d failed (%d)\n", __func__,
			priv->ch->ch_id, offset, value, ret);
	else
		dev_dbg(dev, "%s: ch_%d: offset=%u value=%d completed\n", __func__, priv->ch->ch_id,
			offset, value);

	return ret;
}

/**
 * lstp_gpio_get() - Read a GPIO line value from the remote endpoint.
 * @gc: GPIO chip
 * @offset: GPIO line offset
 *
 * Return: 0/1 on success, negative errno on failure
 */
static int lstp_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct lstp_gpio_priv *priv = gpiochip_get_data(gc);
	struct device *dev = &priv->ch->usb->intf->dev;
	int value;
	int ret;

	dev_dbg(dev, "%s: ch_%d: request offset=%u\n", __func__, priv->ch->ch_id, offset);

	ret = lstp_gpio_check_offset(priv, offset);
	if (ret)
		return ret;

	mutex_lock(&priv->ch->tx_mutex);
	ret = lstp_gpio_get_response_value_locked(priv, offset, &value);
	mutex_unlock(&priv->ch->tx_mutex);
	if (ret)
		return ret;

	dev_dbg(dev, "%s: ch_%d: offset=%u read=%d\n", __func__, priv->ch->ch_id, offset, value);
	return value;
}

/**
 * lstp_gpio_set() - Drive a GPIO line on the remote endpoint.
 * @gc: GPIO chip
 * @offset: GPIO line offset
 * @value: Output value to drive
 *
 * Return: 0 on success, negative errno on failure
 */
static int lstp_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct lstp_gpio_priv *priv = gpiochip_get_data(gc);
	int ret;

	ret = lstp_gpio_check_offset(priv, offset);
	if (ret)
		return ret;

	ret = lstp_gpio_check_output_capable(priv, offset);
	if (ret)
		return ret;

	mutex_lock(&priv->ch->tx_mutex);
	ret = lstp_gpio_set_value_locked(priv, offset, value);
	mutex_unlock(&priv->ch->tx_mutex);
	return ret;
}

static int lstp_gpio_alloc_names(struct lstp_gpio_priv *priv)
{
	if (priv->names)
		return 0;

	priv->names = devm_kcalloc(&priv->ch->usb->intf->dev, priv->gc.ngpio, sizeof(*priv->names),
				   GFP_KERNEL);
	if (!priv->names)
		return -ENOMEM;

	priv->gc.names = priv->names;
	return 0;
}

static int lstp_gpio_init_fw_line_names(struct lstp_gpio_priv *priv)
{
	struct device *dev = &priv->ch->usb->intf->dev;
	unsigned int limit = min_t(unsigned int, priv->gc.ngpio, priv->num_line_cfgs);
	unsigned int i;
	int ret;

	for (i = 0; i < limit; i++) {
		char *name;
		char fw_name[LSTP_GPIO_NAME_MAX_LEN + 1];
		size_t len;

		len = strnlen(priv->line_cfgs[i].gpio_name, LSTP_GPIO_NAME_MAX_LEN);
		if (!len)
			continue;

		ret = lstp_gpio_alloc_names(priv);
		if (ret)
			return ret;

		memcpy(fw_name, priv->line_cfgs[i].gpio_name, len);
		fw_name[len] = '\0';

		name = devm_kstrdup(dev, fw_name, GFP_KERNEL);
		if (!name)
			return -ENOMEM;

		priv->names[i] = name;
	}

	return 0;
}

/**
 * lstp_gpio_read_line_configs() - Read full GPIO channel configuration from ch0.
 * @ch: LSTP GPIO channel
 * @initial_resp: Initial READ_CONFIG response already present in usb->rx_buf
 * @initial_payload_len: Payload length of @initial_resp
 * @line_cfgs_out: Returned per-line configuration array
 * @num_line_cfgs_out: Returned number of GPIOs described by firmware
 *
 * obmf-demo returns GPIO ReadConfig as:
 *   [channel_type][channel_enabled][channel_name[16]]
 *   [channel_num_gpio]
 *   [N * lstp_gpio_line_config]
 *
 * The first read at offset 0, length 0 only returns up to
 * LSTP_GPIO_MAX_CFGS_PER_READ line configs, so fetch the remaining entries via
 * explicit offset/length reads.
 *
 * Return: 0 on success, negative errno on failure
 */
static int lstp_gpio_read_line_configs(struct lstp_channel *ch,
				       const struct lstp_ch0_resp_read *initial_resp,
				       u16 initial_payload_len,
				       struct lstp_gpio_line_config **line_cfgs_out,
				       unsigned int *num_line_cfgs_out)
{
	struct device *dev = &ch->usb->intf->dev;
	const struct lstp_gpio_channel_config *gpio_cfg;
	struct lstp_gpio_line_config *line_cfgs;
	size_t cfg_payload_len;
	size_t copied = 0;
	size_t first_chunk = 0;
	int ret;

	if (initial_payload_len < sizeof(*initial_resp) + sizeof(*gpio_cfg)) {
		dev_err(dev, "%s: ch_%d: Response too small for GPIO channel config\n", __func__,
			ch->ch_id);
		return -EIO;
	}

	cfg_payload_len = initial_payload_len - sizeof(*initial_resp);
	gpio_cfg = (const struct lstp_gpio_channel_config *)initial_resp->ch_config;
	*num_line_cfgs_out = gpio_cfg->channel_num_gpio;
	dev_dbg(dev, "%s: ch_%d: initial payload_len=%u cfg_payload_len=%zu fw_num_gpio=%u\n",
		__func__, ch->ch_id, initial_payload_len, cfg_payload_len, *num_line_cfgs_out);

	if (!*num_line_cfgs_out) {
		*line_cfgs_out = NULL;
		dev_dbg(dev, "%s: ch_%d: firmware reported zero GPIOs\n", __func__, ch->ch_id);
		return 0;
	}

	line_cfgs = devm_kcalloc(dev, *num_line_cfgs_out, sizeof(*line_cfgs), GFP_KERNEL);
	if (!line_cfgs)
		return -ENOMEM;

	if (cfg_payload_len > sizeof(*gpio_cfg)) {
		first_chunk = (cfg_payload_len - sizeof(*gpio_cfg)) / sizeof(*line_cfgs);
		first_chunk = min_t(size_t, first_chunk, *num_line_cfgs_out);
		dev_dbg(dev,
			"%s: ch_%d: initial response contains %zu line configs (cfg_size=%zu)\n",
			__func__, ch->ch_id, first_chunk, sizeof(*line_cfgs));

		memcpy(line_cfgs, initial_resp->ch_config + sizeof(*gpio_cfg),
		       first_chunk * sizeof(*line_cfgs));
		copied = first_chunk;
	}

	while (copied < *num_line_cfgs_out) {
		struct lstp_packet *rx_pkt;
		struct lstp_ch0_resp_read *chunk_resp;
		unsigned int chunk = min_t(unsigned int, *num_line_cfgs_out - copied,
					   LSTP_GPIO_MAX_CFGS_PER_READ);
		u16 req_offset =
			sizeof(struct lstp_gpio_channel_config) + copied * sizeof(*line_cfgs);
		u16 req_length = chunk * sizeof(*line_cfgs);

		dev_dbg(dev,
			"%s: ch_%d: requesting config chunk copied=%zu chunk=%u offset=%u length=%u\n",
			__func__, ch->ch_id, copied, chunk, req_offset, req_length);

		ret = lstp_ch0_read(ch->usb, ch->ch_id, req_offset, req_length);
		if (ret) {
			dev_err(dev, "%s: ch_%d: Could not fetch GPIO config chunk at offset %u\n",
				__func__, ch->ch_id, req_offset);
			return ret;
		}

		rx_pkt = (struct lstp_packet *)ch->usb->rx_buf;
		ret = lstp_validate_resp(ch->usb, rx_pkt,
					 sizeof(struct lstp_ch0_resp_read) + req_length);
		if (ret)
			return ret;

		chunk_resp = LSTP_GET_PAYLOAD(rx_pkt, struct lstp_ch0_resp_read);
		if (!chunk_resp) {
			dev_err(dev, "%s: ch_%d: Missing GPIO config chunk payload\n", __func__,
				ch->ch_id);
			return -EIO;
		}

		if (chunk_resp->ch_type != LSTP_CHANNEL_TYPE_GPIO) {
			dev_err(dev, "%s: ch_%d: Unexpected channel type %u in GPIO config chunk\n",
				__func__, ch->ch_id, chunk_resp->ch_type);
			return -EINVAL;
		}

		memcpy(line_cfgs + copied, chunk_resp->ch_config, req_length);
		copied += chunk;
		dev_dbg(dev, "%s: ch_%d: copied line configs=%zu/%u\n", __func__, ch->ch_id, copied,
			*num_line_cfgs_out);
	}

	*line_cfgs_out = line_cfgs;
	dev_dbg(dev, "%s: ch_%d: finished reading %u GPIO line configs\n", __func__, ch->ch_id,
		*num_line_cfgs_out);
	lstp_gpio_dump_line_configs(ch, line_cfgs, *num_line_cfgs_out);
	return 0;
}

/**
 * lstp_gpio_init_line_names() - Populate gpio_chip line names from firmware/DT.
 * @priv: GPIO channel private data
 *
 * Firmware names from ReadConfig are used as defaults. Device tree
 * gpio-line-names, when present, override the firmware-provided labels.
 *
 * Expected optional DT node structure::
 *
 *   gpio@M {
 *       compatible = "nv,lstp-gpio";
 *       reg = <M>;
 *       gpio-controller;
 *       #gpio-cells = <2>;
 *       ngpios = <...>;
 *       gpio-line-names = "foo", "bar", ...;
 *       label = "lstp-gpio0";
 *   };
 *
 * Return: 0 on success, negative errno on failure
 */
static int lstp_gpio_init_line_names(struct lstp_gpio_priv *priv)
{
	struct device_node *np = priv->ch->of_node;
	int count;
	int i;
	int ret;

	ret = lstp_gpio_init_fw_line_names(priv);
	if (ret)
		return ret;

	if (!np)
		return 0;

	count = of_property_count_strings(np, "gpio-line-names");
	if (count == -EINVAL)
		return 0;
	if (count <= 0)
		return count;

	ret = lstp_gpio_alloc_names(priv);
	if (ret)
		return ret;

	if (count > priv->gc.ngpio) {
		dev_warn(&priv->ch->usb->intf->dev,
			 "%s: ch_%d: gpio-line-names has %d entries, truncating to %u\n", __func__,
			 priv->ch->ch_id, count, priv->gc.ngpio);
		count = priv->gc.ngpio;
	}

	for (i = 0; i < count; i++) {
		ret = of_property_read_string_index(np, "gpio-line-names", i, &priv->names[i]);
		if (ret)
			return ret;
	}

	return 0;
}

/**
 * lstp_gpio_init() - Initialize an LSTP GPIO channel.
 * @ch: LSTP channel configured as GPIO type
 *
 * Parses the management-channel configuration, allocates the gpio_chip, and
 * prepares the channel for registration with gpiolib.
 *
 * Return: 0 on success, negative errno on failure
 */
int lstp_gpio_init(struct lstp_channel *ch)
{
	struct device *dev = &ch->usb->intf->dev;
	struct lstp_packet *rx_pkt = (struct lstp_packet *)ch->usb->rx_buf;
	struct lstp_ch0_resp_read *ch0_resp;
	struct lstp_gpio_line_config *line_cfgs = NULL;
	struct lstp_gpio_priv *priv;
	const char *label;
	char ch_name[LSTP_CH_NAME_LEN];
	u32 dt_ngpio = 0;
	u16 payload_len;
	u16 ngpio = 0;
	unsigned int fw_ngpio = 0;
	int ret;

	ret = lstp_validate_resp(ch->usb, rx_pkt, LSTP_ANY_RX_LEN);
	if (ret)
		return ret;

	payload_len = le16_to_cpu(rx_pkt->hdr.length);
	if (payload_len <
	    sizeof(struct lstp_ch0_resp_read) + sizeof(struct lstp_gpio_channel_config)) {
		dev_err(dev,
			"%s: ch_%d: Response too small for GPIO metadata (got %u, need at least %zu)\n",
			__func__, ch->ch_id, payload_len,
			sizeof(struct lstp_ch0_resp_read) +
				sizeof(struct lstp_gpio_channel_config));
		return -EIO;
	}

	ch0_resp = LSTP_GET_PAYLOAD(rx_pkt, struct lstp_ch0_resp_read);
	if (!ch0_resp) {
		dev_err(dev, "%s: ch_%d: Response too small for GPIO metadata\n", __func__,
			ch->ch_id);
		return -EIO;
	}

	if (ch0_resp->ch_type != LSTP_CHANNEL_TYPE_GPIO) {
		dev_err(dev, "%s: ch_%d: Unexpected channel type %u\n", __func__, ch->ch_id,
			ch0_resp->ch_type);
		return -EINVAL;
	}

	if (ch0_resp->ch_name[0] == '\0') {
		dev_err(dev, "%s: ch_%d: Invalid GPIO chip name\n", __func__, ch->ch_id);
		return -EINVAL;
	}

	strscpy(ch_name, ch0_resp->ch_name, sizeof(ch_name));
	dev_dbg(dev,
		"%s: ch_%d: begin init name=%s payload_len=%u of_node=%pOF dt_ngpios_present=%d\n",
		__func__, ch->ch_id, ch_name, payload_len, ch->of_node,
		ch->of_node ? !of_property_read_u32(ch->of_node, "ngpios", &dt_ngpio) : 0);

	if (ch->of_node && !of_property_read_u32(ch->of_node, "ngpios", &dt_ngpio))
		dev_dbg(dev, "%s: ch_%d: DTS ngpios=%u\n", __func__, ch->ch_id, dt_ngpio);

	ret = lstp_gpio_read_line_configs(ch, ch0_resp, payload_len, &line_cfgs, &fw_ngpio);
	if (ret)
		return ret;

	ngpio = fw_ngpio;
	dev_dbg(dev, "%s: ch_%d: firmware provided %u GPIOs\n", __func__, ch->ch_id, fw_ngpio);

	if (ch->of_node && !of_property_read_u32(ch->of_node, "ngpios", &dt_ngpio)) {
		if (dt_ngpio > U16_MAX) {
			dev_err(dev, "%s: ch_%d: DT ngpios=%u exceeds %u\n", __func__, ch->ch_id,
				dt_ngpio, U16_MAX);
			return -EINVAL;
		}

		if (ngpio && dt_ngpio != ngpio)
			dev_warn(dev, "%s: ch_%d: DT ngpios=%u overrides firmware ngpio=%u\n",
				 __func__, ch->ch_id, dt_ngpio, ngpio);

		ngpio = dt_ngpio;
	}

	if (!ngpio) {
		dev_err(dev, "%s: ch_%d: Missing GPIO count in firmware config and DT\n", __func__,
			ch->ch_id);
		return -EINVAL;
	}

	ch->resp_buf = devm_kzalloc(dev, ch->usb->bulk_rx_size, GFP_KERNEL);
	if (!ch->resp_buf)
		return -ENOMEM;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->ch = ch;
	priv->line_cfgs = line_cfgs;
	priv->num_line_cfgs = fw_ngpio;

	if (ch->of_node && !of_property_read_string(ch->of_node, "label", &label)) {
		priv->gc.label = label;
		dev_dbg(dev, "%s: ch_%d: using DTS label=%s\n", __func__, ch->ch_id, label);
	} else {
		priv->gc.label =
			devm_kasprintf(dev, GFP_KERNEL, "%s_%s", ch->usb->lstp_intf_name, ch_name);
		if (!priv->gc.label)
			return -ENOMEM;
		dev_dbg(dev, "%s: ch_%d: using generated label=%s\n", __func__, ch->ch_id,
			priv->gc.label);
	}

	priv->gc.owner = THIS_MODULE;
	priv->gc.parent = dev;
	priv->gc.base = -1;
	priv->gc.ngpio = ngpio;
	priv->gc.can_sleep = true;
	priv->gc.get_direction = lstp_gpio_get_direction;
	priv->gc.direction_input = lstp_gpio_direction_input;
	priv->gc.direction_output = lstp_gpio_direction_output;
	priv->gc.get = lstp_gpio_get;
	priv->gc.set = lstp_gpio_set;

	if (ch->of_node) {
		priv->gc.fwnode = of_fwnode_handle(ch->of_node);
#if defined(CONFIG_OF_GPIO)
		priv->gc.of_gpio_n_cells = 2;
#endif
	}
	dev_dbg(dev, "%s: ch_%d: final ngpio=%u fw_cfgs=%u names_from_fw=%u of_node=%pOF\n",
		__func__, ch->ch_id, priv->gc.ngpio, priv->num_line_cfgs,
		min_t(unsigned int, priv->gc.ngpio, priv->num_line_cfgs), ch->of_node);

	ret = lstp_gpio_init_line_names(priv);
	if (ret)
		return ret;

	ch->priv = priv;

	dev_info(dev, "%s: ch_%d: Initialized GPIO chip %s with %u lines\n", __func__, ch->ch_id,
		 priv->gc.label, priv->gc.ngpio);
	return 0;
}

/**
 * lstp_gpio_start() - Register an LSTP GPIO channel with gpiolib.
 * @ch: LSTP channel with initialized GPIO state
 *
 * Return: 0 on success, negative errno on failure
 */
int lstp_gpio_start(struct lstp_channel *ch)
{
	struct lstp_gpio_priv *priv = ch->priv;
	int ret;

	if (!priv) {
		dev_err(&ch->usb->intf->dev, "%s: ch_%d: GPIO context not initialized\n", __func__,
			ch->ch_id);
		return -EINVAL;
	}

	dev_dbg(&ch->usb->intf->dev,
		"%s: ch_%d: registering gpiochip label=%s ngpio=%u fw_cfgs=%u of_node=%pOF\n",
		__func__, ch->ch_id, priv->gc.label, priv->gc.ngpio, priv->num_line_cfgs,
		ch->of_node);

	ret = devm_gpiochip_add_data(&ch->usb->intf->dev, &priv->gc, priv);
	if (ret) {
		dev_err(&ch->usb->intf->dev, "%s: ch_%d: Could not register GPIO chip (%d)\n",
			__func__, ch->ch_id, ret);
		return ret;
	}

	ch->child_dev = gpio_device_to_device(priv->gc.gpiodev);

	dev_dbg(&ch->usb->intf->dev, "%s: ch_%d: Registered GPIO chip %s\n", __func__, ch->ch_id,
		priv->gc.label);
	return 0;
}
