/*
 * PDDGPU VRAM 管理器
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#include <linux/dma-mapping.h>
#include <drm/ttm/ttm_range_manager.h>
#include <drm/drm_drv.h>
#include <drm/drm_buddy.h>

#include "include/pddgpu_drv.h"
#include "pddgpu_vram_mgr.h"

#define PDDGPU_MAX_SG_SEGMENT_SIZE	(2UL << 30)

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
	int r;

	lpfn = (u64)place->lpfn << PAGE_SHIFT;
	if (!lpfn || lpfn > man->size)
		lpfn = man->size;

	fpfn = (u64)place->fpfn << PAGE_SHIFT;

	max_bytes = pdev->vram_size;
	if (bo->type != ttm_bo_type_kernel)
		max_bytes -= PDDGPU_VM_RESERVED_VRAM;

	if (pbo->flags & PDDGPU_GEM_CREATE_VRAM_CONTIGUOUS) {
		pages_per_block = ~0ul;
	} else {
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
		pages_per_block = HPAGE_PMD_NR;
#else
		/* 默认 2MB */
		pages_per_block = 2UL << (20UL - PAGE_SHIFT);
#endif
		pages_per_block = max_t(u32, pages_per_block,
					bo->page_alignment);
	}

	vres = kzalloc(sizeof(*vres), GFP_KERNEL);
	if (!vres)
		return -ENOMEM;

	ttm_resource_init(bo, place, &vres->base);

	/* 快速检查是否有足够的 VRAM */
	if (ttm_resource_manager_usage(man) > max_bytes) {
		r = -ENOSPC;
		goto error_fini;
	}

	INIT_LIST_HEAD(&vres->blocks);

	if (place->flags & TTM_PL_FLAG_TOPDOWN)
		vres->flags |= DRM_BUDDY_TOPDOWN_ALLOCATION;

	if (pbo->flags & PDDGPU_GEM_CREATE_VRAM_CONTIGUOUS)
		vres->flags |= DRM_BUDDY_CONTIGUOUS_ALLOCATION;

	if (pbo->flags & PDDGPU_GEM_CREATE_VRAM_CLEARED)
		vres->flags |= DRM_BUDDY_CLEAR_ALLOCATION;

	if (fpfn || lpfn != mgr->mm.size)
		/* 在指定范围内分配块 */
		vres->flags |= DRM_BUDDY_RANGE_ALLOCATION;

	remaining_size = (u64)vres->base.size;

	mutex_lock(&mgr->lock);
	while (remaining_size) {
		if (bo->page_alignment)
			min_block_size = (u64)bo->page_alignment << PAGE_SHIFT;
		else
			min_block_size = mgr->default_page_size;

		size = remaining_size;

		if ((size >= (u64)pages_per_block << PAGE_SHIFT) &&
		    !(size & (((u64)pages_per_block << PAGE_SHIFT) - 1)))
			min_block_size = (u64)pages_per_block << PAGE_SHIFT;

		BUG_ON(min_block_size < mm->chunk_size);

		r = drm_buddy_alloc_blocks(mm, fpfn,
					   lpfn,
					   size,
					   min_block_size,
					   &vres->blocks,
					   vres->flags);

		if (unlikely(r == -ENOSPC) && pages_per_block == ~0ul &&
		    !(place->flags & TTM_PL_FLAG_CONTIGUOUS)) {
			vres->flags &= ~DRM_BUDDY_CONTIGUOUS_ALLOCATION;
			pages_per_block = max_t(u32, 2UL << (20UL - PAGE_SHIFT),
						bo->page_alignment);

			continue;
		}

		if (unlikely(r))
			goto error_free_blocks;

		if (size > remaining_size)
			remaining_size = 0;
		else
			remaining_size -= size;
	}
	mutex_unlock(&mgr->lock);

	vres->base.start = 0;
	size = max_t(u64, pddgpu_vram_mgr_blocks_size(&vres->blocks),
		     vres->base.size);
	list_for_each_entry(block, &vres->blocks, link) {
		unsigned long start;

		start = pddgpu_vram_mgr_block_start(block) +
			pddgpu_vram_mgr_block_size(block);
		start >>= PAGE_SHIFT;

		if (start > PFN_UP(size))
			start -= PFN_UP(size);
		else
			start = 0;
		vres->base.start = max(vres->base.start, start);

		/* 简化版本：假设所有块都是可见的 */
		vis_usage += pddgpu_vram_mgr_block_size(block);
	}

	if (pddgpu_is_vram_mgr_blocks_contiguous(&vres->blocks))
		vres->base.placement |= TTM_PL_FLAG_CONTIGUOUS;

	/* 设置缓存策略 */
	vres->base.bus.caching = ttm_write_combined;

	atomic64_add(vis_usage, &mgr->vis_usage);
	*res = &vres->base;
	return 0;

error_free_blocks:
	drm_buddy_free_list(mm, &vres->blocks, 0);
	mutex_unlock(&mgr->lock);
error_fini:
	ttm_resource_fini(man, &vres->base);
	kfree(vres);

	return r;
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
	uint64_t vis_usage = 0;

	mutex_lock(&mgr->lock);
	list_for_each_entry(block, &vres->blocks, link)
		vis_usage += pddgpu_vram_mgr_block_size(block);

	drm_buddy_free_list(mm, &vres->blocks, 0);
	atomic64_sub(vis_usage, &mgr->vis_usage);
	mutex_unlock(&mgr->lock);

	ttm_resource_fini(man, res);
	kfree(vres);
}

