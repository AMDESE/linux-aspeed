/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021-2022 Advanced Micro Devices, Inc.
 */

#ifndef _AMD_APML_ALERT_L__
#define _AMD_APML_ALERT_L__

#include "sbrmi-common.h"

/* APML_ALERTL send uevent to userspace for temparature
 * alert, RAS (fatal and non-fatal) error with environmental
 * data including socket_die and alertl source information.
 */

struct apml_alertl_data {
	struct apml_sbrmi_device **rmi_dev;
	struct apml_sbtsi_device **tsi_dev;
	struct device *dev;
	u8 num_of_rmi_devs;
	u8 num_of_tsi_devs;
} __packed;

#endif /*_AMD_APML_ALERT_L__*/
