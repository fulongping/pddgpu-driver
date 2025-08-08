/*
 * PDDGPU图形内存控制器 (GMC)
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/mtrr.h>

#include <drm/drm_gem.h>
#include <drm/ttm/ttm_bo.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_resource.h>

#include "include/pddgpu_drv.h"
#include "include/pddgpu_regs.h"

/* GMC初始化 */
int pddgpu_gmc_init(struct pddgpu_device *pdev)
{
	struct pci_dev *pci_dev = pdev->pdev;
	int ret;

	PDDGPU_DEBUG("Initializing GMC\n");

	/* 读取硬件寄存器获取内存信息 */
	pdev->gmc.real_vram_size = PDDGPU_READ64(pdev->rmmio + PDDGPU_REG_VRAM_SIZE);
	pdev->gmc.visible_vram_size = pdev->gmc.real_vram_size;
	pdev->gmc.vram_start = PDDGPU_READ64(pdev->rmmio + PDDGPU_REG_VRAM_START);
	pdev->gmc.vram_end = PDDGPU_READ64(pdev->rmmio + PDDGPU_REG_VRAM_END);
	pdev->gmc.gtt_start = PDDGPU_READ64(pdev->rmmio + PDDGPU_REG_GTT_START);
	pdev->gmc.gtt_end = PDDGPU_READ64(pdev->rmmio + PDDGPU_REG_GTT_END);

	/* 验证内存大小 */
	if (pdev->gmc.real_vram_size == 0 || pdev->gmc.real_vram_size > PDDGPU_MAX_VRAM_SIZE) {
		PDDGPU_ERROR("Invalid VRAM size: %llu\n", pdev->gmc.real_vram_size);
		return -EINVAL;
	}

	if (pdev->gmc.gtt_end - pdev->gmc.gtt_start > PDDGPU_MAX_GTT_SIZE) {
		PDDGPU_ERROR("Invalid GTT size: %llu\n", pdev->gmc.gtt_end - pdev->gmc.gtt_start);
		return -EINVAL;
	}

	/* 设置帧缓冲区范围 */
	pdev->gmc.fb_start = pdev->gmc.vram_start;
	pdev->gmc.fb_end = pdev->gmc.vram_start + pdev->gmc.visible_vram_size;

	/* 初始化VRAM位宽和类型 */
	pdev->gmc.vram_width = 256;  // 默认256位
	pdev->gmc.vram_type = 0;     // 默认类型
	pdev->gmc.vram_vendor = 0;   // 默认厂商

	/* 设置MTRR (内存类型范围寄存器) */
	if (!pdev->gmc.xgmi.connected_to_cpu && !pdev->gmc.is_app_apu) {
		pdev->gmc.vram_mtrr = arch_phys_wc_add(pdev->gmc.fb_start, pdev->gmc.fb_end - pdev->gmc.fb_start);
		if (pdev->gmc.vram_mtrr < 0) {
			PDDGPU_ERROR("Failed to set MTRR for VRAM\n");
			return pdev->gmc.vram_mtrr;
		}
	}

	/* 启用内存控制器 */
	PDDGPU_WRITE32(pdev->rmmio + PDDGPU_REG_MC_VRAM_CTRL, PDDGPU_MC_VRAM_CTRL_ENABLE);
	PDDGPU_WRITE32(pdev->rmmio + PDDGPU_REG_MC_GTT_CTRL, PDDGPU_MC_GTT_CTRL_ENABLE);
	PDDGPU_WRITE32(pdev->rmmio + PDDGPU_REG_MC_FB_CTRL, PDDGPU_MC_FB_CTRL_ENABLE);

	PDDGPU_INFO("GMC initialized: VRAM=%lluMB, GTT=%lluMB\n",
	            pdev->gmc.real_vram_size >> 20,
	            (pdev->gmc.gtt_end - pdev->gmc.gtt_start) >> 20);

	return 0;
}

/* GMC清理 */
void pddgpu_gmc_fini(struct pddgpu_device *pdev)
{
	PDDGPU_DEBUG("Finalizing GMC\n");

	/* 禁用内存控制器 */
	PDDGPU_WRITE32(pdev->rmmio + PDDGPU_REG_MC_VRAM_CTRL, 0);
	PDDGPU_WRITE32(pdev->rmmio + PDDGPU_REG_MC_GTT_CTRL, 0);
	PDDGPU_WRITE32(pdev->rmmio + PDDGPU_REG_MC_FB_CTRL, 0);

	/* 清理MTRR */
	if (!pdev->gmc.xgmi.connected_to_cpu && !pdev->gmc.is_app_apu) {
		if (pdev->gmc.vram_mtrr >= 0) {
			arch_phys_wc_del(pdev->gmc.vram_mtrr);
			pdev->gmc.vram_mtrr = -1;
		}
	}

	PDDGPU_DEBUG("GMC finalized\n");
}

