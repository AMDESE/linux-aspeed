// SPDX-License-Identifier: GPL-2.0+
/**
 * Hardware monitoring driver for Infineon Integrated-pol-voltage-regulators or
 *   Digital-Multiphase-Controllers
 * Driver for TDA38725,TDA38725A,TDA38740,TDA38740A,XDPE1A2G5B,XDPE19284C,XDPE192C4B
 *
 * Copyright (c) 2024 Infineon Technologies
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regulator/driver.h>
#include <linux/delay.h>
#include "pmbus.h"

#define TDA38725_IC_DEVICE_ID	"\x92"
#define TDA38725A_IC_DEVICE_ID	"\xA9"
#define TDA38740_IC_DEVICE_ID	"\x84"
#define TDA38740A_IC_DEVICE_ID	"\xA8"
#define XDPE1A2G5B_IC_DEVICE_ID	"\x01\x9E"
#define XDPE19284C_IC_DEVICE_ID	"\x02\x98"
#define XDPE192C4B_IC_DEVICE_ID	"\x01\x99"
#define IFX_RETRY_BUS_READ	4

static const struct i2c_device_id tda38740_id[];

enum chips {
	tda38725,
	tda38725a,
	tda38740,
	tda38740a,
	xdpe1a2g5b,
	xdpe19284c,
	xdpe192c4b
};

struct tda38740_data {
	enum chips id;
	struct pmbus_driver_info info;
	u32 vout_multiplier[2];
};

#define to_tda38740_data(x)  container_of(x, struct tda38740_data, info)

static int tda38740_read_word_data(struct i2c_client *client, int page,
					int phase, int reg)
{
	const struct pmbus_driver_info *info = pmbus_get_driver_info(client);
	const struct tda38740_data *data = to_tda38740_data(info);
	int ret = 0;
	int retry = 0;

	/* Virtual PMBUS Command not supported */
	if (reg >= PMBUS_VIRT_BASE) {
		ret = -ENXIO;
		return ret;
	}

	do {
		ret = pmbus_read_word_data(client, page, phase, reg);
		if (ret == -EBADMSG || ret == -ENXIO) {
			/* PEC Error or NACK: try again */
			retry++;
			/* Sleep for an approximate time */
			usleep_range(25, 50);
			continue;
		} else {
			break;
		}

	} while (retry < IFX_RETRY_BUS_READ);

	if (ret < 0) {
		pr_warn("PMBUS READ ERROR:%d\n", ret);
		return ret;
	}

	switch (reg) {
	case PMBUS_READ_VOUT:
		ret = ((ret * data->vout_multiplier[0])/data->vout_multiplier[1]);
		break;
	default:
		break;
	}

	return ret;
}

static const struct regulator_desc __maybe_unused tda38740_reg_desc[] = {
	PMBUS_REGULATOR("vout", 0),
};

