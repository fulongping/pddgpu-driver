/*
 * PDDGPU VRAM 管理器
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#include <linux/dma-mapping.h>
#include <drm/ttm/ttm_range_manager.h>
#include <drm/drm_drv.h>
#include <drm/drm_buddy.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/delay.h>

#include "include/pddgpu_drv.h"
#include "pddgpu_vram_mgr.h"

#define PDDGPU_MAX_SG_SEGMENT_SIZE	(2UL << 30)
#define PDDGPU_VRAM_ALLOC_RETRY_COUNT	3
#define PDDGPU_VRAM_ALLOC_RETRY_DELAY	10 /* 毫秒 */

/* VRAM管理器状态标志 */
#define PDDGPU_VRAM_MGR_STATE_INITIALIZING	0x01
#define PDDGPU_VRAM_MGR_STATE_READY		0x02
#define PDDGPU_VRAM_MGR_STATE_SHUTDOWN		0x04
#define PDDGPU_VRAM_MGR_STATE_ERROR		0x08

struct pddgpu_vram_reservation {
	u64 start;
	u64 size;
	struct list_head allocated;
	struct list_head blocks;
};

/* 转换宏 */
static inline struct pddgpu_vram_mgr *
to_vram_mgr(struct ttm_resource_manager *man)
{
	return container_of(man, struct pddgpu_vram_mgr, manager);
}

static inline struct pddgpu_device *
to_pddgpu_device(struct pddgpu_vram_mgr *mgr)
{
	return container_of(mgr, struct pddgpu_device, mman.vram_mgr);
}

static inline struct drm_buddy_block *
pddgpu_vram_mgr_first_block(struct list_head *list)
{
	return list_first_entry_or_null(list, struct drm_buddy_block, link);
}

static inline bool pddgpu_is_vram_mgr_blocks_contiguous(struct list_head *head)
{
	struct drm_buddy_block *block;
	u64 start, size;

	block = pddgpu_vram_mgr_first_block(head);
	if (!block)
		return false;

	while (head != block->link.next) {
		start = pddgpu_vram_mgr_block_start(block);
		size = pddgpu_vram_mgr_block_size(block);

		block = list_entry(block->link.next, struct drm_buddy_block, link);
		if (start + size != pddgpu_vram_mgr_block_start(block))
			return false;
	}

	return true;
}

static inline u64 pddgpu_vram_mgr_blocks_size(struct list_head *head)
{
	struct drm_buddy_block *block;
	u64 size = 0;

	list_for_each_entry(block, head, link)
		size += pddgpu_vram_mgr_block_size(block);

	return size;
}

/* VRAM管理器状态检查 */
static inline bool pddgpu_vram_mgr_is_ready(struct pddgpu_vram_mgr *mgr)
{
	return (atomic_read(&mgr->state) & PDDGPU_VRAM_MGR_STATE_READY) &&
	       !(atomic_read(&mgr->state) & PDDGPU_VRAM_MGR_STATE_SHUTDOWN);
}

/* VRAM管理器错误处理 */
static inline void pddgpu_vram_mgr_set_error(struct pddgpu_vram_mgr *mgr)
{
	atomic_or(PDDGPU_VRAM_MGR_STATE_ERROR, &mgr->state);
	PDDGPU_ERROR("VRAM manager entered error state\n");
}

static inline void pddgpu_vram_mgr_clear_error(struct pddgpu_vram_mgr *mgr)
{
	atomic_and(~PDDGPU_VRAM_MGR_STATE_ERROR, &mgr->state);
}

