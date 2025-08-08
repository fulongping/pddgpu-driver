# PDDGPU 内存管理总结

## 概述

PDDGPU 驱动实现了两种内存管理方式：VRAM 管理和 GTT 管理。这两种管理器都通过 TTM 框架进行集成，为用户提供了完整的内存管理解决方案。

## 架构对比

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

## 关键回调函数

### 1. VRAM 管理器回调

#### 分配函数
```c
static int pddgpu_vram_mgr_alloc(struct ttm_resource_manager *man,
                                  struct ttm_buffer_object *bo,
                                  const struct ttm_place *place,
                                  struct ttm_resource **res)
```

**功能：**
- 使用 DRM Buddy 算法分配 VRAM
- 支持连续和非连续内存分配
- 处理内存对齐要求
- 更新可见内存使用统计

#### 释放函数
```c
static void pddgpu_vram_mgr_free(struct ttm_resource_manager *man,
                                  struct ttm_resource *res)
```

**功能：**
- 释放 VRAM 资源
- 合并相邻的空闲块
- 更新使用统计

### 2. GTT 管理器回调

#### 分配函数
```c
static int pddgpu_gtt_mgr_alloc(struct ttm_resource_manager *man,
                                 struct ttm_buffer_object *bo,
                                 const struct ttm_place *place,
                                 struct ttm_resource **res)
```

**功能：**
- 使用 DRM MM 分配 GTT 地址空间
- 支持临时和永久分配
- 处理地址对齐要求
- 更新使用统计

#### 释放函数
```c
static void pddgpu_gtt_mgr_free(struct ttm_resource_manager *man,
                                 struct ttm_resource *res)
```

**功能：**
- 释放 GTT 地址空间
- 合并相邻的空闲区域
- 更新使用统计

## 调用链分析

### 1. 用户如何调用到自身实现的回调

#### 注册机制
```c
// 1. 定义管理器函数表
const struct ttm_resource_manager_func pddgpu_vram_mgr_func = {
    .alloc = pddgpu_vram_mgr_alloc,      // 用户实现的分配函数
    .free = pddgpu_vram_mgr_free,        // 用户实现的释放函数
    .debug = pddgpu_vram_mgr_debug,      // 用户实现的调试函数
    .intersects = pddgpu_vram_mgr_intersects,    // 用户实现的重叠检查
    .compatible = pddgpu_vram_mgr_compatible,    // 用户实现的兼容性检查
};

// 2. 注册到 TTM 设备
ttm_set_driver_manager(&pdev->mman.bdev, TTM_PL_VRAM, &mgr->manager);
```

#### 调用机制
```c
// TTM 框架内部调用用户回调
int ttm_resource_manager_alloc(struct ttm_resource_manager *man,
                              struct ttm_buffer_object *bo,
                              const struct ttm_place *place,
                              struct ttm_resource **res)
{
    // 调用用户注册的分配函数
    return man->func->alloc(man, bo, place, res);
}
```

### 2. 完整调用链

#### VRAM 分配调用链
```
用户空间: drmIoctl(fd, DRM_IOCTL_PDDGPU_GEM_CREATE, &create)
    ↓
内核空间: pddgpu_gem_create_ioctl()
    ↓
内核空间: pddgpu_bo_create()
    ↓
TTM 框架: ttm_bo_init_reserved()
    ↓
TTM 框架: ttm_bo_validate()
    ↓
TTM 框架: ttm_resource_manager_alloc()
    ↓
TTM 框架: man->func->alloc()  // 调用 pddgpu_vram_mgr_alloc
    ↓
用户实现: pddgpu_vram_mgr_alloc()  // 用户自定义的 VRAM 分配逻辑
    ↓
DRM Buddy: drm_buddy_alloc_blocks()  // 使用 DRM Buddy 分配
```

#### GTT 分配调用链
```
用户空间: drmIoctl(fd, DRM_IOCTL_PDDGPU_GEM_CREATE, &create)
    ↓
内核空间: pddgpu_gem_create_ioctl()
    ↓
内核空间: pddgpu_bo_create()
    ↓
TTM 框架: ttm_bo_init_reserved()
    ↓
TTM 框架: ttm_bo_validate()
    ↓
TTM 框架: ttm_resource_manager_alloc()
    ↓
TTM 框架: man->func->alloc()  // 调用 pddgpu_gtt_mgr_alloc
    ↓
用户实现: pddgpu_gtt_mgr_alloc()  // 用户自定义的 GTT 分配逻辑
    ↓
DRM MM: drm_mm_insert_node_in_range()  // 使用 DRM MM 分配
```

## 管理器类型选择

### 1. TTM 框架选择机制
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

### 2. 放置策略设置
```c
// 在 BO 创建时设置放置策略
if (bp->domain == PDDGPU_GEM_DOMAIN_VRAM) {
    bo->placement.placement[0] = (struct ttm_place){
        .mem_type = TTM_PL_VRAM,
        .flags = TTM_PL_FLAG_VRAM,
    };
} else if (bp->domain == PDDGPU_GEM_DOMAIN_GTT) {
    bo->placement.placement[0] = (struct ttm_place){
        .mem_type = TTM_PL_TT,
        .flags = TTM_PL_FLAG_SYSTEM,
    };
}
```

## 性能优化策略

### 1. VRAM 优化

#### Buddy 算法优化
- **大块优先**：优先使用大块分配，减少碎片
- **连续分配**：支持连续内存分配，提高访问效率
- **对齐优化**：根据硬件要求进行内存对齐

