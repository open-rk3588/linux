// SPDX-License-Identifier: GPL-2.0
/* Copyright 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include <drm/drm_accel.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_of.h>
#include <drm/rocket_accel.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "rocket_drv.h"
#include "rocket_gem.h"
#include "rocket_job.h"

static int
rocket_open(struct drm_device *dev, struct drm_file *file)
{
	struct rocket_device *rdev = to_rocket_device(dev);
	struct rocket_file_priv *rocket_priv;
	int ret;

	rocket_priv = kzalloc(sizeof(*rocket_priv), GFP_KERNEL);
	if (!rocket_priv)
		return -ENOMEM;

	rocket_priv->rdev = rdev;
	file->driver_priv = rocket_priv;

	ret = rocket_job_open(rocket_priv);
	if (ret)
		goto err_free;

	return 0;

err_free:
	kfree(rocket_priv);
	return ret;
}

static void
rocket_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct rocket_file_priv *rocket_priv = file->driver_priv;

	rocket_job_close(rocket_priv);
	kfree(rocket_priv);
}

static const struct drm_ioctl_desc rocket_drm_driver_ioctls[] = {
#define ROCKET_IOCTL(n, func) \
	DRM_IOCTL_DEF_DRV(ROCKET_##n, rocket_ioctl_##func, 0)

	ROCKET_IOCTL(CREATE_BO, create_bo),
	ROCKET_IOCTL(SUBMIT, submit),
	ROCKET_IOCTL(PREP_BO, prep_bo),
	ROCKET_IOCTL(FINI_BO, fini_bo),
};

DEFINE_DRM_ACCEL_FOPS(rocket_accel_driver_fops);

/*
 * Rocket driver version:
 * - 1.0 - initial interface
 */
static const struct drm_driver rocket_drm_driver = {
	.driver_features	= DRIVER_COMPUTE_ACCEL | DRIVER_GEM,
	.open			= rocket_open,
	.postclose		= rocket_postclose,
	.gem_create_object	= rocket_gem_create_object,
	.ioctls			= rocket_drm_driver_ioctls,
	.num_ioctls		= ARRAY_SIZE(rocket_drm_driver_ioctls),
	.fops			= &rocket_accel_driver_fops,
	.name			= "rocket",
	.desc			= "rocket DRM",
};

static int rocket_drm_bind(struct device *dev)
{
	struct device_node *core_node;
	struct rocket_device *rdev;
	struct drm_device *ddev;
	unsigned int num_cores = 1;
	int err;

	rdev = devm_drm_dev_alloc(dev, &rocket_drm_driver, struct rocket_device, ddev);
	if (IS_ERR(ddev))
		return PTR_ERR(ddev);

	ddev = &rdev->ddev;
	dev_set_drvdata(dev, rdev);

	for_each_compatible_node(core_node, NULL, "rockchip,rk3588-rknn-core")
		if (of_device_is_available(core_node))
			num_cores++;

	rdev->cores = devm_kmalloc_array(dev, num_cores, sizeof(*rdev->cores),
					 GFP_KERNEL | __GFP_ZERO);
	if (IS_ERR(rdev->cores))
		return PTR_ERR(rdev->cores);

	/* Add core 0, any other cores will be added later when they are bound */
	rdev->cores[0].rdev = rdev;
	rdev->cores[0].dev = dev;
	rdev->cores[0].index = 0;
	rdev->num_cores = 1;

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40));
	if (err)
		return err;

	err = rocket_device_init(rdev);
	if (err) {
		dev_err_probe(dev, err, "Fatal error during NPU init\n");
		goto err_device_fini;
	}

	err = component_bind_all(dev, rdev);
	if (err)
		goto err_device_fini;

	err = drm_dev_register(ddev, 0);
	if (err < 0)
		goto err_unbind;

	return 0;

err_unbind:
	component_unbind_all(dev, rdev);
err_device_fini:
	rocket_device_fini(rdev);
	return err;
}

static void rocket_drm_unbind(struct device *dev)
{
	struct rocket_device *rdev = dev_get_drvdata(dev);
	struct drm_device *ddev = &rdev->ddev;

	drm_dev_unregister(ddev);

	component_unbind_all(dev, rdev);

	rocket_device_fini(rdev);
}

const struct component_master_ops rocket_drm_ops = {
	.bind = rocket_drm_bind,
	.unbind = rocket_drm_unbind,
};