/* VRAM 分配函数 */
static int pddgpu_vram_mgr_alloc(struct ttm_resource_manager *man,
                                  struct ttm_buffer_object *bo,
                                  const struct ttm_place *place,
                                  struct ttm_resource **res)
{
	struct pddgpu_vram_mgr *mgr = to_vram_mgr(man);
	struct pddgpu_device *pdev = to_pddgpu_device(mgr);
	struct pddgpu_bo *pbo = to_pddgpu_bo(bo);
	u64 vis_usage = 0, max_bytes, min_block_size;
	struct pddgpu_vram_mgr_resource *vres;
	u64 size, remaining_size, lpfn, fpfn;
	struct drm_buddy *mm = &mgr->mm;
	struct drm_buddy_block *block;
	unsigned long pages_per_block;
	int r, retry_count = 0;
	unsigned long flags;

	/* 检查设备状态 */
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		PDDGPU_DEBUG("Device is shutting down, skipping VRAM allocation\n");
		return -ENODEV;
	}

	/* 检查VRAM管理器状态 */
	if (!pddgpu_vram_mgr_is_ready(mgr)) {
		PDDGPU_ERROR("VRAM manager is not ready\n");
		return -ENODEV;
	}

	lpfn = (u64)place->lpfn << PAGE_SHIFT;
	if (!lpfn || lpfn > man->size)
		lpfn = man->size;

	fpfn = (u64)place->fpfn << PAGE_SHIFT;

	max_bytes = pdev->vram_size;
	if (bo->type != ttm_bo_type_kernel)
		max_bytes -= PDDGPU_VM_RESERVED_VRAM;

	/* 验证分配大小 */
	if (bo->base.size > max_bytes) {
		PDDGPU_ERROR("Allocation size %lu exceeds max VRAM size %llu\n",
		             bo->base.size, max_bytes);
		return -ENOMEM;
	}

	/* 分配VRAM资源结构 */
	vres = kzalloc(sizeof(*vres), GFP_KERNEL);
	if (!vres) {
		PDDGPU_ERROR("Failed to allocate VRAM resource structure\n");
		return -ENOMEM;
	}

	ttm_resource_init(bo, place, &vres->base);
	INIT_LIST_HEAD(&vres->blocks);

	size = PFN_UP(bo->base.size) << PAGE_SHIFT;
	remaining_size = size;

	/* 计算最小块大小 */
	pages_per_block = 1 << mgr->default_page_size;
	min_block_size = pages_per_block << PAGE_SHIFT;

	/* 重试机制 */
retry_alloc:
	mutex_lock(&mgr->lock);

	/* 再次检查状态（在锁内） */
	if (!pddgpu_vram_mgr_is_ready(mgr)) {
		mutex_unlock(&mgr->lock);
		PDDGPU_ERROR("VRAM manager state changed during allocation\n");
		kfree(vres);
		return -ENODEV;
	}

	/* 检查可见内存使用量 */
	vis_usage = atomic64_read(&mgr->vis_usage);
	if (vis_usage + size > mgr->visible_size) {
		mutex_unlock(&mgr->lock);
		PDDGPU_ERROR("Insufficient visible VRAM: requested %llu, available %llu\n",
		             size, mgr->visible_size - vis_usage);
		kfree(vres);
		return -ENOMEM;
	}

	/* 分配内存块 */
	while (remaining_size >= min_block_size) {
		u64 block_size = min_block_size;
		u64 block_start = fpfn << PAGE_SHIFT;
		u64 block_end = lpfn << PAGE_SHIFT;

		/* 尝试分配块 */
		r = drm_buddy_alloc_blocks(mm, block_start, block_end,
		                           block_size, min_block_size,
		                           &vres->blocks, vres->flags);
		if (r) {
			/* 如果分配失败，尝试更大的块 */
			block_size <<= 1;
			if (block_size <= remaining_size) {
				continue;
			}
			
			/* 分配失败，释放已分配的块 */
			mutex_unlock(&mgr->lock);
			drm_buddy_free_list(mm, &vres->blocks);
			kfree(vres);
			
			/* 重试机制 */
			if (++retry_count < PDDGPU_VRAM_ALLOC_RETRY_COUNT) {
				PDDGPU_DEBUG("VRAM allocation failed, retrying (%d/%d)\n",
				             retry_count, PDDGPU_VRAM_ALLOC_RETRY_COUNT);
				msleep(PDDGPU_VRAM_ALLOC_RETRY_DELAY);
				goto retry_alloc;
			}
			
			PDDGPU_ERROR("VRAM allocation failed after %d retries\n", retry_count);
			return -ENOMEM;
		}

		remaining_size -= block_size;
	}

	/* 更新统计信息 */
	atomic64_add(size, &mgr->used);
	atomic64_add(size, &mgr->vis_usage);

	/* 更新内存统计 */
	pddgpu_memory_stats_update_usage(pdev, TTM_PL_VRAM, size, true);

	mutex_unlock(&mgr->lock);

	/* 验证分配结果 */
	if (list_empty(&vres->blocks)) {
		PDDGPU_ERROR("No blocks allocated\n");
		kfree(vres);
		return -ENOMEM;
	}

	/* 设置资源属性 */
	vres->base.start = pddgpu_vram_mgr_block_start(
		list_first_entry(&vres->blocks, struct drm_buddy_block, link));
	vres->base.size = size;
	vres->base.num_pages = PFN_UP(size);

	*res = &vres->base;

	PDDGPU_DEBUG("VRAM allocation successful: size=%llu, start=%llu\n",
	             size, vres->base.start);

	return 0;
}

