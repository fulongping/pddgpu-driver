/*
 * PDDGPU对象管理实现
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/io.h>

#include <drm/drm_gem.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_resource.h>
#include <drm/ttm/ttm_tt.h>

#include "pddgpu_object.h"

/* TTM BO函数 */
static const struct ttm_device_funcs pddgpu_ttm_funcs = {
	.ttm_tt_create = ttm_tt_create,
	.ttm_tt_populate = ttm_tt_populate,
	.ttm_tt_unpopulate = ttm_tt_unpopulate,
	.ttm_tt_destroy = ttm_tt_destroy,
	.eviction_valuable = ttm_bo_eviction_valuable,
	.eviction_fence = ttm_bo_eviction_fence,
	.move_notify = NULL,
	.delete_mem_notify = NULL,
	.release_notify = NULL,
};

/* TTM BO验证函数 */
static int pddgpu_bo_verify_access(struct ttm_buffer_object *bo, struct file *filp)
{
	return 0;
}

/* TTM BO移动函数 */
static int pddgpu_bo_move(struct ttm_buffer_object *bo, bool evict,
                          struct ttm_operation_ctx *ctx,
                          struct ttm_resource *new_mem,
                          struct ttm_place *hop)
{
	struct pddgpu_bo *abo = to_pddgpu_bo(bo);
	struct pddgpu_device *pdev = pddgpu_ttm_pdev(bo->bdev);
	int ret;

	PDDGPU_DEBUG("Moving BO: size=%lu, new_mem=%p\n", bo->base.size, new_mem);

	/* 开始内存移动统计 */
	pddgpu_memory_stats_move_start(pdev, abo);

	ret = ttm_bo_move_memcpy(bo, evict, ctx, new_mem);
	if (ret) {
		PDDGPU_ERROR("Failed to move BO: %d\n", ret);
		return ret;
	}

	/* 完成内存移动统计 */
	pddgpu_memory_stats_move_end(pdev, abo);

	/* 更新BO信息 */
	abo->domain = new_mem->mem_type;
	abo->size = bo->base.size;

	return 0;
}

/* TTM BO函数表 */
static const struct ttm_buffer_object_funcs pddgpu_bo_funcs = {
	.verify_access = pddgpu_bo_verify_access,
	.io_mem_reserve = &ttm_bo_default_io_mem_reserve,
	.io_mem_free = &ttm_bo_default_io_mem_free,
	.move = pddgpu_bo_move,
	.swap_notify = NULL,
	.delete_mem_notify = NULL,
	.release_notify = NULL,
};

