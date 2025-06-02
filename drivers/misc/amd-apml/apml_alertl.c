// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * alert_l.c - Alert_l driver for AMD APML devices
 *
 * Copyright (C) 2022-2023 Advanced Micro Devices, Inc.
 */

#include <linux/debugfs.h>
#include <linux/gpio/consumer.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/i3c/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/platform_device.h>

#include "sbrmi-common.h"
#include "apml_alertl.h"
#include "sbtsi-common.h"

#define DRIVER_NAME "apml_alertl"

#define RAS_STATUS_REG		0x4C
#define STATUS_REG		0x2
#define TSI_STATUS_REG		0x2
#define RAS_ALERT_STATUS	BIT(1)
#define RAS_ALERT_ASYNC		BIT(3)

#define MAX_SOC_LEN             11
#define MAX_ERR_LEN             18

MODULE_ALIAS("apml_alertl:" DRIVER_NAME);

/* Read TSI Status register to identify the RAS error */
static int tsi_alert_check(struct apml_alertl_data *oob_adata, int *temp_status, u8 soc_die_num)
{
	struct apml_message msg = { 0 };
	int ret;

	if (!oob_adata->tsi_dev[soc_die_num] || !oob_adata->tsi_dev[soc_die_num]->regmap)
		return 0;
	msg.data_in.reg_in[REG_OFF_INDEX] = TSI_STATUS_REG;

	mutex_lock(&oob_adata->tsi_dev[soc_die_num]->lock);
	ret = regmap_read(oob_adata->tsi_dev[soc_die_num]->regmap,
			  msg.data_in.reg_in[REG_OFF_INDEX],
			  temp_status);
	mutex_unlock(&oob_adata->tsi_dev[soc_die_num]->lock);

	return ret;
}

/* Read RAS Status register to identify the RAS error */
static int rmi_alert_check(struct apml_alertl_data *oob_adata, int *status, u8 soc_die_num)
{
	struct apml_message msg = { 0 };
	int ret;

	if (!oob_adata->rmi_dev[soc_die_num] || !oob_adata->rmi_dev[soc_die_num]->regmap)
		return 0;
	msg.data_in.reg_in[REG_OFF_INDEX] = RAS_STATUS_REG;

	mutex_lock(&oob_adata->rmi_dev[soc_die_num]->lock);
	ret = regmap_read(oob_adata->rmi_dev[soc_die_num]->regmap,
			  msg.data_in.reg_in[REG_OFF_INDEX],
			  status);
	mutex_unlock(&oob_adata->rmi_dev[soc_die_num]->lock);

	return ret;
}

static u8 static_addr_to_socket(u8 static_addr)
{
	/*
	 * [3:0] = Socket Index
	 * [7:4] = die Index
	 * Mapping:
	 * 0x3c, 0x4c	-> Socket 0, die 0,
	 * 0x44		-> Socket 0, die 1,
	 * 0x38, 0x48	-> Socket 1, die 0,
	 * 0x4c		-> Socket 1, die 1.
	 */
	switch (static_addr) {
	case 0x3c:
	case 0x4c:
		return 0;
	case 0x44:
		return (0 | (1 << 4));
	case 0x38:
	case 0x48:
		return 1;
	case 0x45:
		return (1 | (1 << 4));
	default:
		return 0xff;
	}
}

static int send_uevent(u8 static_address, u32 alert_src, struct device *dev)
{
	u8 soc_die_num = 0;
	char sock[MAX_SOC_LEN];
	char src[MAX_ERR_LEN];
	char *alert_source[] = { sock, src, NULL };

	soc_die_num = static_addr_to_socket(static_address);
	if (soc_die_num == 0xFF) {
		pr_err("Device static address not valid\n");
		return -ENODEV;
		}

	snprintf(sock, sizeof(sock), "Socket=0x%x", soc_die_num);
	snprintf(src, sizeof(src), "Source=0x%x", alert_src);
	pr_err("apml_alertl:Sock:0x%x Src:0x%x\n", soc_die_num, alert_src);

	pr_debug("Sending uevent to user space...\n");
	kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, alert_source);
	return 0;
}

