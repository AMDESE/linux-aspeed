// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * apml-common.c - Common registration system for APML devices
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/i3c/device.h>
#include <linux/i3c/master.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/kref.h>

#include "apml_common.h"

/* Global list of registered APML devices */
LIST_HEAD(apml_devices);
EXPORT_SYMBOL_GPL(apml_devices);

/*
 * Global Lock: Protects the shared apml_devices list during registration,
 * unregistration, and iteration through all registered devices
 */
DEFINE_MUTEX(apml_devices_lock);
EXPORT_SYMBOL_GPL(apml_devices_lock);

/* Release function for device node reference counting */
static void apml_device_node_release(struct kref *ref)
{
	struct apml_device_node *node = container_of(ref, struct apml_device_node, ref);

	kfree(node);
}

/* Get a reference to a device node - increases reference count */
struct apml_device_node *apml_get_device_node(struct apml_device_node *node)
{
	if (node && kref_get_unless_zero(&node->ref))
		return node;
	return NULL;
}
EXPORT_SYMBOL_GPL(apml_get_device_node);

/* Put a reference to a device node - decreases reference count */
void apml_put_device_node(struct apml_device_node *node)
{
	if (node)
		kref_put(&node->ref, apml_device_node_release);
}
EXPORT_SYMBOL_GPL(apml_put_device_node);

static void apml_add_device_node(struct apml_device_node *node)
{
	mutex_lock(&apml_devices_lock);
	list_add_tail(&node->list, &apml_devices);
	mutex_unlock(&apml_devices_lock);
}

/* Device registration function */
int apml_register_device(void *device, enum apml_device_type dev_type)
{
	struct apml_device_node *node;
	struct apml_sbrmi_device *rmi_dev;
	struct apml_sbtsi_device *tsi_dev;

	if (!device) {
		pr_info("APML: Cannot register NULL device\n");
		return -EINVAL;
	}

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	node->dev_type = dev_type;
	kref_init(&node->ref);  /* Initialize reference count to 1 */

	switch (node->dev_type) {
	case APML_RMI_DEVICE:
		rmi_dev = (struct apml_sbrmi_device *)device;

		if (!rmi_dev->i3cdev && !rmi_dev->client) {
			pr_err("APML: Invalid RMI device - no I2C or I3C device\n");
			kfree(node);
			return -EINVAL;
		}
		node->rmi_dev = rmi_dev;
		pr_info("APML: Registered SBRMI device at address 0x%x\n",
			rmi_dev->i3cdev ? rmi_dev->dev_static_addr : rmi_dev->client->addr);
		break;
	case APML_TSI_DEVICE:
		tsi_dev = (struct apml_sbtsi_device *)device;

		if (!tsi_dev->i3cdev && !tsi_dev->client) {
			pr_info("APML: Invalid TSI device - no I2C or I3C device\n");
			kfree(node);
			return -EINVAL;
		}
		node->tsi_dev = tsi_dev;
		pr_info("APML: Registered SBTSI device at address 0x%x\n",
			tsi_dev->i3cdev ? tsi_dev->dev_static_addr : tsi_dev->client->addr);
		break;
	default:
		kfree(node);
		return -EINVAL;
	}

	apml_add_device_node(node);
	return 0;
}

/* Register an SBRMI device */
int apml_register_sbrmi_device(struct apml_sbrmi_device *rmi_dev)
{
	return apml_register_device(rmi_dev, APML_RMI_DEVICE);
}
EXPORT_SYMBOL_GPL(apml_register_sbrmi_device);

/* Register an SBTSI device*/
int apml_register_sbtsi_device(struct apml_sbtsi_device *tsi_dev)
{
	return apml_register_device(tsi_dev, APML_TSI_DEVICE);
}
EXPORT_SYMBOL_GPL(apml_register_sbtsi_device);

/* Device unregistration function*/
void apml_unregister_device(void *device, enum apml_device_type dev_type)
{
	struct apml_device_node *node, *tmp, *found_node = NULL;
	bool found = false;

	if (!device)
		return;

	/* Find and mark device as invalid and removing */
	mutex_lock(&apml_devices_lock);
	list_for_each_entry(node, &apml_devices, list) {
		if (node->dev_type != dev_type)
			continue;

		switch (dev_type) {
		case APML_RMI_DEVICE:
			if (node->rmi_dev == (struct apml_sbrmi_device *)device)
				found = true;
			break;
		case APML_TSI_DEVICE:
			if (node->tsi_dev == (struct apml_sbtsi_device *)device)
				found = true;
			break;
		}

		if (found) {
			/* Get a reference to keep node alive during removal */
			found_node = apml_get_device_node(node);
			break;
		}
	}
	mutex_unlock(&apml_devices_lock);

	if (!found_node)
		return;

	/* Remove from list and release reference */
	mutex_lock(&apml_devices_lock);
	list_for_each_entry_safe(node, tmp, &apml_devices, list) {
		if (node == found_node) {
			list_del(&node->list);
			break;
		}
	}
	mutex_unlock(&apml_devices_lock);

	/* Release our reference - this may free the node if no other references exist */
	apml_put_device_node(found_node);
}

/* Unregister an SBRMI device */
void apml_unregister_sbrmi_device(struct apml_sbrmi_device *rmi_dev)
{
	apml_unregister_device(rmi_dev, APML_RMI_DEVICE);
}
EXPORT_SYMBOL_GPL(apml_unregister_sbrmi_device);

/* Unregister an SBTSI device */
void apml_unregister_sbtsi_device(struct apml_sbtsi_device *tsi_dev)
{
	apml_unregister_device(tsi_dev, APML_TSI_DEVICE);
}
EXPORT_SYMBOL_GPL(apml_unregister_sbtsi_device);

MODULE_AUTHOR("Akshay Gupta <akshay.gupta@amd.com>");
MODULE_AUTHOR("sathya priya kumar <SathyaPriya.K@amd.com>");
MODULE_AUTHOR("Naveenkrishna Chatradhi <naveenkrishna.chatradhi@amd.com>");
MODULE_DESCRIPTION("AMD APML Common Device Registration");
MODULE_LICENSE("GPL");