/* 调试函数 */
static void pddgpu_vram_mgr_debug(struct ttm_resource_manager *man,
                                   struct drm_printer *printer)
{
	struct pddgpu_vram_mgr *mgr = to_vram_mgr(man);
	struct drm_buddy *mm = &mgr->mm;

	drm_printf(printer, "  vis usage:%llu\n",
		   atomic64_read(&mgr->vis_usage));

	mutex_lock(&mgr->lock);
	drm_printf(printer, "default_page_size: %lluKiB\n",
		   mgr->default_page_size >> 10);

	drm_buddy_print(mm, printer);
	mutex_unlock(&mgr->lock);
}

/* 兼容性检查 */
static bool pddgpu_vram_mgr_compatible(struct ttm_resource_manager *man,
                                        struct ttm_resource *res,
                                        const struct ttm_place *place,
                                        size_t size)
{
	struct pddgpu_vram_mgr_resource *vres = to_pddgpu_vram_mgr_resource(res);
	struct drm_buddy_block *block;

	if (place->fpfn || place->lpfn != man->size)
		return false;

	list_for_each_entry(block, &vres->blocks, link) {
		if (pddgpu_vram_mgr_block_size(block) < size)
			return false;
	}

	return true;
}

/* 重叠检查 */
static bool pddgpu_vram_mgr_intersects(struct ttm_resource_manager *man,
                                        struct ttm_resource *res,
                                        const struct ttm_place *place,
                                        size_t size)
{
	struct pddgpu_vram_mgr_resource *vres = to_pddgpu_vram_mgr_resource(res);
	struct drm_buddy_block *block;
	u64 place_start, place_end;

	place_start = (u64)place->fpfn << PAGE_SHIFT;
	place_end = (u64)place->lpfn << PAGE_SHIFT;

	list_for_each_entry(block, &vres->blocks, link) {
		u64 block_start = pddgpu_vram_mgr_block_start(block);
		u64 block_end = block_start + pddgpu_vram_mgr_block_size(block);

		if (block_start < place_end && block_end > place_start)
			return true;
	}

	return false;
}

/* 管理器函数表 */
const struct ttm_resource_manager_func pddgpu_vram_mgr_func = {
	.alloc = pddgpu_vram_mgr_alloc,
	.free = pddgpu_vram_mgr_free,
	.debug = pddgpu_vram_mgr_debug,
	.intersects = pddgpu_vram_mgr_intersects,
	.compatible = pddgpu_vram_mgr_compatible,
};

/* VRAM 管理器初始化 */
int pddgpu_vram_mgr_init(struct pddgpu_device *pdev)
{
	struct pddgpu_vram_mgr *mgr = &pdev->mman.vram_mgr;
	struct ttm_resource_manager *man = &mgr->manager;
	int err;

	/* 初始化 TTM 资源管理器 */
	ttm_resource_manager_init(man, &pdev->mman.bdev,
				 pdev->vram_size);

	mutex_init(&mgr->lock);
	INIT_LIST_HEAD(&mgr->reservations_pending);
	INIT_LIST_HEAD(&mgr->reserved_pages);
	mgr->default_page_size = PAGE_SIZE;

	/* 设置管理器函数 */
	man->func = &pddgpu_vram_mgr_func;

	/* 初始化 DRM Buddy 分配器 */
	err = drm_buddy_init(&mgr->mm, man->size, PAGE_SIZE);
	if (err) {
		PDDGPU_ERROR("Failed to initialize DRM Buddy allocator\n");
		return err;
	}

	/* 注册到 TTM 设备 */
	ttm_set_driver_manager(&pdev->mman.bdev, TTM_PL_VRAM, &mgr->manager);
	ttm_resource_manager_set_used(man, true);

	PDDGPU_DEBUG("VRAM manager initialized: size=%llu\n", man->size);
	return 0;
}

/* VRAM 管理器清理 */
void pddgpu_vram_mgr_fini(struct pddgpu_device *pdev)
{
	struct pddgpu_vram_mgr *mgr = &pdev->mman.vram_mgr;
	struct ttm_resource_manager *man = &mgr->manager;
	int ret;
	struct pddgpu_vram_reservation *rsv, *temp;

	ttm_resource_manager_set_used(man, false);

	ret = ttm_resource_manager_evict_all(&pdev->mman.bdev, man);
	if (ret) {
		PDDGPU_ERROR("Failed to evict all VRAM resources\n");
		return;
	}

	mutex_lock(&mgr->lock);
	list_for_each_entry_safe(rsv, temp, &mgr->reservations_pending, blocks)
		kfree(rsv);

	list_for_each_entry_safe(rsv, temp, &mgr->reserved_pages, blocks) {
		drm_buddy_free_list(&mgr->mm, &rsv->allocated, 0);
		kfree(rsv);
	}

	drm_buddy_fini(&mgr->mm);
	mutex_unlock(&mgr->lock);

	ttm_resource_manager_cleanup(man);
	ttm_set_driver_manager(&pdev->mman.bdev, TTM_PL_VRAM, NULL);

	PDDGPU_DEBUG("VRAM manager finalized\n");
}
