# PDDGPU VRAM 管理器详细分析

## 关键回调函数解析

### 1. VRAM 分配函数

```c
static int pddgpu_vram_mgr_alloc(struct ttm_resource_manager *man,
                                  struct ttm_buffer_object *bo,
                                  const struct ttm_place *place,
                                  struct ttm_resource **res)
```

#### 功能分析
- **参数解析**：解析 TTM 放置参数，确定分配范围
- **大小计算**：根据 BO 大小计算所需页面数
- **块大小确定**：根据连续性和对齐要求确定块大小
- **DRM Buddy 分配**：使用 Buddy 算法分配内存块
- **统计更新**：更新可见内存使用统计

#### 关键代码段
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

#### 分配标志设置
```c
// 连续分配
if (pbo->flags & PDDGPU_GEM_CREATE_VRAM_CONTIGUOUS)
    vres->flags |= DRM_BUDDY_CONTIGUOUS_ALLOCATION;

// 清理分配
if (pbo->flags & PDDGPU_GEM_CREATE_VRAM_CLEARED)
    vres->flags |= DRM_BUDDY_CLEAR_ALLOCATION;

// 范围分配
if (fpfn || lpfn != mgr->mm.size)
    vres->flags |= DRM_BUDDY_RANGE_ALLOCATION;
```

### 2. VRAM 释放函数

```c
static void pddgpu_vram_mgr_free(struct ttm_resource_manager *man,
                                  struct ttm_resource *res)
```

#### 功能分析
- **统计计算**：计算要释放的可见内存使用量
- **块释放**：释放 DRM Buddy 块
- **统计更新**：更新使用统计
- **资源清理**：清理 TTM 资源

#### 关键代码段
```c
// 1. 计算可见内存使用量
list_for_each_entry(block, &vres->blocks, link)
    vis_usage += pddgpu_vram_mgr_block_size(block);

// 2. 释放 Buddy 块
drm_buddy_free_list(mm, &vres->blocks, 0);

// 3. 更新统计
atomic64_sub(vis_usage, &mgr->vis_usage);
```

### 3. VRAM 调试函数

```c
static void pddgpu_vram_mgr_debug(struct ttm_resource_manager *man,
                                   struct drm_printer *printer)
```

#### 功能分析
- **统计信息**：输出可见内存使用量
- **配置信息**：输出默认页面大小
- **Buddy 信息**：输出 DRM Buddy 分配器状态

#### 关键代码段
```c
drm_printf(printer, "  vis usage:%llu\n",
           atomic64_read(&mgr->vis_usage));

mutex_lock(&mgr->lock);
drm_printf(printer, "default_page_size: %lluKiB\n",
           mgr->default_page_size >> 10);

drm_buddy_print(mm, printer);
mutex_unlock(&mgr->lock);
```

## 调用链分析

### 1. 用户空间到 VRAM 分配的完整调用链

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

### 2. 详细调用链解析

#### 步骤 1：用户空间调用
```c
// 用户空间代码
struct drm_pddgpu_gem_create create = {
    .size = 1024 * 1024,  // 1MB
    .domain = PDDGPU_GEM_DOMAIN_VRAM,
    .flags = PDDGPU_GEM_CREATE_VRAM_CONTIGUOUS
};
drmIoctl(fd, DRM_IOCTL_PDDGPU_GEM_CREATE, &create);
```

#### 步骤 2：IOCTL 处理
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
        .domain = args->domain,  // PDDGPU_GEM_DOMAIN_VRAM
        .flags = args->flags,
        // ...
    };
    
    // 调用 BO 创建
    ret = pddgpu_bo_create(pdev, &bp, &bo);
}
```

#### 步骤 3：BO 创建
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
    
    // 对于 VRAM 域，设置 TTM_PL_VRAM 放置
    if (bp->domain == PDDGPU_GEM_DOMAIN_VRAM) {
        bo->placement.placement[0] = (struct ttm_place){
            .mem_type = TTM_PL_VRAM,
            .flags = TTM_PL_FLAG_VRAM,
        };
    }
    
    // 调用 TTM 初始化
    r = ttm_bo_init_reserved(&pdev->mman.bdev, &bo->tbo, bp->type,
                             &bo->placement, page_align, &ctx, NULL,
                             bp->resv, bp->destroy);
}
```

