# PDDGPU 内存管理详细分析

## 概述

本文档详细分析了 PDDGPU 驱动中的两种内存管理方式：VRAM 管理和 GTT 管理。包括架构设计、关键回调函数解析、调用链分析以及用户如何调用到自身实现的回调函数。

## 内存管理架构

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

### 1. 架构设计

#### 数据结构
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

#### 管理器函数表
```c
const struct ttm_resource_manager_func pddgpu_vram_mgr_func = {
    .alloc = pddgpu_vram_mgr_alloc,      // 分配函数
    .free = pddgpu_vram_mgr_free,        // 释放函数
    .debug = pddgpu_vram_mgr_debug,      // 调试函数
    .intersects = pddgpu_vram_mgr_intersects,    // 重叠检查
    .compatible = pddgpu_vram_mgr_compatible,    // 兼容性检查
};
```

### 2. 关键回调函数解析

#### 2.1 VRAM 分配函数
```c
static int pddgpu_vram_mgr_alloc(struct ttm_resource_manager *man,
                                  struct ttm_buffer_object *bo,
                                  const struct ttm_place *place,
                                  struct ttm_resource **res)
```

**功能分析：**
- **参数解析**：解析 TTM 放置参数，确定分配范围
- **大小计算**：根据 BO 大小计算所需页面数
- **块大小确定**：根据连续性和对齐要求确定块大小
- **DRM Buddy 分配**：使用 Buddy 算法分配内存块
- **统计更新**：更新可见内存使用统计

**关键代码段：**
```c
// 1. 计算分配参数
lpfn = (u64)place->lpfn << PAGE_SHIFT;
fpfn = (u64)place->fpfn << PAGE_SHIFT;

// 2. 确定块大小
if (pbo->flags & PDDGPU_GEM_CREATE_VRAM_CONTIGUOUS) {
    pages_per_block = ~0ul;  // 连续分配
} else {
    pages_per_block = 2UL << (20UL - PAGE_SHIFT);  // 默认 2MB
}

// 3. 使用 DRM Buddy 分配
r = drm_buddy_alloc_blocks(mm, fpfn, lpfn, size,
                           min_block_size, &vres->blocks, vres->flags);
```

#### 2.2 VRAM 释放函数
```c
static void pddgpu_vram_mgr_free(struct ttm_resource_manager *man,
                                  struct ttm_resource *res)
```

**功能分析：**
- **统计计算**：计算要释放的可见内存使用量
- **块释放**：释放 DRM Buddy 块
- **统计更新**：更新使用统计
- **资源清理**：清理 TTM 资源

**关键代码段：**
```c
// 1. 计算可见内存使用量
list_for_each_entry(block, &vres->blocks, link)
    vis_usage += pddgpu_vram_mgr_block_size(block);

// 2. 释放 Buddy 块
drm_buddy_free_list(mm, &vres->blocks, 0);

// 3. 更新统计
atomic64_sub(vis_usage, &mgr->vis_usage);
```

### 3. 调用链分析

#### 3.1 用户空间到 VRAM 分配的完整调用链

```
用户空间应用程序
    ↓
drmIoctl(fd, DRM_IOCTL_PDDGPU_GEM_CREATE, &create)
    ↓
pddgpu_gem_create_ioctl()
    ↓
pddgpu_gem_create_object()
    ↓
pddgpu_bo_create()
    ↓
ttm_bo_init_reserved()
    ↓
ttm_bo_validate()
    ↓
ttm_resource_manager_alloc()
    ↓
man->func->alloc()  // 调用 pddgpu_vram_mgr_alloc
    ↓
pddgpu_vram_mgr_alloc()
    ↓
drm_buddy_alloc_blocks()  // DRM Buddy 分配
```

#### 3.2 详细调用链解析

**步骤 1：用户空间调用**
```c
// 用户空间代码
struct drm_pddgpu_gem_create create = {
    .size = 1024 * 1024,  // 1MB
    .domain = PDDGPU_GEM_DOMAIN_VRAM,
    .flags = PDDGPU_GEM_CREATE_VRAM_CONTIGUOUS
};
drmIoctl(fd, DRM_IOCTL_PDDGPU_GEM_CREATE, &create);
```

