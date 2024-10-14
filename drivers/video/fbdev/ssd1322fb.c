/*
 * SSD1322 Framebuffer Driver
 * --------------------------
 *
 * Filename: ssd1322fb.c
 * Version: 1.0
 * Date: 2024-08-14
 * Author: Jacob Levinson
 * Company: AMD
 * License: GPL
 *
 * Description:
 * ------------
 * This source file implements the SSD1322 framebuffer driver for a
 * monochrome OLED display. It supports 4-bit grayscale operations and
 * integrates with the system via 3-wire SPI communication.
 *
 * Features:
 * ---------
 * - 128x64 pixel resolution.
 * - 4-bit grayscale support.
 * - Basic framebuffer operations.
 * - NHD-2.7-12864WDXX
 *
 * Requirements:
 * -------------
 * - SPI interface.
 *
 * Pin Configuration:
 * ------------------
 * - SPI_MOSI: MOSI (Master Out Slave In)
 * - SPI_SCLK: SCLK (Serial Clock)
 * - SPI_CS: Chip Select
 *
 * Revision History:
 * -----------------
 * - 1.0: Initial release.
 *
 */

#include "ssd1322fb.h"

static int ssd1322_init(struct ssd1322fb_par *par)
{
	int ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_DISPLAY_ON, NULL, 0);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_COMMAND_LOCK, (u8[]){ 0x12 }, 1);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_SET_CLOCK_DIV,
			  (u8[]){ DISPLAY_CLOCK_FREQUENCY }, 1);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_SET_MULTIPLEX_RATIO,
			  (u8[]){ MULTIPLEX_RATIO }, 1);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_SET_DISPLAY_OFFSET,
			  (u8[]){ DISPLAY_OFFSET }, 1);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_FUNCTION_SELECTION,
			  (u8[]){ FUNCTION_SELECTION }, 1);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_SET_START_LINE, (u8[]){ START_LINE },
			  1);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_SET_REMAP, (u8[]){ REMAP_SETTINGS },
			  2);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_MASTER_CONTRAST,
			  (u8[]){ MASTER_CONTRAST_LEVEL }, 1);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_CONTRAST_CONTROL,
			  (u8[]){ CONTRAST_CONTROL_LEVEL }, 1);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_PHASE_LENGTH, (u8[]){ PHASE_LENGTH },
			  1);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_PRECHARGE_VOLTAGE,
			  (u8[]){ PRECHARGE_VOLTAGE_LEVEL }, 1);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_EXTERNAL_VSL, (u8[]){ EXTERNAL_VSL },
			  2);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_VCOMH_VOLTAGE,
			  (u8[]){ VCOMH_VOLTAGE_LEVEL }, 1);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_DISPLAY_MODE, NULL, 0);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_EXIT_PARTIAL_DISPLAY, NULL, 0);
	if (ret)
		return ret;

	ret = ssd1322_cmd(
		par, SSD1322_CMD_DISPLAY_ENHANCEMENT,
		(u8[]){ DISPLAY_ENHANCEMENT_A, DISPLAY_ENHANCEMENT_B }, 2);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_SET_GPIO, (u8[]){ GPIO_SETTING }, 1);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_DEFAULT_GRAYSCALE, NULL, 0);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_SECOND_PRECHARGE,
			  (u8[]){ SECOND_PRECHARGE_PERIOD }, 1);
	if (ret)
		return ret;

	ret = ssd1322_cmd(par, SSD1322_CMD_DISPLAY_ON, NULL, 0);
	if (ret)
		return ret;

	dev_info(&par->spi->dev, "ssd1322fb oled init done.\n");
	return 0;
}

static ssize_t ssd1322fb_read(struct fb_info *info, char __user *buf,
			       size_t count, loff_t *ppos)
{
	char *src;

	// Check if the position is valid
	if (*ppos >= info->fix.smem_len)
		return 0; // No more data to read

	// Adjust count if it goes beyond the end of the buffer
	if (*ppos + count > info->fix.smem_len)
		count = info->fix.smem_len - *ppos;

	// Point to the framebuffer memory
	src = (char *)info->screen_base + *ppos;

	// Copy framebuffer memory to user-space buffer
	if (copy_to_user(buf, src, count))
		return -EFAULT;

	// Update the position pointer
	*ppos += count;

	return count; // Return the number of bytes read
}