/* VRAM 释放函数 */
static void pddgpu_vram_mgr_free(struct ttm_resource_manager *man,
                                  struct ttm_resource *res)
{
	struct pddgpu_vram_mgr_resource *vres = to_pddgpu_vram_mgr_resource(res);
	struct pddgpu_vram_mgr *mgr = to_vram_mgr(man);
	struct pddgpu_device *pdev = to_pddgpu_device(mgr);
	struct drm_buddy *mm = &mgr->mm;
	struct drm_buddy_block *block;
	u64 freed_size = 0;
	unsigned long flags;

	/* 检查设备状态 */
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		PDDGPU_DEBUG("Device is shutting down, skipping VRAM free\n");
		return;
	}

	/* 检查VRAM管理器状态 */
	if (!pddgpu_vram_mgr_is_ready(mgr)) {
		PDDGPU_ERROR("VRAM manager is not ready during free\n");
		return;
	}

	/* 计算释放的大小 */
	list_for_each_entry(block, &vres->blocks, link) {
		freed_size += pddgpu_vram_mgr_block_size(block);
	}

	mutex_lock(&mgr->lock);

	/* 再次检查状态（在锁内） */
	if (!pddgpu_vram_mgr_is_ready(mgr)) {
		mutex_unlock(&mgr->lock);
		PDDGPU_ERROR("VRAM manager state changed during free\n");
		return;
	}

	/* 释放内存块 */
	drm_buddy_free_list(mm, &vres->blocks);

	/* 更新统计信息 */
	atomic64_sub(freed_size, &mgr->used);
	atomic64_sub(freed_size, &mgr->vis_usage);

	/* 更新内存统计 */
	pddgpu_memory_stats_update_usage(pdev, TTM_PL_VRAM, freed_size, false);

	mutex_unlock(&mgr->lock);

	PDDGPU_DEBUG("VRAM free successful: size=%llu\n", freed_size);
}

/* VRAM 调试函数 */
static void pddgpu_vram_mgr_debug(struct ttm_resource_manager *man,
                                   struct drm_printer *printer)
{
	struct pddgpu_vram_mgr *mgr = to_vram_mgr(man);
	struct drm_buddy *mm = &mgr->mm;
	unsigned long flags;

	/* 检查VRAM管理器状态 */
	if (!pddgpu_vram_mgr_is_ready(mgr)) {
		drm_printf(printer, "VRAM manager is not ready\n");
		return;
	}

	mutex_lock(&mgr->lock);
	