/* 创建BO */
int pddgpu_bo_create(struct pddgpu_device *pdev, struct pddgpu_bo_param *bp, struct pddgpu_bo **bo_ptr)
{
	struct ttm_operation_ctx ctx = {
		.interruptible = (bp->type != ttm_bo_type_kernel),
		.no_wait_gpu = bp->no_wait_gpu,
		/* We opt to avoid OOM on system pages allocations */
		.gfp_retry_mayfail = true,
		.allow_res_evict = bp->type != ttm_bo_type_kernel,
		.resv = bp->resv
	};
	struct pddgpu_bo *bo;
	unsigned long page_align, size = bp->size;
	int r;

	/* 开始内存分配统计 */
	pddgpu_memory_stats_alloc_start(pdev, NULL, size, bp->domain);

	/* 验证大小和域 */
	if (!pddgpu_bo_validate_size(pdev, size, bp->domain)) {
		pddgpu_memory_stats_alloc_end(pdev, NULL, -ENOMEM);
		return -ENOMEM;
	}

	/* 确保BO结构大小足够 */
	BUG_ON(bp->bo_ptr_size < sizeof(struct pddgpu_bo));

	*bo_ptr = NULL;
	bo = kvzalloc(bp->bo_ptr_size, GFP_KERNEL);
	if (bo == NULL) {
		pddgpu_memory_stats_alloc_end(pdev, NULL, -ENOMEM);
		return -ENOMEM;
	}

	/* 初始化GEM对象 */
	drm_gem_private_object_init(pdev_to_drm(pdev), &bo->tbo.base, size);
	bo->tbo.base.funcs = &pddgpu_gem_object_funcs;
	bo->vm_bo = NULL;

	/* 设置首选和允许的域 */
	bo->preferred_domains = bp->preferred_domain ? bp->preferred_domain : bp->domain;
	bo->allowed_domains = bo->preferred_domains;
	if (bp->type != ttm_bo_type_kernel &&
	    !(bp->flags & PDDGPU_GEM_CREATE_DISCARDABLE) &&
	    bo->allowed_domains == PDDGPU_GEM_DOMAIN_VRAM)
		bo->allowed_domains |= PDDGPU_GEM_DOMAIN_GTT;

	bo->flags = bp->flags;

	/* 设置xcp_id */
	// gmc.mem_partitions用于指示GPU是否支持空间分区（spatial partitioning），
	// 如果mem_partitions非零，表示该GPU支持多个内存分区（如多XCP），
	// 此时BO对象的xcp_id可用于指定分配到哪个分区；否则xcp_id恒为0。
	if (pdev->gmc.mem_partitions)
		/* For GPUs with spatial partitioning, bo->xcp_id=-1 means any partition */
		bo->xcp_id = bp->xcp_id_plus1 - 1;
	else
		/* For GPUs without spatial partitioning */
		bo->xcp_id = 0;

	/* 检查USWC支持 */
	if (!pddgpu_bo_support_uswc(bo->flags))
		bo->flags &= ~PDDGPU_GEM_CREATE_CPU_GTT_USWC;

	/* 设置TTM设备 */
	bo->tbo.bdev = &pdev->mman.bdev;

	/* 设置放置策略 */
	pddgpu_bo_placement_from_domain(bo, bp->domain);

	/* 设置优先级 */
	if (bp->type == ttm_bo_type_kernel)
		bo->tbo.priority = 2;
	else if (!(bp->flags & PDDGPU_GEM_CREATE_DISCARDABLE))
		bo->tbo.priority = 1;

	/* 设置销毁函数 */
	if (!bp->destroy)
		bp->destroy = &pddgpu_bo_destroy;

	/* 计算页面对齐 */
	page_align = ALIGN(bp->byte_align, PAGE_SIZE) >> PAGE_SHIFT;
	size = ALIGN(size, PAGE_SIZE);

	/* 通过TTM初始化缓冲区 */
	r = ttm_bo_init_reserved(&pdev->mman.bdev, &bo->tbo, bp->type,
				 &bo->placement, page_align, &ctx, NULL,
				 bp->resv, bp->destroy);
	if (unlikely(r != 0)) {
		pddgpu_memory_stats_alloc_end(pdev, bo, r);
		return r;
	}

	/* 报告移动的字节数 */
	if (!pddgpu_gmc_vram_full_visible(&pdev->gmc) &&
	    pddgpu_res_cpu_visible(pdev, bo->tbo.resource))
		/*
		 * 为什么需要move？
		 * 变量ctx.bytes_moved用于记录在TTM缓冲区对象初始化过程中，内存迁移（move）操作所移动的字节数。
		 * 某些情况下（如BO首次分配或内存回收时），TTM可能会将BO从一个内存区域迁移到另一个区域（如从GTT到VRAM），
		 * 这时需要统计迁移的字节数以便驱动层进行性能分析或调度优化。
		 */
		pddgpu_cs_report_moved_bytes(pdev, ctx.bytes_moved,
					     ctx.bytes_moved);
	else
		pddgpu_cs_report_moved_bytes(pdev, ctx.bytes_moved, 0);

	/* VRAM清理（如果需要） */
	if (bp->flags & PDDGPU_GEM_CREATE_VRAM_CLEARED &&
	    bo->tbo.resource->mem_type == TTM_PL_VRAM) {
		struct dma_fence *fence;

		r = pddgpu_ttm_clear_buffer(bo, bo->tbo.base.resv, &fence);
		if (unlikely(r))
			goto fail_unreserve;

		dma_resv_add_fence(bo->tbo.base.resv, fence,
				   DMA_RESV_USAGE_KERNEL);
		dma_fence_put(fence);
	}

	/* 如果没有预留，则取消预留 */
	if (!bp->resv)
		pddgpu_bo_unreserve(bo);

	*bo_ptr = bo;

	/* 完成内存分配统计 */
	pddgpu_memory_stats_alloc_end(pdev, bo, 0);

	PDDGPU_DEBUG("BO created successfully: %p, size=%lu, domain=0x%x\n",
	             bo, size, bp->domain);

	/* 对于用户空间BO，CPU_ACCESS_REQUIRED只作为提示 */
	if (bp->type == ttm_bo_type_device)
		bo->flags &= ~PDDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;

	return 0;

fail_unreserve:
	if (!bp->resv)
		dma_resv_unlock(bo->tbo.base.resv);
	pddgpu_bo_unref(&bo);
	return r;
}

