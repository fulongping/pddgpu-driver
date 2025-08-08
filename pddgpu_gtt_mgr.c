/*
 * PDDGPU GTT 管理器
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#include <drm/ttm/ttm_range_manager.h>
#include <drm/drm_drv.h>
#include <drm/drm_mm.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/delay.h>

#include "include/pddgpu_drv.h"
#include "pddgpu_gtt_mgr.h"

#define PDDGPU_GTT_MAX_TRANSFER_SIZE	(2UL << 20)
#define PDDGPU_GTT_NUM_TRANSFER_WINDOWS	2
#define PDDGPU_GTT_ALLOC_RETRY_COUNT	3
#define PDDGPU_GTT_ALLOC_RETRY_DELAY	5 /* 毫秒 */

/* GTT管理器状态标志 */
#define PDDGPU_GTT_MGR_STATE_INITIALIZING	0x01
#define PDDGPU_GTT_MGR_STATE_READY		0x02
#define PDDGPU_GTT_MGR_STATE_SHUTDOWN		0x04
#define PDDGPU_GTT_MGR_STATE_ERROR		0x08

/* 转换宏 */
static inline struct pddgpu_gtt_mgr *
to_gtt_mgr(struct ttm_resource_manager *man)
{
	return container_of(man, struct pddgpu_gtt_mgr, manager);
}

/* GTT管理器状态检查 */
static inline bool pddgpu_gtt_mgr_is_ready(struct pddgpu_gtt_mgr *mgr)
{
	return (atomic_read(&mgr->state) & PDDGPU_GTT_MGR_STATE_READY) &&
	       !(atomic_read(&mgr->state) & PDDGPU_GTT_MGR_STATE_SHUTDOWN);
}

/* GTT管理器错误处理 */
static inline void pddgpu_gtt_mgr_set_error(struct pddgpu_gtt_mgr *mgr)
{
	atomic_or(PDDGPU_GTT_MGR_STATE_ERROR, &mgr->state);
	PDDGPU_ERROR("GTT manager entered error state\n");
}

static inline void pddgpu_gtt_mgr_clear_error(struct pddgpu_gtt_mgr *mgr)
{
	atomic_and(~PDDGPU_GTT_MGR_STATE_ERROR, &mgr->state);
}

/* GTT 分配函数 */
static int pddgpu_gtt_mgr_alloc(struct ttm_resource_manager *man,
                                 struct ttm_buffer_object *bo,
                                 const struct ttm_place *place,
                                 struct ttm_resource **res)
{
	struct pddgpu_gtt_mgr *mgr = to_gtt_mgr(man);
	struct pddgpu_device *pdev = container_of(mgr, struct pddgpu_device, mman.gtt_mgr);
	uint32_t num_pages = PFN_UP(bo->base.size);
	struct ttm_range_mgr_node *node;
	int r, retry_count = 0;
	unsigned long flags;

	/* 检查设备状态 */
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		PDDGPU_DEBUG("Device is shutting down, skipping GTT allocation\n");
		return -ENODEV;
	}

	/* 检查GTT管理器状态 */
	if (!pddgpu_gtt_mgr_is_ready(mgr)) {
		PDDGPU_ERROR("GTT manager is not ready\n");
		return -ENODEV;
	}

	/* 验证分配大小 */
	if (num_pages == 0) {
		PDDGPU_ERROR("Invalid allocation size: %lu\n", bo->base.size);
		return -EINVAL;
	}

	/* 分配GTT节点结构 */
	node = kzalloc(struct_size(node, mm_nodes, 1), GFP_KERNEL);
	if (!node) {
		PDDGPU_ERROR("Failed to allocate GTT node structure\n");
		return -ENOMEM;
	}

	ttm_resource_init(bo, place, &node->base);
	
	/* 检查 GTT 使用量 */
	if (!(place->flags & TTM_PL_FLAG_TEMPORARY) &&
	    ttm_resource_manager_usage(man) > man->size) {
		PDDGPU_ERROR("GTT usage exceeds limit: %llu > %llu\n",
		             ttm_resource_manager_usage(man), man->size);
		r = -ENOSPC;
		goto err_free;
	}

	/* 重试机制 */
