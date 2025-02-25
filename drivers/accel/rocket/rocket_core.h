/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#ifndef __ROCKET_CORE_H__
#define __ROCKET_CORE_H__

#include <drm/gpu_scheduler.h>
#include <linux/mutex_types.h>
#include <linux/io.h>

#define rocket_read(core, reg) readl((core)->iomem + (reg))
#define rocket_write(core, reg, value) writel(value, (core)->iomem + (reg))

struct rocket_core {
	struct device *dev;
	struct rocket_device *rdev;
	struct device_link *link;
	unsigned int index;

	int irq;
	void __iomem *iomem;
	struct clk *a_clk;
	struct clk *h_clk;
};

int rocket_core_init(struct rocket_core *core);
void rocket_core_fini(struct rocket_core *core);

#endif