#### 统计跟踪
- **可见内存跟踪**：跟踪实际可见的 VRAM 使用量
- **碎片分析**：通过 DRM Buddy 提供碎片分析
- **性能监控**：监控分配和释放性能

### 2. GTT 优化

#### MM 算法优化
- **最佳适配**：使用 `DRM_MM_INSERT_BEST` 进行最佳适配
- **地址对齐**：根据页面大小和对齐要求优化分配
- **临时分配**：支持临时分配不占用实际地址空间

#### 并发控制
- **自旋锁保护**：使用自旋锁保护并发访问
- **原子操作**：使用原子操作更新统计信息
- **锁粒度优化**：最小化锁的持有时间

## 调试和监控

### 1. 调试接口

#### VRAM 调试
```c
static void pddgpu_vram_mgr_debug(struct ttm_resource_manager *man,
                                   struct drm_printer *printer)
{
    struct pddgpu_vram_mgr *mgr = to_vram_mgr(man);
    struct drm_buddy *mm = &mgr->mm;

    drm_printf(printer, "  vis usage:%llu\n",
               atomic64_read(&mgr->vis_usage));

    mutex_lock(&mgr->lock);
    drm_printf(printer, "default_page_size: %lluKiB\n",
               mgr->default_page_size >> 10);

    drm_buddy_print(mm, printer);
    mutex_unlock(&mgr->lock);
}
```

#### GTT 调试
```c
static void pddgpu_gtt_mgr_debug(struct ttm_resource_manager *man,
                                  struct drm_printer *printer)
{
    struct pddgpu_gtt_mgr *mgr = to_gtt_mgr(man);

    spin_lock(&mgr->lock);
    drm_mm_print(&mgr->mm, printer);
    spin_unlock(&mgr->lock);
}
```

### 2. 统计信息

#### VRAM 统计
- **总 VRAM 大小**：设备的总 VRAM 容量
- **已使用 VRAM**：当前已分配的 VRAM 大小
- **可见 VRAM 使用量**：实际可见的 VRAM 使用量
- **碎片信息**：通过 DRM Buddy 提供的碎片分析

#### GTT 统计
- **总 GTT 大小**：设备的总 GTT 容量
- **已使用 GTT**：当前已分配的 GTT 大小
- **地址空间使用**：GTT 地址空间的使用情况
- **碎片信息**：通过 DRM MM 提供的碎片分析

## 错误处理

### 1. 分配失败处理

#### VRAM 分配失败
```c
// 检查可用空间
if (ttm_resource_manager_usage(man) > max_bytes) {
    r = -ENOSPC;
    goto error_fini;
}

// 连续分配失败时的回退
if (unlikely(r == -ENOSPC) && pages_per_block == ~0ul &&
    !(place->flags & TTM_PL_FLAG_CONTIGUOUS)) {
    vres->flags &= ~DRM_BUDDY_CONTIGUOUS_ALLOCATION;
    pages_per_block = max_t(u32, 2UL << (20UL - PAGE_SHIFT),
                            bo->page_alignment);
    continue;
}
```

#### GTT 分配失败
```c
// 检查 GTT 使用量
if (!(place->flags & TTM_PL_FLAG_TEMPORARY) &&
    ttm_resource_manager_usage(man) > man->size) {
    r = -ENOSPC;
    goto err_free;
}
```

### 2. 资源清理

#### VRAM 资源清理
```c
error_free_blocks:
    drm_buddy_free_list(mm, &vres->blocks, 0);
    mutex_unlock(&mgr->lock);
error_fini:
    ttm_resource_fini(man, &vres->base);
    kfree(vres);
    return r;
```

#### GTT 资源清理
```c
err_free:
    ttm_resource_fini(man, &node->base);
    kfree(node);
    return r;
```

## 扩展性设计

### 1. 模块化设计
- **独立管理器**：VRAM 和 GTT 管理器独立实现
- **标准化接口**：通过 TTM 框架提供标准化接口
- **可扩展架构**：支持添加新的内存管理器

### 2. 自定义能力
- **回调函数**：用户可以自定义分配、释放、调试等回调函数
- **分配策略**：支持自定义分配策略和优化算法
- **统计跟踪**：支持自定义统计信息和监控指标

### 3. 调试支持
- **详细日志**：提供详细的操作日志和错误信息
- **统计接口**：提供丰富的统计信息接口
- **调试工具**：支持各种调试工具和分析方法

## 总结

PDDGPU 的内存管理系统通过以下机制实现用户自定义回调：

### 1. **注册机制**
- 通过 `ttm_resource_manager_func` 结构体注册回调函数
- 通过 `ttm_set_driver_manager` 注册管理器到 TTM 设备

### 2. **调用机制**
- TTM 框架通过 `man->func->alloc` 调用用户注册的分配函数
- TTM 框架通过 `man->func->free` 调用用户注册的释放函数

### 3. **选择机制**
- 根据内存类型（`TTM_PL_VRAM` 或 `TTM_PL_TT`）选择对应的管理器
- 通过 `ttm_manager_type` 函数获取正确的管理器

### 4. **扩展性**
- 用户可以轻松添加新的内存管理器
- 支持自定义分配算法和优化策略
- 提供丰富的调试和监控接口

这种设计既保持了与 TTM 框架的兼容性，又提供了高度的可扩展性，允许用户根据具体需求实现自定义的内存管理策略。通过 VRAM 和 GTT 两种管理器的配合，PDDGPU 能够为不同的应用场景提供最优的内存管理解决方案。