static struct pmbus_driver_info tda38740_info[] = {
	[tda38725] = {
		.pages = 1,
		.read_word_data = tda38740_read_word_data,
		.format[PSC_VOLTAGE_IN] = linear,
		.format[PSC_VOLTAGE_OUT] = linear,
		.format[PSC_CURRENT_OUT] = linear,
		.format[PSC_CURRENT_IN] = linear,
		.format[PSC_POWER] = linear,
		.format[PSC_TEMPERATURE] = linear,

		.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_STATUS_INPUT
			| PMBUS_HAVE_TEMP | PMBUS_HAVE_STATUS_TEMP
			| PMBUS_HAVE_IIN
			| PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT
			| PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT
			| PMBUS_HAVE_POUT | PMBUS_HAVE_PIN,
#if IS_ENABLED(CONFIG_SENSORS_TDA38740_REGULATOR)
		.num_regulators = 1,
		.reg_desc = tda38740_reg_desc,
#endif
	},
	[tda38725a] = {
		.pages = 1,
		.read_word_data = tda38740_read_word_data,
		.format[PSC_VOLTAGE_IN] = linear,
		.format[PSC_VOLTAGE_OUT] = linear,
		.format[PSC_CURRENT_OUT] = linear,
		.format[PSC_CURRENT_IN] = linear,
		.format[PSC_POWER] = linear,
		.format[PSC_TEMPERATURE] = linear,

		.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_STATUS_INPUT
			| PMBUS_HAVE_TEMP | PMBUS_HAVE_STATUS_TEMP
			| PMBUS_HAVE_IIN
			| PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT
			| PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT
			| PMBUS_HAVE_POUT | PMBUS_HAVE_PIN,
#if IS_ENABLED(CONFIG_SENSORS_TDA38740_REGULATOR)
		.num_regulators = 1,
		.reg_desc = tda38740_reg_desc,
#endif
	},
	[tda38740] = {
		.pages = 1,
		.read_word_data = tda38740_read_word_data,
		.format[PSC_VOLTAGE_IN] = linear,
		.format[PSC_VOLTAGE_OUT] = linear,
		.format[PSC_CURRENT_OUT] = linear,
		.format[PSC_CURRENT_IN] = linear,
		.format[PSC_POWER] = linear,
		.format[PSC_TEMPERATURE] = linear,

		.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_STATUS_INPUT
			| PMBUS_HAVE_TEMP | PMBUS_HAVE_STATUS_TEMP
			| PMBUS_HAVE_IIN
			| PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT
			| PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT
			| PMBUS_HAVE_POUT | PMBUS_HAVE_PIN,
#if IS_ENABLED(CONFIG_SENSORS_TDA38740_REGULATOR)
		.num_regulators = 1,
		.reg_desc = tda38740_reg_desc,
#endif
	},
	[tda38740a] = {
		.pages = 1,
		.read_word_data = tda38740_read_word_data,
		.format[PSC_VOLTAGE_IN] = linear,
		.format[PSC_VOLTAGE_OUT] = linear,
		.format[PSC_CURRENT_OUT] = linear,
		.format[PSC_CURRENT_IN] = linear,
		.format[PSC_POWER] = linear,
		.format[PSC_TEMPERATURE] = linear,

		.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_STATUS_INPUT
			| PMBUS_HAVE_TEMP | PMBUS_HAVE_STATUS_TEMP
			| PMBUS_HAVE_IIN
			| PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT
			| PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT
			| PMBUS_HAVE_POUT | PMBUS_HAVE_PIN,
#if IS_ENABLED(CONFIG_SENSORS_TDA38740_REGULATOR)
		.num_regulators = 1,
		.reg_desc = tda38740_reg_desc,
#endif
	},
	[xdpe1a2g5b] = {
		.pages = 2,
		.read_word_data = tda38740_read_word_data,
		.format[PSC_VOLTAGE_IN] = linear,
		.format[PSC_VOLTAGE_OUT] = linear,
		.format[PSC_TEMPERATURE] = linear,
		.format[PSC_CURRENT_IN] = linear,
		.format[PSC_CURRENT_OUT] = linear,
		.format[PSC_POWER] = linear,
		.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT |
			PMBUS_HAVE_IIN | PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT |
			PMBUS_HAVE_TEMP | PMBUS_HAVE_TEMP2 | PMBUS_HAVE_STATUS_TEMP |
			PMBUS_HAVE_POUT | PMBUS_HAVE_PIN | PMBUS_HAVE_STATUS_INPUT,
		.func[1] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT |
			PMBUS_HAVE_IIN | PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT |
			PMBUS_HAVE_TEMP | PMBUS_HAVE_TEMP2 | PMBUS_HAVE_STATUS_TEMP |
			PMBUS_HAVE_POUT | PMBUS_HAVE_PIN | PMBUS_HAVE_STATUS_INPUT,
	},
	[xdpe19284c] = {
		.pages = 2,
		.read_word_data = tda38740_read_word_data,
		.format[PSC_VOLTAGE_IN] = linear,
		.format[PSC_VOLTAGE_OUT] = linear,
		.format[PSC_TEMPERATURE] = linear,
		.format[PSC_CURRENT_IN] = linear,
		.format[PSC_CURRENT_OUT] = linear,
		.format[PSC_POWER] = linear,
		.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT |
			PMBUS_HAVE_IIN | PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT |
			PMBUS_HAVE_TEMP | PMBUS_HAVE_TEMP2 | PMBUS_HAVE_STATUS_TEMP |
			PMBUS_HAVE_POUT | PMBUS_HAVE_PIN | PMBUS_HAVE_STATUS_INPUT,
		.func[1] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT |
			PMBUS_HAVE_IIN | PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT |
			PMBUS_HAVE_TEMP | PMBUS_HAVE_TEMP2 | PMBUS_HAVE_STATUS_TEMP |
			PMBUS_HAVE_POUT | PMBUS_HAVE_PIN | PMBUS_HAVE_STATUS_INPUT,
	},
	[xdpe192c4b] = {
		.pages = 2,
		.read_word_data = tda38740_read_word_data,
		.format[PSC_VOLTAGE_IN] = linear,
		.format[PSC_VOLTAGE_OUT] = linear,
		.format[PSC_TEMPERATURE] = linear,
		.format[PSC_CURRENT_IN] = linear,
		.format[PSC_CURRENT_OUT] = linear,
		.format[PSC_POWER] = linear,
		.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT |
			PMBUS_HAVE_IIN | PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT |
			PMBUS_HAVE_TEMP | PMBUS_HAVE_TEMP2 | PMBUS_HAVE_STATUS_TEMP |
			PMBUS_HAVE_POUT | PMBUS_HAVE_PIN | PMBUS_HAVE_STATUS_INPUT,
		.func[1] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT |
			PMBUS_HAVE_IIN | PMBUS_HAVE_IOUT | PMBUS_HAVE_STATUS_IOUT |
			PMBUS_HAVE_TEMP | PMBUS_HAVE_TEMP2 | PMBUS_HAVE_STATUS_TEMP |
			PMBUS_HAVE_POUT | PMBUS_HAVE_PIN | PMBUS_HAVE_STATUS_INPUT,
	},
};

