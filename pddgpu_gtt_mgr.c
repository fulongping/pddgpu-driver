/*
 * PDDGPU GTT 管理器
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#include <drm/ttm/ttm_range_manager.h>
#include <drm/drm_drv.h>
#include <drm/drm_mm.h>

#include "include/pddgpu_drv.h"
#include "pddgpu_gtt_mgr.h"

#define PDDGPU_GTT_MAX_TRANSFER_SIZE	(2UL << 20)
#define PDDGPU_GTT_NUM_TRANSFER_WINDOWS	2

/* 转换宏 */
static inline struct pddgpu_gtt_mgr *
to_gtt_mgr(struct ttm_resource_manager *man)
{
	return container_of(man, struct pddgpu_gtt_mgr, manager);
}

/* GTT 分配函数 */
static int pddgpu_gtt_mgr_alloc(struct ttm_resource_manager *man,
                                 struct ttm_buffer_object *bo,
                                 const struct ttm_place *place,
                                 struct ttm_resource **res)
{
	struct pddgpu_gtt_mgr *mgr = to_gtt_mgr(man);
	uint32_t num_pages = PFN_UP(bo->base.size);
	struct ttm_range_mgr_node *node;
	int r;

	node = kzalloc(struct_size(node, mm_nodes, 1), GFP_KERNEL);
	if (!node)
		return -ENOMEM;

	ttm_resource_init(bo, place, &node->base);
	
	/* 检查 GTT 使用量 */
	if (!(place->flags & TTM_PL_FLAG_TEMPORARY) &&
	    ttm_resource_manager_usage(man) > man->size) {
		r = -ENOSPC;
		goto err_free;
	}

	/* 分配 GTT 地址空间 */
	if (place->lpfn) {
		spin_lock(&mgr->lock);
		r = drm_mm_insert_node_in_range(&mgr->mm, &node->mm_nodes[0],
						num_pages, bo->page_alignment,
						0, place->fpfn, place->lpfn,
						DRM_MM_INSERT_BEST);
		spin_unlock(&mgr->lock);
		if (unlikely(r))
			goto err_free;

		node->base.start = node->mm_nodes[0].start;
	} else {
		/* 临时分配，不分配实际地址 */
		node->mm_nodes[0].start = 0;
		node->mm_nodes[0].size = PFN_UP(node->base.size);
		node->base.start = PDDGPU_BO_INVALID_OFFSET;
	}

	*res = &node->base;
	return 0;

err_free:
	ttm_resource_fini(man, &node->base);
	kfree(node);
	return r;
}

/* GTT 释放函数 */
static void pddgpu_gtt_mgr_free(struct ttm_resource_manager *man,
                                 struct ttm_resource *res)
{
	struct ttm_range_mgr_node *node = to_ttm_range_mgr_node(res);
	struct pddgpu_gtt_mgr *mgr = to_gtt_mgr(man);

	spin_lock(&mgr->lock);
	if (drm_mm_node_allocated(&node->mm_nodes[0]))
		drm_mm_remove_node(&node->mm_nodes[0]);
	spin_unlock(&mgr->lock);

	ttm_resource_fini(man, res);
	kfree(node);
}

/* GTT 调试函数 */
static void pddgpu_gtt_mgr_debug(struct ttm_resource_manager *man,
                                  struct drm_printer *printer)
{
	struct pddgpu_gtt_mgr *mgr = to_gtt_mgr(man);

	spin_lock(&mgr->lock);
	drm_mm_print(&mgr->mm, printer);
	spin_unlock(&mgr->lock);
}

/* GTT 兼容性检查 */
static bool pddgpu_gtt_mgr_compatible(struct ttm_resource_manager *man,
                                       struct ttm_resource *res,
                                       const struct ttm_place *place,
                                       size_t size)
{
	return !place->lpfn || pddgpu_gtt_mgr_has_gart_addr(res);
}