**步骤 2：IOCTL 处理**
```c
// pddgpu_gem.c
static int pddgpu_gem_create_ioctl(struct drm_device *dev, void *data,
                                   struct drm_file *file_priv)
{
    struct drm_pddgpu_gem_create *args = data;
    struct pddgpu_bo *bo;
    
    // 创建 BO 参数
    struct pddgpu_bo_param bp = {
        .size = args->size,
        .domain = args->domain,
        .flags = args->flags,
        // ...
    };
    
    // 调用 BO 创建
    ret = pddgpu_bo_create(pdev, &bp, &bo);
}
```

**步骤 3：BO 创建**
```c
// pddgpu_object.c
int pddgpu_bo_create(struct pddgpu_device *pdev,
                     struct pddgpu_bo_param *bp,
                     struct pddgpu_bo **bo_ptr)
{
    // 设置 TTM 操作上下文
    struct ttm_operation_ctx ctx = {
        .interruptible = true,
        .no_wait_gpu = bp->no_wait_gpu,
    };
    
    // 设置放置策略
    pddgpu_bo_placement_from_domain(bo, bp->domain);
    
    // 调用 TTM 初始化
    r = ttm_bo_init_reserved(&pdev->mman.bdev, &bo->tbo, bp->type,
                             &bo->placement, page_align, &ctx, NULL,
                             bp->resv, bp->destroy);
}
```

**步骤 4：TTM 初始化**
```c
// TTM 框架内部
int ttm_bo_init_reserved(struct ttm_device *bdev,
                         struct ttm_buffer_object *bo,
                         enum ttm_bo_type type,
                         struct ttm_placement *placement,
                         uint32_t page_alignment,
                         struct ttm_operation_ctx *ctx,
                         struct sg_table *sg,
                         struct dma_resv *resv,
                         void (*destroy)(struct ttm_buffer_object *))
{
    // 初始化 BO 结构
    ttm_bo_init(bdev, bo, size, type, placement, page_alignment,
                false, sg, resv, destroy);
    
    // 验证和分配内存
    ret = ttm_bo_validate(bo, placement, ctx);
    
    // 设置预留状态
    ttm_bo_reserve(bo, false, false, NULL);
}
```

**步骤 5：TTM 验证**
```c
// TTM 框架内部
int ttm_bo_validate(struct ttm_buffer_object *bo,
                    struct ttm_placement *placement,
                    struct ttm_operation_ctx *ctx)
{
    // 为每个放置策略尝试分配
    for (i = 0; i < placement->num_placement; i++) {
        ret = ttm_resource_manager_alloc(man, bo, &place, &res);
        if (ret == 0)
            break;
    }
}
```

**步骤 6：资源管理器分配**
```c
// TTM 框架内部
int ttm_resource_manager_alloc(struct ttm_resource_manager *man,
                              struct ttm_buffer_object *bo,
                              const struct ttm_place *place,
                              struct ttm_resource **res)
{
    // 调用管理器的分配函数
    return man->func->alloc(man, bo, place, res);
}
```

**步骤 7：VRAM 管理器分配**
```c
// pddgpu_vram_mgr.c
static int pddgpu_vram_mgr_alloc(struct ttm_resource_manager *man,
                                  struct ttm_buffer_object *bo,
                                  const struct ttm_place *place,
                                  struct ttm_resource **res)
{
    // 使用 DRM Buddy 分配 VRAM
    r = drm_buddy_alloc_blocks(mm, fpfn, lpfn, size,
                               min_block_size, &vres->blocks, vres->flags);
    
    // 设置资源属性
    vres->base.start = 0;
    vres->base.bus.caching = ttm_write_combined;
    
    *res = &vres->base;
    return 0;
}
```

## GTT 内存管理

### 1. 架构设计

#### 数据结构
```c
struct pddgpu_gtt_mgr {
    struct ttm_resource_manager manager;  // TTM 资源管理器
    struct drm_mm mm;                    // DRM MM 分配器
    spinlock_t lock;                     // 保护锁
};
```

#### 管理器函数表
```c
const struct ttm_resource_manager_func pddgpu_gtt_mgr_func = {
    .alloc = pddgpu_gtt_mgr_alloc,      // 分配函数
    .free = pddgpu_gtt_mgr_free,        // 释放函数
    .debug = pddgpu_gtt_mgr_debug,      // 调试函数
    .intersects = pddgpu_gtt_mgr_intersects,    // 重叠检查
    .compatible = pddgpu_gtt_mgr_compatible,    // 兼容性检查
};
```

### 2. 关键回调函数解析

#### 2.1 GTT 分配函数
```c
static int pddgpu_gtt_mgr_alloc(struct ttm_resource_manager *man,
                                 struct ttm_buffer_object *bo,
                                 const struct ttm_place *place,
                                 struct ttm_resource **res)
```

