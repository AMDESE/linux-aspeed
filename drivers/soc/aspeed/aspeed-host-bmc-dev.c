// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) ASPEED Technology Inc.

#include <linux/init.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/serial_core.h>
#include <linux/serial_8250.h>

#define PCI_BMC_HOST2BMC_Q1		0x30000
#define PCI_BMC_HOST2BMC_Q2		0x30010
#define PCI_BMC_BMC2HOST_Q1		0x30020
#define PCI_BMC_BMC2HOST_Q2		0x30030
#define PCI_BMC_BMC2HOST_STS		0x30040
#define	 BMC2HOST_INT_STS_DOORBELL	BIT(31)
#define	 BMC2HOST_ENABLE_INTB		BIT(30)

#define	 BMC2HOST_Q1_FULL		BIT(27)
#define	 BMC2HOST_Q1_EMPTY		BIT(26)
#define	 BMC2HOST_Q2_FULL		BIT(25)
#define	 BMC2HOST_Q2_EMPTY		BIT(24)
#define	 BMC2HOST_Q1_FULL_UNMASK	BIT(23)
#define	 BMC2HOST_Q1_EMPTY_UNMASK	BIT(22)
#define	 BMC2HOST_Q2_FULL_UNMASK	BIT(21)
#define	 BMC2HOST_Q2_EMPTY_UNMASK	BIT(20)

#define PCI_BMC_HOST2BMC_STS		0x30044
#define	 HOST2BMC_INT_STS_DOORBELL	BIT(31)
#define	 HOST2BMC_ENABLE_INTB		BIT(30)

#define	 HOST2BMC_Q1_FULL		BIT(27)
#define	 HOST2BMC_Q1_EMPTY		BIT(26)
#define	 HOST2BMC_Q2_FULL		BIT(25)
#define	 HOST2BMC_Q2_EMPTY		BIT(24)
#define	 HOST2BMC_Q1_FULL_UNMASK	BIT(23)
#define	 HOST2BMC_Q1_EMPTY_UNMASK	BIT(22)
#define	 HOST2BMC_Q2_FULL_UNMASK	BIT(21)
#define	 HOST2BMC_Q2_EMPTY_UNMASK	BIT(20)

static DEFINE_IDA(bmc_device_ida);

#define MSI_INDX		4
#define VUART_MAX_PARMS		2
#define ASPEED_QUEUE_NUM 2

enum queue_index {
	QUEUE1 = 0,
	QUEUE2,
};

enum msi_index {
	BMC_MSI,
	MBX_MSI,
	VUART0_MSI,
	VUART1_MSI,
};

static int ast2600_msi_idx_table[MSI_INDX] = { 4, 21, 16, 15 };
static int ast2700_msi_idx_table[MSI_INDX] = { 0, 11, 6, 5 };

struct aspeed_queue_message {
	/* Queue waiters for idle engine */
	wait_queue_head_t tx_wait;
	wait_queue_head_t rx_wait;
	struct kernfs_node *kn;
	struct bin_attribute bin;
	int index;
	struct aspeed_pci_bmc_dev *pci_bmc_device;
};

struct aspeed_pci_bmc_dev {
	struct device *dev;
	struct miscdevice miscdev;
	int id;

	unsigned long mem_bar_base;
	unsigned long mem_bar_size;
	void __iomem *mem_bar_reg;

	unsigned long message_bar_base;
	unsigned long message_bar_size;
	void __iomem *msg_bar_reg;

	void __iomem *pcie_sio_decode_addr;

	struct aspeed_queue_message queue[ASPEED_QUEUE_NUM];

	void __iomem *sio_mbox_reg;
	struct uart_8250_port uart[VUART_MAX_PARMS];
	int uart_line[VUART_MAX_PARMS];

	/* Interrupt
	 * The index of array is using to enum msi_index
	 */
	int *msi_idx_table;
	int irq_table[MSI_INDX];
};

#define HOST_BMC_QUEUE_SIZE			(16 * 4)
#define PCIE_DEVICE_SIO_ADDR		(0x2E * 4)
#define BMC_MULTI_MSI	32

#define DRIVER_NAME "aspeed-host-bmc-dev"

