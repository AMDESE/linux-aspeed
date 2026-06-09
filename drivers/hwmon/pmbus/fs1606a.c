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


static ssize_t fs1606a_label_show(struct device *dev,
                                  struct device_attribute *attr, char *buf);

/*
 * Per-chip VOUT telemetry parameters. VOUT (mV) is calculated from
 * register 0x0D as: reg * vout_lsb_mv + vout_offset_mv.
 *
 * FS1604/FS1606/FS1606A use a 10mV resolution with a 300mV offset, while
 * FS1006 uses a 20mV resolution with a 600mV offset (see the Telemetry
 * section of each datasheet). Using the wrong parameters on FS1006 yields
 * exactly half of the real VOUT.
 */
struct fs1606a_chip {
        int vout_lsb_mv;
        int vout_offset_mv;
};

static const struct fs1606a_chip fs1006_chip = {
        .vout_lsb_mv = 20,
        .vout_offset_mv = 600,
};

static const struct fs1606a_chip fs160x_chip = {
        .vout_lsb_mv = 10,
        .vout_offset_mv = 300,
};

struct fs1606a_data {
        struct i2c_client *client;
        const struct fs1606a_chip *chip;
};


static ssize_t fs1606a_label_show(struct device *dev,
                                  struct device_attribute *attr, char *buf)
{
        struct sensor_device_attribute *sda = to_sensor_dev_attr(attr);

        switch (sda->index) {
        case 0:
                return sysfs_emit(buf, "vin1\n");
        case 1:
                return sysfs_emit(buf, "vout1\n");
        case 2:
                return sysfs_emit(buf, "iout1\n");
        default:
                return -EINVAL;
        }
}

static SENSOR_DEVICE_ATTR(in0_label, 0444, fs1606a_label_show, NULL, 0);
static SENSOR_DEVICE_ATTR(in1_label, 0444, fs1606a_label_show, NULL, 1);
static SENSOR_DEVICE_ATTR(curr1_label, 0444, fs1606a_label_show, NULL, 2);

static struct attribute *fs1606a_attrs[] = {
        &sensor_dev_attr_in0_label.dev_attr.attr,
        &sensor_dev_attr_in1_label.dev_attr.attr,
        &sensor_dev_attr_curr1_label.dev_attr.attr,
        NULL
};


static const struct attribute_group fs1606a_group = {
        .attrs = fs1606a_attrs,
};

static const struct attribute_group *fs1606a_groups[] = {
        &fs1606a_group,
        NULL
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
                        /* VOUT = reg * vout_lsb_mv + vout_offset_mv */
                        *val = (reg_val * data->chip->vout_lsb_mv) +
                                data->chip->vout_offset_mv;
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
        data->chip = i2c_get_match_data(client);
        if (!data->chip)
                data->chip = &fs160x_chip;

        hwmon_dev = devm_hwmon_device_register_with_info(dev, client->name,
                                                         data, &fs1606a_chip_info,
                                                         fs1606a_groups);
        return PTR_ERR_OR_ZERO(hwmon_dev);
}

static const struct i2c_device_id fs1606a_id[] = {
        { "fs1006", (kernel_ulong_t)&fs1006_chip },
        { "fs1604", (kernel_ulong_t)&fs160x_chip },
        { "fs1606", (kernel_ulong_t)&fs160x_chip },
        { "fs1606a", (kernel_ulong_t)&fs160x_chip },
        { }
};
MODULE_DEVICE_TABLE(i2c, fs1606a_id);

static const struct of_device_id fs1606a_of_match[] = {
        { .compatible = "tdk,fs1006", .data = &fs1006_chip },
        { .compatible = "tdk,fs1604", .data = &fs160x_chip },
        { .compatible = "tdk,fs1606", .data = &fs160x_chip },
        { .compatible = "tdk,fs1606a", .data = &fs160x_chip },
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

MODULE_AUTHOR("PEGATRON");
MODULE_DESCRIPTION("TDK FS1606A HWMON Driver");
MODULE_LICENSE("GPL");
