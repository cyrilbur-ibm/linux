/*
 * Copyright 2016 IBM Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

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
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/mbox-host.h>

#define DEVICE_NAME	"mbox-host"

#define MBOX_NUM_REGS 16
#define MBOX_NUM_DATA_REGS 14

#define MBOX_DATA_0 0x00
#define MBOX_STATUS_0 0x40
#define MBOX_STATUS_1 0x44
#define MBOX_BMC_CTRL 0x48
	#define MBOX_CTRL_RECV 0x80
	#define MBOX_CTRL_MASK 0x02
	#define MBOX_CTRL_SEND 0x01
#define MBOX_HOST_CTRL 0x4c
#define MBOX_INTERRUPT_0 0x50
#define MBOX_INTERRUPT_1 0x54

struct mbox_host {
	struct miscdevice	miscdev;
	void __iomem		*base;
	int			irq;
	wait_queue_head_t	queue;
	struct timer_list	poll_timer;
};

static u8 mbox_inb(struct mbox_host *mbox_host, int reg)
{
	return ioread8(mbox_host->base + reg);
}

static void mbox_outb(struct mbox_host *mbox_host, u8 data, int reg)
{
	iowrite8(data, mbox_host->base + reg);
}

static struct mbox_host *file_mbox_host(struct file *file)
{
	return container_of(file->private_data, struct mbox_host, miscdev);
}

static int mbox_host_open(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t mbox_host_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct mbox_host *mbox_host = file_mbox_host(file);
	char __user *p = buf;
	int i;

	if (!access_ok(VERIFY_WRITE, buf, count))
		return -EFAULT;

	WARN_ON(*ppos);

	if (wait_event_interruptible(mbox_host->queue,
				mbox_inb(mbox_host, MBOX_BMC_CTRL) & MBOX_CTRL_RECV))
		return -ERESTARTSYS;

	for (i = 0; i < MBOX_NUM_DATA_REGS; i++)
		if (__put_user(mbox_inb(mbox_host, MBOX_DATA_0 + (i * 4)), p++))
			return -EFAULT;

	/* MBOX_CTRL_RECV bit is W1C, this also unmasks in 1 step */
	mbox_outb(mbox_host, MBOX_CTRL_RECV, MBOX_BMC_CTRL);
	return p - buf;
}

static ssize_t mbox_host_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos)
{
	struct mbox_host *mbox_host = file_mbox_host(file);
	const char __user *p = buf;
	char c;
	int i;

	if (!access_ok(VERIFY_READ, buf, count))
		return -EFAULT;

	WARN_ON(*ppos);

	for (i = 0; i < MBOX_NUM_DATA_REGS; i++) {
		if (__get_user(c, p))
			return -EFAULT;

		mbox_outb(mbox_host, c, MBOX_DATA_0 + (i * 4));
		p++;
	}

	mbox_outb(mbox_host, MBOX_CTRL_SEND, MBOX_BMC_CTRL);

	return p - buf;
}

static long mbox_host_ioctl(struct file *file, unsigned int cmd,
		unsigned long param)
{
	struct mbox_host *mbox_host = file_mbox_host(file);

	switch (cmd) {
		case MBOX_HOST_IOCTL_ATN:
			mbox_outb(mbox_host, param, MBOX_DATA_0 + 15);
		return 0;
	}

	return -EINVAL;
}

static int mbox_host_release(struct inode *inode, struct file *file)
{
	return 0;
}

static unsigned int mbox_host_poll(struct file *file, poll_table *wait)
{
	struct mbox_host *mbox_host = file_mbox_host(file);
	unsigned int mask = 0;

	poll_wait(file, &mbox_host->queue, wait);

	if (mbox_inb(mbox_host, MBOX_BMC_CTRL) & MBOX_CTRL_RECV)
		mask |= POLLIN;

	return mask;
}

static const struct file_operations mbox_host_fops = {
	.owner		= THIS_MODULE,
	.open		= mbox_host_open,
	.read		= mbox_host_read,
	.write		= mbox_host_write,
	.release	= mbox_host_release,
	.poll		= mbox_host_poll,
	.unlocked_ioctl	= mbox_host_ioctl,
};

static void poll_timer(unsigned long data)
{
	struct mbox_host *mbox_host = (void *)data;
	mbox_host->poll_timer.expires += msecs_to_jiffies(500);
	wake_up(&mbox_host->queue);
	add_timer(&mbox_host->poll_timer);
}

