/*
 * PDDGPU驱动头文件
 *
 * Copyright (C) 2024 PDDGPU Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef __PDDGPU_DRV_H__
#define __PDDGPU_DRV_H__

#include <linux/types.h>
#include <linux/pci.h>
#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/atomic.h>

#include <drm/drm_gem.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_resource.h>

#include "pddgpu_regs.h"

/* 前向声明 */
struct pddgpu_device;
struct pddgpu_bo;
struct pddgpu_vram_mgr;
struct pddgpu_gtt_mgr;
struct pddgpu_gmc;
struct pddgpu_bo_param;

/* PDDGPU GMC (图形内存控制器) */
struct pddgpu_gmc {
	u64 real_vram_size;        // 实际VRAM大小
	u64 visible_vram_size;     // 可见VRAM大小
	u64 vram_start;           // VRAM起始地址
	u64 vram_end;             // VRAM结束地址
	u64 gtt_start;            // GTT起始地址
	u64 gtt_end;              // GTT结束地址
	u64 fb_start;             // 帧缓冲区起始地址
	u64 fb_end;               // 帧缓冲区结束地址
	unsigned vram_width;       // VRAM位宽
	uint32_t vram_type;       // VRAM类型
	uint8_t vram_vendor;      // VRAM厂商
	int vram_mtrr;            // VRAM MTRR
	bool suspended;           // 挂起状态
	
	/* XGMI相关 */
	struct {
		bool connected_to_cpu;
	} xgmi;
	
	/* APU相关 */
	bool is_app_apu;
};

/* PDDGPU内存信息 */
struct pddgpu_memory_info {
	u64 total_vram;           // 总VRAM大小
	u64 visible_vram;         // 可见VRAM大小
	u64 total_gtt;            // 总GTT大小
	u64 vram_start;           // VRAM起始地址
	u64 vram_end;             // VRAM结束地址
	u64 gtt_start;            // GTT起始地址
	u64 gtt_end;              // GTT结束地址
};

/* PDDGPU GTT 管理器 */
struct pddgpu_gtt_mgr {
	struct ttm_resource_manager manager;
	struct drm_mm mm;
	spinlock_t lock;
};

/* PDDGPU设备 */
struct pddgpu_device {
	struct drm_device *ddev;
	struct pci_dev *pdev;
	
	/* MMIO映射 */
	void __iomem *rmmio;
	
	/* 内存管理 */
	struct {
		struct ttm_device bdev;
		struct ttm_resource_manager *man[TTM_NUM_MEM_TYPES];
		struct pddgpu_vram_mgr *vram_mgr;
		struct pddgpu_gtt_mgr gtt_mgr;
		bool buffer_funcs_enabled;
	} mman;
	
	/* 图形内存控制器 */
	struct pddgpu_gmc gmc;
	
	/* 设备信息 */
	u32 chip_id;
	u32 chip_rev;
	u64 vram_size;
	u64 gtt_size;
	
	/* 统计信息 */
	atomic_t num_evictions;
	atomic64_t num_bytes_moved;
};

/* PDDGPU BO */
struct pddgpu_bo {
	struct ttm_buffer_object tbo;
	struct ttm_placement placement;
	struct ttm_place placements[TTM_PL_MAX];
	unsigned pin_count;
	
	/* GEM相关 */
	struct drm_gem_object base;
	u64 size;
	u32 domain;
	u32 flags;
	
	/* 映射相关 */
	void *kptr;
	struct sg_table *sg;
	
	/* 父对象 */
	struct pddgpu_bo *parent;
};

/* PDDGPU VRAM管理器 */
struct pddgpu_vram_mgr {
	struct ttm_resource_manager manager;
	spinlock_t lock;
	u64 size;
	u64 used;
};

/* PDDGPU BO参数 */
struct pddgpu_bo_param {
	unsigned long size;
	int alignment;
	u32 domain;
	u32 flags;
	enum ttm_bo_type type;
	struct dma_resv *resv;
	size_t bo_ptr_size;
	void (*destroy)(struct ttm_buffer_object *);
};

/* PDDGPU GEM域 */
#define PDDGPU_GEM_DOMAIN_CPU    0x1
#define PDDGPU_GEM_DOMAIN_GTT    0x2
#define PDDGPU_GEM_DOMAIN_VRAM   0x4

/* PDDGPU GEM标志 */
#define PDDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED (1 << 0)
#define PDDGPU_GEM_CREATE_NO_CPU_ACCESS      (1 << 1)
#define PDDGPU_GEM_CREATE_CP_MQD_GFX         (1 << 2)
#define PDDGPU_GEM_CREATE_FLAG_NO_DEFER      (1 << 3)
#define PDDGPU_GEM_CREATE_VRAM_CLEARED       (1 << 4)
#define PDDGPU_GEM_CREATE_VM_ALWAYS_VALID    (1 << 5)
#define PDDGPU_GEM_CREATE_EXPLICIT_SYNC      (1 << 6)

/* PDDGPU GEM创建参数 */
struct drm_pddgpu_gem_create {
	__u64 size;
	__u32 alignment;
	__u32 domains;
	__u32 flags;
	__u32 handle;
	__u64 pad;
};

