/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright IBM Corp 2019 */

#ifndef _LINUX_ASPEED_XDMA_H_
#define _LINUX_ASPEED_XDMA_H_

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/types.h>
#include <uapi/linux/aspeed-xdma.h>

struct aspeed_xdma;

struct aspeed_xdma *aspeed_xdma_get(struct device *dev);
void aspeed_xdma_put(struct aspeed_xdma *ctx);
void *aspeed_xdma_alloc_coherent(struct aspeed_xdma *ctx, size_t size,
				 dma_addr_t *dma_handle);
void aspeed_xdma_free_coherent(struct aspeed_xdma *ctx, size_t size,
			       void *vaddr, dma_addr_t dma_handle);
int aspeed_xdma_transfer(struct aspeed_xdma *ctx, dma_addr_t bmc_addr,
			 u64 host_addr, u32 len, bool upstream,
			 void (*callback)(void *data, bool error),
			 void *callback_data);
int aspeed_xdma_transfer_sync(struct aspeed_xdma *ctx, dma_addr_t bmc_addr,
			      u64 host_addr, u32 len, bool upstream);

#endif /* _LINUX_ASPEED_XDMA_H_ */
