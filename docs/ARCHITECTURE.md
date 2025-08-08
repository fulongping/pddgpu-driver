# PDDGPU驱动架构文档

## 概述

PDDGPU驱动是一个模仿AMDGPU设计的简单GPU驱动框架，实现了完整的PCI设备管理、TTM内存管理和GEM对象管理。

## 架构设计

### 1. 整体架构

```
用户空间应用程序
    ↓
DRM接口层 (drm_ioctl)
    ↓
PDDGPU驱动核心
    ├── PCI设备管理 (pddgpu_drv.c)
    ├── 设备初始化 (pddgpu_device.c)
    ├── GMC内存控制器 (pddgpu_gmc.c)
    ├── TTM内存管理 (pddgpu_ttm.c)
    ├── VRAM管理器 (pddgpu_vram_mgr.c)
    ├── BO对象管理 (pddgpu_object.c)
    └── GEM接口 (pddgpu_gem.c)
    ↓
TTM框架
    ↓
Linux内核内存管理
```

### 2. 核心组件

#### 2.1 PCI设备管理 (pddgpu_drv.c)

**功能：**
- PCI设备探测和初始化
- DRM驱动注册
- 设备生命周期管理

**关键结构：**
```c
struct pci_driver pddgpu_pci_driver = {
    .name = "pddgpu",
    .id_table = pddgpu_pci_table,
    .probe = pddgpu_pci_probe,
    .remove = pddgpu_pci_remove,
};
```

**调用链：**
```
pci_register_driver() → pddgpu_pci_probe() → pddgpu_device_init()
```

#### 2.2 设备管理 (pddgpu_device.c)

**功能：**
- MMIO寄存器映射
- 设备信息读取
- 组件初始化协调

**关键函数：**
- `pddgpu_device_init()`: 设备初始化
- `pddgpu_device_fini()`: 设备清理
- `pddgpu_pm_suspend()`: 电源管理挂起
- `pddgpu_pm_resume()`: 电源管理恢复

#### 2.3 TTM内存管理 (pddgpu_ttm.c)

**功能：**
- TTM设备回调实现
- 内存分配和释放
- 缓冲区移动和交换

**关键回调：**
```c
static struct ttm_device_funcs pddgpu_bo_driver = {
    .ttm_tt_create = pddgpu_ttm_tt_create,
    .ttm_tt_populate = pddgpu_ttm_tt_populate,
    .ttm_tt_unpopulate = pddgpu_ttm_tt_unpopulate,
    .ttm_tt_destroy = pddgpu_ttm_backend_destroy,
    .eviction_valuable = pddgpu_ttm_bo_eviction_valuable,
    .evict_flags = pddgpu_evict_flags,
    .move = pddgpu_bo_move,
    .io_mem_reserve = pddgpu_ttm_io_mem_reserve,
    .io_mem_pfn = pddgpu_ttm_io_mem_pfn,
    .access_memory = pddgpu_ttm_access_memory,
    .delete_mem_notify = pddgpu_bo_delete_mem_notify,
};
```

#### 2.4 VRAM管理器 (pddgpu_vram_mgr.c)

**功能：**
- VRAM内存分配
- Buddy分配器管理
- 内存碎片处理

**关键回调：**
```c
static const struct ttm_resource_manager_func pddgpu_vram_mgr_func = {
    .new = pddgpu_vram_mgr_new,        // 分配VRAM
    .del = pddgpu_vram_mgr_del,        // 释放VRAM
    .debug = pddgpu_vram_mgr_debug,    // 调试接口
    .intersects = pddgpu_vram_mgr_intersects,
    .compatible = pddgpu_vram_mgr_compatible,
};
```

#### 2.5 BO对象管理 (pddgpu_object.c)

**功能：**
- 缓冲区对象创建和销毁
- 内存域管理
- 映射和固定

**关键函数：**
- `pddgpu_bo_create()`: 创建BO
- `pddgpu_bo_unref()`: 释放BO
- `pddgpu_bo_create_kernel()`: 内核BO创建
- `pddgpu_bo_free_kernel()`: 内核BO释放

#### 2.6 GEM接口 (pddgpu_gem.c)

**功能：**
- 用户空间GEM接口
- IOCTL处理
- Prime导出支持