static irqreturn_t alert_l_irq_thread_handler(int irq, void *dev_id)
{
	struct apml_alertl_data *oob_adata = dev_id;
	struct apml_message msg = { 0 };
	struct device *dev = oob_adata->dev;
	unsigned int ras_status = 0;
	unsigned int temp_status = 0;
	u32 rt_src = 0;
	u8 static_addr = 0;
	int ret, i;

	for (i = 0; i < oob_adata->num_of_tsi_devs; i++) {
		rt_src = 0;
		/* Check for TSI alert */
		ret = tsi_alert_check(oob_adata, &temp_status, i);
		if (ret < 0)
			pr_err("Failed to read TSI status register\n");

		if (!temp_status)
			continue;

		rt_src = (temp_status << 24);
		static_addr = oob_adata->tsi_dev[i]->dev_static_addr;

		ret = send_uevent(static_addr, rt_src, dev);
		if (ret)
			return ret;
	}

	for (i = 0; i < oob_adata->num_of_rmi_devs; i++) {
		rt_src = 0;
		/* Check for RAS alerts */
		ret = rmi_alert_check(oob_adata, &ras_status, i);
		if (ret < 0)
			pr_err("Failed to read RAS status register\n");

		if (!ras_status)
			continue;

		rt_src = ras_status;
		static_addr = oob_adata->rmi_dev[i]->dev_static_addr;

		ret = send_uevent(static_addr, rt_src, dev);
		if (ret)
			return ret;

		 /* Clear the RMI Status and RAS Status register */
		if (!oob_adata->rmi_dev[i] || !oob_adata->rmi_dev[i]->regmap)
			return -ENODEV;

		if (ras_status) {
			mutex_lock(&oob_adata->rmi_dev[i]->lock);
			msg.data_in.reg_in[REG_OFF_INDEX] = RAS_STATUS_REG;
			ret = regmap_write(oob_adata->rmi_dev[i]->regmap,
					   msg.data_in.reg_in[REG_OFF_INDEX],
					   ras_status);
			if (ret < 0)
				pr_err("Failed to clear RMI status register\n");

			msg.data_in.reg_in[REG_OFF_INDEX] = STATUS_REG;
			ret = regmap_write(oob_adata->rmi_dev[i]->regmap,
					   msg.data_in.reg_in[REG_OFF_INDEX],
					   RAS_ALERT_ASYNC);
			mutex_unlock(&oob_adata->rmi_dev[i]->lock);
			if (ret < 0) {
				pr_err("Failed to clear RAS status register\n");
				return ret;
			}
		}
	}
	return IRQ_HANDLED;
}

static void *get_apml_dev_byphandle(struct device_node *dnode,
				    const char *phandle_name,
				    int index)
{
	struct device_node *d_node;
	struct device *dev;
	void *apml_dev;

	if (!phandle_name || !dnode)
		return NULL;

	d_node = of_parse_phandle(dnode, phandle_name, index);
	if (IS_ERR_OR_NULL(d_node))
		return NULL;

	if (strcmp(phandle_name, "sbrmi") == 0) {
		dev = bus_find_device(&i3c_bus_type, NULL, d_node, sbrmi_match_i3c);
		if (!dev) {
			dev = bus_find_device(&i2c_bus_type, NULL, d_node, sbrmi_match_i2c);
			if (IS_ERR_OR_NULL(dev)) {
				of_node_put(d_node);
				return NULL;
			}
		}
	} else if (strcmp(phandle_name, "sbtsi") == 0) {
		dev = bus_find_device(&i3c_bus_type, NULL, d_node, sbtsi_match_i3c);
		if (!dev) {
			dev = bus_find_device(&i2c_bus_type, NULL, d_node, sbtsi_match_i2c);
			if (IS_ERR_OR_NULL(dev)) {
				of_node_put(d_node);
				return NULL;
			}
		}
	}

	of_node_put(d_node);
	apml_dev = dev_get_drvdata(dev);
	if (IS_ERR_OR_NULL(apml_dev))
		return NULL;

	return apml_dev;
}