static struct aspeed_pci_bmc_dev *file_aspeed_bmc_device(struct file *file)
{
	return container_of(file->private_data, struct aspeed_pci_bmc_dev,
			miscdev);
}

static int aspeed_pci_bmc_dev_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct aspeed_pci_bmc_dev *pci_bmc_dev = file_aspeed_bmc_device(file);
	unsigned long vsize = vma->vm_end - vma->vm_start;
	pgprot_t prot = vma->vm_page_prot;

	if (vma->vm_pgoff + vsize > pci_bmc_dev->mem_bar_base + 0x100000)
		return -EINVAL;

	prot = pgprot_noncached(prot);

	if (remap_pfn_range(vma, vma->vm_start,
			    (pci_bmc_dev->mem_bar_base >> PAGE_SHIFT) + vma->vm_pgoff,
			    vsize, prot))
		return -EAGAIN;

	return 0;
}

static const struct file_operations aspeed_pci_bmc_dev_fops = {
	.owner		= THIS_MODULE,
	.mmap		= aspeed_pci_bmc_dev_mmap,
};

static ssize_t aspeed_queue_rx(struct file *filp, struct kobject *kobj, struct bin_attribute *attr,
			       char *buf, loff_t off, size_t count)
{
	struct aspeed_queue_message *queue = attr->private;
	struct aspeed_pci_bmc_dev *pci_bmc_device = queue->pci_bmc_device;
	int index = queue->index;
	u32 *data = (u32 *)buf;
	int ret;

	ret = wait_event_interruptible(queue->rx_wait,
				       !(readl(pci_bmc_device->msg_bar_reg + PCI_BMC_BMC2HOST_STS) &
				       ((index == QUEUE1) ? BMC2HOST_Q1_EMPTY : BMC2HOST_Q2_EMPTY)));
	if (ret)
		return -EINTR;

	data[0] = readl(pci_bmc_device->msg_bar_reg +
			((index == QUEUE1) ? PCI_BMC_BMC2HOST_Q1 : PCI_BMC_BMC2HOST_Q2));

	writel(HOST2BMC_INT_STS_DOORBELL | HOST2BMC_ENABLE_INTB,
	       pci_bmc_device->msg_bar_reg + PCI_BMC_HOST2BMC_STS);

	return sizeof(u32);
}

static ssize_t aspeed_queue_tx(struct file *filp, struct kobject *kobj, struct bin_attribute *attr,
			       char *buf, loff_t off, size_t count)
{
	struct aspeed_queue_message *queue = attr->private;
	struct aspeed_pci_bmc_dev *pci_bmc_device = queue->pci_bmc_device;
	int index = queue->index;
	u32 tx_buff;
	int ret;

	if (count != sizeof(u32))
		return -EINVAL;

	ret = wait_event_interruptible(queue->tx_wait,
				       !(readl(pci_bmc_device->msg_bar_reg + PCI_BMC_HOST2BMC_STS) &
				       ((index == QUEUE1) ? HOST2BMC_Q1_FULL : HOST2BMC_Q2_FULL)));
	if (ret)
		return -EINTR;

	memcpy(&tx_buff, buf, 4);
	writel(tx_buff, pci_bmc_device->msg_bar_reg +
				((index == QUEUE1) ? PCI_BMC_HOST2BMC_Q1 : PCI_BMC_HOST2BMC_Q2));
	//trigger to host
	writel(HOST2BMC_INT_STS_DOORBELL | HOST2BMC_ENABLE_INTB,
	       pci_bmc_device->msg_bar_reg + PCI_BMC_HOST2BMC_STS);

	return sizeof(u32);
}