static irqreturn_t mbox_host_irq(int irq, void *arg)
{
	struct mbox_host *mbox_host = arg;

	if (!(mbox_inb(mbox_host, MBOX_BMC_CTRL) & MBOX_CTRL_RECV))
		return IRQ_NONE;

	/*
	 * Leave the status bit set so that we know the data is for us,
	 * clear it once it has been read.
	 */

	/* Mask it off, we'll clear it when we the data gets read */
	mbox_outb(mbox_host, MBOX_CTRL_MASK, MBOX_BMC_CTRL);

	wake_up(&mbox_host->queue);
	return IRQ_HANDLED;
}

static int mbox_host_config_irq(struct mbox_host *mbox_host,
		struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int rc;

	mbox_host->irq = irq_of_parse_and_map(dev->of_node, 0);
	if (!mbox_host->irq)
		return -ENODEV;

	rc = devm_request_irq(dev, mbox_host->irq, mbox_host_irq, IRQF_SHARED,
			DEVICE_NAME, mbox_host);
	if (rc < 0) {
		dev_warn(dev, "Unable to request IRQ %d\n", mbox_host->irq);
		mbox_host->irq = 0;
		return rc;
	}

	/*
	 * Disable all register based interrupts, we'll have to change
	 * this, protocol seemingly will require regs 0 and 15
	 */
	mbox_outb(mbox_host, 0x00, MBOX_INTERRUPT_0); /* regs 0 - 7 */
	mbox_outb(mbox_host, 0x00, MBOX_INTERRUPT_1); /* regs 8 - 15 */

	/* W1C */
	mbox_outb(mbox_host, 0xff, MBOX_STATUS_0);
	mbox_outb(mbox_host, 0xff, MBOX_STATUS_1);

	mbox_outb(mbox_host, MBOX_CTRL_RECV, MBOX_BMC_CTRL);
	return 0;
}

static int mbox_host_probe(struct platform_device *pdev)
{
	struct mbox_host *mbox_host;
	struct device *dev;
	struct resource *res;
	int rc;

	if (!pdev || !pdev->dev.of_node)
		return -ENODEV;

	dev = &pdev->dev;
	dev_info(dev, "Found mbox host device\n");

	mbox_host = devm_kzalloc(dev, sizeof(*mbox_host), GFP_KERNEL);
	if (!mbox_host)
		return -ENOMEM;

	dev_set_drvdata(&pdev->dev, mbox_host);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "Unable to find resources\n");
		rc = -ENXIO;
		goto out_free;
	}

	mbox_host->base = devm_ioremap_resource(&pdev->dev, res);
	if (!mbox_host->base) {
		rc = -ENOMEM;
		goto out_free;
	}
	init_waitqueue_head(&mbox_host->queue);

	mbox_host->miscdev.minor = MISC_DYNAMIC_MINOR;
	mbox_host->miscdev.name = DEVICE_NAME;
	mbox_host->miscdev.fops = &mbox_host_fops;
	mbox_host->miscdev.parent = dev;
	rc = misc_register(&mbox_host->miscdev);
	if (rc) {
		dev_err(dev, "Unable to register device\n");
		goto out_unmap;
	}

	mbox_host_config_irq(mbox_host, pdev);

	if (mbox_host->irq) {
		dev_info(dev, "Using IRQ %d\n", mbox_host->irq);
	} else {
		dev_info(dev, "No IRQ; using timer\n");
		setup_timer(&mbox_host->poll_timer, poll_timer,
				(unsigned long)mbox_host);
		mbox_host->poll_timer.expires = jiffies + msecs_to_jiffies(10);
		add_timer(&mbox_host->poll_timer);
	}
	return 0;

out_unmap:
	devm_iounmap(&pdev->dev, mbox_host->base);

out_free:
	devm_kfree(dev, mbox_host);
	return rc;

}

static int mbox_host_remove(struct platform_device *pdev)
{
	struct mbox_host *mbox_host = dev_get_drvdata(&pdev->dev);
	misc_deregister(&mbox_host->miscdev);
	if (!mbox_host->irq)
		del_timer_sync(&mbox_host->poll_timer);
	devm_iounmap(&pdev->dev, mbox_host->base);
	devm_kfree(&pdev->dev, mbox_host);
	mbox_host = NULL;

	return 0;
}

static const struct of_device_id mbox_host_match[] = {
	{ .compatible = "aspeed,mbox-host" },
	{ },
};

static struct platform_driver mbox_host_driver = {
	.driver = {
		.name		= DEVICE_NAME,
		.of_match_table = mbox_host_match,
	},
	.probe = mbox_host_probe,
	.remove = mbox_host_remove,
};

module_platform_driver(mbox_host_driver);

MODULE_DEVICE_TABLE(of, mbox_host_match);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cyril Bur <cyrilbur@gmail.com>");
MODULE_DESCRIPTION("Linux device interface to the MBOX interface");
