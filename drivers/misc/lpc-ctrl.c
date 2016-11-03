/*
 * Copyright 2016 IBM Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/errno.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/lpc-ctrl.h>

#define DEVICE_NAME	"lpc-ctrl"

#define LPC_HICR7 0x88
#define LPC_HICR8 0x8c

struct lpc_ctrl {
	struct miscdevice	miscdev;
	void __iomem		*ctrl;
	phys_addr_t			base;
	resource_size_t		size;
};


static struct lpc_ctrl *file_lpc_ctrl(struct file *file)
{
	return container_of(file->private_data, struct lpc_ctrl, miscdev);
}

static int lpc_ctrl_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct lpc_ctrl *lpc_ctrl = file_lpc_ctrl(file);
	unsigned long vsize = vma->vm_end - vma->vm_start;

	if (vma->vm_pgoff + vsize > lpc_ctrl->base + lpc_ctrl->size)
		return -EINVAL;

	/* Other checks? */

	if (remap_pfn_range(vma, vma->vm_start,
		(lpc_ctrl->base >> PAGE_SHIFT) + vma->vm_pgoff,
		vsize, vma->vm_page_prot))
		return -EAGAIN;

	return 0;
}

static int lpc_ctrl_open(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t lpc_ctrl_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	if (!access_ok(VERIFY_WRITE, buf, count))
		return -EFAULT;

	WARN_ON(*ppos);

	return -ENOSYS;
}

static ssize_t lpc_ctrl_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	if (!access_ok(VERIFY_READ, buf, count))
		return -EFAULT;

	WARN_ON(*ppos);

	return -ENOSYS;
}

static long lpc_ctrl_ioctl(struct file *file, unsigned int cmd,
		unsigned long param)
{
	struct lpc_mapping map;
	struct lpc_ctrl *lpc_ctrl = file_lpc_ctrl(file);
	void __user *p = (void __user *)param;

	switch (cmd) {
		case LPC_CTRL_IOCTL_SIZE:
			return copy_to_user(p, &lpc_ctrl->size,
				sizeof(lpc_ctrl->size)) ? -EFAULT : 0;
		case LPC_CTRL_IOCTL_MAP:
			if (copy_from_user(&map, p, sizeof(map)))
				return -EFAULT;

			if ((map.hostaddr | ~0xffff) == 0 || (map.hostaddr & 0xffff) ||
				(map.size | ~0xffff) == 0 || (map.size & 0xffff)) {
				dev_err(lpc_ctrl->miscdev.parent, "Didn't think values have alignments: 0x%08x, %08x\n", map.hostaddr, map.size);
				return -EINVAL;
			}

			/*
			 * But we might want to remap the flash?
			 *
			 *if (map.bmcaddr < lpc_ctrl->base ||
			 *	map.bmcaddr + map.size > lpc_ctrl->base + lpc_ctrl->size) {
			 *	dev_err(lpc_ctrl->miscdev.parent, "Didn't think values matched size requirements\n");
			 *	return -EINVAL;
			 *}
			 */

			/* Alignment checks */

			iowrite32(lpc_ctrl->base | (map.hostaddr >> 16),
					lpc_ctrl->ctrl + LPC_HICR7);
			iowrite32((~(map.size - 1)) | ((map.size >> 16) - 1),
					lpc_ctrl->ctrl + LPC_HICR8);
			return 0;
		case LPC_CTRL_IOCTL_UNMAP:
			iowrite32((0x3000 << 16) | (0x0e00),
					lpc_ctrl->ctrl + LPC_HICR7);
			iowrite32(((~(0x0200 - 1)) << 16) | ((0x0200) - 1), /* 32MB on my test palm */
					lpc_ctrl->ctrl + LPC_HICR8);
			return 0;

	}

	return -EINVAL;
}

static int lpc_ctrl_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations lpc_ctrl_fops = {
	.owner		= THIS_MODULE,
	.mmap		= lpc_ctrl_mmap,
	.open		= lpc_ctrl_open,
	.read		= lpc_ctrl_read,
	.write		= lpc_ctrl_write,
	.release	= lpc_ctrl_release,
	.unlocked_ioctl	= lpc_ctrl_ioctl,
};

static int lpc_ctrl_probe(struct platform_device *pdev)
{
	struct lpc_ctrl *lpc_ctrl;
	struct device *dev;
	struct device_node *node;
	struct resource *res;
	struct resource resm;
	int rc;

	if (!pdev || !pdev->dev.of_node)
		return -ENODEV;

	dev = &pdev->dev;
	dev_info(dev, "Found lpc control device\n");

	lpc_ctrl = devm_kzalloc(dev, sizeof(*lpc_ctrl), GFP_KERNEL);
	if (!lpc_ctrl)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, lpc_ctrl);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "Unable to find resources\n");
		rc = -ENXIO;
		goto out_free;
	}

	/* Todo unmap this on fail cases and exit */
	lpc_ctrl->ctrl = devm_ioremap_resource(&pdev->dev, res);
	if (!lpc_ctrl->ctrl) {
		rc = -ENOMEM;
		goto out_free;
	}

	node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!node) {
		/*
		 * Should probaby handle this by allocating 4-64k now and
		 * using that
		 */
		dev_err(dev, "Didn't find reserved memory\n");
		rc = -EINVAL;
		goto out;
	}

	rc = of_address_to_resource(node, 0, &resm);
	of_node_put(node);
	if (rc) {
		dev_err(dev, "Could address to resource\n");
		rc = -ENOMEM;
		goto out;
	}

	lpc_ctrl->size = resource_size(&resm);
	lpc_ctrl->base = resm.start;

	lpc_ctrl->miscdev.minor = MISC_DYNAMIC_MINOR;
	lpc_ctrl->miscdev.name = DEVICE_NAME;
	lpc_ctrl->miscdev.fops = &lpc_ctrl_fops;
	lpc_ctrl->miscdev.parent = dev;
	rc = misc_register(&lpc_ctrl->miscdev);
	if (rc) {
		dev_err(dev, "Unable to register device\n");
		goto out;
	}

	dev_info(dev, "Loaded at 0x%08x (0x%08x)\n", lpc_ctrl->base, lpc_ctrl->size);
	return 0;

out:
	devm_iounmap(&pdev->dev, lpc_ctrl->ctrl);
out_free:
	devm_kfree(dev, lpc_ctrl);
	return rc;

}

static int lpc_ctrl_remove(struct platform_device *pdev)
{
	struct lpc_ctrl *lpc_ctrl = dev_get_drvdata(&pdev->dev);
	misc_deregister(&lpc_ctrl->miscdev);
	devm_iounmap(&pdev->dev, lpc_ctrl->ctrl);
	devm_kfree(&pdev->dev, lpc_ctrl);
	lpc_ctrl = NULL;

	return 0;
}

static const struct of_device_id lpc_ctrl_match[] = {
	{ .compatible = "aspeed,lpc-ctrl" },
	{ },
};

static struct platform_driver lpc_ctrl_driver = {
	.driver = {
		.name		= DEVICE_NAME,
		.of_match_table = lpc_ctrl_match,
	},
	.probe = lpc_ctrl_probe,
	.remove = lpc_ctrl_remove,
};

module_platform_driver(lpc_ctrl_driver);

MODULE_DEVICE_TABLE(of, lpc_ctrl_match);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cyril Bur <cyrilbur@gmail.com>");
MODULE_DESCRIPTION("Linux device interface to control LPC bus");