static int tda38740_get_device_id(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	u8 device_id[I2C_SMBUS_BLOCK_MAX + 1];
	enum chips id;
	int status;

	status = i2c_smbus_read_block_data(client, PMBUS_IC_DEVICE_ID, device_id);
	if (status < 0) {
		dev_err(dev, "Failed to read Device Id\n");
		return status;
	}
	if(status==1){
		pr_info("PMBUS IC DEVICE_ID: 0x%x\n", device_id[0]);
	} else if (status==2){
		pr_info("PMBUS IC DEVICE_ID: 0x%x, 0x%x\n", device_id[0], device_id[1]);
	} else {
		dev_err(&client->dev, "Unsupported device\n");
		return -ENODEV;
	}
	


	if (status == 1 && !memcmp(device_id, TDA38725_IC_DEVICE_ID, 1))
		id = tda38725;
	else if (status == 1 && !memcmp(device_id, TDA38725A_IC_DEVICE_ID, 1))
		id = tda38725a;
	else if (status == 1 && !memcmp(device_id, TDA38740_IC_DEVICE_ID, 1))
		id = tda38740;
	else if (status == 1 && !memcmp(device_id, TDA38740A_IC_DEVICE_ID, 1))
		id = tda38740a;
	else if (status == 2 && !memcmp(device_id, XDPE1A2G5B_IC_DEVICE_ID, 2))
		id = xdpe1a2g5b;
	else if (status == 2 && !memcmp(device_id, XDPE19284C_IC_DEVICE_ID, 2))
		id = xdpe19284c;
	else if (status == 2 && !memcmp(device_id, XDPE192C4B_IC_DEVICE_ID, 2))
		id = xdpe192c4b;
	else {
		dev_err(&client->dev, "Unsupported device\n");
		return -ENODEV;
	}

	return id;
}

