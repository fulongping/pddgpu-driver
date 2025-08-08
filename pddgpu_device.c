/*
 * PDDGPU设备管理
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>

#include "include/pddgpu_drv.h"
#include "pddgpu_gmc.h"
#include "pddgpu_ttm.h"
#include "pddgpu_vram_mgr.h"
#include "pddgpu_gtt_mgr.h"

/* 设备初始化 */
int pddgpu_device_init(struct pddgpu_device *pdev)
{
	struct pci_dev *pci_dev = pdev->pdev;
	int ret;
	
	PDDGPU_DEBUG("Initializing PDDGPU device\n");
	
	/* 设置设备状态为初始化中 */
	atomic_set(&pdev->device_state, PDDGPU_DEVICE_STATE_INITIALIZING);
	
	/* 映射MMIO区域 */
	pdev->rmmio = pci_iomap(pci_dev, 0, 0);
	if (!pdev->rmmio) {
		PDDGPU_ERROR("Failed to map MMIO region\n");
		ret = -ENOMEM;
		goto err_exit;
	}
	
	/* 读取设备信息 */
	pdev->chip_id = PDDGPU_READ32(pdev, PDDGPU_REG_CHIP_ID);
	pdev->chip_rev = PDDGPU_READ32(pdev, PDDGPU_REG_CHIP_REV);
	pdev->vram_size = PDDGPU_READ64(pdev, PDDGPU_REG_VRAM_SIZE);
	pdev->gtt_size = PDDGPU_READ64(pdev, PDDGPU_REG_GTT_SIZE);
	
	PDDGPU_INFO("PDDGPU device: chip_id=0x%08x, chip_rev=0x%08x\n",
	            pdev->chip_id, pdev->chip_rev);
	PDDGPU_INFO("Memory: VRAM=%llu MB, GTT=%llu MB\n",
	            pdev->vram_size >> 20, pdev->gtt_size >> 20);
	
	/* 初始化内存统计模块 */
	ret = pddgpu_memory_stats_init(pdev);
	if (ret) {
		PDDGPU_ERROR("Failed to initialize memory statistics module\n");
		goto err_unmap_mmio;
	}
	
	/* 初始化GMC */
	ret = pddgpu_gmc_init(pdev);
	if (ret) {
		PDDGPU_ERROR("Failed to initialize GMC\n");
		goto err_memory_stats_fini;
	}
	
	/* 初始化TTM */
	ret = pddgpu_ttm_init(pdev);
	if (ret) {
		PDDGPU_ERROR("Failed to initialize TTM\n");
		goto err_gmc_fini;
	}
	
	/* 初始化VRAM管理器 */
	ret = pddgpu_vram_mgr_init(pdev);
	if (ret) {
		PDDGPU_ERROR("Failed to initialize VRAM manager\n");
		goto err_ttm_fini;
	}
	
	/* 初始化GTT管理器 */
	ret = pddgpu_gtt_mgr_init(pdev, pdev->gtt_size);
	if (ret) {
		PDDGPU_ERROR("Failed to initialize GTT manager\n");
		goto err_vram_mgr_fini;
	}
	
	/* 设置设备状态为就绪 */
	atomic_set(&pdev->device_state, PDDGPU_DEVICE_STATE_READY);
	
	PDDGPU_DEBUG("PDDGPU device initialized successfully\n");
	return 0;

err_vram_mgr_fini:
	pddgpu_vram_mgr_fini(pdev);
err_ttm_fini:
	pddgpu_ttm_fini(pdev);
err_gmc_fini:
	pddgpu_gmc_fini(pdev);
err_memory_stats_fini:
	pddgpu_memory_stats_fini(pdev);
err_unmap_mmio:
	if (pdev->rmmio)
		pci_iounmap(pci_dev, pdev->rmmio);
err_exit:
	/* 设置设备状态为关闭 */
	atomic_set(&pdev->device_state, PDDGPU_DEVICE_STATE_SHUTDOWN);
	return ret;
}

/* 设备清理 */
void pddgpu_device_fini(struct pddgpu_device *pdev)
{
	struct pci_dev *pci_dev = pdev->pdev;
	
	PDDGPU_DEBUG("Finalizing PDDGPU device\n");
	
	/* 设置设备状态为关闭中 */
	atomic_set(&pdev->device_state, PDDGPU_DEVICE_STATE_SHUTDOWN);
	
	/* 清理GTT管理器 */
	pddgpu_gtt_mgr_fini(pdev);
	
	/* 清理VRAM管理器 */
	pddgpu_vram_mgr_fini(pdev);
	
	/* 清理TTM */
	pddgpu_ttm_fini(pdev);
	
	/* 清理GMC */
	pddgpu_gmc_fini(pdev);
	
	/* 清理内存统计模块 */
	pddgpu_memory_stats_fini(pdev);
	
	/* 取消映射MMIO区域 */
	if (pdev->rmmio)
		pci_iounmap(pci_dev, pdev->rmmio);
	
	PDDGPU_DEBUG("PDDGPU device finalized\n");
}

/* 电源管理挂起 */
int pddgpu_pm_suspend(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct drm_device *ddev = pci_get_drvdata(pci_dev);
	struct pddgpu_device *pdev = to_pddgpu_device(ddev);
	
	PDDGPU_DEBUG("Suspending PDDGPU device\n");
	
	/* 保存设备状态 */
	pddgpu_gmc_suspend(pdev);
	
	return 0;
}

/* 电源管理恢复 */
int pddgpu_pm_resume(struct device *dev)
{
	struct pci_dev *pci_dev = to_pci_dev(dev);
	struct drm_device *ddev = pci_get_drvdata(pci_dev);
	struct pddgpu_device *pdev = to_pddgpu_device(ddev);
	
	PDDGPU_DEBUG("Resuming PDDGPU device\n");
	
	/* 恢复设备状态 */
	pddgpu_gmc_resume(pdev);
	
	return 0;
}
