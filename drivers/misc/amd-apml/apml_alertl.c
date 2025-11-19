// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * apml_alertl.c - Alert_L driver for AMD APML devices
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>
#include <linux/i3c/device.h>
#include <linux/i3c/master.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/list.h>
#include <linux/mutex.h>

#include "apml_alertl.h"

#define DRIVER_NAME "apml_alertl"

#define RAS_STATUS_REG		0x4C
#define RMI_STATUS_REG		0x2
#define TSI_STATUS_REG		0x2
#define RAS_ALERT_STATUS	BIT(1)
#define RAS_ALERT_ASYNC		BIT(3)
#define TSI_STATUS_SHIFT	24

#define ENVP_SRC_INDX		0
#define ENVP_BUS_NUM_INDX	1
#define ENVP_PID_INDX		2
#define ENVP_ADDR_INDX		3
#define NUM_ENVP		5

MODULE_ALIAS("apml_alertl:" DRIVER_NAME);

/*
 * The driver generates uevents for Temperature and RAS alerts (both fatal and non-fatal).
 * Event data contains address, bus number, PID (for I3C devices; 0 otherwise), and alert
 * source information. See amd-apml.h for alert source details.
 */
static int send_uevent(u8 address, u8 bus_num, u32 alert_src,
		       u64 pid, struct device *dev)
{
	char *alert_source[NUM_ENVP];

	alert_source[ENVP_SRC_INDX] = devm_kasprintf(dev, GFP_KERNEL, "SOURCE=0x%08x", alert_src);
	alert_source[ENVP_BUS_NUM_INDX] = devm_kasprintf(dev, GFP_KERNEL, "BUS_NUM=%u", bus_num);
	alert_source[ENVP_PID_INDX] = devm_kasprintf(dev, GFP_KERNEL, "PID=0x%016llx", pid);
	alert_source[ENVP_ADDR_INDX] = devm_kasprintf(dev, GFP_KERNEL, "ADDRESS=0x%02x", address);
	alert_source[NUM_ENVP - 1] = NULL;

	dev_dbg(dev, "Sending uevent: Addr:0x%x Src:0x%08x\n bus:%d pid: 0x%llx\n",
		 address, alert_src, bus_num, pid);
	kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, alert_source);

	return 0;
}

static int handle_rmi_device_alert(struct apml_device_node *device_node, struct device *dev)
{
	int status = 0, ret;
	u8 addr, bus_num;
	u64 pid;

	if (!device_node->rmi_dev || !device_node->rmi_dev->regmap) {
		dev_warn(dev, "Invalid RMI device found\n");
		return -EINVAL;
	}

	/* Protects individual device state and regmap transactions */
	mutex_lock(&device_node->rmi_dev->lock);
	/* Read RAS Status register */
	ret = regmap_read(device_node->rmi_dev->regmap, RAS_STATUS_REG, &status);
	mutex_unlock(&device_node->rmi_dev->lock);
	if (ret)
		return ret;

	if (!status) {
		/* No alert status - normal condition */
		return ret;
	}
	/* Extract device information based on bus type (I3C or I2C) */
	if (device_node->rmi_dev->i3cdev) {
		/* I3C device path */
		addr = device_node->rmi_dev->dev_static_addr;
		bus_num = device_node->rmi_dev->i3cdev->desc->dev->bus->id;
		pid = device_node->rmi_dev->i3cdev->desc->info.pid;
	} else if (device_node->rmi_dev->client) {
		/* I2C device path */
		addr = device_node->rmi_dev->client->addr;
		bus_num = device_node->rmi_dev->client->adapter->nr;
		pid = 0; /* I2C devices do not have PID */
	} else {
		return -EINVAL;
	}

	if (!addr)
		return -EINVAL;

	/* Send uevent for RAS alert */
	ret = send_uevent(addr, bus_num, status, pid, dev);
	if (ret) {
		dev_info(dev, "Failed to send uevent for RAS alert (device: 0x%x, err: %d)\n",
			 addr, ret);
	}

	/* Clear RAS and RMI status registers */
	mutex_lock(&device_node->rmi_dev->lock);
	ret = regmap_write(device_node->rmi_dev->regmap, RAS_STATUS_REG, status);
	if (ret)
		dev_warn(dev, "Failed to clear RAS status register (device: 0x%x): %d\n",
			 addr, ret);

	ret = regmap_write(device_node->rmi_dev->regmap, RMI_STATUS_REG, RAS_ALERT_ASYNC);
	if (ret)
		dev_warn(dev, "Failed to clear RMI status register (device: 0x%x): %d\n",
			 addr, ret);

	mutex_unlock(&device_node->rmi_dev->lock);

	return ret; /* Alert was processed */
}