retry_alloc:
	/* 分配 GTT 地址空间 */
	if (place->lpfn) {
		spin_lock(&mgr->lock);
		
		/* 再次检查状态（在锁内） */
		if (!pddgpu_gtt_mgr_is_ready(mgr)) {
			spin_unlock(&mgr->lock);
			PDDGPU_ERROR("GTT manager state changed during allocation\n");
			r = -ENODEV;
			goto err_free;
		}

		r = drm_mm_insert_node_in_range(&mgr->mm, &node->mm_nodes[0],
						num_pages, bo->page_alignment,
						0, place->fpfn, place->lpfn,
						DRM_MM_INSERT_BEST);
		spin_unlock(&mgr->lock);
		
		if (unlikely(r)) {
			/* 重试机制 */
			if (++retry_count < PDDGPU_GTT_ALLOC_RETRY_COUNT) {
				PDDGPU_DEBUG("GTT allocation failed, retrying (%d/%d)\n",
				             retry_count, PDDGPU_GTT_ALLOC_RETRY_COUNT);
				msleep(PDDGPU_GTT_ALLOC_RETRY_DELAY);
				goto retry_alloc;
			}
			
			PDDGPU_ERROR("GTT allocation failed after %d retries: %d\n", retry_count, r);
			goto err_free;
		}

		node->base.start = node->mm_nodes[0].start;
	} else {
		/* 临时分配，不分配实际地址 */
		node->mm_nodes[0].start = 0;
		node->mm_nodes[0].size = PFN_UP(node->base.size);
		node->base.start = PDDGPU_BO_INVALID_OFFSET;
	}

	/* 更新内存统计 */
	pddgpu_memory_stats_update_usage(pdev, TTM_PL_TT, bo->base.size, true);

	*res = &node->base;
	
	PDDGPU_DEBUG("GTT allocation successful: pages=%u, start=%llu\n",
	             num_pages, node->base.start);

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
	struct pddgpu_device *pdev = container_of(mgr, struct pddgpu_device, mman.gtt_mgr);
	u64 freed_size = node->base.size;

	/* 检查设备状态 */
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		PDDGPU_DEBUG("Device is shutting down, skipping GTT free\n");
		return;
	}

	/* 检查GTT管理器状态 */
	if (!pddgpu_gtt_mgr_is_ready(mgr)) {
		PDDGPU_ERROR("GTT manager is not ready during free\n");
		return;
	}

	spin_lock(&mgr->lock);
	
	/* 再次检查状态（在锁内） */
	if (!pddgpu_gtt_mgr_is_ready(mgr)) {
		spin_unlock(&mgr->lock);
		PDDGPU_ERROR("GTT manager state changed during free\n");
		return;
	}

	if (drm_mm_node_allocated(&node->mm_nodes[0]))
		drm_mm_remove_node(&node->mm_nodes[0]);
	
	spin_unlock(&mgr->lock);

	/* 更新内存统计 */
	pddgpu_memory_stats_update_usage(pdev, TTM_PL_TT, freed_size, false);

	ttm_resource_fini(man, res);
	kfree(node);
	
	PDDGPU_DEBUG("GTT free successful: size=%llu\n", freed_size);
}

/* GTT 调试函数 */
static void pddgpu_gtt_mgr_debug(struct ttm_resource_manager *man,
                                  struct drm_printer *printer)
{
	struct pddgpu_gtt_mgr *mgr = to_gtt_mgr(man);

	/* 检查GTT管理器状态 */
	if (!pddgpu_gtt_mgr_is_ready(mgr)) {
		drm_printf(printer, "GTT manager is not ready\n");
		return;
	}

	spin_lock(&mgr->lock);
	
	/* 再次检查状态（在锁内） */
	if (!pddgpu_gtt_mgr_is_ready(mgr)) {
		spin_unlock(&mgr->lock);
		drm_printf(printer, "GTT manager state changed during debug\n");
		return;
	}

	drm_printf(printer, "GTT Manager Debug Info:\n");
	drm_printf(printer, "  Total size: %llu bytes\n", mgr->mm.size << PAGE_SHIFT);
	drm_printf(printer, "  State: 0x%x\n", atomic_read(&mgr->state));
	
	drm_mm_print(&mgr->mm, printer);
	
	spin_unlock(&mgr->lock);
}

/* GTT 兼容性检查 */
static bool pddgpu_gtt_mgr_compatible(struct ttm_resource_manager *man,
                                       struct ttm_resource *res,
                                       const struct ttm_place *place,
                                       size_t size)
{
	struct ttm_range_mgr_node *node = to_ttm_range_mgr_node(res);
	struct pddgpu_gtt_mgr *mgr = to_gtt_mgr(man);

	/* 检查GTT管理器状态 */
	if (!pddgpu_gtt_mgr_is_ready(mgr)) {
		return false;
	}

	return node->mm_nodes[0].size >= PFN_UP(size);
}

/* GTT 交集检查 */
static bool pddgpu_gtt_mgr_intersects(struct ttm_resource_manager *man,
                                       struct ttm_resource *res,
                                       const struct ttm_place *place,
                                       size_t size)
{
	struct ttm_range_mgr_node *node = to_ttm_range_mgr_node(res);
	struct pddgpu_gtt_mgr *mgr = to_gtt_mgr(man);
	u64 res_start, res_end, place_start, place_end;

	/* 检查GTT管理器状态 */
	if (!pddgpu_gtt_mgr_is_ready(mgr)) {
		return false;
	}

	place_start = (u64)place->fpfn << PAGE_SHIFT;
	place_end = (u64)place->lpfn << PAGE_SHIFT;

	res_start = (u64)node->mm_nodes[0].start << PAGE_SHIFT;
	res_end = res_start + ((u64)node->mm_nodes[0].size << PAGE_SHIFT);

	return res_start < place_end && place_start < res_end;
}