static irqreturn_t aspeed_pci_host_bmc_device_interrupt(int irq, void *dev_id)
{
	struct aspeed_pci_bmc_dev *pci_bmc_device = dev_id;
	u32 bmc2host_q_sts = readl(pci_bmc_device->msg_bar_reg + PCI_BMC_BMC2HOST_STS);

	if (bmc2host_q_sts & BMC2HOST_INT_STS_DOORBELL)
		writel(BMC2HOST_INT_STS_DOORBELL,
		       pci_bmc_device->msg_bar_reg + PCI_BMC_BMC2HOST_STS);

	if (bmc2host_q_sts & BMC2HOST_ENABLE_INTB)
		writel(BMC2HOST_ENABLE_INTB, pci_bmc_device->msg_bar_reg + PCI_BMC_BMC2HOST_STS);

	if (bmc2host_q_sts & BMC2HOST_Q1_FULL)
		dev_info(pci_bmc_device->dev, "Q1 Full\n");

	if (bmc2host_q_sts & BMC2HOST_Q2_FULL)
		dev_info(pci_bmc_device->dev, "Q2 Full\n");

	//check q1
	if (!(readl(pci_bmc_device->msg_bar_reg + PCI_BMC_HOST2BMC_STS) & HOST2BMC_Q1_FULL))
		wake_up_interruptible(&pci_bmc_device->queue[QUEUE1].tx_wait);

	if (!(readl(pci_bmc_device->msg_bar_reg + PCI_BMC_BMC2HOST_STS) & BMC2HOST_Q1_EMPTY))
		wake_up_interruptible(&pci_bmc_device->queue[QUEUE1].rx_wait);
	//chech q2
	if (!(readl(pci_bmc_device->msg_bar_reg + PCI_BMC_HOST2BMC_STS) & HOST2BMC_Q2_FULL))
		wake_up_interruptible(&pci_bmc_device->queue[QUEUE2].tx_wait);

	if (!(readl(pci_bmc_device->msg_bar_reg + PCI_BMC_BMC2HOST_STS) & BMC2HOST_Q2_EMPTY))
		wake_up_interruptible(&pci_bmc_device->queue[QUEUE2].rx_wait);

	return IRQ_HANDLED;
}

static irqreturn_t aspeed_pci_host_mbox_interrupt(int irq, void *dev_id)
{
	struct aspeed_pci_bmc_dev *pci_bmc_device = dev_id;
	u32 isr = readl(pci_bmc_device->sio_mbox_reg + 0x94);

	if (isr & BIT(7))
		writel(BIT(7), pci_bmc_device->sio_mbox_reg + 0x94);

	return IRQ_HANDLED;
}

static void aspeed_pci_setup_irq_resource(struct pci_dev *pdev)
{
	struct aspeed_pci_bmc_dev *pci_bmc_dev = pci_get_drvdata(pdev);
	int nr_entries, i;

	/* Assign static msi index table by platform */
	if (pdev->revision == 0x27)
		pci_bmc_dev->msi_idx_table = ast2700_msi_idx_table;
	else
		pci_bmc_dev->msi_idx_table = ast2600_msi_idx_table;

	nr_entries = pci_alloc_irq_vectors(pdev, 1, BMC_MULTI_MSI, PCI_IRQ_LEGACY | PCI_IRQ_MSI);
	/* If number is one, use legacy interrupt or ONE MSI */
	if (nr_entries <= 1)
		/* Set all msi index to the first vector */
		memset(pci_bmc_dev->msi_idx_table, 0, sizeof(int) * MSI_INDX);

	/* Get msi irq number from vector */
	for (i = 0; i < MSI_INDX; i++)
		pci_bmc_dev->irq_table[i] = pci_irq_vector(pdev, pci_bmc_dev->msi_idx_table[i]);
}

static int aspeed_pci_bmc_device_setup_queue(struct pci_dev *pdev)
{
	struct aspeed_pci_bmc_dev *pci_bmc_device = pci_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	int ret, i;

	for (i = 0; i < ASPEED_QUEUE_NUM; i++) {
		struct aspeed_queue_message *queue = &pci_bmc_device->queue[i];

		init_waitqueue_head(&queue->tx_wait);
		init_waitqueue_head(&queue->rx_wait);

		sysfs_bin_attr_init(&queue->bin);

		/* Queue name index starts from 1 */
		queue->bin.attr.name =
			devm_kasprintf(dev, GFP_KERNEL, "pci-bmc-dev-queue%d", (i + 1));
		queue->bin.attr.mode = 0600;
		queue->bin.read = aspeed_queue_rx;
		queue->bin.write = aspeed_queue_tx;
		queue->bin.size = 4;
		queue->bin.private = queue;

		ret = sysfs_create_bin_file(&pdev->dev.kobj, &queue->bin);
		if (ret) {
			dev_err(dev, "error for bin%d file\n", i);
			return ret;
		}

		queue->kn = kernfs_find_and_get(dev->kobj.sd, queue->bin.attr.name);
		if (!queue->kn) {
			sysfs_remove_bin_file(&dev->kobj, &queue->bin);
			return ret;
		}

		queue->index = i;
		queue->pci_bmc_device = pci_bmc_device;
	}

	return 0;
}