// Function that is called when data is written to the fb
static ssize_t ssd1322fb_write(struct fb_info *info, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct ssd1322fb_par *par;
	char *dst;

	// Initializing variables
	par = info->par;
	dst = (char *)info->screen_base + *ppos;

	dev_dbg(&par->spi->dev, "SPI ssd1322fb_write called!\n");
	dev_dbg(&par->spi->dev, "ppos is: %llu, count is: %zu\n", *ppos, count);

	// Check for overflow and adjust count if necessary
	if (*ppos >= info->fix.smem_len) {
		dev_err(&par->spi->dev,
			"Framebuffer write error: ppos (%llu) is beyond the framebuffer size (%u)\n",
			*ppos, info->fix.smem_len);
		return -ENOSPC; // No space left in the framebuffer
	}

	if (*ppos + count > info->fix.smem_len) {
		dev_info(
			&par->spi->dev,
			"Framebuffer write adjustment: ppos (%llu) + count (%zu) exceeds framebuffer size (%u)\n",
			*ppos, count, info->fix.smem_len);
		count = info->fix.smem_len - *ppos;
	}

	// Copy data from user space to framebuffer memory
	if (copy_from_user(dst, buf, count)) {
		return -EFAULT;
	}

	// Update the position pointer
	*ppos += count;

	// Trigger display update here
	ssd1322fb_update_display(par);

	return count; // Return the number of bytes written
}

static int ssd1322fb_update_display(struct ssd1322fb_par *par)
{
	u8 *image;
	int ret;
	int i, j;
	u8 col[2];
	u8 row[2];
	u8 *duplicated_image;
	int duplicated_size;

	// Initialize variables
	image = par->info->screen_base;
	col[0] = 0x1C; // Start column address
	col[1] = 0x5B; // End column address
	row[0] = 0x00; // Start row address
	row[1] = 0x3F; // End row address

	// Set column address
	ret = ssd1322_cmd(par, SSD1322_CMD_SET_COLUMN_ADDR, col, 2);
	if (ret) {
		return ret;
	}

	// Set row address
	ret = ssd1322_cmd(par, SSD1322_CMD_SET_ROW_ADDR, row, 2);
	if (ret) {
		return ret;
	}

	// Calculate the size for the duplicated image data
	// Image must have each nibble duplicated horizonatally
	duplicated_size = SSD1322_WIDTH * SSD1322_HEIGHT;
	duplicated_image = kmalloc(duplicated_size, GFP_KERNEL);
	if (!duplicated_image) {
		dev_err(&par->spi->dev,
			"Failed to allocate memory for duplicated image\n");
		return -ENOMEM;
	}
	// Initialize the allocated memory to 0
	memset(duplicated_image, 0, duplicated_size);

	// Duplicate and remap image data

	// For each row
	for (i = 0; i < SSD1322_HEIGHT; i++) {
		// For each column in the original image (128 columns, 64 bytes)
		for (j = 0; j < SSD1322_WIDTH / 2; j++) {
			// Get the original byte (2 pixels)
			u8 byte = image[i * SSD1322_HEIGHT + j];

			// Isolate the upper and lower nibbles
			u8 upper_nibble = (byte & 0xF0) >> 4; // Upper nibble
			u8 lower_nibble = byte & 0x0F; // Lower nibble

			// Duplicate each nibble into its own byte
			duplicated_image[i * SSD1322_WIDTH + j * 2] =
				(upper_nibble << 4) | upper_nibble;
			duplicated_image[i * SSD1322_WIDTH + j * 2 + 1] =
				(lower_nibble << 4) | lower_nibble;
		}
	}

	// Write the duplicated image data to RAM
	ret = ssd1322_cmd(par, SSD1322_CMD_WRITE_RAM, duplicated_image,
			  duplicated_size);
	kfree(duplicated_image);

	if (ret) {
		dev_err(&par->spi->dev,
			"SPI transfer for duplicated_image failed: %d\n", ret);
		return ret;
	}

	dev_dbg(&par->spi->dev,
		"SPI transfer for duplicated_image complete!\n");

	return 0;
}

static int ssd1322_cmd(struct ssd1322fb_par *par, u8 cmd, const u8 *data,
		       size_t data_len)
{
	struct spi_device *spi = par->spi;
	size_t total_bits;
	size_t total_bytes;
	u8 *tx_buf;
	int bit_offset;
	int ret;
	size_t i;
	struct spi_transfer xfer;
	struct spi_message msg;

	total_bits = (data_len + 1) * 9;
	total_bytes = (total_bits + 7) / 8; // Round up to nearest byte
	tx_buf = kmalloc(total_bytes, GFP_KERNEL);
	if (!tx_buf)
		return -ENOMEM;
	memset(tx_buf, 0, total_bytes);

	// Fill tx_buf with cmd and data

	// Add cmd (command bit is 0)
	bit_offset = 1;
	// Insert the most significant bits of the command
	tx_buf[0] |= (cmd >> bit_offset);
	// Insert the remaining bits of the command
	tx_buf[1] |= (cmd << (8 - bit_offset)) & 0xFF;

	bit_offset += 8; // 1 bit command flag + 8 bits command

	// Add data (data bit is 1)
	for (i = 0; i < data_len; i++) {
		int byte_index = bit_offset / 8;
		int bit_index = bit_offset % 8;
		// Set data/command bit to 1 for data
		tx_buf[byte_index] |= (1 << (7 - bit_index));
		if (bit_index < 7) {
			tx_buf[byte_index] |= (data[i] >> (bit_index + 1));
			if (byte_index + 1 < total_bytes) {
				tx_buf[byte_index + 1] |=
					(data[i] << (7 - bit_index)) & 0xFF;
			}
		} else {
			if (byte_index + 1 < total_bytes) {
				tx_buf[byte_index + 1] |= data[i];
			}
		}
		bit_offset += 9;
	}

	// SPI transfer setup
	spi_message_init(&msg);
	memset(&xfer, 0, sizeof(xfer)); // Initialize the spi_transfer structure
	xfer.tx_buf = tx_buf;
	xfer.len = total_bytes;
	xfer.cs_change = 0; // Ensure CS is deasserted after transfer
	spi_message_add_tail(&xfer, &msg);

	ret = spi_sync(spi, &msg);
	if (ret)
		dev_err(&spi->dev, "Failed to write to SSD1322: %d\n", ret);

	kfree(tx_buf);
	return ret;
}