/* 释放BO引用 */
void pddgpu_bo_unref(struct pddgpu_bo **bo)
{
	struct ttm_buffer_object *tbo = &(*bo)->tbo;

	PDDGPU_DEBUG("Unref BO: %p\n", *bo);

	ttm_bo_put(tbo);
	*bo = NULL;
}

/* 销毁BO */
void pddgpu_bo_destroy(struct ttm_buffer_object *tbo)
{
	struct pddgpu_bo *bo = to_pddgpu_bo(tbo);
	struct pddgpu_device *pdev = pddgpu_ttm_pdev(tbo->bdev);

	PDDGPU_DEBUG("Destroying BO: %p\n", bo);

	/* 开始内存释放统计 */
	pddgpu_memory_stats_free_start(pdev, bo);

	/* 清理映射 */
	if (bo->kmap.virtual) {
		ttm_bo_kunmap(tbo, &bo->kmap);
	}

#ifdef CONFIG_MMU_NOTIFIER
	if (bo->notifier.ops)
		mmu_interval_notifier_remove(&bo->notifier);
#endif

	/* 完成内存释放统计 */
	pddgpu_memory_stats_free_end(pdev, bo);

	/* 释放BO结构 */
	kvfree(bo);
}

/* 创建内核BO */
int pddgpu_bo_create_kernel(struct pddgpu_device *pdev, unsigned long size,
                            int domain, struct pddgpu_bo **bo_ptr,
                            u64 *gpu_addr, void **cpu_addr)
{
	struct pddgpu_bo_param bp = {};
	int r;

	if (!size) {
		pddgpu_bo_unref(bo_ptr);
		return 0;
	}

	memset(&bp, 0, sizeof(bp));
	bp.size = size;
	bp.byte_align = PAGE_SIZE;
	bp.domain = domain;
	bp.flags = cpu_addr ? PDDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED
		: PDDGPU_GEM_CREATE_NO_CPU_ACCESS;
	bp.flags |= PDDGPU_GEM_CREATE_VRAM_CONTIGUOUS;
	bp.type = ttm_bo_type_kernel;
	bp.resv = NULL;
	bp.bo_ptr_size = sizeof(struct pddgpu_bo);

	if (!*bo_ptr) {
		r = pddgpu_bo_create(pdev, &bp, bo_ptr);
		if (r) {
			PDDGPU_ERROR("(%d) failed to allocate kernel bo\n", r);
			return r;
		}
	}

	r = pddgpu_bo_reserve(*bo_ptr, false);
	if (r) {
		PDDGPU_ERROR("(%d) failed to reserve kernel bo\n", r);
		goto error_free;
	}

	r = pddgpu_bo_pin(*bo_ptr, domain);
	if (r) {
		PDDGPU_ERROR("(%d) kernel bo pin failed\n", r);
		goto error_unreserve;
	}

	r = pddgpu_ttm_alloc_gart(&(*bo_ptr)->tbo);
	if (r) {
		PDDGPU_ERROR("%p bind failed\n", *bo_ptr);
		goto error_unpin;
	}

	if (gpu_addr)
		*gpu_addr = pddgpu_bo_gpu_offset(*bo_ptr);

	if (cpu_addr) {
		r = pddgpu_bo_kmap(*bo_ptr, cpu_addr);
		if (r) {
			PDDGPU_ERROR("(%d) kernel bo map failed\n", r);
			goto error_unpin;
		}
	}

	return 0;

error_unpin:
	pddgpu_bo_unpin(*bo_ptr);
error_unreserve:
	pddgpu_bo_unreserve(*bo_ptr);
error_free:
	if (!*bo_ptr)
		pddgpu_bo_unref(bo_ptr);

	return r;
}

