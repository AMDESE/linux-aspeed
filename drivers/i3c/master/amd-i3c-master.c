// SPDX-License-Identifier: GPL-2.0
/*
 * I3C master driver for the AMD I3C controller.
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */

#include <linux/platform_data/pcie-io.h>  // override iowrite32
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/i3c/master.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/pm_runtime.h>

#define PCIE_INTERFACE_SUPPORT 1
// #define DYNAMIC_ADDRESS_WORKAROUND 1
// #define RESPONSE_LENGTH_WORKAROUND 1
// Enable debug messages during initial BU
#define XI3C_DEBUG_ENABLE 1

#define XI3C_RESET_OFFSET			0x04	/* Soft Reset Register */
#define XI3C_CR_OFFSET				0x08	/* Control Register */
#define XI3C_ADDRESS_OFFSET			0x0C	/* Target Address Register */
#define XI3C_SR_OFFSET				0x10	/* Status Register */
#define XI3C_CMD_FIFO_OFFSET			0x20	/* I3C Command FIFO Register */
#define XI3C_WR_FIFO_OFFSET			0x24	/* I3C Write Data FIFO Register */
#define XI3C_RD_FIFO_OFFSET			0x28	/* I3C Read Data FIFO Register */
#define XI3C_RESP_STATUS_FIFO_OFFSET		0x2C	/* I3C Response status FIFO Register */
#define XI3C_FIFO_LVL_STATUS_OFFSET		0x30	/* I3C CMD & WR FIFO LVL Register */
#define XI3C_FIFO_LVL_STATUS_1_OFFSET		0x34	/* I3C RESP & RD FIFO LVL  Register */

#define XI3C_SCL_HIGH_TIME_OFFSET		0x38	/**< I3C SCL HIGH Register */
#define XI3C_SCL_LOW_TIME_OFFSET		0x3C	/**< I3C SCL LOW  Register */
#define XI3C_SDA_HOLD_TIME_OFFSET		0x40	/**< I3C SDA HOLD Register */
#define XI3C_BUS_IDLE_OFFSET			0x44	/**< I3C CONTROLLER BUS IDLE Register */
#define XI3C_TSU_START_OFFSET			0x48	/**< I3C START SETUP Register  */
#define XI3C_THD_START_OFFSET			0x4C	/**< I3C START HOLD Register */
#define XI3C_TSU_STOP_OFFSET			0x50	/**< I3C STOP Setup Register  */
#define XI3C_OD_SCL_HIGH_TIME_OFFSET		0x54	/**< I3C OD SCL HIGH Register */
#define XI3C_OD_SCL_LOW_TIME_OFFSET		0x58	/**< I3C OD SCL LOW  Register */
#define XI3C_PID0_OFFSET			0x6C	/* LSB 4 bytes of the PID */
#define XI3C_PID1_BCR_DCR			0x70	/* MSB 2 bytes of the PID, BCR and DCR */

 #define XI3C_CR_EN_MASK				BIT(0)	/* Core Enable */
 #define XI3C_SR_RESP_NOT_EMPTY_MASK		BIT(4)	/* Resp Fifo not empty status */

 #define XI3C_RESP_CODE_SHIFT			5

 #define XI3C_MAXDATA_LENGTH			4095
 #define XI3C_MAX_DEVS				32
 #define XI3C_DAA_SLAVEINFO_READ_BYTECOUNT	8

 #define XI3C_I2C_MODE				0
 #define XI3C_I2C_TID				0
 #define XI3C_SDR_MODE				1
 #define XI3C_SDR_TID				1

#ifdef RESPONSE_LENGTH_WORKAROUND
 #define I3C_CCC_DIRECT_GETMXDS  0x94
#endif

#ifndef PCIE_INTERFACE_SUPPORT
 #define XI3C_PM_TIMEOUT_MS			1000
#endif

 /* timeout waiting for the controller finish transfers */
 #define XI3C_XFER_TIMEOUT			(msecs_to_jiffies(1000))

 #define xi3c_wrfifolevel(master)							\
     ((u16)(ioread32((master)->membase + XI3C_FIFO_LVL_STATUS_OFFSET) & GENMASK(15, 0)))

 #define xi3c_rdfifolevel(master)							\
     ((u16)(ioread32((master)->membase + XI3C_FIFO_LVL_STATUS_1_OFFSET) & GENMASK(15, 0)))

