/*
 * PDDGPU GTT 管理器
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#ifndef __PDDGPU_GTT_MGR_H__
#define __PDDGPU_GTT_MGR_H__

#include <drm/drm_mm.h>
#include <drm/ttm/ttm_resource.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/types.h>

struct pddgpu_device;

/* GTT管理器状态标志 */
#define PDDGPU_GTT_MGR_STATE_INITIALIZING	0x01
#define PDDGPU_GTT_MGR_STATE_READY		0x02
#define PDDGPU_GTT_MGR_STATE_SHUTDOWN		0x04
#define PDDGPU_GTT_MGR_STATE_ERROR		0x08

/* GTT统计信息结构 */
struct pddgpu_gtt_stats {
	u64 total_size;
	u64 used_size;
	u32 state;
	bool is_healthy;
};

/* PDDGPU GTT 管理器 */
struct pddgpu_gtt_mgr {
	struct ttm_resource_manager manager;
	struct drm_mm mm;
	spinlock_t lock;
	atomic_t state;
};

/* 转换宏 */
static inline struct pddgpu_gtt_mgr *
to_pddgpu_gtt_mgr(struct ttm_resource_manager *man)
{
	return container_of(man, struct pddgpu_gtt_mgr, manager);
}

static inline struct pddgpu_device *
to_pddgpu_device_from_gtt_mgr(struct pddgpu_gtt_mgr *mgr)
{
	return container_of(mgr, struct pddgpu_device, mman.gtt_mgr);
}

/* 函数声明 */
int pddgpu_gtt_mgr_init(struct pddgpu_device *pdev, uint64_t gtt_size);
void pddgpu_gtt_mgr_fini(struct pddgpu_device *pdev);
int pddgpu_gtt_mgr_recover(struct pddgpu_gtt_mgr *mgr);
bool pddgpu_gtt_mgr_is_healthy(struct pddgpu_gtt_mgr *mgr);
void pddgpu_gtt_mgr_get_stats(struct pddgpu_gtt_mgr *mgr,
                               struct pddgpu_gtt_stats *stats);

/* 辅助函数 */
static inline bool pddgpu_gtt_mgr_has_gart_addr(struct ttm_resource *res)
{
	struct ttm_range_mgr_node *node = to_ttm_range_mgr_node(res);
	return drm_mm_node_allocated(&node->mm_nodes[0]);
}

#endif /* __PDDGPU_GTT_MGR_H__ */
