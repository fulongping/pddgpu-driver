# PDDGPU 内存管理概述

## 架构设计

### 1. 整体架构

```
用户空间应用程序
        ↓
    DRM IOCTL 接口
        ↓
    PDDGPU GEM 层
        ↓
    TTM 框架
        ↓
    PDDGPU 内存管理器
    ├── VRAM 管理器 (DRM Buddy)
    └── GTT 管理器 (DRM MM)
        ↓
    硬件内存资源
```

### 2. 内存类型对比

| 特性 | VRAM | GTT |
|------|------|-----|
| 物理位置 | GPU 显存 | 系统内存 + GPU 映射 |
| 分配器 | DRM Buddy | DRM MM |
| 访问方式 | GPU 直接访问 | 通过 GTT 映射 |
| 性能特点 | 高带宽，低延迟 | 较低带宽，较高延迟 |
| 用途 | 纹理、帧缓冲 | 用户数据、临时缓冲 |

## VRAM 内存管理

### 1. 数据结构

```c
struct pddgpu_vram_mgr {
    struct ttm_resource_manager manager;  // TTM 资源管理器
    struct drm_buddy mm;                 // DRM Buddy 分配器
    struct mutex lock;                   // 保护锁
    struct list_head reservations_pending; // 待处理预留
    struct list_head reserved_pages;      // 已预留页面
    atomic64_t vis_usage;                // 可见内存使用量
    u64 default_page_size;               // 默认页面大小
};

struct pddgpu_vram_mgr_resource {
    struct ttm_resource base;            // TTM 资源基类
    struct list_head blocks;             // Buddy 块列表
    unsigned long flags;                 // 分配标志
};
```

### 2. 管理器函数表

```c
const struct ttm_resource_manager_func pddgpu_vram_mgr_func = {
    .alloc = pddgpu_vram_mgr_alloc,      // 分配函数
    .free = pddgpu_vram_mgr_free,        // 释放函数
    .debug = pddgpu_vram_mgr_debug,      // 调试函数
    .intersects = pddgpu_vram_mgr_intersects,    // 重叠检查
    .compatible = pddgpu_vram_mgr_compatible,    // 兼容性检查
};
```

## GTT 内存管理

### 1. 数据结构

```c
struct pddgpu_gtt_mgr {
    struct ttm_resource_manager manager;  // TTM 资源管理器
    struct drm_mm mm;                    // DRM MM 分配器
    spinlock_t lock;                     // 保护锁
};
```

### 2. 管理器函数表

```c
const struct ttm_resource_manager_func pddgpu_gtt_mgr_func = {
    .alloc = pddgpu_gtt_mgr_alloc,      // 分配函数
    .free = pddgpu_gtt_mgr_free,        // 释放函数
    .debug = pddgpu_gtt_mgr_debug,      // 调试函数
    .intersects = pddgpu_gtt_mgr_intersects,    // 重叠检查
    .compatible = pddgpu_gtt_mgr_compatible,    // 兼容性检查
};
```

## 初始化流程

### 1. VRAM 管理器初始化

```c
int pddgpu_vram_mgr_init(struct pddgpu_device *pdev)
{
    struct pddgpu_vram_mgr *mgr = &pdev->mman.vram_mgr;
    struct ttm_resource_manager *man = &mgr->manager;
    int err;

    // 1. 初始化 TTM 资源管理器
    ttm_resource_manager_init(man, &pdev->mman.bdev, pdev->vram_size);

    // 2. 初始化互斥锁和链表
    mutex_init(&mgr->lock);
    INIT_LIST_HEAD(&mgr->reservations_pending);
    INIT_LIST_HEAD(&mgr->reserved_pages);
    mgr->default_page_size = PAGE_SIZE;

    // 3. 设置管理器函数
    man->func = &pddgpu_vram_mgr_func;

    // 4. 初始化 DRM Buddy 分配器
    err = drm_buddy_init(&mgr->mm, man->size, PAGE_SIZE);
    if (err)
        return err;

    // 5. 注册到 TTM 设备
    ttm_set_driver_manager(&pdev->mman.bdev, TTM_PL_VRAM, &mgr->manager);
    ttm_resource_manager_set_used(man, true);

    return 0;
}
```