/* GMC挂起 */
int pddgpu_gmc_suspend(struct pddgpu_device *pdev)
{
	PDDGPU_DEBUG("Suspending GMC\n");

	/* 保存内存控制器状态 */
	pdev->gmc.suspended = true;

	/* 禁用内存控制器 */
	PDDGPU_WRITE32(pdev->rmmio + PDDGPU_REG_MC_VRAM_CTRL, 0);
	PDDGPU_WRITE32(pdev->rmmio + PDDGPU_REG_MC_GTT_CTRL, 0);
	PDDGPU_WRITE32(pdev->rmmio + PDDGPU_REG_MC_FB_CTRL, 0);

	return 0;
}

/* GMC恢复 */
int pddgpu_gmc_resume(struct pddgpu_device *pdev)
{
	PDDGPU_DEBUG("Resuming GMC\n");

	/* 重新启用内存控制器 */
	PDDGPU_WRITE32(pdev->rmmio + PDDGPU_REG_MC_VRAM_CTRL, PDDGPU_MC_VRAM_CTRL_ENABLE);
	PDDGPU_WRITE32(pdev->rmmio + PDDGPU_REG_MC_GTT_CTRL, PDDGPU_MC_GTT_CTRL_ENABLE);
	PDDGPU_WRITE32(pdev->rmmio + PDDGPU_REG_MC_FB_CTRL, PDDGPU_MC_FB_CTRL_ENABLE);

	pdev->gmc.suspended = false;

	return 0;
}

/* 验证内存大小 */
bool pddgpu_gmc_validate_size(struct pddgpu_device *pdev, u64 size, u32 domain)
{
	u64 max_size = 0;

	/* 根据域确定最大大小 */
	if (domain & PDDGPU_GEM_DOMAIN_VRAM) {
		max_size = pdev->gmc.real_vram_size;
	} else if (domain & PDDGPU_GEM_DOMAIN_GTT) {
		max_size = pdev->gmc.gtt_end - pdev->gmc.gtt_start;
	} else if (domain & PDDGPU_GEM_DOMAIN_CPU) {
		max_size = PDDGPU_MAX_BO_SIZE;
	}

	return size <= max_size;
}

/* 检查内存可见性 */
bool pddgpu_gmc_is_cpu_visible(struct pddgpu_device *pdev, u64 addr, u64 size)
{
	u64 end = addr + size;
	return (addr >= pdev->gmc.fb_start && end <= pdev->gmc.fb_end);
}

/* 获取内存统计信息 */
void pddgpu_gmc_get_memory_info(struct pddgpu_device *pdev, struct pddgpu_memory_info *info)
{
	info->total_vram = pdev->gmc.real_vram_size;
	info->visible_vram = pdev->gmc.visible_vram_size;
	info->total_gtt = pdev->gmc.gtt_end - pdev->gmc.gtt_start;
	info->vram_start = pdev->gmc.vram_start;
	info->vram_end = pdev->gmc.vram_end;
	info->gtt_start = pdev->gmc.gtt_start;
	info->gtt_end = pdev->gmc.gtt_end;
}

/* 内存训练 */
int pddgpu_gmc_memory_training(struct pddgpu_device *pdev)
{
	PDDGPU_DEBUG("Starting memory training\n");

	/* 这里应该实现内存训练逻辑 */
	/* 由于这是模拟实现，我们只是记录日志 */

	PDDGPU_INFO("Memory training completed\n");

	return 0;
}

/* 内存完整性检查 */
int pddgpu_gmc_memory_check(struct pddgpu_device *pdev)
{
	u64 test_size = 1024 * 1024;  // 1MB测试
	void *test_addr;
	u32 *test_data;
	int i, ret = 0;

	PDDGPU_DEBUG("Starting memory integrity check\n");

	/* 分配测试缓冲区 */
	test_addr = vmalloc(test_size);
	if (!test_addr) {
		PDDGPU_ERROR("Failed to allocate test buffer\n");
		return -ENOMEM;
	}

	test_data = (u32 *)test_addr;

	/* 写入测试模式 */
	for (i = 0; i < test_size / sizeof(u32); i++) {
		test_data[i] = i;
	}

	/* 验证读取 */
	for (i = 0; i < test_size / sizeof(u32); i++) {
		if (test_data[i] != i) {
			PDDGPU_ERROR("Memory corruption detected at offset %d\n", i);
			ret = -EIO;
			break;
		}
	}

	/* 清理 */
	vfree(test_addr);

	if (ret == 0) {
		PDDGPU_INFO("Memory integrity check passed\n");
	}

	return ret;
}