#### 步骤 4：TTM 初始化
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

#### 步骤 5：TTM 验证
```c
// TTM 框架内部
int ttm_bo_validate(struct ttm_buffer_object *bo,
                    struct ttm_placement *placement,
                    struct ttm_operation_ctx *ctx)
{
    // 为每个放置策略尝试分配
    for (i = 0; i < placement->num_placement; i++) {
        // 对于 VRAM，man 是 VRAM 管理器
        man = ttm_manager_type(bdev, TTM_PL_VRAM);
        ret = ttm_resource_manager_alloc(man, bo, &place, &res);
        if (ret == 0)
            break;
    }
}
```

#### 步骤 6：资源管理器分配
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

#### 步骤 7：VRAM 管理器分配
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

## 性能优化策略

### 1. Buddy 算法优化

#### 1.1 大块优先
```c
// 优先使用大块分配，减少碎片
if ((size >= (u64)pages_per_block << PAGE_SHIFT) &&
    !(size & (((u64)pages_per_block << PAGE_SHIFT) - 1)))
    min_block_size = (u64)pages_per_block << PAGE_SHIFT;
```

#### 1.2 连续分配回退
```c
// 如果连续分配失败，回退到非连续分配
if (unlikely(r == -ENOSPC) && pages_per_block == ~0ul &&
    !(place->flags & TTM_PL_FLAG_CONTIGUOUS)) {
    vres->flags &= ~DRM_BUDDY_CONTIGUOUS_ALLOCATION;
    pages_per_block = max_t(u32, 2UL << (20UL - PAGE_SHIFT),
                            bo->page_alignment);
    continue;
}
```

### 2. 内存对齐优化

```c
// 根据页面大小和对齐要求确定最小块大小
if (bo->page_alignment)
    min_block_size = (u64)bo->page_alignment << PAGE_SHIFT;
else
    min_block_size = mgr->default_page_size;
```

### 3. 统计跟踪优化

```c
// 跟踪可见内存使用量
atomic64_add(vis_usage, &mgr->vis_usage);

// 在释放时更新统计
atomic64_sub(vis_usage, &mgr->vis_usage);
```

## 调试和监控

### 1. 调试接口

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

### 2. 统计信息

- **可见内存使用量**：跟踪实际可见的 VRAM 使用量
- **分配统计**：记录分配和释放操作
- **碎片信息**：通过 DRM Buddy 提供碎片分析

## 错误处理

### 1. 分配失败处理

```c
// 检查可用空间
if (ttm_resource_manager_usage(man) > max_bytes) {
    r = -ENOSPC;
    goto error_fini;
}

// 分配失败时的清理
error_free_blocks:
    drm_buddy_free_list(mm, &vres->blocks, 0);
    mutex_unlock(&mgr->lock);
error_fini:
    ttm_resource_fini(man, &vres->base);
    kfree(vres);
    return r;
```

### 2. 内存不足处理

```c
// 连续分配失败时的回退
if (unlikely(r == -ENOSPC) && pages_per_block == ~0ul &&
    !(place->flags & TTM_PL_FLAG_CONTIGUOUS)) {
    vres->flags &= ~DRM_BUDDY_CONTIGUOUS_ALLOCATION;
    pages_per_block = max_t(u32, 2UL << (20UL - PAGE_SHIFT),
                            bo->page_alignment);
    continue;
}
```

## 总结

PDDGPU VRAM 管理器通过以下机制实现高效的内存管理：

### 1. **DRM Buddy 分配器**
- 使用 Buddy 算法提供高效的内存分配
- 支持连续和非连续内存分配
- 自动处理内存碎片

### 2. **TTM 框架集成**
- 完整的 TTM 资源管理器集成
- 支持所有 TTM 管理器函数
- 提供标准化的接口

### 3. **性能优化**
- 大块优先分配策略
- 连续分配回退机制
- 内存对齐优化

### 4. **调试支持**
- 详细的调试接口
- 统计信息跟踪
- 碎片分析功能

这种设计既保持了与 TTM 框架的兼容性，又提供了高效的内存管理能力，为 PDDGPU 提供了可靠的 VRAM 管理解决方案。