static int rocket_core_bind(struct device *dev, struct device *master, void *data)
{
	struct rocket_device *rdev = data;
	unsigned int core = rdev->num_cores;
	int err;

	dev_set_drvdata(dev, rdev);

	rdev->cores[core].rdev = rdev;
	rdev->cores[core].dev = dev;
	rdev->cores[core].index = core;
	rdev->cores[core].link = device_link_add(dev, rdev->cores[0].dev,
						 DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME);

	rdev->num_cores++;

	err = rocket_core_init(&rdev->cores[core]);
	if (err) {
		rocket_device_fini(rdev);
		return err;
	}

	return 0;
}

static void rocket_core_unbind(struct device *dev, struct device *master, void *data)
{
	struct rocket_device *rdev = data;

	for (unsigned int core = 1; core < rdev->num_cores; core++) {
		if (rdev->cores[core].dev == dev) {
			rocket_core_fini(&rdev->cores[core]);
			device_link_del(rdev->cores[core].link);
			break;
		}
	}
}

const struct component_ops rocket_core_ops = {
	.bind = rocket_core_bind,
	.unbind = rocket_core_unbind,
};

static int rocket_probe(struct platform_device *pdev)
{
	struct component_match *match = NULL;
	struct device_node *core_node;

	if (fwnode_device_is_compatible(pdev->dev.fwnode, "rockchip,rk3588-rknn-core"))
		return component_add(&pdev->dev, &rocket_core_ops);

	for_each_compatible_node(core_node, NULL, "rockchip,rk3588-rknn-core") {
		if (!of_device_is_available(core_node))
			continue;

		drm_of_component_match_add(&pdev->dev, &match,
					   component_compare_of, core_node);
	}

	return component_master_add_with_match(&pdev->dev, &rocket_drm_ops, match);
}

static void rocket_remove(struct platform_device *pdev)
{
	if (fwnode_device_is_compatible(pdev->dev.fwnode, "rockchip,rk3588-rknn-core-top"))
		component_master_del(&pdev->dev, &rocket_drm_ops);
	else if (fwnode_device_is_compatible(pdev->dev.fwnode, "rockchip,rk3588-rknn-core"))
		component_del(&pdev->dev, &rocket_core_ops);
}

static const struct of_device_id dt_match[] = {
	{ .compatible = "rockchip,rk3588-rknn-core-top" },
	{ .compatible = "rockchip,rk3588-rknn-core" },
	{}
};
MODULE_DEVICE_TABLE(of, dt_match);

static int rocket_device_runtime_resume(struct device *dev)
{
	struct rocket_device *rdev = dev_get_drvdata(dev);

	for (unsigned int core = 0; core < rdev->num_cores; core++) {
		if (dev != rdev->cores[core].dev)
			continue;

		if (core == 0) {
			clk_prepare_enable(rdev->clk_npu);
			clk_prepare_enable(rdev->pclk);
		}

		clk_prepare_enable(rdev->cores[core].a_clk);
		clk_prepare_enable(rdev->cores[core].h_clk);
	}

	return 0;
}

static int rocket_device_runtime_suspend(struct device *dev)
{
	struct rocket_device *rdev = dev_get_drvdata(dev);

	for (unsigned int core = 0; core < rdev->num_cores; core++) {
		if (dev != rdev->cores[core].dev)
			continue;

		if (!rocket_job_is_idle(&rdev->cores[core]))
			return -EBUSY;

		clk_disable_unprepare(rdev->cores[core].a_clk);
		clk_disable_unprepare(rdev->cores[core].h_clk);

		if (core == 0) {
			clk_disable_unprepare(rdev->pclk);
			clk_disable_unprepare(rdev->clk_npu);
		}
	}

	return 0;
}

EXPORT_GPL_DEV_PM_OPS(rocket_pm_ops) = {
	RUNTIME_PM_OPS(rocket_device_runtime_suspend, rocket_device_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
};

static struct platform_driver rocket_driver = {
	.probe = rocket_probe,
	.remove = rocket_remove,
	.driver	 = {
		.name = "rocket",
		.pm = pm_ptr(&rocket_pm_ops),
		.of_match_table = dt_match,
	},
};
module_platform_driver(rocket_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DRM driver for the Rockchip NPU IP");
MODULE_AUTHOR("Tomeu Vizoso");