/* 释放内核BO */
void pddgpu_bo_free_kernel(struct pddgpu_bo **bo, u64 *gpu_addr, void **cpu_addr)
{
	if (!*bo)
		return;

	if (cpu_addr && *cpu_addr) {
		pddgpu_bo_kunmap(*bo);
		*cpu_addr = NULL;
	}

	if (gpu_addr)
		*gpu_addr = 0;

	pddgpu_bo_unpin(*bo);
	pddgpu_bo_unreserve(*bo);
	pddgpu_bo_unref(bo);
}

/* VRAM管理器初始化 */
int pddgpu_vram_mgr_init(struct pddgpu_device *pdev)
{
	struct pddgpu_vram_mgr *mgr;
	int ret;

	PDDGPU_DEBUG("Initializing VRAM manager\n");

	mgr = kzalloc(sizeof(*mgr), GFP_KERNEL);
	if (!mgr) {
		PDDGPU_ERROR("Failed to allocate VRAM manager\n");
		return -ENOMEM;
	}

	/* 初始化TTM资源管理器 */
	ret = ttm_resource_manager_init(&mgr->manager, pdev->vram_size,
	                               TTM_PL_VRAM);
	if (ret) {
		PDDGPU_ERROR("Failed to initialize VRAM manager: %d\n", ret);
		kfree(mgr);
		return ret;
	}

	/* 设置管理器函数 */
	mgr->manager.func = &pddgpu_vram_mgr_func;

	spin_lock_init(&mgr->lock);
	mgr->size = pdev->vram_size;
	mgr->used = 0;

	pdev->mman.vram_mgr = mgr;
	pdev->mman.man[TTM_PL_VRAM] = &mgr->manager;

	PDDGPU_DEBUG("VRAM manager initialized: size=%llu\n", mgr->size);

	return 0;
}

/* VRAM管理器清理 */
void pddgpu_vram_mgr_fini(struct pddgpu_device *pdev)
{
	struct pddgpu_vram_mgr *mgr = pdev->mman.vram_mgr;

	if (!mgr)
		return;

	PDDGPU_DEBUG("Finalizing VRAM manager\n");

	ttm_resource_manager_cleanup(&mgr->manager);
	kfree(mgr);

	pdev->mman.vram_mgr = NULL;
	pdev->mman.man[TTM_PL_VRAM] = NULL;
}

/* GTT管理器初始化 */
int pddgpu_gtt_mgr_init(struct pddgpu_device *pdev)
{
	struct pddgpu_gtt_mgr *mgr;
	int ret;

	PDDGPU_DEBUG("Initializing GTT manager\n");

	mgr = kzalloc(sizeof(*mgr), GFP_KERNEL);
	if (!mgr) {
		PDDGPU_ERROR("Failed to allocate GTT manager\n");
		return -ENOMEM;
	}

	/* 初始化TTM资源管理器 */
	ret = ttm_resource_manager_init(&mgr->manager, pdev->gtt_size,
	                               TTM_PL_TT);
	if (ret) {
		PDDGPU_ERROR("Failed to initialize GTT manager: %d\n", ret);
		kfree(mgr);
		return ret;
	}

	/* 设置管理器函数 */
	mgr->manager.func = &pddgpu_gtt_mgr_func;

	spin_lock_init(&mgr->lock);
	mgr->size = pdev->gtt_size;
	mgr->used = 0;

	pdev->mman.gtt_mgr = mgr;
	pdev->mman.man[TTM_PL_TT] = &mgr->manager;

	PDDGPU_DEBUG("GTT manager initialized: size=%llu\n", mgr->size);

	return 0;
}

/* GTT管理器清理 */
void pddgpu_gtt_mgr_fini(struct pddgpu_device *pdev)
{
	struct pddgpu_gtt_mgr *mgr = pdev->mman.gtt_mgr;

	if (!mgr)
		return;

	PDDGPU_DEBUG("Finalizing GTT manager\n");

	ttm_resource_manager_cleanup(&mgr->manager);
	kfree(mgr);

	pdev->mman.gtt_mgr = NULL;
	pdev->mman.man[TTM_PL_TT] = NULL;
}

