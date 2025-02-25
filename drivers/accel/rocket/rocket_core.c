// SPDX-License-Identifier: GPL-2.0
/* Copyright 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "rocket_core.h"
#include "rocket_registers.h"

static int rocket_clk_init(struct rocket_core *core)
{
	struct device *dev = core->dev;
	int err;

	core->a_clk = devm_clk_get(dev, "aclk");
	if (IS_ERR(core->a_clk)) {
		err = PTR_ERR(core->a_clk);
		dev_err(dev, "devm_clk_get_enabled failed %d for core %d\n", err, core->index);
		return err;
	}

	core->h_clk = devm_clk_get(dev, "hclk");
	if (IS_ERR(core->h_clk)) {
		err = PTR_ERR(core->h_clk);
		dev_err(dev, "devm_clk_get_enabled failed %d for core %d\n", err, core->index);
		clk_disable_unprepare(core->a_clk);
		return err;
	}

	return 0;
}

int rocket_core_init(struct rocket_core *core)
{
	struct device *dev = core->dev;
	struct platform_device *pdev = to_platform_device(dev);
	uint32_t version;
	int err = 0;

	err = rocket_clk_init(core);
	if (err) {
		dev_err(dev, "clk init failed %d\n", err);
		return err;
	}

	core->iomem = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(core->iomem))
		return PTR_ERR(core->iomem);

	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_autosuspend_delay(dev, 50); /* ~3 frames */
	pm_runtime_enable(dev);

	err = pm_runtime_get_sync(dev);

	version = rocket_read(core, REG_PC_VERSION);
	version += rocket_read(core, REG_PC_VERSION_NUM) & 0xffff;

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	dev_info(dev, "Rockchip NPU core %d version: %d\n", core->index, version);

	return 0;
}

void rocket_core_fini(struct rocket_core *core)
{
	pm_runtime_disable(core->dev);
}