/* GTT 重叠检查 */
static bool pddgpu_gtt_mgr_intersects(struct ttm_resource_manager *man,
                                       struct ttm_resource *res,
                                       const struct ttm_place *place,
                                       size_t size)
{
	struct ttm_range_mgr_node *node = to_ttm_range_mgr_node(res);
	uint64_t place_start, place_end;

	if (!drm_mm_node_allocated(&node->mm_nodes[0]))
		return false;

	place_start = (u64)place->fpfn << PAGE_SHIFT;
	place_end = (u64)place->lpfn << PAGE_SHIFT;

	return (node->mm_nodes[0].start < place_end &&
		(node->mm_nodes[0].start + node->mm_nodes[0].size) > place_start);
}

/* 管理器函数表 */
const struct ttm_resource_manager_func pddgpu_gtt_mgr_func = {
	.alloc = pddgpu_gtt_mgr_alloc,
	.free = pddgpu_gtt_mgr_free,
	.debug = pddgpu_gtt_mgr_debug,
	.intersects = pddgpu_gtt_mgr_intersects,
	.compatible = pddgpu_gtt_mgr_compatible,
};

/* GTT 管理器初始化 */
int pddgpu_gtt_mgr_init(struct pddgpu_device *pdev, uint64_t gtt_size)
{
	struct pddgpu_gtt_mgr *mgr = &pdev->mman.gtt_mgr;
	struct ttm_resource_manager *man = &mgr->manager;
	uint64_t start, size;

	/* 设置 TTM 管理器属性 */
	man->use_tt = true;
	man->func = &pddgpu_gtt_mgr_func;

	/* 初始化 TTM 资源管理器 */
	ttm_resource_manager_init(man, &pdev->mman.bdev, gtt_size);

	/* 初始化 DRM MM 分配器 */
	start = PDDGPU_GTT_MAX_TRANSFER_SIZE * PDDGPU_GTT_NUM_TRANSFER_WINDOWS;
	size = (pdev->gtt_size >> PAGE_SHIFT) - start;
	drm_mm_init(&mgr->mm, start, size);
	spin_lock_init(&mgr->lock);

	/* 注册到 TTM 设备 */
	ttm_set_driver_manager(&pdev->mman.bdev, TTM_PL_TT, &mgr->manager);
	ttm_resource_manager_set_used(man, true);

	PDDGPU_DEBUG("GTT manager initialized: size=%llu\n", man->size);
	return 0;
}

/* GTT 管理器清理 */
void pddgpu_gtt_mgr_fini(struct pddgpu_device *pdev)
{
	struct pddgpu_gtt_mgr *mgr = &pdev->mman.gtt_mgr;
	struct ttm_resource_manager *man = &mgr->manager;
	int ret;

	ttm_resource_manager_set_used(man, false);

	ret = ttm_resource_manager_evict_all(&pdev->mman.bdev, man);
	if (ret) {
		PDDGPU_ERROR("Failed to evict all GTT resources\n");
		return;
	}

	spin_lock(&mgr->lock);
	drm_mm_takedown(&mgr->mm);
	spin_unlock(&mgr->lock);

	ttm_resource_manager_cleanup(man);
	ttm_set_driver_manager(&pdev->mman.bdev, TTM_PL_TT, NULL);

	PDDGPU_DEBUG("GTT manager finalized\n");
}

/* GTT 管理器恢复 */
void pddgpu_gtt_mgr_recover(struct pddgpu_gtt_mgr *mgr)
{
	struct ttm_range_mgr_node *node;
	struct drm_mm_node *mm_node;
	struct pddgpu_device *pdev;

	pdev = to_pddgpu_device_from_gtt_mgr(mgr);
	
	spin_lock(&mgr->lock);
	drm_mm_for_each_node(mm_node, &mgr->mm) {
		node = container_of(mm_node, typeof(*node), mm_nodes[0]);
		/* TODO: 实现 GART 恢复功能 */
		PDDGPU_DEBUG("Recovering GTT node: start=%llu, size=%llu\n",
		             mm_node->start, mm_node->size);
	}
	spin_unlock(&mgr->lock);
}
