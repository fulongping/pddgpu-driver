/*
 * PDDGPU对象管理
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#ifndef __PDDGPU_OBJECT_H__
#define __PDDGPU_OBJECT_H__

#include <drm/drm_gem.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_resource.h>

#include "include/pddgpu_drv.h"

/* PDDGPU BO参数 */
struct pddgpu_bo_param {
	unsigned long size;
	int byte_align;
	u32 bo_ptr_size;
	u32 domain;
	u32 preferred_domain;
	u64 flags;
	enum ttm_bo_type type;
	bool no_wait_gpu;
	struct dma_resv *resv;
	void (*destroy)(struct ttm_buffer_object *);
	/* xcp partition number plus 1, 0 means any partition */
	int8_t xcp_id_plus1;
};

/* PDDGPU BO */
struct pddgpu_bo {
	/* Protected by tbo.reserved */
	u32 preferred_domains;
	u32 allowed_domains;
	struct ttm_place placements[TTM_PL_MAX];
	struct ttm_placement placement;
	struct ttm_buffer_object tbo;
	struct ttm_bo_kmap_obj kmap;
	u64 flags;
	/* per VM structure for page tables and with virtual addresses */
	struct pddgpu_vm_bo_base *vm_bo;
	/* Constant after initialization */
	struct pddgpu_bo *parent;

#ifdef CONFIG_MMU_NOTIFIER
	struct mmu_interval_notifier notifier;
#endif

	/*
	 * For GPUs with spatial partitioning, xcp partition number, -1 means
	 * any partition. For other ASICs without spatial partition, always 0
	 * for memory accounting.
	 */
	int8_t xcp_id;
};

/* PDDGPU VRAM管理器 */
struct pddgpu_vram_mgr {
	struct ttm_resource_manager manager;
	spinlock_t lock;
	u64 size;
	u64 used;
};

/* PDDGPU GTT管理器 */
struct pddgpu_gtt_mgr {
	struct ttm_resource_manager manager;
	spinlock_t lock;
	u64 size;
	u64 used;
};

/* 函数声明 */
int pddgpu_bo_create(struct pddgpu_device *pdev, struct pddgpu_bo_param *bp,
                     struct pddgpu_bo **bo_ptr);
void pddgpu_bo_unref(struct pddgpu_bo **bo);
void pddgpu_bo_destroy(struct ttm_buffer_object *tbo);
int pddgpu_bo_create_kernel(struct pddgpu_device *pdev, unsigned long size,
                            int domain, struct pddgpu_bo **bo_ptr,
                            u64 *gpu_addr, void **cpu_addr);
void pddgpu_bo_free_kernel(struct pddgpu_bo **bo, u64 *gpu_addr,
                           void **cpu_addr);

/* 辅助函数 */
bool pddgpu_bo_validate_size(struct pddgpu_device *pdev, unsigned long size, u32 domain);
void pddgpu_bo_placement_from_domain(struct pddgpu_bo *bo, u32 domain);
bool pddgpu_bo_support_uswc(u64 bo_flags);
u64 pddgpu_bo_gpu_offset(struct pddgpu_bo *bo);
int pddgpu_bo_pin(struct pddgpu_bo *bo, u32 domain);
void pddgpu_bo_unpin(struct pddgpu_bo *bo);
int pddgpu_bo_kmap(struct pddgpu_bo *bo, void **ptr);
void *pddgpu_bo_kptr(struct pddgpu_bo *bo);
void pddgpu_bo_kunmap(struct pddgpu_bo *bo);

/* VRAM管理器函数 */
int pddgpu_vram_mgr_init(struct pddgpu_device *pdev);
void pddgpu_vram_mgr_fini(struct pddgpu_device *pdev);

/* GTT管理器函数 */
int pddgpu_gtt_mgr_init(struct pddgpu_device *pdev);
void pddgpu_gtt_mgr_fini(struct pddgpu_device *pdev);

/* 转换宏 */
#define to_pddgpu_bo(x) container_of(x, struct pddgpu_bo, tbo)
#define to_pddgpu_vram_mgr(x) container_of(x, struct pddgpu_vram_mgr, manager)
#define to_pddgpu_gtt_mgr(x) container_of(x, struct pddgpu_gtt_mgr, manager)

/* 常量定义 */
#define PDDGPU_MAX_BO_SIZE (1ULL << 30)  /* 1GB */
#define PDDGPU_MAX_ALIGNMENT (1 << 20)   /* 1MB */

#endif /* __PDDGPU_OBJECT_H__ */
