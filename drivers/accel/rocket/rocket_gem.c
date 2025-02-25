// SPDX-License-Identifier: GPL-2.0
/* Copyright 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net> */

#include <drm/drm_device.h>
#include <drm/drm_utils.h>
#include <drm/rocket_accel.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>

#include "rocket_device.h"
#include "rocket_gem.h"

static void rocket_gem_bo_free(struct drm_gem_object *obj)
{
	struct rocket_device *rdev = to_rocket_device(obj->dev);
	struct rocket_gem_object *bo = to_rocket_bo(obj);
	struct sg_table *sgt;

	drm_WARN_ON(obj->dev, bo->base.pages_use_count > 1);

	mutex_lock(&rdev->iommu_lock);

	sgt = drm_gem_shmem_get_pages_sgt(&bo->base);

	/* Unmap this object from the IOMMUs for cores > 0 */
	for (unsigned int core = 1; core < rdev->num_cores; core++) {
		struct iommu_domain *domain = iommu_get_domain_for_dev(rdev->cores[core].dev);
		size_t unmapped = iommu_unmap(domain, sgt->sgl->dma_address, bo->size);

		drm_WARN_ON(obj->dev, unmapped != bo->size);
	}

	/* This will unmap the pages from the IOMMU linked to core 0 */
	drm_gem_shmem_free(&bo->base);

	mutex_unlock(&rdev->iommu_lock);
}

static const struct drm_gem_object_funcs rocket_gem_funcs = {
	.free = rocket_gem_bo_free,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = drm_gem_shmem_object_mmap,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

/**
 * rocket_gem_create_object - Implementation of driver->gem_create_object.
 * @dev: DRM device
 * @size: Size in bytes of the memory the object will reference
 *
 * This lets the GEM helpers allocate object structs for us, and keep
 * our BO stats correct.
 */
struct drm_gem_object *rocket_gem_create_object(struct drm_device *dev, size_t size)
{
	struct rocket_gem_object *obj;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	obj->base.base.funcs = &rocket_gem_funcs;

	return &obj->base.base;
}

int rocket_ioctl_create_bo(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_rocket_create_bo *args = data;
	struct rocket_device *rdev = to_rocket_device(dev);
	struct drm_gem_shmem_object *shmem_obj;
	struct rocket_gem_object *rkt_obj;
	struct drm_gem_object *gem_obj;
	struct sg_table *sgt;
	int ret;

	shmem_obj = drm_gem_shmem_create(dev, args->size);
	if (IS_ERR(shmem_obj))
		return PTR_ERR(shmem_obj);

	gem_obj = &shmem_obj->base;
	rkt_obj = to_rocket_bo(gem_obj);

	rkt_obj->size = args->size;
	rkt_obj->offset = 0;
	mutex_init(&rkt_obj->mutex);

	ret = drm_gem_handle_create(file, gem_obj, &args->handle);
	drm_gem_object_put(gem_obj);
	if (ret)
		goto err;

	mutex_lock(&rdev->iommu_lock);

	/* This will map the pages to the IOMMU linked to core 0 */
	sgt = drm_gem_shmem_get_pages_sgt(shmem_obj);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto err_unlock;
	}

	/* Map the pages to the IOMMUs linked to the other cores, so all cores can access this BO */
	for (unsigned int core = 1; core < rdev->num_cores; core++) {

		ret = iommu_map_sgtable(iommu_get_domain_for_dev(rdev->cores[core].dev),
					sgt->sgl->dma_address,
					sgt,
					IOMMU_READ | IOMMU_WRITE);
		if (ret < 0 || ret < args->size) {
			DRM_ERROR("failed to map buffer: size=%d request_size=%u\n",
				ret, args->size);
			ret = -ENOMEM;
			goto err_unlock;
		}

		/* iommu_map_sgtable might have aligned the size */
		rkt_obj->size = ret;

		dma_sync_sgtable_for_device(rdev->cores[core].dev, shmem_obj->sgt,
					    DMA_BIDIRECTIONAL);
	}

	mutex_unlock(&rdev->iommu_lock);

	args->offset = drm_vma_node_offset_addr(&gem_obj->vma_node);
	args->dma_address = sg_dma_address(shmem_obj->sgt->sgl);

	return 0;

err_unlock:
	mutex_unlock(&rdev->iommu_lock);
err:
	drm_gem_shmem_object_free(gem_obj);

	return ret;
}