static int aspeed_pci_bmc_device_setup_vuart(struct pci_dev *pdev)
{
	struct aspeed_pci_bmc_dev *pci_bmc_dev = pci_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	u16 vuart_ioport;
	int ret, i;

	for (i = 0; i < VUART_MAX_PARMS; i++) {
		/* Assign the line to non-exist device */
		pci_bmc_dev->uart_line[i] = -ENOENT;
		vuart_ioport = 0x3F8 - (i * 0x100);
		pci_bmc_dev->uart[i].port.flags = UPF_SKIP_TEST | UPF_BOOT_AUTOCONF | UPF_SHARE_IRQ;
		pci_bmc_dev->uart[i].port.uartclk = 115200 * 16;
		pci_bmc_dev->uart[i].port.irq = pci_bmc_dev->irq_table[VUART0_MSI + i];
		pci_bmc_dev->uart[i].port.dev = dev;
		pci_bmc_dev->uart[i].port.iotype = UPIO_MEM32;
		pci_bmc_dev->uart[i].port.iobase = 0;
		pci_bmc_dev->uart[i].port.mapbase =
			pci_bmc_dev->message_bar_base + (vuart_ioport << 2);
		pci_bmc_dev->uart[i].port.membase = 0;
		pci_bmc_dev->uart[i].port.type = PORT_16550A;
		pci_bmc_dev->uart[i].port.flags |= (UPF_IOREMAP | UPF_FIXED_PORT | UPF_FIXED_TYPE);
		pci_bmc_dev->uart[i].port.regshift = 2;
		ret = serial8250_register_8250_port(&pci_bmc_dev->uart[i]);
		if (ret < 0) {
			dev_err_probe(dev, ret, "Can't setup PCIe VUART\n");
			return ret;
		}
		pci_bmc_dev->uart_line[i] = ret;
	}
	return 0;
}

static int aspeed_pci_bmc_device_setup_memory_mapping(struct pci_dev *pdev)
{
	struct aspeed_pci_bmc_dev *pci_bmc_dev = pci_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	int ret;

	pci_bmc_dev->miscdev.minor = MISC_DYNAMIC_MINOR;
	pci_bmc_dev->miscdev.name =
		devm_kasprintf(dev, GFP_KERNEL, "%s%d", DRIVER_NAME, pci_bmc_dev->id);
	pci_bmc_dev->miscdev.fops = &aspeed_pci_bmc_dev_fops;
	pci_bmc_dev->miscdev.parent = dev;

	ret = misc_register(&pci_bmc_dev->miscdev);
	if (ret) {
		pr_err("host bmc register fail %d\n", ret);
		return ret;
	}

	return 0;
}

static int aspeed_pci_bmc_device_setup_mbox(struct pci_dev *pdev)
{
	struct aspeed_pci_bmc_dev *pci_bmc_dev = pci_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	int ret;

	/* setup mbox */
	pci_bmc_dev->pcie_sio_decode_addr = pci_bmc_dev->msg_bar_reg + PCIE_DEVICE_SIO_ADDR;
	writel(0xaa, pci_bmc_dev->pcie_sio_decode_addr);
	writel(0xa5, pci_bmc_dev->pcie_sio_decode_addr);
	writel(0xa5, pci_bmc_dev->pcie_sio_decode_addr);
	writel(0x07, pci_bmc_dev->pcie_sio_decode_addr);
	writel(0x0e, pci_bmc_dev->pcie_sio_decode_addr + 0x04);
	/* disable */
	writel(0x30, pci_bmc_dev->pcie_sio_decode_addr);
	writel(0x00, pci_bmc_dev->pcie_sio_decode_addr + 0x04);
	/* set decode address 0x100 */
	writel(0x60, pci_bmc_dev->pcie_sio_decode_addr);
	writel(0x01, pci_bmc_dev->pcie_sio_decode_addr + 0x04);
	writel(0x61, pci_bmc_dev->pcie_sio_decode_addr);
	writel(0x00, pci_bmc_dev->pcie_sio_decode_addr + 0x04);
	/* enable */
	writel(0x30, pci_bmc_dev->pcie_sio_decode_addr);
	writel(0x01, pci_bmc_dev->pcie_sio_decode_addr + 0x04);
	pci_bmc_dev->sio_mbox_reg = pci_bmc_dev->msg_bar_reg + 0x400;

	ret = request_irq(pci_bmc_dev->irq_table[MBX_MSI], aspeed_pci_host_mbox_interrupt,
			  IRQF_SHARED,
			  devm_kasprintf(dev, GFP_KERNEL, "aspeed-sio-mbox%d", pci_bmc_dev->id),
			  pci_bmc_dev);
	if (ret) {
		pr_err("host bmc device Unable to get IRQ %d\n", ret);
		return ret;
	}

	return 0;
}