	/* 再次检查状态（在锁内） */
	if (!pddgpu_vram_mgr_is_ready(mgr)) {
		mutex_unlock(&mgr->lock);
		drm_printf(printer, "VRAM manager state changed during debug\n");
		return;
	}

	drm_printf(printer, "VRAM Manager Debug Info:\n");
	drm_printf(printer, "  Total size: %llu bytes\n", mgr->size);
	drm_printf(printer, "  Used: %llu bytes\n", atomic64_read(&mgr->used));
	drm_printf(printer, "  Visible used: %llu bytes\n", atomic64_read(&mgr->vis_usage));
	drm_printf(printer, "  State: 0x%x\n", atomic_read(&mgr->state));
	
	drm_buddy_print(mm, printer);
	
	mutex_unlock(&mgr->lock);
}

/* VRAM 兼容性检查 */
static bool pddgpu_vram_mgr_compatible(struct ttm_resource_manager *man,
                                        struct ttm_resource *res,
                                        const struct ttm_place *place,
                                        size_t size)
{
	struct pddgpu_vram_mgr_resource *vres = to_pddgpu_vram_mgr_resource(res);
	struct drm_buddy_block *block;
	u64 res_size = 0;

	/* 检查VRAM管理器状态 */
	if (!pddgpu_vram_mgr_is_ready(to_vram_mgr(man))) {
		return false;
	}

	list_for_each_entry(block, &vres->blocks, link)
		res_size += pddgpu_vram_mgr_block_size(block);

	return res_size >= size;
}

/* VRAM 交集检查 */
static bool pddgpu_vram_mgr_intersects(struct ttm_resource_manager *man,
                                        struct ttm_resource *res,
                                        const struct ttm_place *place,
                                        size_t size)
{
	struct pddgpu_vram_mgr_resource *vres = to_pddgpu_vram_mgr_resource(res);
	struct drm_buddy_block *block;
	u64 res_start, res_end, place_start, place_end;

	/* 检查VRAM管理器状态 */
	if (!pddgpu_vram_mgr_is_ready(to_vram_mgr(man))) {
		return false;
	}

	place_start = (u64)place->fpfn << PAGE_SHIFT;
	place_end = (u64)place->lpfn << PAGE_SHIFT;

	list_for_each_entry(block, &vres->blocks, link) {
		res_start = pddgpu_vram_mgr_block_start(block);
		res_end = res_start + pddgpu_vram_mgr_block_size(block);

		if (res_start < place_end && place_start < res_end)
			return true;
	}

	return false;
}

/* VRAM管理器函数表 */
const struct ttm_resource_manager_func pddgpu_vram_mgr_func = {
	.alloc = pddgpu_vram_mgr_alloc,
	.free = pddgpu_vram_mgr_free,
	.debug = pddgpu_vram_mgr_debug,
	.compatible = pddgpu_vram_mgr_compatible,
	.intersects = pddgpu_vram_mgr_intersects
};

/* VRAM管理器初始化 */
int pddgpu_vram_mgr_init(struct pddgpu_device *pdev)
{
	struct pddgpu_vram_mgr *mgr = &pdev->mman.vram_mgr;
	struct ttm_resource_manager *man = &mgr->manager;
	int r;

	PDDGPU_DEBUG("Initializing VRAM manager\n");

	/* 设置初始状态 */
	atomic_set(&mgr->state, PDDGPU_VRAM_MGR_STATE_INITIALIZING);

	/* 初始化互斥锁 */
	mutex_init(&mgr->lock);

	/* 初始化DRM Buddy分配器 */
	r = drm_buddy_init(&mgr->mm, pdev->vram_size, mgr->default_page_size);
	if (r) {
		PDDGPU_ERROR("Failed to initialize DRM Buddy: %d\n", r);
		pddgpu_vram_mgr_set_error(mgr);
		return r;
	}

	/* 设置管理器属性 */
	man->func = &pddgpu_vram_mgr_func;
	man->use_tt = true;
	man->size = pdev->vram_size;

	/* 初始化统计信息 */
	atomic64_set(&mgr->used, 0);
	atomic64_set(&mgr->vis_usage, 0);
	mgr->size = pdev->vram_size;
	mgr->visible_size = pdev->gmc.visible_vram_size;

	/* 设置就绪状态 */
	atomic_set(&mgr->state, PDDGPU_VRAM_MGR_STATE_READY);

	PDDGPU_INFO("VRAM manager initialized: size=%llu, visible=%llu\n",
	            mgr->size, mgr->visible_size);

	return 0;
}