**功能分析：**
- **节点分配**：分配 TTM 范围管理器节点
- **资源初始化**：初始化 TTM 资源结构
- **使用量检查**：检查 GTT 使用量是否超限
- **地址分配**：使用 DRM MM 分配 GTT 地址空间
- **临时分配**：支持临时分配不占用实际地址

**关键代码段：**
```c
// 1. 分配 TTM 范围管理器节点
node = kzalloc(struct_size(node, mm_nodes, 1), GFP_KERNEL);

// 2. 初始化 TTM 资源
ttm_resource_init(bo, place, &node->base);

// 3. 检查 GTT 使用量
if (!(place->flags & TTM_PL_FLAG_TEMPORARY) &&
    ttm_resource_manager_usage(man) > man->size) {
    r = -ENOSPC;
    goto err_free;
}

// 4. 分配 GTT 地址空间
if (place->lpfn) {
    spin_lock(&mgr->lock);
    r = drm_mm_insert_node_in_range(&mgr->mm, &node->mm_nodes[0],
                                   num_pages, bo->page_alignment,
                                   0, place->fpfn, place->lpfn,
                                   DRM_MM_INSERT_BEST);
    spin_unlock(&mgr->lock);
    if (unlikely(r))
        goto err_free;

    node->base.start = node->mm_nodes[0].start;
} else {
    // 临时分配，不分配实际地址
    node->mm_nodes[0].start = 0;
    node->mm_nodes[0].size = PFN_UP(node->base.size);
    node->base.start = PDDGPU_BO_INVALID_OFFSET;
}
```

#### 2.2 GTT 释放函数
```c
static void pddgpu_gtt_mgr_free(struct ttm_resource_manager *man,
                                 struct ttm_resource *res)
```

**功能分析：**
- **节点检查**：检查 DRM MM 节点是否已分配
- **节点移除**：从 DRM MM 中移除节点
- **资源清理**：清理 TTM 资源

**关键代码段：**
```c
// 1. 检查节点是否已分配
if (drm_mm_node_allocated(&node->mm_nodes[0]))
    drm_mm_remove_node(&node->mm_nodes[0]);

// 2. 清理资源
ttm_resource_fini(man, res);
kfree(node);
```

### 3. 调用链分析

#### 3.1 用户空间到 GTT 分配的完整调用链

```
用户空间应用程序
    ↓
drmIoctl(fd, DRM_IOCTL_PDDGPU_GEM_CREATE, &create)
    ↓
pddgpu_gem_create_ioctl()
    ↓
pddgpu_gem_create_object()
    ↓
pddgpu_bo_create()
    ↓
ttm_bo_init_reserved()
    ↓
ttm_bo_validate()
    ↓
ttm_resource_manager_alloc()
    ↓
man->func->alloc()  // 调用 pddgpu_gtt_mgr_alloc
    ↓
pddgpu_gtt_mgr_alloc()
    ↓
drm_mm_insert_node_in_range()  // DRM MM 分配
```

#### 3.2 详细调用链解析

**步骤 1：用户空间调用**
```c
// 用户空间代码
struct drm_pddgpu_gem_create create = {
    .size = 1024 * 1024,  // 1MB
    .domain = PDDGPU_GEM_DOMAIN_GTT,
    .flags = 0
};
drmIoctl(fd, DRM_IOCTL_PDDGPU_GEM_CREATE, &create);
```

**步骤 2：IOCTL 处理**
```c
// pddgpu_gem.c
static int pddgpu_gem_create_ioctl(struct drm_device *dev, void *data,
                                   struct drm_file *file_priv)
{
    struct drm_pddgpu_gem_create *args = data;
    struct pddgpu_bo *bo;
    
    // 创建 BO 参数
    struct pddgpu_bo_param bp = {
        .size = args->size,
        .domain = args->domain,  // PDDGPU_GEM_DOMAIN_GTT
        .flags = args->flags,
        // ...
    };
    
    // 调用 BO 创建
    ret = pddgpu_bo_create(pdev, &bp, &bo);
}
```