### 2. GTT 管理器初始化

```c
int pddgpu_gtt_mgr_init(struct pddgpu_device *pdev, uint64_t gtt_size)
{
    struct pddgpu_gtt_mgr *mgr = &pdev->mman.gtt_mgr;
    struct ttm_resource_manager *man = &mgr->manager;
    uint64_t start, size;

    // 1. 设置 TTM 管理器属性
    man->use_tt = true;
    man->func = &pddgpu_gtt_mgr_func;

    // 2. 初始化 TTM 资源管理器
    ttm_resource_manager_init(man, &pdev->mman.bdev, gtt_size);

    // 3. 初始化 DRM MM 分配器
    start = PDDGPU_GTT_MAX_TRANSFER_SIZE * PDDGPU_GTT_NUM_TRANSFER_WINDOWS;
    size = (pdev->gtt_size >> PAGE_SHIFT) - start;
    drm_mm_init(&mgr->mm, start, size);
    spin_lock_init(&mgr->lock);

    // 4. 注册到 TTM 设备
    ttm_set_driver_manager(&pdev->mman.bdev, TTM_PL_TT, &mgr->manager);
    ttm_resource_manager_set_used(man, true);

    return 0;
}
```

## 注册机制

### 1. 管理器函数表注册

```c
// VRAM 管理器
const struct ttm_resource_manager_func pddgpu_vram_mgr_func = {
    .alloc = pddgpu_vram_mgr_alloc,      // 用户实现的分配函数
    .free = pddgpu_vram_mgr_free,        // 用户实现的释放函数
    .debug = pddgpu_vram_mgr_debug,      // 用户实现的调试函数
    .intersects = pddgpu_vram_mgr_intersects,    // 用户实现的重叠检查
    .compatible = pddgpu_vram_mgr_compatible,    // 用户实现的兼容性检查
};

// GTT 管理器
const struct ttm_resource_manager_func pddgpu_gtt_mgr_func = {
    .alloc = pddgpu_gtt_mgr_alloc,       // 用户实现的分配函数
    .free = pddgpu_gtt_mgr_free,         // 用户实现的释放函数
    .debug = pddgpu_gtt_mgr_debug,       // 用户实现的调试函数
    .intersects = pddgpu_gtt_mgr_intersects,     // 用户实现的重叠检查
    .compatible = pddgpu_gtt_mgr_compatible,     // 用户实现的兼容性检查
};
```

### 2. 管理器注册到 TTM 设备

```c
// VRAM 管理器注册
ttm_set_driver_manager(&pdev->mman.bdev, TTM_PL_VRAM, &mgr->manager);

// GTT 管理器注册
ttm_set_driver_manager(&pdev->mman.bdev, TTM_PL_TT, &mgr->manager);
```

## 回调调用机制

### 1. TTM 框架调用用户回调

```c
// TTM 框架内部
int ttm_resource_manager_alloc(struct ttm_resource_manager *man,
                              struct ttm_buffer_object *bo,
                              const struct ttm_place *place,
                              struct ttm_resource **res)
{
    // 调用用户注册的分配函数
    return man->func->alloc(man, bo, place, res);
}

void ttm_resource_manager_free(struct ttm_resource_manager *man,
                              struct ttm_resource *res)
{
    // 调用用户注册的释放函数
    man->func->free(man, res);
}
```

### 2. 管理器类型选择

```c
// TTM 框架内部
struct ttm_resource_manager *ttm_manager_type(struct ttm_device *bdev,
                                             int mem_type)
{
    switch (mem_type) {
    case TTM_PL_VRAM:
        return bdev->man[TTM_PL_VRAM];  // 返回 VRAM 管理器
    case TTM_PL_TT:
        return bdev->man[TTM_PL_TT];    // 返回 GTT 管理器
    default:
        return NULL;
    }
}
```
