/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#ifndef _AMD_APML_COMMON_H_
#define _AMD_APML_COMMON_H_

#include <linux/list.h>
#include <linux/kref.h>
#include "sbrmi-common.h"
#include "sbtsi-common.h"

extern struct list_head apml_devices;
extern struct mutex apml_devices_lock;

enum apml_device_type {
	APML_RMI_DEVICE,
	APML_TSI_DEVICE,
};

struct apml_device_node {
	struct list_head list;
	struct kref ref;
	enum apml_device_type dev_type;
	union {
		struct apml_sbrmi_device *rmi_dev;
		struct apml_sbtsi_device *tsi_dev;
	};
};

/* Function declarations - available when APML_COMMON is built */
#if IS_ENABLED(CONFIG_APML_COMMON)
int apml_register_sbrmi_device(struct apml_sbrmi_device *rmi_dev);
void apml_unregister_sbrmi_device(struct apml_sbrmi_device *rmi_dev);
int apml_register_sbtsi_device(struct apml_sbtsi_device *tsi_dev);
void apml_unregister_sbtsi_device(struct apml_sbtsi_device *tsi_dev);
struct apml_device_node *apml_get_device_node(struct apml_device_node *node);
void apml_put_device_node(struct apml_device_node *node);
#else
/* Stub functions when APML_COMMON is not available */
static inline int apml_register_sbrmi_device(struct apml_sbrmi_device *rmi_dev)
{
	return 0;
}

static inline void apml_unregister_sbrmi_device(struct apml_sbrmi_device *rmi_dev)
{
	return 0;
}

static inline int apml_register_sbtsi_device(struct apml_sbtsi_device *tsi_dev)
{
	return 0;
}

static inline void apml_unregister_sbtsi_device(struct apml_sbtsi_device *tsi_dev)
{
	return 0
}

static inline struct apml_device_node *apml_get_device_node(struct apml_device_node *node)
{
	return NULL;
}

static inline void apml_put_device_node(struct apml_device_node *node)
{
	return 0;
}

#endif /* IS_ENABLED(CONFIG_APML_COMMON) */
#endif /* _AMD_APML_COMMON_H_ */