#define ioread32_poll_timeout(addr, val, cond, delay_us, timeout_us) \
({ \
    unsigned long timeout = jiffies + usecs_to_jiffies(timeout_us); \
    for (;;) { \
        (val) = ioread32(addr); \
        if (cond) \
            break; \
        if (timeout_us && time_after(jiffies, timeout)) { \
            (val) = ioread32(addr); /* Get the last value before timeout */ \
            break; \
        } \
        if (delay_us) \
            udelay(delay_us); \
        else \
            cpu_relax(); \
    } \
    (cond) ? 0 : -ETIMEDOUT; \
})

 struct xi3c_cmd {
     u16 tx_len;
     u16 rx_len;
     void *tx_buf;
     void *rx_buf;
     u8 addr;
     u8 type;
     u8 tid;
     bool rnw;
     bool is_daa;
     bool continued;
 };

 struct xi3c_xfer {
     struct list_head node;
     struct completion comp;
     int ret;
     unsigned int ncmds;
     struct xi3c_cmd cmds[] __counted_by(ncmds);
 };


 /**
  * struct xi3c_master - I3C Master structure
  * @base: I3C master controller
  * @dev: Pointer to device structure
  * @xferqueue: Transfer queue structure
  * @xferqueue.list: List member
  * @xferqueue.cur: Current ongoing transfer
  * @xferqueue.lock: Queue lock
  * @membase: Memory base of the HW registers
  * @pclk: Input clock
  * @lock: Transfer lock
  * @pid_bcr_dcr: Poniter to PID, BCR and DCR value
  * @num_targets: Number of i3c target devices
  * @addrs: Slave addresses array
  */
 struct xi3c_master {
     struct i3c_master_controller base;
     struct device *dev;
     struct {
         struct list_head list;
         struct xi3c_xfer *cur;
         /* Queue lock */
         struct mutex lock;
     } xferqueue;
     void __iomem *membase;
     struct clk *pclk;
     /* Transfer lock */
     struct mutex lock;
     u64 *pid_bcr_dcr;
     u32 num_targets;
     u32 axi_clk_freq;
     u32 scl_clk_freq;
     u32 od_scl_high_time;
     u32 od_scl_low_time;
     u8 addrs[] __counted_by(num_targets);
 };

 static inline struct xi3c_master *
 to_xi3c_master(struct i3c_master_controller *master)
 {
     return container_of(master, struct xi3c_master, base);
 }

 static int xi3c_get_response(struct xi3c_master *master)
 {
     int ret;
     u32 resp_reg, response_data;

     ret = ioread32_poll_timeout(master->membase + XI3C_SR_OFFSET,
                  resp_reg,
                  resp_reg & XI3C_SR_RESP_NOT_EMPTY_MASK,
                  0, 1000);
     if (ret) {
         dev_err(master->dev, "AXI I3C response timeout\n");
         return ret;
     }

     response_data = ioread32(master->membase + XI3C_RESP_STATUS_FIFO_OFFSET);

     /* Return response code */
     return  (response_data & GENMASK(8, 5)) >> XI3C_RESP_CODE_SHIFT;
 }

 static void xi3c_master_wr_to_tx_fifo(struct xi3c_master *master, struct xi3c_cmd *cmd)
 {
     u32 data = 0;
     u8 *tx_buf = (u8 *)cmd->tx_buf;

    if (cmd->tx_len > 3) {
         memcpy(&data, tx_buf, 4);
         tx_buf += 4;
         cmd->tx_len -= 4;
     } else {
         if (cmd->tx_len > 0)  {
             memcpy(&data, tx_buf, cmd->tx_len);
             tx_buf += cmd->tx_len;
			 cmd->tx_len = 0;
         }
     }
     cmd->tx_buf = tx_buf;

     /* Write the 32-bit value to the Tx FIFO */
     iowrite32be(data, master->membase + XI3C_WR_FIFO_OFFSET);
 }

 static void xi3c_master_rd_from_rx_fifo(struct xi3c_master *master, struct xi3c_cmd *cmd)
 {
     u32 data;
     u8 *rx_buf = (u8 *)cmd->rx_buf;

    /* Read from Rx FIFO */
     data = ioread32be(master->membase + XI3C_RD_FIFO_OFFSET);

     /* Data extraction to rx_buf */
     if (cmd->rx_len > 3) {
         memcpy(rx_buf, &data, 4);
         rx_buf += 4;
         cmd->rx_len -= 4;
     } else {
         if (cmd->rx_len > 0) {
            memcpy(rx_buf, &data, cmd->rx_len);
			rx_buf += cmd->rx_len;
			cmd->rx_len = 0;
         }
     }
     cmd->rx_buf = rx_buf;
 }

 static void xi3c_master_write_to_cmdfifo(struct xi3c_master *master, struct xi3c_cmd *cmd, u16 len)
 {
     u32 transfer_cmd = 0;
     u8 addr;

     addr = ((cmd->addr & GENMASK(6, 0)) << 1) | (cmd->rnw & BIT(0));

     transfer_cmd = cmd->type & GENMASK(3, 0);
     transfer_cmd |= (u32)(!cmd->continued)  << 4;
     transfer_cmd |= (u32)(addr) << 8;
     transfer_cmd |= (u32)(cmd->tid & GENMASK(3, 0)) << 28;

     /* For dynamic addressing, an additional 1-byte length must be added
      * to the command FIFO to account for the address present in the TX FIFO.
      */
       if(cmd->is_daa) {
        xi3c_master_wr_to_tx_fifo(master, cmd);
        len = len + 1;
        cmd->is_daa = false;
     }

     transfer_cmd |= (u32)(len & GENMASK(11, 0)) << 16;
     iowrite32(transfer_cmd, master->membase + XI3C_CMD_FIFO_OFFSET);
 }

 static void xi3c_master_enable(struct xi3c_master *master)
 {
    iowrite32(ioread32(master->membase + XI3C_CR_OFFSET) | XI3C_CR_EN_MASK,
            master->membase + XI3C_CR_OFFSET);
 }

 static void xi3c_master_disable(struct xi3c_master *master)
 {
     iowrite32(ioread32(master->membase + XI3C_CR_OFFSET) & (~XI3C_CR_EN_MASK),
            master->membase + XI3C_CR_OFFSET);
 }

 static void xi3c_master_init(struct xi3c_master *master)
 {
     u32 data;

    /* Reset fifos */
     data = ioread32(master->membase + XI3C_RESET_OFFSET);
     data |= GENMASK(4, 1);
     iowrite32(data, master->membase + XI3C_RESET_OFFSET);
     usleep_range(1, 2);
     data &= ~GENMASK(4, 1);
     iowrite32(data, master->membase + XI3C_RESET_OFFSET);
     usleep_range(1, 2);

#ifdef PCIE_INTERFACE_SUPPORT
     // Reset controller
     data = ioread32(master->membase + XI3C_RESET_OFFSET);
     data |= 0x01;
     iowrite32(data, master->membase + XI3C_RESET_OFFSET);
     usleep_range(1, 2);
     data = ioread32(master->membase + XI3C_RESET_OFFSET);
     data &= ~0x1;
     iowrite32(data, master->membase + XI3C_RESET_OFFSET);
     usleep_range(1, 2);
#endif

     /* Enable controller */
     xi3c_master_enable(master);
 }

 static struct xi3c_xfer *
 xi3c_master_alloc_xfer(struct xi3c_master *master, unsigned int ncmds)
 {
     struct xi3c_xfer *xfer;

     xfer = kzalloc(struct_size(xfer, cmds, ncmds), GFP_KERNEL);
     if (!xfer)
         return NULL;

     INIT_LIST_HEAD(&xfer->node);
     xfer->ncmds = ncmds;
     xfer->ret = -ETIMEDOUT;

     return xfer;
 }

 static void xi3c_master_free_xfer(struct xi3c_xfer *xfer)
 {
     kfree(xfer);
 }

 static u8 xi3c_even_parity(u8 p)
 {
     p ^= p >> 4;
     p &= 0xf;

     return (0x9669 >> p) & 1;
 }

 static int xi3c_master_read(struct xi3c_master *master, struct xi3c_cmd *cmd)
 {
     u16 rx_data_available;
     u16 data_index;
     unsigned long timeout;
     u16 len;

     if (!cmd->rx_buf || cmd->rx_len > XI3C_MAXDATA_LENGTH) {
        return -EINVAL;
     }

     /* Fill command fifo */
    xi3c_master_write_to_cmdfifo(master, cmd, cmd->rx_len);

     timeout = jiffies + XI3C_XFER_TIMEOUT;
     len = cmd->rx_len;
     /* Read data from rx fifo */
     while (cmd->rx_len > 0){
         if (time_after(jiffies, timeout)) {
                         dev_err(master->dev, "XI3C read timeout\n");
                         goto err_read;
                 }

         rx_data_available = xi3c_rdfifolevel(master);
         for (data_index = 0;
            data_index < rx_data_available && cmd->rx_len > 0;
            data_index++) {
             xi3c_master_rd_from_rx_fifo(master, cmd);
         }
     }
     return 0;

 err_read:
        return -EIO;
 }

 static int xi3c_master_write(struct xi3c_master *master, struct xi3c_cmd *cmd)
 {
     u16 wrfifo_space;
     u16 space_index;
     u16 len;
     unsigned long timeout;

     if (!cmd->tx_buf || cmd->tx_len > XI3C_MAXDATA_LENGTH) {
        return -EINVAL;
     }
     len = cmd->tx_len;

     /* Fill Tx fifo */
     wrfifo_space = xi3c_wrfifolevel(master);
     for (space_index = 0; space_index < wrfifo_space && cmd->tx_len > 0; space_index++)
         xi3c_master_wr_to_tx_fifo(master, cmd);

     /* Write to command fifo */
     xi3c_master_write_to_cmdfifo(master, cmd, len);

     timeout = jiffies + XI3C_XFER_TIMEOUT;
     /* Fill if any remaining data to tx fifo */
     while (cmd->tx_len > 0){
         if (time_after(jiffies, timeout)) {
             dev_err(master->dev, "XI3C write timeout\n");
             goto err_write;
         }

         wrfifo_space = xi3c_wrfifolevel(master);
         for (space_index = 0; space_index < wrfifo_space && cmd->tx_len > 0; space_index++)
             xi3c_master_wr_to_tx_fifo(master, cmd);
     }

     return 0;

 err_write:
     return -EIO;
 }

 static int xi3c_master_xfer(struct xi3c_master *master, struct xi3c_cmd *cmd)
 {
     int ret;

     if (cmd->rnw) {
         ret = xi3c_master_read(master, cmd);
     }
     else {
         ret = xi3c_master_write(master, cmd);
     }

     if (ret < 0) {
        goto err_xfer_out;
     }

     if (xi3c_get_response(master)) {
        goto err_xfer_out;
     }

     return 0;

 err_xfer_out:
    xi3c_master_init(master);
     return -EIO;
 }

 static void xi3c_master_dequeue_xfer_locked(struct xi3c_master *master,
                         struct xi3c_xfer *xfer)
 {
     if (master->xferqueue.cur == xfer)
         master->xferqueue.cur = NULL;
     else
         list_del_init(&xfer->node);
 }

 static void xi3c_master_dequeue_xfer(struct xi3c_master *master,
                      struct xi3c_xfer *xfer)
 {
     mutex_lock(&master->xferqueue.lock);
     xi3c_master_dequeue_xfer_locked(master, xfer);
     mutex_unlock(&master->xferqueue.lock);
 }

 static void xi3c_master_start_xfer_locked(struct xi3c_master *master)
 {
     struct xi3c_xfer *xfer = master->xferqueue.cur;
     int ret, i;

     if (!xfer) {
        return;
     }

     for (i = 0; i < xfer->ncmds; i++) {
         struct xi3c_cmd *cmd = &xfer->cmds[i];

         ret = xi3c_master_xfer(master, cmd);
         if (ret)
             break;
     }

     xfer->ret = ret;
     complete(&xfer->comp);

     xfer = list_first_entry_or_null(&master->xferqueue.list,
                     struct xi3c_xfer,
                     node);
     if (xfer)
         list_del_init(&xfer->node);

     master->xferqueue.cur = xfer;
     xi3c_master_start_xfer_locked(master);
 }

 static void xi3c_master_enqueue_xfer(struct xi3c_master *master,
                      struct xi3c_xfer *xfer)
 {
#ifndef PCIE_INTERFACE_SUPPORT
    int ret;

     ret = pm_runtime_resume_and_get(master->dev);

     if (ret < 0) {
         dev_err(master->dev, "<%s> Cannot get runtime PM.\n", __func__);
         return;
     }
#endif
     init_completion(&xfer->comp);
     mutex_lock(&master->xferqueue.lock);
     if (master->xferqueue.cur) {
         list_add_tail(&xfer->node, &master->xferqueue.list);
     } else {
         master->xferqueue.cur = xfer;
         xi3c_master_start_xfer_locked(master);
     }
     mutex_unlock(&master->xferqueue.lock);

#ifndef PCIE_INTERFACE_SUPPORT
     pm_runtime_mark_last_busy(master->dev);
     pm_runtime_put_autosuspend(master->dev);
#endif
 }

