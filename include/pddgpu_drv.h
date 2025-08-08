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
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/ttm/ttm_bo_driver.h>
#include <drm/ttm/ttm_resource.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/drm_mm.h>

#include "pddgpu_regs.h"

/* 前向声明 */
struct pddgpu_vram_mgr;
struct pddgpu_gtt_mgr;
struct pddgpu_gmc;
struct pddgpu_bo_param;

/* PDDGPU设备结构 */
struct pddgpu_device {
	struct drm_device *ddev;
	struct pci_dev *pdev;
	
	/* 内存管理 */
	struct {
		struct ttm_device bdev;
		struct ttm_resource_manager *man[TTM_NUM_MEM_TYPES];
		struct pddgpu_vram_mgr *vram_mgr;
		struct pddgpu_gtt_mgr *gtt_mgr;
		bool buffer_funcs_enabled;
	} mman;
	
	/* 图形内存控制器 */
	struct pddgpu_gmc gmc;
	
	/* 设备信息 */
	u32 chip_id;
	u32 chip_rev;
	u64 vram_size;
	u64 gtt_size;
	
	/* 调试 */
#ifdef CONFIG_DRM_PDDGPU_DEBUG
	struct dentry *debugfs_root;
#endif
};

/* PDDGPU缓冲区对象 */
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

/* PDDGPU GEM域 */
#define PDDGPU_GEM_DOMAIN_CPU    0x1
#define PDDGPU_GEM_DOMAIN_GTT    0x2
#define PDDGPU_GEM_DOMAIN_VRAM   0x4
#define PDDGPU_GEM_DOMAIN_GDS    0x8
#define PDDGPU_GEM_DOMAIN_GWS    0x10
#define PDDGPU_GEM_DOMAIN_OA     0x20

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

/* 函数声明 */
int pddgpu_bo_create(struct pddgpu_device *pdev, struct pddgpu_bo_param *bp,
                     struct pddgpu_bo **bo_ptr);
void pddgpu_bo_unref(struct pddgpu_bo **bo);
int pddgpu_bo_create_kernel(struct pddgpu_device *pdev, unsigned long size,
                            int domain, struct pddgpu_bo **bo_ptr,
                            u64 *gpu_addr, void **cpu_addr);
void pddgpu_bo_free_kernel(struct pddgpu_bo **bo, u64 *gpu_addr,
                           void **cpu_addr);

/* 转换宏 */
#define to_pddgpu_device(x) container_of(x, struct pddgpu_device, ddev)
#define to_pddgpu_bo(x) container_of(x, struct pddgpu_bo, tbo)
#define pddgpu_to_drm(x) ((x)->ddev)

/* 调试宏 */
#ifdef CONFIG_DRM_PDDGPU_DEBUG
#define PDDGPU_DEBUG(fmt, ...) pr_debug("PDDGPU: " fmt, ##__VA_ARGS__)
#else
#define PDDGPU_DEBUG(fmt, ...) do { } while (0)
#endif

#define PDDGPU_INFO(fmt, ...) pr_info("PDDGPU: " fmt, ##__VA_ARGS__)
#define PDDGPU_WARN(fmt, ...) pr_warn("PDDGPU: " fmt, ##__VA_ARGS__)
#define PDDGPU_ERROR(fmt, ...) pr_err("PDDGPU: " fmt, ##__VA_ARGS__)

#endif /* __PDDGPU_DRV_H__ */
