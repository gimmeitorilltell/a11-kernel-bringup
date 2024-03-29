/*
 * Generic GPIO card-detect helper
 *
 * Copyright (C) 2011, Guennadi Liakhovetski <g.liakhovetski@gmx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/mmc/cd-gpio.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/ratelimit.h>
#include <linux/delay.h>

static struct workqueue_struct *enable_detection_workqueue;	/* For unmask SD detection pin interrupt */

struct mmc_cd_gpio {
	unsigned int gpio;
	char label[0];
	bool status;
};

void mmc_enable_detection(struct work_struct *work)
{
	struct mmc_host *host =	container_of(work, struct mmc_host, enable_detect.work);

	enable_irq(host->hotplug.irq);
	printk("%s %s leave\n", mmc_hostname(host), __func__);
}
EXPORT_SYMBOL(mmc_enable_detection);

int mmc_cd_get_status(struct mmc_host *host)
{
	int ret = -ENOSYS;
	struct mmc_cd_gpio *cd = host->hotplug.handler_priv;

	if (!cd || !gpio_is_valid(cd->gpio))
		goto out;

	ret = !gpio_get_value_cansleep(cd->gpio) ^
		!!(host->caps2 & MMC_CAP2_CD_ACTIVE_HIGH);
out:
	return ret;
}
EXPORT_SYMBOL(mmc_cd_get_status);

static irqreturn_t mmc_cd_gpio_irqt(int irq, void *dev_id)
{
	struct mmc_host *host = dev_id;
	struct mmc_cd_gpio *cd = host->hotplug.handler_priv;
	int status;

	disable_irq_nosync(host->hotplug.irq);
	queue_delayed_work(enable_detection_workqueue, &host->enable_detect, msecs_to_jiffies(50));

	status = mmc_cd_get_status(host);
	if (unlikely(status < 0))
		goto out;

	if (status ^ cd->status) {
		printk_ratelimited(KERN_INFO "%s: slot status change detected (%d -> %d), GPIO_ACTIVE_%s\n",
				mmc_hostname(host), cd->status, status,
				(host->caps2 & MMC_CAP2_CD_ACTIVE_HIGH) ?
				"HIGH" : "LOW");
		cd->status = status;
		/* Recover Host capabilities */
		host->caps |= host->caps_uhs;
		host->redetect_cnt = 0;
		host->crc_count = 0;
		/* Schedule a card detection after a debounce timeout */
		mmc_detect_change(host, msecs_to_jiffies(100));
	}
out:
	return IRQ_HANDLED;
}

int mmc_cd_gpio_request(struct mmc_host *host, unsigned int gpio)
{
	size_t len = strlen(dev_name(host->parent)) + 4;
	struct mmc_cd_gpio *cd;
	int irq = gpio_to_irq(gpio);
	int ret;

	if (irq < 0)
		return irq;

	enable_detection_workqueue = create_singlethread_workqueue("enable_sd_detect");
	if (!enable_detection_workqueue)
		return -ENOMEM;

	irq_set_irq_wake(irq, 1);

	cd = kmalloc(sizeof(*cd) + len, GFP_KERNEL);
	if (!cd)
		return -ENOMEM;

	snprintf(cd->label, len, "%s cd", dev_name(host->parent));

	ret = gpio_request_one(gpio, GPIOF_DIR_IN, cd->label);
	if (ret < 0)
		goto egpioreq;

	cd->gpio = gpio;
	host->hotplug.irq = irq;
	host->hotplug.handler_priv = cd;

	ret = mmc_cd_get_status(host);
	if (ret < 0)
		goto eirqreq;

	cd->status = ret;

	ret = request_threaded_irq(irq, NULL, mmc_cd_gpio_irqt,
				   IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				   cd->label, host);
	if (ret < 0)
		goto eirqreq;

	return 0;

eirqreq:
	gpio_free(gpio);
egpioreq:
	kfree(cd);
	destroy_workqueue(enable_detection_workqueue);
	return ret;
}
EXPORT_SYMBOL(mmc_cd_gpio_request);

void mmc_cd_gpio_free(struct mmc_host *host)
{
	struct mmc_cd_gpio *cd = host->hotplug.handler_priv;

	if (!cd || !gpio_is_valid(cd->gpio))
		return;

	free_irq(host->hotplug.irq, host);
	gpio_free(cd->gpio);
	cd->gpio = -EINVAL;
	kfree(cd);
	destroy_workqueue(enable_detection_workqueue);
}
EXPORT_SYMBOL(mmc_cd_gpio_free);
