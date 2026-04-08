// SPDX-License-Identifier: GPL-2.0-or-later
/*
* Hardware monitoring driver for MPS2869/29608
* Monolithic Power Systems VR Controllers
*
* Copyright (C) 2025 Quanta Computer lnc.
*/
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pmbus.h>
#include "pmbus.h"

#define MP2869_MFR_VOUT_SCALE_LOOP  0x29
#define MP2869_IIN_SCALE_BIT_R1    	0x53
#define MP2869_IOUT_SCALE_BIT       0x67
#define MP2869_MFR_VOUT_LOOP_CTRL_R2	0xBD

#define MP2869_MAX_PHASE_RAIL1		16
#define MP2869_MAX_PHASE_RAIL2		8

#define MP29608_MAX_PHASE_RAIL1		8
#define MP29608_MAX_PHASE_RAIL2		4

#define MP2869_PAGE_NUM			2

enum chips { mp2869, mp29608};

static const int mp2869_max_phases[][MP2869_PAGE_NUM] = {
	[mp2869] = { MP2869_MAX_PHASE_RAIL1, MP2869_MAX_PHASE_RAIL2 },
	[mp29608] = { MP29608_MAX_PHASE_RAIL1, MP29608_MAX_PHASE_RAIL2 },
};

static const struct i2c_device_id mp2869_id[] = {
	{"mp2869", mp2869},
	{"mp29608", mp29608},
	{}
};

MODULE_DEVICE_TABLE(i2c, mp2869_id);

struct mp2869_data {
	struct pmbus_driver_info info;
	int vout_gain[MP2869_PAGE_NUM];
	int curr_iin_gain[MP2869_PAGE_NUM];
	int curr_iout_gain[MP2869_PAGE_NUM];
	int power_gain[MP2869_PAGE_NUM];
	int max_phases[MP2869_PAGE_NUM];
	enum chips chip_id;
};

#define to_mp2869_data(x)	container_of(x, struct mp2869_data, info)
#define MAX_LIN_MANTISSA	(1023 * 1000)
#define MIN_LIN_MANTISSA	(511 * 1000)



// some values are SMBus LINEAR11 data which need a conversion 
static int corsairpsu_linear11_to_int(int val)
{
	const int exp = ((s16)val) >> 11;
	const int mant = (((s16)(val & 0x7ff)) << 5) >> 5;
	const int result = mant;

	return (exp >= 0) ? (result << exp) : (result >> -exp);
}

static int
mp2869_read_vout(struct i2c_client *client, struct mp2869_data *data, int page,
		 int phase, u8 reg)
{
	int ret;

	ret = pmbus_read_word_data(client, page, phase, reg);

	/* convert vout result to direct format */
	ret = (ret*1000) / data->vout_gain[page];
	
	return ret;
}



static int
mp2869_read_word_data(struct i2c_client *client, int page,
		      int phase, int reg)
{
	const struct pmbus_driver_info *info = pmbus_get_driver_info(client);
	struct mp2869_data *data = to_mp2869_data(info);
	int ret;

	switch (reg) {
	case PMBUS_READ_VOUT:
		ret = mp2869_read_vout(client, data, page, phase, reg);
		break;

	default:
		return -ENODATA;
	}

	return ret;
}

static int
mp2869_read_byte_data(struct i2c_client *client, int page, int reg)
{
	switch (reg) {
	case PMBUS_VOUT_MODE:
		/* Enforce VOUT direct format. */
		return PB_VOUT_MODE_DIRECT;
	default:
		return -ENODATA;
	}
}



static int
mp2869_voltage_vout_gain_get(struct i2c_client *client,
			      struct mp2869_data *data)
{
	int vout_gain, ret;
	//Voltage gain VOUT1
	ret = i2c_smbus_write_byte_data(client, PMBUS_PAGE, 0);
	if(ret < 0)
		return ret;

	ret = i2c_smbus_read_word_data(client,MP2869_MFR_VOUT_SCALE_LOOP);
	printk("mp2869_voltage_vout_gain_get  MP2869_MFR_VOUT_SCALE_LOOP ret =%d\n",ret);
	
	//VOUT1 scale bit is 29h page0 bit[12:10]
	ret = ret & GENMASK(12,10);
	ret = ret >> 10;