**步骤 3：BO 创建**
```c
// pddgpu_object.c
int pddgpu_bo_create(struct pddgpu_device *pdev,
                     struct pddgpu_bo_param *bp,
                     struct pddgpu_bo **bo_ptr)
{
    // 设置放置策略
    pddgpu_bo_placement_from_domain(bo, bp->domain);
    
    // 对于 GTT 域，设置 TTM_PL_TT 放置
    if (bp->domain == PDDGPU_GEM_DOMAIN_GTT) {
        bo->placement.placement[0] = (struct ttm_place){
            .mem_type = TTM_PL_TT,
            .flags = TTM_PL_FLAG_SYSTEM,
        };
    }
    
    // 调用 TTM 初始化
    r = ttm_bo_init_reserved(&pdev->mman.bdev, &bo->tbo, bp->type,
                             &bo->placement, page_align, &ctx, NULL,
                             bp->resv, bp->destroy);
}
```

**步骤 4：TTM 验证**
```c
// TTM 框架内部
int ttm_bo_validate(struct ttm_buffer_object *bo,
                    struct ttm_placement *placement,
                    struct ttm_operation_ctx *ctx)
{
    // 为每个放置策略尝试分配
    for (i = 0; i < placement->num_placement; i++) {
        // 对于 GTT，man 是 GTT 管理器
        man = ttm_manager_type(bdev, TTM_PL_TT);
        ret = ttm_resource_manager_alloc(man, bo, &place, &res);
        if (ret == 0)
            break;
    }
}
```

**步骤 5：GTT 管理器分配**
```c
// pddgpu_gtt_mgr.c
static int pddgpu_gtt_mgr_alloc(struct ttm_resource_manager *man,
                                 struct ttm_buffer_object *bo,
                                 const struct ttm_place *place,
                                 struct ttm_resource **res)
{
    // 使用 DRM MM 分配 GTT 地址空间
    r = drm_mm_insert_node_in_range(&mgr->mm, &node->mm_nodes[0],
                                   num_pages, bo->page_alignment,
                                   0, place->fpfn, place->lpfn,
                                   DRM_MM_INSERT_BEST);
    
    // 设置资源属性
    node->base.start = node->mm_nodes[0].start;
    
    *res = &node->base;
    return 0;
}
```

## 内存管理器初始化

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

## 用户如何调用到自身实现的回调

### 1. 注册机制

#### 1.1 管理器函数表注册
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

#### 1.2 管理器注册到 TTM 设备
```c
// VRAM 管理器注册
ttm_set_driver_manager(&pdev->mman.bdev, TTM_PL_VRAM, &mgr->manager);

// GTT 管理器注册
ttm_set_driver_manager(&pdev->mman.bdev, TTM_PL_TT, &mgr->manager);
```

### 2. 回调调用机制

#### 2.1 TTM 框架调用用户回调
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

#### 2.2 管理器类型选择
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

### 3. 完整调用流程示例

#### 3.1 VRAM 分配调用流程
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

#### 3.2 GTT 分配调用流程
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

## 性能优化策略

### 1. VRAM 优化

#### 1.1 Buddy 算法优化
- **大块优先**：优先使用大块分配，减少碎片
- **连续分配**：支持连续内存分配，提高访问效率
- **对齐优化**：根据硬件要求进行内存对齐

#### 1.2 统计跟踪
- **可见内存跟踪**：跟踪实际可见的 VRAM 使用量
- **碎片分析**：通过 DRM Buddy 提供碎片分析
- **性能监控**：监控分配和释放性能

### 2. GTT 优化

#### 2.1 MM 算法优化
- **最佳适配**：使用 `DRM_MM_INSERT_BEST` 进行最佳适配
- **地址对齐**：根据页面大小和对齐要求优化分配
- **临时分配**：支持临时分配不占用实际地址空间

#### 2.2 并发控制
- **自旋锁保护**：使用自旋锁保护并发访问
- **原子操作**：使用原子操作更新统计信息
- **锁粒度优化**：最小化锁的持有时间

## 调试和监控

### 1. 调试接口

#### 1.1 VRAM 调试
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

#### 1.2 GTT 调试
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

#### 2.1 VRAM 统计
- **总 VRAM 大小**：设备的总 VRAM 容量
- **已使用 VRAM**：当前已分配的 VRAM 大小
- **可见 VRAM 使用量**：实际可见的 VRAM 使用量
- **碎片信息**：通过 DRM Buddy 提供的碎片分析

#### 2.2 GTT 统计
- **总 GTT 大小**：设备的总 GTT 容量
- **已使用 GTT**：当前已分配的 GTT 大小
- **地址空间使用**：GTT 地址空间的使用情况
- **碎片信息**：通过 DRM MM 提供的碎片分析

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

这种设计既保持了与 TTM 框架的兼容性，又提供了高度的可扩展性，允许用户根据具体需求实现自定义的内存管理策略。