static void aspeed_pci_host_bmc_device_release_queue(struct pci_dev *pdev)
{
	struct aspeed_pci_bmc_dev *pci_bmc_dev = pci_get_drvdata(pdev);
	int i;

	for (i = 0; i < ASPEED_QUEUE_NUM; i++)
		sysfs_remove_bin_file(&pdev->dev.kobj, &pci_bmc_dev->queue[i].bin);
}

static void aspeed_pci_host_bmc_device_release_vuart(struct pci_dev *pdev)
{
	struct aspeed_pci_bmc_dev *pci_bmc_dev = pci_get_drvdata(pdev);
	int i;

	for (i = 0; i < VUART_MAX_PARMS; i++) {
		if (pci_bmc_dev->uart_line[i] >= 0)
			serial8250_unregister_port(pci_bmc_dev->uart_line[i]);
	}
}

static void aspeed_pci_host_bmc_device_release_memory_mapping(struct pci_dev *pdev)
{
	struct aspeed_pci_bmc_dev *pci_bmc_dev = pci_get_drvdata(pdev);

	if (!list_empty(&pci_bmc_dev->miscdev.list))
		misc_deregister(&pci_bmc_dev->miscdev);
}

static int aspeed_pci_host_bmc_device_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct aspeed_pci_bmc_dev *pci_bmc_dev;
	int rc = 0;

	pr_info("ASPEED BMC PCI ID %04x:%04x, IRQ=%u\n", pdev->vendor, pdev->device, pdev->irq);

	pci_bmc_dev = kzalloc(sizeof(*pci_bmc_dev), GFP_KERNEL);
	if (!pci_bmc_dev) {
		rc = -ENOMEM;
		dev_err(&pdev->dev, "kmalloc() returned NULL memory.\n");
		goto out_err;
	}

	pci_bmc_dev->id = ida_simple_get(&bmc_device_ida, 0, 0, GFP_KERNEL);
	if (pci_bmc_dev->id < 0)
		goto out_free;

	rc = pci_enable_device(pdev);
	if (rc != 0) {
		dev_err(&pdev->dev, "pci_enable_device() returned error %d\n", rc);
		goto out_free;
	}

	/* set PCI host mastering  */
	pci_set_master(pdev);

	pci_set_drvdata(pdev, pci_bmc_dev);

	aspeed_pci_setup_irq_resource(pdev);

	pr_info("ASPEED BMC PCI ID %04x:%04x, IRQ=%u\n", pdev->vendor, pdev->device, pdev->irq);

	//Get MEM bar
	pci_bmc_dev->mem_bar_base = pci_resource_start(pdev, 0);
	pci_bmc_dev->mem_bar_size = pci_resource_len(pdev, 0);

	pr_info("BAR0 I/O Mapped Base Address is: %08lx End %08lx\n",
		pci_bmc_dev->mem_bar_base, pci_bmc_dev->mem_bar_size);

	pci_bmc_dev->mem_bar_reg = pci_ioremap_bar(pdev, 0);
	if (!pci_bmc_dev->mem_bar_reg) {
		rc = -ENOMEM;
		goto out_free;
	}

	//Get MSG BAR info
	pci_bmc_dev->message_bar_base = pci_resource_start(pdev, 1);
	pci_bmc_dev->message_bar_size = pci_resource_len(pdev, 1);

	pr_info("MSG BAR1 Memory Mapped Base Address is: %08lx End %08lx\n",
		pci_bmc_dev->message_bar_base, pci_bmc_dev->message_bar_size);

	pci_bmc_dev->msg_bar_reg = pci_ioremap_bar(pdev, 1);
	if (!pci_bmc_dev->msg_bar_reg) {
		rc = -ENOMEM;
		goto out_free0;
	}

	/* AST2600 ERRTA40: dummy read */
	if (pdev->revision < 0x27)
		(void)__raw_readl((void __iomem *)pci_bmc_dev->msg_bar_reg);

	rc = aspeed_pci_bmc_device_setup_queue(pdev);
	if (rc) {
		pr_err("Cannot setup queue message");
		goto out_free1;
	}

	rc = aspeed_pci_bmc_device_setup_memory_mapping(pdev);
	if (rc) {
		pr_err("Cannot setup memory mapping");
		goto out_free_queue;
	}

	rc = aspeed_pci_bmc_device_setup_mbox(pdev);
	if (rc) {
		pr_err("Cannot setup MBOX");
		goto out_free_mmapping;
	}

	rc = aspeed_pci_bmc_device_setup_vuart(pdev);
	if (rc) {
		pr_err("Cannot setup VUART");
		goto out_free_mbox;
	}

	rc = request_irq(pci_bmc_dev->irq_table[BMC_MSI], aspeed_pci_host_bmc_device_interrupt,
			 IRQF_SHARED, pci_bmc_dev->miscdev.name, pci_bmc_dev);
	if (rc) {
		pr_err("host bmc device Unable to get IRQ %d\n", rc);
		goto out_free_uart;
	}

	return 0;

