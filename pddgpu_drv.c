/*
 * PDDGPU驱动主文件
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_shmem_helper.h>

#include "include/pddgpu_drv.h"
#include "include/pddgpu_regs.h"

/* PCI设备表 */
static const struct pci_device_id pddgpu_pci_table[] = {
	{ PCI_VENDOR_ID_AMD, PDDGPU_DEVICE_ID_PDD1000, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ PCI_VENDOR_ID_AMD, PDDGPU_DEVICE_ID_PDD2000, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ PCI_VENDOR_ID_AMD, PDDGPU_DEVICE_ID_PDD3000, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{ 0, }
};

MODULE_DEVICE_TABLE(pci, pddgpu_pci_table);

/* DRM驱动结构 */
static struct drm_driver pddgpu_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.name = "pddgpu",
	.desc = "PDDGPU Graphics Driver",
	.date = "20240101",
	.major = 1,
	.minor = 0,
	.patchlevel = 0,
	
	/* GEM操作 */
	.gem_create_object = pddgpu_gem_create_object,
	.gem_free_object_unlocked = pddgpu_gem_free_object,
	.gem_open_object = pddgpu_gem_open_object,
	.gem_close_object = pddgpu_gem_close_object,
	
	/* 文件操作 */
	.fops = &pddgpu_driver_fops,
	
	/* IOCTL */
	.ioctls = pddgpu_ioctls,
	.num_ioctls = ARRAY_SIZE(pddgpu_ioctls),
	
	/* 调试 */
#ifdef CONFIG_DRM_PDDGPU_DEBUG
	.debugfs_init = pddgpu_debugfs_init,
#endif
};

/* 文件操作 */
static const struct file_operations pddgpu_driver_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl = drm_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = pddgpu_mmap,
};

/* IOCTL表 */
static const struct drm_ioctl_desc pddgpu_ioctls[] = {
	DRM_IOCTL_DEF_DRV(PDDGPU_GEM_CREATE, pddgpu_gem_create_ioctl, DRM_AUTH | DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(PDDGPU_GEM_MAP, pddgpu_gem_map_ioctl, DRM_AUTH | DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(PDDGPU_GEM_INFO, pddgpu_gem_info_ioctl, DRM_AUTH | DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(PDDGPU_GEM_DESTROY, pddgpu_gem_destroy_ioctl, DRM_AUTH | DRM_UNLOCKED),
};

/* PCI探测函数 */
static int pddgpu_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct pddgpu_device *pddgpu;
	struct drm_device *ddev;
	int ret;

	PDDGPU_INFO("PDDGPU PCI probe: vendor=0x%04x, device=0x%04x\n",
		    pdev->vendor, pdev->device);

	/* 启用PCI设备 */
	ret = pci_enable_device(pdev);
	if (ret) {
		PDDGPU_ERROR("Failed to enable PCI device\n");
		return ret;
	}

	/* 启用PCI DMA */
	ret = pci_set_dma_mask(pdev, DMA_BIT_MASK(64));
	if (ret) {
		ret = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
		if (ret) {
			PDDGPU_ERROR("Failed to set DMA mask\n");
			goto err_disable_device;
		}
	}

	/* 请求PCI区域 */
	ret = pci_request_regions(pdev, "pddgpu");
	if (ret) {
		PDDGPU_ERROR("Failed to request PCI regions\n");
		goto err_disable_device;
	}

	/* 分配DRM设备 */
	ddev = drm_dev_alloc(&pddgpu_driver, &pdev->dev);
	if (IS_ERR(ddev)) {
		ret = PTR_ERR(ddev);
		PDDGPU_ERROR("Failed to allocate DRM device\n");
		goto err_release_regions;
	}

	/* 分配PDDGPU设备 */
	pddgpu = kzalloc(sizeof(*pddgpu), GFP_KERNEL);
	if (!pddgpu) {
		ret = -ENOMEM;
		PDDGPU_ERROR("Failed to allocate PDDGPU device\n");
		goto err_free_drm;
	}

	/* 初始化PDDGPU设备 */
	pddgpu->ddev = ddev;
	pddgpu->pdev = pdev;
	ddev->dev_private = pddgpu;

	/* 初始化设备 */
	ret = pddgpu_device_init(pddgpu);
	if (ret) {
		PDDGPU_ERROR("Failed to initialize PDDGPU device\n");
		goto err_free_pddgpu;
	}

	/* 注册DRM设备 */
	ret = drm_dev_register(ddev, 0);
	if (ret) {
		PDDGPU_ERROR("Failed to register DRM device\n");
		goto err_device_fini;
	}

	PDDGPU_INFO("PDDGPU device initialized successfully\n");
	return 0;

err_device_fini:
	pddgpu_device_fini(pddgpu);
err_free_pddgpu:
	kfree(pddgpu);
err_free_drm:
	drm_dev_put(ddev);
err_release_regions:
	pci_release_regions(pdev);
err_disable_device:
	pci_disable_device(pdev);
	return ret;
}

/* PCI移除函数 */
static void pddgpu_pci_remove(struct pci_dev *pdev)
{
	struct drm_device *ddev = pci_get_drvdata(pdev);
	struct pddgpu_device *pddgpu;

	if (!ddev) {
		PDDGPU_WARN("No DRM device found\n");
		return;
	}

	pddgpu = ddev->dev_private;
	PDDGPU_INFO("Removing PDDGPU device\n");

	drm_dev_unregister(ddev);
	pddgpu_device_fini(pddgpu);
	kfree(pddgpu);
	drm_dev_put(ddev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}

/* PCI驱动结构 */
static struct pci_driver pddgpu_pci_driver = {
	.name = "pddgpu",
	.id_table = pddgpu_pci_table,
	.probe = pddgpu_pci_probe,
	.remove = pddgpu_pci_remove,
	.driver = {
		.pm = &pddgpu_pm_ops,
	},
};

/* 模块初始化 */
static int __init pddgpu_init(void)
{
	int ret;

	PDDGPU_INFO("PDDGPU driver initializing\n");

	ret = pci_register_driver(&pddgpu_pci_driver);
	if (ret) {
		PDDGPU_ERROR("Failed to register PCI driver\n");
		return ret;
	}

	PDDGPU_INFO("PDDGPU driver initialized successfully\n");
	return 0;
}

/* 模块退出 */
static void __exit pddgpu_exit(void)
{
	PDDGPU_INFO("PDDGPU driver exiting\n");
	pci_unregister_driver(&pddgpu_pci_driver);
	PDDGPU_INFO("PDDGPU driver exited\n");
}

module_init(pddgpu_init);
module_exit(pddgpu_exit);

MODULE_AUTHOR("PDDGPU Project");
MODULE_DESCRIPTION("PDDGPU Graphics Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