/* 默认放置策略 */
struct ttm_place pddgpu_bo_placements[TTM_PL_MAX] = {
	[TTM_PL_SYSTEM] = {
		.flags = TTM_PL_FLAG_SYSTEM,
		.mem_type = TTM_PL_SYSTEM,
	},
	[TTM_PL_TT] = {
		.flags = TTM_PL_FLAG_TT,
		.mem_type = TTM_PL_TT,
	},
	[TTM_PL_VRAM] = {
		.flags = TTM_PL_FLAG_VRAM,
		.mem_type = TTM_PL_VRAM,
	},
};

struct ttm_placement pddgpu_bo_placement = {
	.num_placement = TTM_PL_MAX,
	.placement = pddgpu_bo_placements,
	.num_busy_placement = TTM_PL_MAX,
	.busy_placement = pddgpu_bo_placements,
};

/* 验证BO大小 */
bool pddgpu_bo_validate_size(struct pddgpu_device *pdev, unsigned long size, u32 domain)
{
	struct ttm_resource_manager *man = NULL;

	/*
	 * If GTT is part of requested domains the check must succeed to
	 * allow fall back to GTT.
	 */
	if (domain & PDDGPU_GEM_DOMAIN_GTT)
		man = ttm_manager_type(&pdev->mman.bdev, TTM_PL_TT);
	else if (domain & PDDGPU_GEM_DOMAIN_VRAM)
		man = ttm_manager_type(&pdev->mman.bdev, TTM_PL_VRAM);
	else
		return true;

	if (!man) {
		if (domain & PDDGPU_GEM_DOMAIN_GTT)
			WARN_ON_ONCE("GTT domain requested but GTT mem manager uninitialized");
		return false;
	}

	/* TODO add more domains checks, such as PDDGPU_GEM_DOMAIN_CPU */
	if (size < man->size)
		return true;

	PDDGPU_DEBUG("BO size %lu > total memory in domain: %llu\n", size, man->size);
	return false;
}

/* 检查USWC支持 */
bool pddgpu_bo_support_uswc(u64 bo_flags)
{
#ifdef CONFIG_X86_32
	/* XXX: Write-combined CPU mappings of GTT seem broken on 32-bit */
	return false;
#elif defined(CONFIG_X86) && !defined(CONFIG_X86_PAT)
	/* Don't try to enable write-combining when it can't work */
	if (bo_flags & PDDGPU_GEM_CREATE_CPU_GTT_USWC)
		PDDGPU_INFO_ONCE("Please enable CONFIG_MTRR and CONFIG_X86_PAT for "
			      "better performance thanks to write-combining\n");
	return false;
#else
	/* For architectures that don't support WC memory */
	if (!drm_arch_can_wc_memory())
		return false;

	return true;
#endif
}