#ifndef DYNAMIC_ADDRESS_WORKAROUND
 static u64 xi3c_swap_pid_bytes(const u8 *pid_buf)
 {
     u64 pid = 0;
     int i;

     for (i = 0; i < 6; i++) {
         pid <<= 8;
         pid |= pid_buf[i];
     }

     return pid;
 }
#endif

 static int xi3c_master_do_daa(struct i3c_master_controller *m)
 {
     struct xi3c_master *master = to_xi3c_master(m);
     struct xi3c_xfer *xfer;
     struct xi3c_cmd *daa_cmd;
     int ret, i, timeout;
     u8 pid_bufs[XI3C_MAX_DEVS][XI3C_DAA_SLAVEINFO_READ_BYTECOUNT];
     u8 *pid_buf;
     u8 addr, data, last_addr = 0;
#ifdef DYNAMIC_ADDRESS_WORKAROUND
     bool masterinit = 0;
#endif

#ifdef XI3C_DEBUG_ENABLE
    dev_info(master->dev, "xi3c_master_do_daa: Start");
#endif

     if (master->num_targets == 0 || master->num_targets > XI3C_MAX_DEVS) {
         dev_err(master->dev, "Invalid / No target devices connected\n");
         return -EIO;
     }

     master->pid_bcr_dcr = kcalloc(master->num_targets, sizeof(u64), GFP_KERNEL);
     if (!master->pid_bcr_dcr) {
        return -ENOMEM;
     }

     xfer = xi3c_master_alloc_xfer(master, master->num_targets + 1);
     if (!xfer) {
         ret = -ENOMEM;
         goto err_daa_mem;
     }

     /* Fill ENTDAA CCC */
     data = I3C_CCC_ENTDAA;
     daa_cmd = &xfer->cmds[0];
     daa_cmd->addr = I3C_BROADCAST_ADDR;
     daa_cmd->rnw = 0;
     daa_cmd->tx_buf = &data;
     daa_cmd->tx_len = 1;
     daa_cmd->type = XI3C_SDR_MODE;
     daa_cmd->tid = XI3C_SDR_TID;
     daa_cmd->continued = true;

      for (i = 1; i < master->num_targets + 1; i++) {
         struct xi3c_cmd *cmd = &xfer->cmds[i];

         pid_buf = pid_bufs[i - 1];

         addr = i3c_master_get_free_addr(m, last_addr + 1);
         if (addr < 0) {
             ret = -ENOSPC;
             goto err_daa;
         }
         last_addr = addr;
         master->addrs[i - 1] = addr;
         addr = (addr << 1) | xi3c_even_parity(addr);

         cmd->tx_buf = &addr;
         cmd->tx_len = 1;
         cmd->addr = I3C_BROADCAST_ADDR;
         cmd->rnw = 1;
         cmd->rx_buf = pid_buf;
         cmd->rx_len = XI3C_DAA_SLAVEINFO_READ_BYTECOUNT;
         cmd->is_daa = true;
         cmd->type = XI3C_SDR_MODE;
         cmd->tid = XI3C_SDR_TID;
         cmd->continued = i < master->num_targets;
     }


     mutex_lock(&master->lock);

     xi3c_master_enqueue_xfer(master, xfer);

     timeout = wait_for_completion_timeout(&xfer->comp,
					      msecs_to_jiffies(1000 * master->num_targets));
	if (!timeout) {
        ret = -ETIMEDOUT;
        }
	else {
		ret = xfer->ret;
    }

    if (ret)
		xi3c_master_dequeue_xfer(master, xfer);


     mutex_unlock(&master->lock);
#ifdef DYNAMIC_ADDRESS_WORKAROUND
     if (ret)
        masterinit = 1;
#endif

#ifndef DYNAMIC_ADDRESS_WORKAROUND
     if (ret)
         goto err_daa;


     for (i = 0; i < master->num_targets; i++) {
         ret = i3c_master_add_i3c_dev_locked(m, master->addrs[i]);

         if (ret) {
             goto err_daa;
         }

         master->pid_bcr_dcr[i] = xi3c_swap_pid_bytes(pid_bufs[i]);
         dev_info(master->dev, "Client %d: PID: 0x%llx\n", i, master->pid_bcr_dcr[i]);
     }
#endif

     kfree(master->pid_bcr_dcr);
     if(ret)  {
#ifdef DYNAMIC_ADDRESS_WORKAROUND
        if (masterinit == 1)
#endif
            xi3c_master_init(master);
     }
     xi3c_master_free_xfer(xfer);
#ifdef XI3C_DEBUG_ENABLE
    dev_info(master->dev, "xi3c_master_do_daa: Success");
#endif
     return 0;

 err_daa:
     xi3c_master_init(master);
     xi3c_master_free_xfer(xfer);
 err_daa_mem:
     kfree(master->pid_bcr_dcr);
#ifdef XI3C_DEBUG_ENABLE
    dev_err(master->dev, "xi3c_master_do_daa: Error %x", ret);
#endif
     return -EIO;
 }

 static bool
 xi3c_master_supports_ccc_cmd(struct i3c_master_controller *master,
                  const struct i3c_ccc_cmd *cmd)
 {
     if (cmd->ndests > 1)
         return false;

     switch (cmd->id) {
     case I3C_CCC_ENEC(true):
     case I3C_CCC_ENEC(false):
     case I3C_CCC_DISEC(true):
     case I3C_CCC_DISEC(false):
     case I3C_CCC_ENTAS(0, true):
     case I3C_CCC_ENTAS(0, false):
     case I3C_CCC_RSTDAA(true):
     case I3C_CCC_RSTDAA(false):
     case I3C_CCC_ENTDAA:
     case I3C_CCC_SETMWL(true):
     case I3C_CCC_SETMWL(false):
     case I3C_CCC_SETMRL(true):
     case I3C_CCC_SETMRL(false):
     case I3C_CCC_ENTHDR(0):
     case I3C_CCC_SETDASA:
     case I3C_CCC_SETNEWDA:
     case I3C_CCC_GETMWL:
     case I3C_CCC_GETMRL:
     case I3C_CCC_GETPID:
     case I3C_CCC_GETBCR:
     case I3C_CCC_GETDCR:
     case I3C_CCC_GETSTATUS:
     case I3C_CCC_GETMXDS:
         return true;
     default:
         return false;
     }
 }

 static int xi3c_master_send_bdcast_ccc_cmd(struct xi3c_master *master,
                        struct i3c_ccc_cmd *ccc)
 {
     u16 xfer_len = ccc->dests[0].payload.len + 1;
     struct xi3c_xfer *xfer;
     struct xi3c_cmd *cmd;
     int ret, timeout;
     u8 *buf;

     xfer = xi3c_master_alloc_xfer(master, 1);
     if (!xfer) {
        return -ENOMEM;
     }

     buf = kmalloc(xfer_len, GFP_KERNEL);
     if (!buf) {
         xi3c_master_free_xfer(xfer);
         return -ENOMEM;
     }

     buf[0] = ccc->id;
     memcpy(&buf[1], ccc->dests[0].payload.data, ccc->dests[0].payload.len);

     cmd = &xfer->cmds[0];
     cmd->addr = ccc->dests[0].addr;
     cmd->rnw = ccc->rnw;
     cmd->tx_buf = buf;
     cmd->tx_len = xfer_len;
     cmd->type = XI3C_SDR_MODE;
     cmd->tid = XI3C_SDR_TID;
     cmd->continued = false;

     mutex_lock(&master->lock);
     xi3c_master_enqueue_xfer(master, xfer);
     timeout = wait_for_completion_timeout(&xfer->comp, msecs_to_jiffies(1000));
	if (!timeout)
		ret = -ETIMEDOUT;
	else
		ret = xfer->ret;


	if (ret)
		xi3c_master_dequeue_xfer(master, xfer);
     mutex_unlock(&master->lock);
     kfree(buf);

     xi3c_master_free_xfer(xfer);

     return ret;
 }

 static int xi3c_master_send_direct_ccc_cmd(struct xi3c_master *master,
                        struct i3c_ccc_cmd *ccc)
 {
     struct xi3c_xfer *xfer;
     struct xi3c_cmd *cmd;
     int ret, timeout;

     xfer = xi3c_master_alloc_xfer(master, 2);
     if (!xfer) {
        return -ENOMEM;
     }

     /* Broadcasted message */
     cmd = &xfer->cmds[0];
     cmd->addr = I3C_BROADCAST_ADDR;
     cmd->rnw = 0;
     cmd->tx_buf = &ccc->id;
     cmd->tx_len = 1;
     cmd->type = XI3C_SDR_MODE;
     cmd->tid = XI3C_SDR_TID;
     cmd->continued = true;

     /* Directed message */
     cmd = &xfer->cmds[1];
     cmd->addr = ccc->dests[0].addr;
     cmd->rnw = ccc->rnw;
     if (cmd->rnw) {
         cmd->rx_buf = ccc->dests[0].payload.data;
         cmd->rx_len = ccc->dests[0].payload.len;
#ifdef RESPONSE_LENGTH_WORKAROUND
         if(ccc->id == I3C_CCC_DIRECT_GETMXDS)
            cmd->rx_len = 2;
#endif
     } else {
         cmd->tx_buf = ccc->dests[0].payload.data;
         cmd->tx_len = ccc->dests[0].payload.len;
     }
     cmd->type = XI3C_SDR_MODE;
     cmd->tid = XI3C_SDR_TID;
     cmd->continued = false;

     mutex_lock(&master->lock);
     xi3c_master_enqueue_xfer(master, xfer);
     timeout = wait_for_completion_timeout(&xfer->comp, msecs_to_jiffies(1000));
	if (!timeout)
		ret = -ETIMEDOUT;
	else
		ret = xfer->ret;

	if (ret)
		xi3c_master_dequeue_xfer(master, xfer);
     mutex_unlock(&master->lock);

     xi3c_master_free_xfer(xfer);

     return ret;
 }

 static int xi3c_master_send_ccc_cmd(struct i3c_master_controller *m,
                     struct i3c_ccc_cmd *cmd)
 {
     struct xi3c_master *master = to_xi3c_master(m);
     bool broadcast = cmd->id < 0x80;
     int ret;

     if (broadcast) {
         ret = xi3c_master_send_bdcast_ccc_cmd(master, cmd);
     }
     else {
         ret = xi3c_master_send_direct_ccc_cmd(master, cmd);
     }
#ifdef XI3C_DEBUG_ENABLE
    dev_info(master->dev, "xi3c_master_send_ccc_cmd: cmd %x status %x", cmd->id, ret);
#endif

     return ret;
 }

 static int xi3c_master_priv_xfers(struct i3c_dev_desc *dev,
                   struct i3c_priv_xfer *xfers,
                   int nxfers)
 {
     struct i3c_master_controller *m = i3c_dev_get_master(dev);
     struct xi3c_master *master = to_xi3c_master(m);
     struct xi3c_xfer *xfer;
     int ret, i, timeout;

     if (!nxfers)
         return 0;

     xfer = xi3c_master_alloc_xfer(master, nxfers);
     if (!xfer)
         return -ENOMEM;

     for (i = 0; i < nxfers; i++) {
         struct xi3c_cmd *cmd = &xfer->cmds[i];

         cmd->addr = dev->info.dyn_addr;
         cmd->rnw = xfers[i].rnw;

         if (cmd->rnw) {
             cmd->rx_buf = xfers[i].data.in;
             cmd->rx_len = xfers[i].len;
         } else {
             cmd->tx_buf = (void *)xfers[i].data.out;
             cmd->tx_len = xfers[i].len;
         }

         cmd->type = XI3C_SDR_MODE;
         cmd->tid = XI3C_SDR_TID;
         cmd->continued = (i + 1) < nxfers;
     }

     mutex_lock(&master->lock);
     xi3c_master_enqueue_xfer(master, xfer);

     timeout = wait_for_completion_timeout(&xfer->comp, msecs_to_jiffies(1000 * nxfers));
	if (!timeout)
		ret = -ETIMEDOUT;
	else
		ret = xfer->ret;

	if (ret)
		xi3c_master_dequeue_xfer(master, xfer);
     mutex_unlock(&master->lock);

     xi3c_master_free_xfer(xfer);

     return ret;
 }

 static int xi3c_master_i2c_xfers(struct i2c_dev_desc *dev,
                  const struct i2c_msg *xfers,
                  int nxfers)
 {
     struct i3c_master_controller *m = i2c_dev_get_master(dev);
     struct xi3c_master *master = to_xi3c_master(m);
     struct xi3c_xfer *xfer;
     int ret, i, timeout;

     if (!nxfers)
         return 0;

     xfer = xi3c_master_alloc_xfer(master, nxfers);
     if (!xfer)
         return -ENOMEM;

     for (i = 0; i < nxfers; i++) {
         struct xi3c_cmd *cmd = &xfer->cmds[i];

         cmd->addr = xfers[i].addr & GENMASK(6, 0);
         cmd->rnw = xfers[i].flags & I2C_M_RD;

         if (cmd->rnw) {
             cmd->rx_buf = xfers[i].buf;
             cmd->rx_len = xfers[i].len;
         } else {
             cmd->tx_buf = (void *)xfers[i].buf;
             cmd->tx_len = xfers[i].len;
         }

         cmd->type = XI3C_I2C_MODE;
         cmd->tid = XI3C_I2C_TID;
         cmd->continued = (i + 1) < nxfers;
     }

     mutex_lock(&master->lock);
     xi3c_master_enqueue_xfer(master, xfer);

     timeout = wait_for_completion_timeout(&xfer->comp, msecs_to_jiffies(1000 * nxfers));
	if (!timeout)
		ret = -ETIMEDOUT;
	else
		ret = xfer->ret;

	if (ret)
		xi3c_master_dequeue_xfer(master, xfer);
     mutex_unlock(&master->lock);

     xi3c_master_free_xfer(xfer);

     return ret;
 }

 static int xi3c_master_bus_init(struct i3c_master_controller *m)
 {
     struct xi3c_master *master = to_xi3c_master(m);
     struct i3c_bus *bus = i3c_master_get_bus(m);
     struct i3c_device_info info = { };
     int ret;
     u64 pid1_bcr_dcr;

#ifndef PCIE_INTERFACE_SUPPORT
     ret = pm_runtime_resume_and_get(master->dev);
     if (ret < 0) {
         dev_err(master->dev,
                 "<%s> cannot resume i3c bus master, err: %d\n", __func__, ret);
         return ret;
     }
#endif

     if (bus->mode != I3C_BUS_MODE_PURE) {
         ret = -EINVAL;
         goto rpm_out;
     }

     /* Get an address for the master. */
     ret = i3c_master_get_free_addr(m, 0);
     if (ret < 0) {
         goto rpm_out;
     }

     info.dyn_addr = (u8)ret;

     /*
      * Write the dynamic address value to the address register
      */
     iowrite32(info.dyn_addr, master->membase + XI3C_ADDRESS_OFFSET);
#ifdef PCIE_INTERFACE_SUPPORT
     // Open Drain timings
     if(master->od_scl_high_time)
        iowrite32(master->od_scl_high_time, master->membase + XI3C_OD_SCL_HIGH_TIME_OFFSET);
    if(master->od_scl_low_time)
        iowrite32(master->od_scl_low_time, master->membase + XI3C_OD_SCL_LOW_TIME_OFFSET);
#endif

     /*
      * Read PID, BCR and DCR values, and assign to i3c device info
      */
     pid1_bcr_dcr = ioread32(master->membase + XI3C_PID1_BCR_DCR);

     info.pid = (((pid1_bcr_dcr & GENMASK(15, 0)) << 32) |
		    ioread32(master->membase + XI3C_PID0_OFFSET));
     info.bcr = (u8)((pid1_bcr_dcr & GENMASK(23, 16)) >> 16);
     info.dcr = (u8)((pid1_bcr_dcr & GENMASK(31, 24)) >> 24);

     ret = i3c_master_set_info(&master->base, &info);

     if (ret) {
         goto rpm_out;
     }
     xi3c_master_init(master);


 rpm_out:
#ifndef PCIE_INTERFACE_SUPPORT
     pm_runtime_mark_last_busy(master->dev);
     pm_runtime_put_autosuspend(master->dev);
#endif

    return ret;
 }

 static void xi3c_master_bus_cleanup(struct i3c_master_controller *m)
 {
     struct xi3c_master *master = to_xi3c_master(m);
#ifndef PCIE_INTERFACE_SUPPORT
     int ret;

     ret = pm_runtime_resume_and_get(master->dev);
     if (ret < 0) {
         dev_err(master->dev, "<%s> Cannot get runtime PM.\n", __func__);
         return;
     }
#endif

     xi3c_master_disable(master);
#ifndef PCIE_INTERFACE_SUPPORT
     pm_runtime_mark_last_busy(master->dev);
     pm_runtime_put_autosuspend(master->dev);
#endif
 }

 static const struct i3c_master_controller_ops xi3c_master_ops = {
     .bus_init = xi3c_master_bus_init,
     .bus_cleanup = xi3c_master_bus_cleanup,
     .do_daa = xi3c_master_do_daa,
     .supports_ccc_cmd = xi3c_master_supports_ccc_cmd,
     .send_ccc_cmd = xi3c_master_send_ccc_cmd,
     .priv_xfers = xi3c_master_priv_xfers,
     .i2c_xfers = xi3c_master_i2c_xfers,
 };

 static const struct of_device_id xi3c_master_of_ids[] = {
     { .compatible = "xlnx,axi-i3c-1.0" },
     { },
 };

 static int xi3c_master_probe(struct platform_device *pdev)
 {
     struct xi3c_master *master;
     int ret;

     master = devm_kzalloc(&pdev->dev, sizeof(*master), GFP_KERNEL);
     if (!master) {
        return -ENOMEM;
     }

     master->membase = devm_platform_ioremap_resource(pdev, 0);
     if (IS_ERR(master->membase)) {
         return PTR_ERR(master->membase);
     }

     ret = of_property_read_u32(pdev->dev.of_node, "xlnx,num-targets", &master->num_targets);
     if (ret) {
         dev_err(&pdev->dev, "Failed to read xlnx,num-targets\n");
         return ret;
     }

     ret = of_property_read_u32(pdev->dev.of_node, "xlnx,axi-clk-freq", &master->axi_clk_freq);
     if (ret) {
         dev_info(&pdev->dev, "Failed to read xlnx,axi_clk_freq\n");
     }

     ret = of_property_read_u32(pdev->dev.of_node, "xlnx,scl-clk-freq", &master->scl_clk_freq);
     if (ret) {
         dev_info(&pdev->dev, "Failed to read xlnx,scl-clk-freq\n");
     }
     ret = of_property_read_u32(pdev->dev.of_node, "xlnx,od-scl-high-time", &master->od_scl_high_time);
     if (ret) {
         dev_info(&pdev->dev, "Failed to read xlnx,od-scl-high-time\n");
         master->od_scl_high_time = 0;
     }
     ret = of_property_read_u32(pdev->dev.of_node, "xlnx,od-scl-low-time", &master->od_scl_low_time);
     if (ret) {
         dev_info(&pdev->dev, "Failed to read xlnx,od-scl-low-time\n");
         master->od_scl_low_time = 0;
     }

#ifndef PCIE_INTERFACE_SUPPORT
     master->pclk = devm_clk_get_enabled(&pdev->dev, "s_axi_aclk");
     if (IS_ERR(master->pclk)) {
         ret = PTR_ERR(master->pclk);
         dev_err(&pdev->dev, "Failed to get and enable clock: %d\n", ret);
         return ret;
     }
#endif

     master->dev = &pdev->dev;

     mutex_init(&master->lock);
     mutex_init(&master->xferqueue.lock);
     INIT_LIST_HEAD(&master->xferqueue.list);

     platform_set_drvdata(pdev, master);
#ifndef PCIE_INTERFACE_SUPPORT
     pm_runtime_set_autosuspend_delay(&pdev->dev, XI3C_PM_TIMEOUT_MS);
     pm_runtime_use_autosuspend(&pdev->dev);
     pm_runtime_get_noresume(&pdev->dev);
     pm_runtime_set_active(&pdev->dev);
     pm_runtime_enable(&pdev->dev);
#endif
     ret = i3c_master_register(&master->base, &pdev->dev,
                   &xi3c_master_ops, false);
     if (ret) {
         goto err_rpm_disable;
     }
#ifndef PCIE_INTERFACE_SUPPORT
     pm_runtime_mark_last_busy(&pdev->dev);
     pm_runtime_put_autosuspend(&pdev->dev);
#endif
    return 0;

 err_rpm_disable:
#ifndef PCIE_INTERFACE_SUPPORT
         pm_runtime_dont_use_autosuspend(&pdev->dev);
         pm_runtime_put_noidle(&pdev->dev);
         pm_runtime_disable(&pdev->dev);
         pm_runtime_set_suspended(&pdev->dev);
#endif
    return ret;
 }

 static void xi3c_master_remove(struct platform_device *pdev)
 {
     struct xi3c_master *master = platform_get_drvdata(pdev);

     i3c_master_unregister(&master->base);
#ifndef PCIE_INTERFACE_SUPPORT
     clk_disable_unprepare(master->pclk);
#endif
 }

 #ifndef PCIE_INTERFACE_SUPPORT
 static int __maybe_unused xi3c_master_runtime_suspend(struct device *dev)
 {
     struct xi3c_master *master = dev_get_drvdata(dev);
     xi3c_master_disable(master);

     clk_disable_unprepare(master->pclk);
         return 0;
 }

static int __maybe_unused xi3c_master_runtime_resume(struct device *dev)
 {
     struct xi3c_master *master = dev_get_drvdata(dev);
     int ret;

     ret = clk_prepare_enable(master->pclk);
     if (ret)
         return ret;

     xi3c_master_enable(master);
     return 0;
 }
#endif

 #ifndef PCIE_INTERFACE_SUPPORT
 static const struct dev_pm_ops xi3c_pm_ops = {
         SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
         SET_RUNTIME_PM_OPS(xi3c_master_runtime_suspend, xi3c_master_runtime_resume, NULL)
 };
#endif

 static struct platform_driver xi3c_master_driver = {
     .probe = xi3c_master_probe,
     .remove = xi3c_master_remove,
     .driver = {
         .name = "axi-i3c-master",
         .of_match_table = xi3c_master_of_ids,
#ifndef PCIE_INTERFACE_SUPPORT
         .pm = &xi3c_pm_ops,
#endif
     },
 };
 module_platform_driver(xi3c_master_driver);

 MODULE_AUTHOR("Manikanta Guntupalli <manikanta.guntupalli@amd.com>");
 MODULE_DESCRIPTION("AXI I3C master driver");
 MODULE_LICENSE("GPL v2");