/* VRAM管理器清理 */
void pddgpu_vram_mgr_fini(struct pddgpu_device *pdev)
{
	struct pddgpu_vram_mgr *mgr = &pdev->mman.vram_mgr;
	struct ttm_resource_manager *man = &mgr->manager;
	struct pddgpu_vram_reservation *rsv, *temp;

	PDDGPU_DEBUG("Finalizing VRAM manager\n");

	/* 设置关闭状态 */
	atomic_set(&mgr->state, PDDGPU_VRAM_MGR_STATE_SHUTDOWN);

	/* 清理DRM Buddy分配器 */
	mutex_lock(&mgr->lock);
	drm_buddy_fini(&mgr->mm);
	mutex_unlock(&mgr->lock);

	/* 清理预留列表 */
	list_for_each_entry_safe(rsv, temp, &mgr->reservations, list) {
		list_del(&rsv->list);
		kfree(rsv);
	}

	PDDGPU_DEBUG("VRAM manager finalized\n");
}

/* VRAM管理器恢复 */
int pddgpu_vram_mgr_recover(struct pddgpu_vram_mgr *mgr)
{
	struct pddgpu_device *pdev = to_pddgpu_device(mgr);
	int r;

	PDDGPU_DEBUG("Recovering VRAM manager\n");

	/* 检查设备状态 */
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		PDDGPU_ERROR("Device is shutting down, cannot recover VRAM manager\n");
		return -ENODEV;
	}

	/* 清除错误状态 */
	pddgpu_vram_mgr_clear_error(mgr);

	/* 重新初始化DRM Buddy分配器 */
	mutex_lock(&mgr->lock);
	r = drm_buddy_init(&mgr->mm, mgr->size, mgr->default_page_size);
	mutex_unlock(&mgr->lock);

	if (r) {
		PDDGPU_ERROR("Failed to recover DRM Buddy: %d\n", r);
		pddgpu_vram_mgr_set_error(mgr);
		return r;
	}

	/* 重置统计信息 */
	atomic64_set(&mgr->used, 0);
	atomic64_set(&mgr->vis_usage, 0);

	/* 设置就绪状态 */
	atomic_set(&mgr->state, PDDGPU_VRAM_MGR_STATE_READY);

	PDDGPU_INFO("VRAM manager recovered successfully\n");

	return 0;
}

/* VRAM管理器状态查询 */
bool pddgpu_vram_mgr_is_healthy(struct pddgpu_vram_mgr *mgr)
{
	return pddgpu_vram_mgr_is_ready(mgr) && 
	       !(atomic_read(&mgr->state) & PDDGPU_VRAM_MGR_STATE_ERROR);
}

/* VRAM管理器统计信息 */
void pddgpu_vram_mgr_get_stats(struct pddgpu_vram_mgr *mgr,
                                struct pddgpu_vram_stats *stats)
{
	if (!mgr || !stats)
		return;

	stats->total_size = mgr->size;
	stats->used_size = atomic64_read(&mgr->used);
	stats->visible_used = atomic64_read(&mgr->vis_usage);
	stats->state = atomic_read(&mgr->state);
	stats->is_healthy = pddgpu_vram_mgr_is_healthy(mgr);
}