/* 设置放置策略 */
void pddgpu_bo_placement_from_domain(struct pddgpu_bo *bo, u32 domain)
{
	struct pddgpu_device *pdev = pddgpu_ttm_pdev(bo->tbo.bdev);
	struct ttm_placement *placement = &bo->placement;
	struct ttm_place *places = bo->placements;
	u64 flags = bo->flags;
	u32 c = 0;

	if (domain & PDDGPU_GEM_DOMAIN_VRAM) {
		unsigned int visible_pfn = pdev->gmc.visible_vram_size >> PAGE_SHIFT;

		places[c].fpfn = 0;
		places[c].lpfn = 0;
		places[c].mem_type = TTM_PL_VRAM;
		places[c].flags = 0;

		if (flags & PDDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED)
			places[c].lpfn = min_not_zero(places[c].lpfn, visible_pfn);
		else
			places[c].flags |= TTM_PL_FLAG_TOPDOWN;

		if (bo->tbo.type == ttm_bo_type_kernel &&
		    flags & PDDGPU_GEM_CREATE_VRAM_CONTIGUOUS)
			places[c].flags |= TTM_PL_FLAG_CONTIGUOUS;

		c++;
	}

	if (domain & PDDGPU_GEM_DOMAIN_GTT) {
		places[c].fpfn = 0;
		places[c].lpfn = 0;
		places[c].mem_type = TTM_PL_TT;
		places[c].flags = 0;
		/*
		 * When GTT is just an alternative to VRAM make sure that we
		 * only use it as fallback and still try to fill up VRAM first.
		 */
		if (bo->tbo.resource && !(pdev->flags & PDD_IS_APU) &&
		    domain & bo->preferred_domains & PDDGPU_GEM_DOMAIN_VRAM)
			places[c].flags |= TTM_PL_FLAG_FALLBACK;
		c++;
	}

	if (domain & PDDGPU_GEM_DOMAIN_CPU) {
		places[c].fpfn = 0;
		places[c].lpfn = 0;
		places[c].mem_type = TTM_PL_SYSTEM;
		places[c].flags = 0;
		c++;
	}

	if (!c) {
		places[c].fpfn = 0;
		places[c].lpfn = 0;
		places[c].mem_type = TTM_PL_SYSTEM;
		places[c].flags = 0;
		c++;
	}

	placement->num_placement = c;
	placement->placement = places;
	placement->num_busy_placement = c;
	placement->busy_placement = places;
}

/* 获取GPU偏移 */
u64 pddgpu_bo_gpu_offset(struct pddgpu_bo *bo)
{
	struct pddgpu_device *pdev = pddgpu_ttm_pdev(bo->tbo.bdev);

	if (bo->tbo.resource->mem_type == TTM_PL_VRAM)
		return pdev->gmc.vram_start + bo->tbo.resource->start << PAGE_SHIFT;
	else if (bo->tbo.resource->mem_type == TTM_PL_TT)
		return pdev->gmc.gtt_start + bo->tbo.resource->start << PAGE_SHIFT;
	else
		return 0;
}

/* 固定BO */
int pddgpu_bo_pin(struct pddgpu_bo *bo, u32 domain)
{
	struct pddgpu_device *pdev = pddgpu_ttm_pdev(bo->tbo.bdev);
	struct ttm_operation_ctx ctx = { false, false };
	int r;

	if (bo->tbo.pin_count)
		return 0;

	r = ttm_bo_reserve(&bo->tbo, false, false, NULL);
	if (unlikely(r != 0))
		return r;

	r = ttm_bo_validate(&bo->tbo, &bo->placement, &ctx);
	if (likely(r == 0))
		ttm_bo_pin(&bo->tbo);

	ttm_bo_unreserve(&bo->tbo);
	return r;
}

/* 取消固定BO */
void pddgpu_bo_unpin(struct pddgpu_bo *bo)
{
	if (!bo->tbo.pin_count)
		return;

	ttm_bo_unpin(&bo->tbo);
}

/* 内核映射BO */
int pddgpu_bo_kmap(struct pddgpu_bo *bo, void **ptr)
{
	void *kptr;
	long r;

	if (bo->flags & PDDGPU_GEM_CREATE_NO_CPU_ACCESS)
		return -EPERM;

	r = dma_resv_wait_timeout(bo->tbo.base.resv, DMA_RESV_USAGE_KERNEL,
				  false, MAX_SCHEDULE_TIMEOUT);
	if (r < 0)
		return r;

	kptr = pddgpu_bo_kptr(bo);
	if (kptr) {
		if (ptr)
			*ptr = kptr;
		return 0;
	}

	r = ttm_bo_kmap(&bo->tbo, 0, PFN_UP(bo->tbo.base.size), &bo->kmap);
	if (r)
		return r;

	if (ptr)
		*ptr = pddgpu_bo_kptr(bo);

	return 0;
}

/* 获取内核指针 */
void *pddgpu_bo_kptr(struct pddgpu_bo *bo)
{
	bool is_iomem;

	return ttm_kmap_obj_virtual(&bo->kmap, &is_iomem);
}

/* 取消内核映射 */
void pddgpu_bo_kunmap(struct pddgpu_bo *bo)
{
	ttm_bo_kunmap(&bo->tbo, &bo->kmap);
}