// Function to set grayscale values
static int ssd1322fb_setcolreg(unsigned regno, unsigned red, unsigned green,
				unsigned blue, unsigned transp,
				struct fb_info *info)
{
	// Ensure grayscale is within range
	if (red >= SSD1322_GRAYSCALE)
		return -EINVAL;

	// Implement grayscale setting here if needed
	// For monochrome, just ensure value fits within the expected range
	return 0;
}

// Framebuffer operations structure
static struct fb_ops ssd1322fb_ops = {
	.owner = THIS_MODULE,
	.fb_setcolreg = ssd1322fb_setcolreg,
	.fb_fillrect = sys_fillrect,
	.fb_copyarea = sys_copyarea,
	.fb_imageblit = sys_imageblit,
	.fb_write = ssd1322fb_write,
	.fb_read = ssd1322fb_read,
};

// Probe function for initializing the SSD1322 driver
static int ssd1322fb_probe(struct spi_device *spi)
{
	struct fb_info *info;
	struct ssd1322fb_par *par;
	int retval;

	retval = -ENOMEM;
	info = framebuffer_alloc(sizeof(struct ssd1322fb_par), &spi->dev);
	if (!info)
		return retval;

	par = info->par;
	par->spi = spi;
	par->info = info;
	// Allocate buffer for grayscale
	par->buf = vzalloc(SSD1322_WIDTH * SSD1322_HEIGHT / 2);
	if (!par->buf)
		goto err_alloc;

	// Zero out the framebuffer memory
	memset(par->buf, 0, SSD1322_WIDTH * SSD1322_HEIGHT / 2);

	info->screen_base = par->buf;
	info->fbops = &ssd1322fb_ops;
	info->var.xres = SSD1322_WIDTH;
	info->var.yres = SSD1322_HEIGHT;
	info->var.bits_per_pixel = 4; // 4 bits per pixel for grayscale
	info->fix.line_length = SSD1322_WIDTH / 2;
	info->fix.smem_len = SSD1322_WIDTH * SSD1322_HEIGHT / 2;

	spi_set_drvdata(spi, info);

	retval = register_framebuffer(info);
	if (retval < 0)
		goto err_fb;

	dev_err(&spi->dev,
		"fb%d: %s frame buffer device, using %d KiB of video memory\n",
		info->node, info->fix.id, info->fix.smem_len >> 10);

	retval = ssd1322_init(par);
	if (retval)
		goto err_fb;

	return 0;

err_fb:
	vfree(par->buf);
err_alloc:
	framebuffer_release(info);
	return retval;
}

// Remove function for cleaning up the SSD1322 driver
static void ssd1322fb_remove(struct spi_device *spi)
{
	struct fb_info *info = spi_get_drvdata(spi);
	struct ssd1322fb_par *par = info->par;

	unregister_framebuffer(info);
	vfree(par->buf);
	framebuffer_release(info);
}

// Device tree match table
static const struct of_device_id ssd1322fb_of_match[] = {
	{
		.compatible = "ssd,ssd1322",
	},
	{}
};
MODULE_DEVICE_TABLE(of, ssd1322fb_of_match);

// SPI driver structure for the SSD1322
static struct spi_driver ssd1322fb_driver = {
    .driver = {
        .name   = "ssd1322fb",
        .owner  = THIS_MODULE,
        .of_match_table = ssd1322fb_of_match,
    },
    .probe  = ssd1322fb_probe,
    .remove = ssd1322fb_remove,
};

module_spi_driver(ssd1322fb_driver);

MODULE_DESCRIPTION("SSD1322 Framebuffer Driver");
MODULE_AUTHOR("Jacob Levinson");
MODULE_LICENSE("GPL");