/* Handle TSI device alerts */
static int handle_tsi_device_alert(struct apml_device_node *device_node, struct device *dev)
{
	int status = 0, ret;
	u8 addr, bus_num;
	u64 pid;

	if (!device_node->tsi_dev || !device_node->tsi_dev->regmap) {
		dev_warn(dev, "Invalid TSI device found\n");
		return -EINVAL;
	}

	/* Protects individual device state and regmap transactions */
	mutex_lock(&device_node->tsi_dev->lock);
	/* Read TSI Status register */
	ret = regmap_read(device_node->tsi_dev->regmap, TSI_STATUS_REG, &status);
	mutex_unlock(&device_node->tsi_dev->lock);

	if (ret) {
		dev_warn(dev, "Failed to read TSI status from device %d\n", ret);
		return ret;
	}

	if (!status) {
		/* No alert status - normal condition */
		return ret;
	}

	if (device_node->tsi_dev->i3cdev) {
		addr = device_node->tsi_dev->dev_static_addr;
		bus_num = device_node->tsi_dev->i3cdev->desc->dev->bus->id;
		pid = device_node->tsi_dev->i3cdev->desc->info.pid;
	} else if (device_node->tsi_dev->client) {
		addr = device_node->tsi_dev->client->addr;
		bus_num = device_node->tsi_dev->client->adapter->nr;
		pid = 0;
	} else {
		return -EINVAL;
	}

	if (!addr || !bus_num)
		return -EINVAL;

	/* Send uevent for temperature alert (shifted to avoid RAS bit overlap) */
	ret = send_uevent(addr, bus_num, status << TSI_STATUS_SHIFT, pid, dev);
	if (ret) {
		dev_info(dev, "Failed to send uevent for temp alert (device: 0x%x, err: %d)\n",
			 addr, ret);
	}
	return ret; /* Alert was processed */
}

static void handle_apml_alerts(struct device *dev)
{
	struct apml_device_node *device_node;
	int ret;

	mutex_lock(&apml_devices_lock);
	list_for_each_entry(device_node, &apml_devices, list) {
		/* Get a safe reference to the device node */
		if (!kref_get_unless_zero(&device_node->ref))
			continue;

		/* Device-specific alert processing */
		switch (device_node->dev_type) {
		case APML_RMI_DEVICE:
			ret = handle_rmi_device_alert(device_node, dev);
			break;
		case APML_TSI_DEVICE:
			ret = handle_tsi_device_alert(device_node, dev);
			break;
		default:
			dev_warn(dev, "Unknown device type: %d\n", device_node->dev_type);
			ret = -EINVAL;
			break;
		}

		if (ret) {
			dev_dbg(dev, "Alert processing failed for device type %d: %d\n",
				device_node->dev_type, ret);
		}
		/* Always release the reference */
		apml_put_device_node(device_node);
	}
	mutex_unlock(&apml_devices_lock);
}

/* Handles Alert_L interrupts by delegating to unified alert handler */
static irqreturn_t alert_l_irq_thread_handler(int irq, void *dev_id)
{
	struct device *dev = (struct device *)dev_id;

	handle_apml_alerts(dev);

	return IRQ_HANDLED;
}

static int apml_alertl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct apml_alertl_data *oob_alert;
	struct gpio_desc *alertl_gpiod;
	int ret;
	u8 socket_num = 0;
	char *irq_name;

	oob_alert = devm_kzalloc(dev, sizeof(*oob_alert), GFP_KERNEL);
	if (!oob_alert)
		return -ENOMEM;

	oob_alert->dev = dev;

	/* Get the alert_l gpio */
	alertl_gpiod = devm_gpiod_get(dev, NULL, GPIOD_IN);
	if (IS_ERR(alertl_gpiod))
		return PTR_ERR(alertl_gpiod);

	/* Get IRQ number from GPIO */
	ret = gpiod_to_irq(alertl_gpiod);
	if (ret < 0) {
		dev_err(dev,
			"APML AlertL: No corresponding irq for gpio error: %d\n",
			ret);
		return ret;
	}

	oob_alert->irq_num = ret;

	/* Try to read socket-id property from DTS */
	ret = of_property_read_u8(np, "socket-num", &socket_num);
	if (!ret) {
		irq_name = devm_kasprintf(dev, GFP_KERNEL, "apml_irq%u", socket_num);
		if (!irq_name)
			return -ENOMEM;
	} else {
		irq_name = devm_kstrdup(dev, "apml_irq", GFP_KERNEL);
		if (!irq_name)
			return -ENOMEM;
	}
	dev_info(dev, "APML Alert_L for socket %u, IRQ %u\n", socket_num, oob_alert->irq_num);
	/* Register threaded IRQ handler */
	ret = devm_request_threaded_irq(dev, oob_alert->irq_num,
					NULL,
					alert_l_irq_thread_handler,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					irq_name,
					dev);
	if (ret) {
		dev_err(dev, "Cannot register IRQ:%u\n", oob_alert->irq_num);
		return ret;
	}

	platform_set_drvdata(pdev, oob_alert);
	return 0;
}

static int apml_alertl_remove(struct platform_device *pdev)
{
	struct apml_alertl_data *alertl_data = platform_get_drvdata(pdev);

	if (alertl_data)
		/* Ensure any running interrupt handlers complete */
		synchronize_irq(alertl_data->irq_num);

	return 0;
}

static const struct of_device_id apml_alertl_dt_ids[] = {
	{.compatible = "apml-alertl", },
	{},
};
MODULE_DEVICE_TABLE(of, apml_alertl_dt_ids);

static struct platform_driver apml_alertl_driver = {
	.driver = {
		.name	= DRIVER_NAME,
		.of_match_table = of_match_ptr(apml_alertl_dt_ids),
	},
	.probe		= apml_alertl_probe,
	.remove		= apml_alertl_remove,
};

module_platform_driver(apml_alertl_driver);

MODULE_AUTHOR("Akshay Gupta <akshay.gupta@amd.com>");
MODULE_AUTHOR("Naveenkrishna Chatradhi <naveenkrishna.chatradhi@amd.com>");
MODULE_DESCRIPTION("AMD APML ALERT_L Driver");
MODULE_LICENSE("GPL");
