/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#ifndef _AMD_APML_ALERT_L__
#define _AMD_APML_ALERT_L__

#include "apml_common.h"

/* struct apml_alertl_data - APML Alert_L driver data structure */
struct apml_alertl_data {
	struct device *dev;
	struct gpio_desc *alertl_gpiod;
	int irq_num;
} __packed;

#endif /*_AMD_APML_ALERT_L__*/
