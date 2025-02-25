/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#ifndef __ROCKET_DEVICE_H__
#define __ROCKET_DEVICE_H__

#include <drm/drm_device.h>

#include "rocket_core.h"

struct rocket_device {
	struct drm_device ddev;

	struct mutex sched_lock;

	struct clk *clk_npu;
	struct clk *pclk;

	struct mutex iommu_lock;

	struct rocket_core *cores;
	unsigned int num_cores;
};

int rocket_device_init(struct rocket_device *rdev);
void rocket_device_fini(struct rocket_device *rdev);

static inline struct rocket_device *to_rocket_device(struct drm_device *dev)
{
	return (struct rocket_device *)dev;
}

#endif