static int tda38740_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tda38740_data *data;
	enum chips id;
	int chip_id;

	pr_info("Inside %s\n", __func__);

	/* FUNC for I2C SMBUS */
	if (!i2c_check_functionality(client->adapter,
			I2C_FUNC_SMBUS_BYTE |
			I2C_FUNC_SMBUS_BYTE_DATA |
			I2C_FUNC_SMBUS_WORD_DATA |
			I2C_FUNC_SMBUS_BLOCK_DATA |
			I2C_FUNC_SMBUS_PEC))
		pr_warn("One of the required Func not supported by I2C adapter.\n");

	chip_id = tda38740_get_device_id(client);
	if (chip_id < 0)
		return chip_id;

	if (dev->of_node)
		id = (uintptr_t)of_device_get_match_data(dev);
	else
		id = i2c_match_id(tda38740_id, client)->driver_data;

	if (chip_id != id)
		dev_warn(&client->dev, "Device mismatch: %d %d\n", id, chip_id);
	else
		pr_info("Device Match %d %d\n", id, chip_id);

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->id = chip_id;
	memcpy(&data->info, &tda38740_info[chip_id], sizeof(data->info));

	if (!of_property_read_u32_array(client->dev.of_node, "vout_multiplier",
				data->vout_multiplier, ARRAY_SIZE(data->vout_multiplier))) {
		pr_info("vout_multiplier from Device Tree:%d %d\n",
				data->vout_multiplier[0], data->vout_multiplier[1]);
	} else {
		pr_info("vout_multiplier not available from Device Tree");
		data->vout_multiplier[0] = 0x01;
		data->vout_multiplier[1] = 0x01;
		pr_info("vout_multiplier default value:%d %d\n",
				data->vout_multiplier[0], data->vout_multiplier[1]);
	}

	return pmbus_do_probe(client, &data->info);
}

static const struct i2c_device_id tda38740_id[] = {
	{"tda38725",   tda38725},
	{"tda38725a",  tda38725a},
	{"tda38740",   tda38740},
	{"tda38740a",  tda38740a},
	{"xdpe1a2g5b", xdpe1a2g5b},
	{"xdpe19284c", xdpe19284c},
	{"xdpe192c4b", xdpe192c4b},
	{}
};

MODULE_DEVICE_TABLE(i2c, tda38740_id);

static const struct of_device_id __maybe_unused tda38740_of_match[] = {
	{.compatible = "infineon,tda38725",  .data = (void *)tda38725  },
	{.compatible = "infineon,tda38725a", .data = (void *)tda38725a },
	{.compatible = "infineon,tda38740",  .data = (void *)tda38740  },
	{.compatible = "infineon,tda38740a", .data = (void *)tda38740a },
	{.compatible = "infineon,xdpe1a2g5b", .data = (void *)xdpe1a2g5b},
	{.compatible = "infineon,xdpe19284c", .data = (void *)xdpe19284c},
	{.compatible = "infineon,xdpe192c4b", .data = (void *)xdpe192c4b},
	{ }
};

MODULE_DEVICE_TABLE(of, tda38740_of_match);

/**
 *  This is the driver that will be inserted
 */
static struct i2c_driver tda38740_driver = {
	.driver = {
		.name = "tda38740",
		.of_match_table = of_match_ptr(tda38740_of_match),
	},
	.probe = tda38740_probe,
	.id_table = tda38740_id,
};

module_i2c_driver(tda38740_driver);

MODULE_AUTHOR("Ashish Yadav <Ashish.Yadav@infineon.com>");
MODULE_DESCRIPTION("PMBus driver for Infineon IPOL/DMC");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(PMBUS);