out_free_uart:
	aspeed_pci_host_bmc_device_release_vuart(pdev);
out_free_mbox:
	free_irq(pci_bmc_dev->irq_table[MBX_MSI], pci_bmc_dev);
out_free_mmapping:
	aspeed_pci_host_bmc_device_release_memory_mapping(pdev);
out_free_queue:
	aspeed_pci_host_bmc_device_release_queue(pdev);
out_free1:
	iounmap(pci_bmc_dev->msg_bar_reg);
out_free0:
	iounmap(pci_bmc_dev->mem_bar_reg);
out_free:
	pci_release_regions(pdev);
	kfree(pci_bmc_dev);
out_err:
	pci_disable_device(pdev);

	return rc;
}

static void aspeed_pci_host_bmc_device_remove(struct pci_dev *pdev)
{
	struct aspeed_pci_bmc_dev *pci_bmc_dev = pci_get_drvdata(pdev);

	aspeed_pci_host_bmc_device_release_queue(pdev);
	aspeed_pci_host_bmc_device_release_memory_mapping(pdev);
	aspeed_pci_host_bmc_device_release_vuart(pdev);

	free_irq(pci_bmc_dev->irq_table[BMC_MSI], pci_bmc_dev);
	free_irq(pci_bmc_dev->irq_table[MBX_MSI], pci_bmc_dev);

	pci_release_regions(pdev);
	kfree(pci_bmc_dev);
	pci_disable_device(pdev);
}

/**
 * This table holds the list of (VendorID,DeviceID) supported by this driver
 *
 */
static struct pci_device_id aspeed_host_bmc_dev_pci_ids[] = {
	{ PCI_DEVICE(0x1A03, 0x2402), },
	{ 0, }
};

MODULE_DEVICE_TABLE(pci, aspeed_host_bmc_dev_pci_ids);

static struct pci_driver aspeed_host_bmc_dev_driver = {
	.name		= DRIVER_NAME,
	.id_table	= aspeed_host_bmc_dev_pci_ids,
	.probe		= aspeed_pci_host_bmc_device_probe,
	.remove		= aspeed_pci_host_bmc_device_remove,
};

module_pci_driver(aspeed_host_bmc_dev_driver);

MODULE_AUTHOR("Ryan Chen <ryan_chen@aspeedtech.com>");
MODULE_DESCRIPTION("ASPEED Host BMC DEVICE Driver");
MODULE_LICENSE("GPL");