/* GTT管理器函数表 */
const struct ttm_resource_manager_func pddgpu_gtt_mgr_func = {
	.alloc = pddgpu_gtt_mgr_alloc,
	.free = pddgpu_gtt_mgr_free,
	.debug = pddgpu_gtt_mgr_debug,
	.compatible = pddgpu_gtt_mgr_compatible,
	.intersects = pddgpu_gtt_mgr_intersects
};

/* GTT管理器初始化 */
int pddgpu_gtt_mgr_init(struct pddgpu_device *pdev, uint64_t gtt_size)
{
	struct pddgpu_gtt_mgr *mgr = &pdev->mman.gtt_mgr;
	struct ttm_resource_manager *man = &mgr->manager;
	int r;

	PDDGPU_DEBUG("Initializing GTT manager\n");

	/* 设置初始状态 */
	atomic_set(&mgr->state, PDDGPU_GTT_MGR_STATE_INITIALIZING);

	/* 初始化自旋锁 */
	spin_lock_init(&mgr->lock);

	/* 初始化DRM MM分配器 */
	r = drm_mm_init(&mgr->mm, 0, gtt_size >> PAGE_SHIFT);
	if (r) {
		PDDGPU_ERROR("Failed to initialize DRM MM: %d\n", r);
		pddgpu_gtt_mgr_set_error(mgr);
		return r;
	}

	/* 设置管理器属性 */
	man->func = &pddgpu_gtt_mgr_func;
	man->use_tt = true;
	man->size = gtt_size;

	/* 设置就绪状态 */
	atomic_set(&mgr->state, PDDGPU_GTT_MGR_STATE_READY);

	PDDGPU_INFO("GTT manager initialized: size=%llu\n", gtt_size);

	return 0;
}

/* GTT管理器清理 */
void pddgpu_gtt_mgr_fini(struct pddgpu_device *pdev)
{
	struct pddgpu_gtt_mgr *mgr = &pdev->mman.gtt_mgr;
	struct ttm_resource_manager *man = &mgr->manager;
	struct ttm_range_mgr_node *node;
	struct drm_mm_node *mm_node;

	PDDGPU_DEBUG("Finalizing GTT manager\n");

	/* 设置关闭状态 */
	atomic_set(&mgr->state, PDDGPU_GTT_MGR_STATE_SHUTDOWN);

	/* 清理DRM MM分配器 */
	spin_lock(&mgr->lock);
	drm_mm_takedown(&mgr->mm);
	spin_unlock(&mgr->lock);

	PDDGPU_DEBUG("GTT manager finalized\n");
}

/* GTT管理器恢复 */
int pddgpu_gtt_mgr_recover(struct pddgpu_gtt_mgr *mgr)
{
	struct pddgpu_device *pdev = container_of(mgr, struct pddgpu_device, mman.gtt_mgr);
	int r;

	PDDGPU_DEBUG("Recovering GTT manager\n");

	/* 检查设备状态 */
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		PDDGPU_ERROR("Device is shutting down, cannot recover GTT manager\n");
		return -ENODEV;
	}

	/* 清除错误状态 */
	pddgpu_gtt_mgr_clear_error(mgr);

	/* 重新初始化DRM MM分配器 */
	spin_lock(&mgr->lock);
	r = drm_mm_init(&mgr->mm, 0, mgr->mm.size);
	spin_unlock(&mgr->lock);

	if (r) {
		PDDGPU_ERROR("Failed to recover DRM MM: %d\n", r);
		pddgpu_gtt_mgr_set_error(mgr);
		return r;
	}

	/* 设置就绪状态 */
	atomic_set(&mgr->state, PDDGPU_GTT_MGR_STATE_READY);

	PDDGPU_INFO("GTT manager recovered successfully\n");

	return 0;
}

/* GTT管理器状态查询 */
bool pddgpu_gtt_mgr_is_healthy(struct pddgpu_gtt_mgr *mgr)
{
	return pddgpu_gtt_mgr_is_ready(mgr) && 
	       !(atomic_read(&mgr->state) & PDDGPU_GTT_MGR_STATE_ERROR);
}

/* GTT管理器统计信息 */
void pddgpu_gtt_mgr_get_stats(struct pddgpu_gtt_mgr *mgr,
                               struct pddgpu_gtt_stats *stats)
{
	if (!mgr || !stats)
		return;

	stats->total_size = mgr->mm.size << PAGE_SHIFT;
	stats->used_size = mgr->mm.allocated_size << PAGE_SHIFT;
	stats->state = atomic_read(&mgr->state);
	stats->is_healthy = pddgpu_gtt_mgr_is_healthy(mgr);
}