/* PDDGPU GEM映射参数 */
struct drm_pddgpu_gem_map {
	__u32 handle;
	__u32 pad;
	__u64 offset;
	__u64 size;
	__u64 flags;
};

/* PDDGPU GEM信息参数 */
struct drm_pddgpu_gem_info {
	__u32 handle;
	__u32 pad;
	__u64 size;
	__u64 offset;
	__u32 domain;
	__u32 flags;
};

/* IOCTL定义 */
#define DRM_PDDGPU_GEM_CREATE    0x00
#define DRM_PDDGPU_GEM_MAP       0x01
#define DRM_PDDGPU_GEM_INFO      0x02
#define DRM_PDDGPU_GEM_DESTROY   0x03

#define DRM_IOCTL_PDDGPU_GEM_CREATE  DRM_IOWR(DRM_COMMAND_BASE + DRM_PDDGPU_GEM_CREATE, struct drm_pddgpu_gem_create)
#define DRM_IOCTL_PDDGPU_GEM_MAP     DRM_IOWR(DRM_COMMAND_BASE + DRM_PDDGPU_GEM_MAP, struct drm_pddgpu_gem_map)
#define DRM_IOCTL_PDDGPU_GEM_INFO    DRM_IOWR(DRM_COMMAND_BASE + DRM_PDDGPU_GEM_INFO, struct drm_pddgpu_gem_info)
#define DRM_IOCTL_PDDGPU_GEM_DESTROY DRM_IOW(DRM_COMMAND_BASE + DRM_PDDGPU_GEM_DESTROY, struct drm_pddgpu_gem_create)

/* 转换宏 */
static inline struct pddgpu_device *pdev_to_drm(struct pddgpu_device *pdev)
{
	return pdev->ddev;
}

static inline struct pddgpu_device *drm_to_pdev(struct drm_device *ddev)
{
	return (struct pddgpu_device *)ddev->dev_private;
}

/* 内存访问宏 */
#define PDDGPU_READ32(addr) readl(addr)
#define PDDGPU_WRITE32(addr, val) writel(val, addr)
#define PDDGPU_READ64(addr) readq(addr)
#define PDDGPU_WRITE64(addr, val) writeq(val, addr)

/* 调试宏 */
#define PDDGPU_DEBUG(fmt, ...) pr_debug("PDDGPU: " fmt, ##__VA_ARGS__)
#define PDDGPU_INFO(fmt, ...) pr_info("PDDGPU: " fmt, ##__VA_ARGS__)
#define PDDGPU_ERROR(fmt, ...) pr_err("PDDGPU: " fmt, ##__VA_ARGS__)

/* 常量定义 */
#define PDDGPU_VM_RESERVED_VRAM (256 * 1024 * 1024)  // 256MB 保留VRAM
#define PDDGPU_BO_INVALID_OFFSET 0xffffffffffffffff

/* 函数声明 */
int pddgpu_bo_create(struct pddgpu_device *pdev, struct pddgpu_bo_param *bp,
                     struct pddgpu_bo **bo_ptr);
void pddgpu_bo_unref(struct pddgpu_bo **bo);
int pddgpu_bo_create_kernel(struct pddgpu_device *pdev, unsigned long size,
                            int domain, struct pddgpu_bo **bo_ptr,
                            u64 *gpu_addr, void **cpu_addr);
void pddgpu_bo_free_kernel(struct pddgpu_bo **bo, u64 *gpu_addr,
                           void **cpu_addr);

/* GMC函数声明 */
int pddgpu_gmc_init(struct pddgpu_device *pdev);
void pddgpu_gmc_fini(struct pddgpu_device *pdev);
int pddgpu_gmc_suspend(struct pddgpu_device *pdev);
int pddgpu_gmc_resume(struct pddgpu_device *pdev);
bool pddgpu_gmc_validate_size(struct pddgpu_device *pdev, u64 size, u32 domain);
bool pddgpu_gmc_is_cpu_visible(struct pddgpu_device *pdev, u64 addr, u64 size);
void pddgpu_gmc_get_memory_info(struct pddgpu_device *pdev, struct pddgpu_memory_info *info);

/* TTM函数声明 */
int pddgpu_ttm_init(struct pddgpu_device *pdev);
void pddgpu_ttm_fini(struct pddgpu_device *pdev);
int pddgpu_ttm_pools_init(struct pddgpu_device *pdev);
void pddgpu_ttm_pools_fini(struct pddgpu_device *pdev);
void pddgpu_bo_placement_from_domain(struct pddgpu_bo *abo, u32 domain);

/* VRAM管理器函数 */
int pddgpu_vram_mgr_init(struct pddgpu_device *pdev);
void pddgpu_vram_mgr_fini(struct pddgpu_device *pdev);

/* GTT管理器函数 */
int pddgpu_gtt_mgr_init(struct pddgpu_device *pdev, uint64_t gtt_size);
void pddgpu_gtt_mgr_fini(struct pddgpu_device *pdev);
void pddgpu_gtt_mgr_recover(struct pddgpu_gtt_mgr *mgr);

#endif /* __PDDGPU_DRV_H__ */