	switch (ret) {
		case 0:
			vout_gain = 160;
			break;
		case 1:
			vout_gain = 200;
			break;
		case 2:
			vout_gain = 400;
			break;
		case 3:
			vout_gain = 500;
			break;
		case 4:
			vout_gain = 1000;
			break;
		case 5:
			vout_gain = 256;
			break;
		case 6:
			vout_gain = 512;
			break;
		case 7:
			vout_gain = 1024;			
			break;
		default:
			printk("error get vout_gain in  MP2869_MFR_VOUT_SCALE_LOOP\n");
			break;
	}
	data->vout_gain[0] = vout_gain;

	//Voltage gain VOUT2
	ret = i2c_smbus_write_byte_data(client, PMBUS_PAGE, 1);
	if(ret < 0)
		return ret;	

	ret = i2c_smbus_read_word_data(client,MP2869_MFR_VOUT_SCALE_LOOP);
	printk("mp2869_voltage_vout_gain_get  MP2869_MFR_VOUT_SCALE_LOOP_R2 ret =%d\n",ret);
	
	//VOUT1 scale bit is 29h page0 bit[12:10]
	ret = ret & GENMASK(12,10);
	ret = ret >> 10;

	switch (ret) {
		case 0:
			vout_gain = 160;
			break;
		case 1:
			vout_gain = 200;
			break;
		case 2:
			vout_gain = 400;
			break;
		case 3:
			vout_gain = 500;
			break;
		case 4:
			vout_gain = 1000;
			break;
		case 5:
			vout_gain = 256;
			break;
		case 6:
			vout_gain = 512;
			break;
		case 7:
			vout_gain = 1024;			
			break;
		default:
			printk("error get vout_gain in  MP2869_MFR_VOUT_SCALE_LOOP\n");
			break;
	}
	data->vout_gain[1] = vout_gain;

	//zsjadd test
	for(int i=0;i <2;i++)
	{
		printk("data->vout_gain[%d]=%d\n",i,data->vout_gain[i]);
	}

	return 0;

}


static struct pmbus_driver_info mp2869_info = {
	.pages = MP2869_PAGE_NUM,
	.format[PSC_VOLTAGE_IN] = linear,
	.format[PSC_VOLTAGE_OUT] = direct,
	.format[PSC_TEMPERATURE] = linear,
	.format[PSC_CURRENT_IN] = linear,
	.format[PSC_CURRENT_OUT] = linear,
	.format[PSC_POWER] = linear,
	.m[PSC_VOLTAGE_OUT] = 1,
	.R[PSC_VOLTAGE_OUT] = 3,
	.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT  |
		PMBUS_HAVE_IIN | PMBUS_HAVE_IOUT |
		PMBUS_HAVE_TEMP | PMBUS_HAVE_POUT |
		PMBUS_HAVE_PIN ,
	.func[1] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT  |
		PMBUS_HAVE_IIN | PMBUS_HAVE_IOUT |
		PMBUS_HAVE_TEMP | PMBUS_HAVE_POUT ,
	.read_byte_data = mp2869_read_byte_data,
	.read_word_data = mp2869_read_word_data,
};

static int mp2869_probe(struct i2c_client *client)
{
	struct pmbus_driver_info *info;
	struct mp2869_data *data;
	int ret;

	data = devm_kzalloc(&client->dev, sizeof(struct mp2869_data),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

    data->chip_id = (enum chips)(uintptr_t)i2c_get_match_data(client);

    memcpy(data->max_phases, mp2869_max_phases[data->chip_id],
           sizeof(data->max_phases));

    memcpy(&data->info, &mp2869_info, sizeof(*info));
    info = &data->info;

	/* Get Vout stage. */
	ret = mp2869_voltage_vout_gain_get(client, data);
	if (ret)
		return ret;

	i2c_smbus_write_byte_data(client, PMBUS_PAGE, 0);

	return pmbus_do_probe(client, info);	
}

static const struct of_device_id __maybe_unused mp2869_of_match[] = {
	{.compatible = "mps,mp2869", .data = (void *)mp2869},
	{.compatible = "mps,mp29608", .data = (void *)mp29608},
	{}
};
MODULE_DEVICE_TABLE(of, mp2869_of_match);
 
static struct i2c_driver mp2869_driver = {
    .driver = {
        .name = "mp2869",
        .of_match_table = mp2869_of_match,
    },
    .probe = mp2869_probe,
    .id_table = mp2869_id,
};

module_i2c_driver(mp2869_driver);

MODULE_AUTHOR("Shaojie Zhang");
MODULE_DESCRIPTION("PMBus driver for MPS MP2869/MP29608 device");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(PMBUS);
