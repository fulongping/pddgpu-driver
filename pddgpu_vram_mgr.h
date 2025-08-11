/*
 * PDDGPU VRAM 管理器
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#ifndef __PDDGPU_VRAM_MGR_H__
#define __PDDGPU_VRAM_MGR_H__

#include <drm/drm_buddy.h>
#include <drm/ttm/ttm_resource.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/types.h>

struct pddgpu_device;

/* VRAM管理器状态标志 */
#define PDDGPU_VRAM_MGR_STATE_INITIALIZING	0x01
#define PDDGPU_VRAM_MGR_STATE_READY		0x02
#define PDDGPU_VRAM_MGR_STATE_SHUTDOWN		0x04
#define PDDGPU_VRAM_MGR_STATE_ERROR		0x08

/* VRAM统计信息结构 */
struct pddgpu_vram_stats {
	u64 total_size;
	u64 used_size;
	u64 visible_used;
	u32 state;
	bool is_healthy;
};

/* PDDGPU VRAM 管理器 */
struct pddgpu_vram_mgr {
	struct ttm_resource_manager manager;
	struct drm_buddy mm;
	/* 保护缓冲区对象访问 */
	struct mutex lock;
	struct list_head reservations_pending;
	struct list_head reserved_pages;
	atomic64_t vis_usage;
	atomic64_t used;
	atomic_t state;
	u64 default_page_size;
	u64 size;
	u64 visible_size;
};

/* PDDGPU VRAM 管理器资源 */
struct pddgpu_vram_mgr_resource {
	struct ttm_resource base;
	struct list_head blocks;
	unsigned long flags;
};

/* 转换宏 */
static inline struct pddgpu_vram_mgr *
to_pddgpu_vram_mgr(struct ttm_resource_manager *man)
{
	return container_of(man, struct pddgpu_vram_mgr, manager);
}

static inline struct pddgpu_device *
to_pddgpu_device(struct pddgpu_vram_mgr *mgr)
{
	return container_of(mgr, struct pddgpu_device, mman.vram_mgr);
}

/* Buddy 块操作 */
static inline u64 pddgpu_vram_mgr_block_start(struct drm_buddy_block *block)
{
	return drm_buddy_block_offset(block);
}

static inline u64 pddgpu_vram_mgr_block_size(struct drm_buddy_block *block)
{
	return (u64)PAGE_SIZE << drm_buddy_block_order(block);
}

static inline bool pddgpu_vram_mgr_is_cleared(struct drm_buddy_block *block)
{
	return drm_buddy_block_is_clear(block);
}

static inline struct pddgpu_vram_mgr_resource *
to_pddgpu_vram_mgr_resource(struct ttm_resource *res)
{
	return container_of(res, struct pddgpu_vram_mgr_resource, base);
}

static inline void pddgpu_vram_mgr_set_cleared(struct ttm_resource *res)
{
	to_pddgpu_vram_mgr_resource(res)->flags |= DRM_BUDDY_CLEARED;
}

/* 函数声明 */
int pddgpu_vram_mgr_init(struct pddgpu_device *pdev);
void pddgpu_vram_mgr_fini(struct pddgpu_device *pdev);
int pddgpu_vram_mgr_recover(struct pddgpu_vram_mgr *mgr);
bool pddgpu_vram_mgr_is_healthy(struct pddgpu_vram_mgr *mgr);
void pddgpu_vram_mgr_get_stats(struct pddgpu_vram_mgr *mgr,
                                struct pddgpu_vram_stats *stats);

/* 辅助函数 */
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

#endif /* __PDDGPU_VRAM_MGR_H__ */
