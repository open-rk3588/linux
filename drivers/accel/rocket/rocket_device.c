// SPDX-License-Identifier: GPL-2.0
/* Copyright 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include "linux/mutex.h"
#include <linux/clk.h>

#include "rocket_device.h"

int rocket_device_init(struct rocket_device *rdev)
{
	struct device *dev = rdev->cores[0].dev;
	int err;

	mutex_init(&rdev->iommu_lock);
	mutex_init(&rdev->sched_lock);

	rdev->clk_npu = devm_clk_get(dev, "npu");
	rdev->pclk = devm_clk_get(dev, "pclk");

	/* Initialize core 0 (top) */
	err = rocket_core_init(&rdev->cores[0]);
	if (err) {
		rocket_device_fini(rdev);
		return err;
	}

	return 0;
}

void rocket_device_fini(struct rocket_device *rdev)
{
	rocket_core_fini(&rdev->cores[0]);
	mutex_destroy(&rdev->sched_lock);
	mutex_destroy(&rdev->iommu_lock);
}
