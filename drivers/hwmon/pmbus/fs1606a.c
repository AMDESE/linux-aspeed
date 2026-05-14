#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>

/* FS1606A Registers */
#define FS1606A_REG_PVIN   0x0C
#define FS1606A_REG_VOUT   0x0D
#define FS1606A_REG_IOUT   0x0E
#define FS1606A_REG_TEMP   0x0F

struct fs1606a_data {
	struct i2c_client *client;
};

static int fs1606a_read_reg(struct i2c_client *client, u8 reg)
{
	u8 buf[1];
	struct i2c_msg msgs[2] = {
		{
			.addr = client->addr,
			.flags = 0, /* Write register address */
			.len = 1,
			.buf = &reg,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD, /* Read 1 byte data */
			.len = 1,
			.buf = buf,
		}
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, 2);
	if (ret == 2)
		return buf[0];

	return ret < 0 ? ret : -EIO;
}

static int fs1606a_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct fs1606a_data *data = dev_get_drvdata(dev);
	int reg_val;

	switch (type) {
	case hwmon_in:
		if (channel == 0) { /* PVIN */
			reg_val = fs1606a_read_reg(data->client, FS1606A_REG_PVIN);
			if (reg_val < 0)
				return reg_val;
			/* Resolution 1/16 V, hwmon wants mV */
			*val = (reg_val * 1000) / 16;
		} else if (channel == 1) { /* VOUT */
			reg_val = fs1606a_read_reg(data->client, FS1606A_REG_VOUT);
			if (reg_val < 0)
				return reg_val;
			/* VOUT = (reg_val * 10) + 300 mV (Standard) */
			*val = (reg_val * 10) + 300;
		} else {
			return -EOPNOTSUPP;
		}
		break;
	case hwmon_curr: /* IOUT */
		reg_val = fs1606a_read_reg(data->client, FS1606A_REG_IOUT);
		if (reg_val < 0)
			return reg_val;
		/* Resolution 1/32 A, hwmon wants mA */
		*val = (reg_val * 1000) / 32;
		break;
	case hwmon_temp: /* Temperature */
		reg_val = fs1606a_read_reg(data->client, FS1606A_REG_TEMP);
		if (reg_val < 0)
			return reg_val;
		/* Resolution 1 degC, hwmon wants millidegrees C */
		*val = reg_val * 1000;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static umode_t fs1606a_is_visible(const void *data, enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	switch (type) {
	case hwmon_in:
		if (channel == 0 || channel == 1)
			return 0444;
		break;
	case hwmon_curr:
	case hwmon_temp:
		return 0444;
	default:
		break;
	}
	return 0;
}

static const struct hwmon_channel_info *const fs1606a_info[] = {
	HWMON_CHANNEL_INFO(in, HWMON_I_INPUT, HWMON_I_INPUT),
	HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL
};

static const struct hwmon_chip_info fs1606a_chip_info = {
	.ops = &(const struct hwmon_ops){
		.is_visible = fs1606a_is_visible,
		.read = fs1606a_read,
	},
	.info = fs1606a_info,
};

static int fs1606a_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fs1606a_data *data;
	struct device *hwmon_dev;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;

	hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name,
							 data, &fs1606a_chip_info,
							 NULL);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct i2c_device_id fs1606a_id[] = {
	{ "fs1606a", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fs1606a_id);

static const struct of_device_id fs1606a_of_match[] = {
	{ .compatible = "tdk,fs1606a" },
	{ }
};
MODULE_DEVICE_TABLE(of, fs1606a_of_match);

static struct i2c_driver fs1606a_driver = {
	.driver = {
		.name = "fs1606a",
		.of_match_table = fs1606a_of_match,
	},
	.probe = fs1606a_probe,
	.id_table = fs1606a_id,
};

module_i2c_driver(fs1606a_driver);

MODULE_AUTHOR("Bobby");
MODULE_DESCRIPTION("TDK FS1606A HWMON Driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("PMBUS");