**关键IOCTL：**
- `PDDGPU_GEM_CREATE`: 创建GEM对象
- `PDDGPU_GEM_MAP`: 映射GEM对象
- `PDDGPU_GEM_INFO`: 获取GEM信息
- `PDDGPU_GEM_DESTROY`: 销毁GEM对象

## 内存管理流程

### 1. VRAM分配流程

```
用户空间分配:
用户应用 → drm_ioctl → pddgpu_gem_create_ioctl → pddgpu_bo_create 
→ ttm_bo_init_reserved → ttm_bo_validate → ttm_resource_alloc 
→ pddgpu_vram_mgr_new → drm_buddy_alloc_blocks

内核空间分配:
内核驱动 → pddgpu_bo_create_kernel → pddgpu_bo_create 
→ ttm_bo_init_reserved → ttm_bo_validate → ttm_resource_alloc 
→ pddgpu_vram_mgr_new → drm_buddy_alloc_blocks
```

### 2. VRAM释放流程

```
用户空间释放:
用户应用 → drm_ioctl → pddgpu_gem_destroy_ioctl → drm_gem_object_put 
→ pddgpu_bo_destroy → ttm_bo_put → ttm_bo_cleanup_memtype_use 
→ pddgpu_vram_mgr_del → drm_buddy_free_block

内核空间释放:
内核驱动 → pddgpu_bo_free_kernel → pddgpu_bo_unref 
→ drm_gem_object_put → pddgpu_bo_destroy → ttm_bo_put 
→ ttm_bo_cleanup_memtype_use → pddgpu_vram_mgr_del 
→ drm_buddy_free_block
```

## 设备管理流程

### 1. 设备初始化流程

```
pci_register_driver() → pddgpu_pci_probe() → pddgpu_device_init()
    ├── pci_enable_device()
    ├── pci_request_regions()
    ├── pci_set_dma_mask()
    ├── pddgpu_gmc_init()
    ├── pddgpu_ttm_init()
    ├── pddgpu_vram_mgr_init()
    └── drm_dev_register()
```

### 2. 设备清理流程

```
pddgpu_pci_remove() → pddgpu_device_fini()
    ├── pddgpu_vram_mgr_fini()
    ├── pddgpu_ttm_fini()
    ├── pddgpu_gmc_fini()
    └── pci_iounmap()
```

## 性能优化

### 1. 内存管理优化

- **Buddy分配器**: 减少内存碎片
- **智能放置**: 根据缓冲区特性选择最优内存域
- **动态移动**: 在内存域间动态移动缓冲区
- **预留机制**: 为关键组件预留VRAM空间

### 2. 驱动优化

- **异步操作**: 使用工作队列处理耗时操作
- **批量操作**: 支持批量内存操作
- **缓存管理**: 智能缓存策略
- **中断处理**: 高效的中断处理机制

## 调试和监控

### 1. 调试接口

- **DebugFS**: 提供调试文件系统接口
- **日志系统**: 分级日志输出
- **统计信息**: 内存使用统计
- **错误处理**: 完善的错误检测和恢复

### 2. 监控功能

- **内存统计**: VRAM/GTT使用情况
- **性能监控**: 分配/释放性能统计
- **错误监控**: 错误计数和报告
- **状态监控**: 设备状态监控

## 扩展性设计

### 1. 模块化设计

- **组件分离**: 各功能模块独立
- **接口标准化**: 标准化的组件接口
- **插件支持**: 支持功能插件扩展
- **配置灵活**: 灵活的配置系统

### 2. 硬件抽象

- **寄存器抽象**: 硬件寄存器抽象层
- **芯片支持**: 支持多种芯片型号
- **特性检测**: 运行时特性检测
- **兼容性**: 向后兼容性支持

## 总结

PDDGPU驱动采用了现代化的Linux内核驱动架构，具有以下特点：

1. **完整的内存管理**: 基于TTM框架的完整内存管理系统
2. **标准化的接口**: 符合DRM/GEM标准的用户空间接口
3. **高性能设计**: 优化的内存分配和访问机制
4. **良好的扩展性**: 模块化设计支持功能扩展
5. **完善的调试**: 丰富的调试和监控功能

这个架构为GPU驱动开发提供了一个良好的基础框架，可以在此基础上进行功能扩展和优化。
