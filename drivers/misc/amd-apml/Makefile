# SPDX-License-Identifier: GPL-2.0
#
# Makefile for linux/drivers/platform/x86/amd-apml
# AMD APML BMC interface drivers
#

obj-$(CONFIG_APML_COMMON)	+= apml_common.o
obj-$(CONFIG_APML_SBRMI)	+= apml_sbrmi.o
apml_sbrmi-y			:= sbrmi.o sbrmi-common.o
obj-$(CONFIG_APML_SBTSI)	+= apml_sbtsi.o
obj-$(CONFIG_APML_ALERTL)	+= apml_alertl.o