static int apml_alertl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *dnode = dev->of_node;
	struct apml_sbrmi_device **rmi_dev;
	struct apml_sbtsi_device **tsi_dev;
	struct apml_alertl_data *oob_alert;
	struct gpio_desc *alertl_gpiod;
	u32 irq_num;
	u32 num_dev = 0;
	int ret = 0;
	int i = 0;

	/* Allocate memory to oob_alert_data structure */
	oob_alert = devm_kzalloc(dev, sizeof(struct apml_alertl_data),
				 GFP_KERNEL);
	if (!oob_alert)
		return -ENOMEM;
	/* identify the number of devices associated with each RMI alert */
	num_dev = of_property_count_elems_of_size(dnode, "sbrmi",
						  sizeof(phandle));
	oob_alert->num_of_rmi_devs = num_dev;

	/* identify the number of devices associated with each TSI alert */
	num_dev = of_property_count_elems_of_size(dnode, "sbtsi",
						  sizeof(phandle));
	oob_alert->num_of_tsi_devs = num_dev;

	/* Allocate memory as per the number of RMI devices */
	rmi_dev = devm_kzalloc(dev, oob_alert->num_of_rmi_devs * sizeof(struct apml_sbrmi_device),
			       GFP_KERNEL);
	if (!rmi_dev)
		return -ENOMEM;
	oob_alert->rmi_dev = rmi_dev;

	/* Allocate memory as per the number of TSI devices */
	tsi_dev = devm_kzalloc(dev, oob_alert->num_of_tsi_devs * sizeof(struct apml_sbtsi_device),
			       GFP_KERNEL);
	if (!tsi_dev)
		return -ENOMEM;
	oob_alert->tsi_dev = tsi_dev;
	oob_alert->dev = dev;

	/*
	 * For each of the Alerts get the device associated
	 * Currently the ALert_L driver identification is only supported
	 * over I3C. We can add property in dts to identify the bus type
	 */

	for (i = 0; i < oob_alert->num_of_rmi_devs; i++) {
		rmi_dev[i] = get_apml_dev_byphandle(pdev->dev.of_node, "sbrmi", i);
		if (!rmi_dev[i]) {
			pr_err("Error getting APML SBRMI device. Exiting\n");
			return -EINVAL;
		}
	}

	for (i = 0; i < oob_alert->num_of_tsi_devs; i++) {
		tsi_dev[i] = get_apml_dev_byphandle(pdev->dev.of_node, "sbtsi", i);
		if (!tsi_dev[i]) {
			pr_err("Error getting APML SBTSI device. Exiting\n");
			return -EINVAL;
		}
	}

	/* Get the alert_l gpios, irq_number for the GPIO and register ISR*/
	alertl_gpiod = devm_gpiod_get(dev, NULL, GPIOD_IN);
	if (IS_ERR(alertl_gpiod)) {
		dev_err(&pdev->dev, "Unable to retrieve gpio\n");
		return PTR_ERR(alertl_gpiod);
	}

	ret = gpiod_to_irq(alertl_gpiod);
	if (ret < 0) {
		dev_err(dev, "No corresponding irq for gpio error: %d\n", ret);
		return ret;
	}
	irq_num = ret;
	pr_debug("Register IRQ:%u\n", irq_num);
	/*
	 * TODO: naming can be updated for the irq on
	 * basis of socket number
	 */
	ret = devm_request_threaded_irq(dev, irq_num,
					NULL,
					(void *)alert_l_irq_thread_handler,
					IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
					"apml_irq", oob_alert);
	if (ret) {
		pr_err("Cannot register IRQ:%u\n", irq_num);
		return ret;
	}

	/* Set the platform data to pdev */
	platform_set_drvdata(pdev, oob_alert);

	return 0;
}

static int alert_remove(struct platform_device *pdev)
{
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
	.remove		= alert_remove,
};

module_platform_driver(apml_alertl_driver);
MODULE_AUTHOR("Akshay Gupta <akshay.gupta@amd.com>");
MODULE_AUTHOR("Naveenkrishna Chatradhi <naveenkrishna.chatradhi@amd.com>");
MODULE_DESCRIPTION("AMD APML ALERT_L Driver");
MODULE_LICENSE("GPL");
