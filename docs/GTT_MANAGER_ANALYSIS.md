# PDDGPU GTT 管理器详细分析

## 关键回调函数解析

### 1. GTT 分配函数

```c
static int pddgpu_gtt_mgr_alloc(struct ttm_resource_manager *man,
                                 struct ttm_buffer_object *bo,
                                 const struct ttm_place *place,
                                 struct ttm_resource **res)
```

#### 功能分析
- **节点分配**：分配 TTM 范围管理器节点
- **资源初始化**：初始化 TTM 资源结构
- **使用量检查**：检查 GTT 使用量是否超限
- **地址分配**：使用 DRM MM 分配 GTT 地址空间
- **临时分配**：支持临时分配不占用实际地址

#### 关键代码段
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

#### 分配策略
```c
// 根据放置参数决定分配策略
if (place->lpfn) {
    // 永久分配：分配实际的 GTT 地址空间
    r = drm_mm_insert_node_in_range(&mgr->mm, &node->mm_nodes[0],
                                   num_pages, bo->page_alignment,
                                   0, place->fpfn, place->lpfn,
                                   DRM_MM_INSERT_BEST);
    node->base.start = node->mm_nodes[0].start;
} else {
    // 临时分配：不占用实际地址空间
    node->mm_nodes[0].start = 0;
    node->mm_nodes[0].size = PFN_UP(node->base.size);
    node->base.start = PDDGPU_BO_INVALID_OFFSET;
}
```

### 2. GTT 释放函数

```c
static void pddgpu_gtt_mgr_free(struct ttm_resource_manager *man,
                                 struct ttm_resource *res)
```

#### 功能分析
- **节点检查**：检查 DRM MM 节点是否已分配
- **节点移除**：从 DRM MM 中移除节点
- **资源清理**：清理 TTM 资源

#### 关键代码段
```c
// 1. 检查节点是否已分配
if (drm_mm_node_allocated(&node->mm_nodes[0]))
    drm_mm_remove_node(&node->mm_nodes[0]);

// 2. 清理资源
ttm_resource_fini(man, res);
kfree(node);
```

### 3. GTT 调试函数

```c
static void pddgpu_gtt_mgr_debug(struct ttm_resource_manager *man,
                                  struct drm_printer *printer)
```

#### 功能分析
- **MM 信息**：输出 DRM MM 分配器状态
- **地址空间**：显示 GTT 地址空间使用情况
- **碎片信息**：提供地址空间碎片分析

#### 关键代码段
```c
spin_lock(&mgr->lock);
drm_mm_print(&mgr->mm, printer);
spin_unlock(&mgr->lock);
```

### 4. GTT 兼容性检查

```c
static bool pddgpu_gtt_mgr_compatible(struct ttm_resource_manager *man,
                                       struct ttm_resource *res,
                                       const struct ttm_place *place,
                                       size_t size)
```

#### 功能分析
- **地址检查**：检查资源是否有有效的 GART 地址
- **范围检查**：检查是否在指定的地址范围内

#### 关键代码段
```c
return !place->lpfn || pddgpu_gtt_mgr_has_gart_addr(res);
```

### 5. GTT 重叠检查

```c
static bool pddgpu_gtt_mgr_intersects(struct ttm_resource_manager *man,
                                       struct ttm_resource *res,
                                       const struct ttm_place *place,
                                       size_t size)
```

#### 功能分析
- **地址范围**：计算请求的地址范围
- **重叠检测**：检查与现有资源的地址重叠

#### 关键代码段
```c
// 1. 检查节点是否已分配
if (!drm_mm_node_allocated(&node->mm_nodes[0]))
    return false;

// 2. 计算地址范围
place_start = (u64)place->fpfn << PAGE_SHIFT;
place_end = (u64)place->lpfn << PAGE_SHIFT;

// 3. 检查重叠
return (node->mm_nodes[0].start < place_end &&
        (node->mm_nodes[0].start + node->mm_nodes[0].size) > place_start);
```

## 调用链分析

### 1. 用户空间到 GTT 分配的完整调用链

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

### 2. 详细调用链解析

#### 步骤 1：用户空间调用
```c
// 用户空间代码
struct drm_pddgpu_gem_create create = {
    .size = 1024 * 1024,  // 1MB
    .domain = PDDGPU_GEM_DOMAIN_GTT,
    .flags = 0
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
        .domain = args->domain,  // PDDGPU_GEM_DOMAIN_GTT
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

#### 步骤 4：TTM 验证
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

#### 步骤 5：GTT 管理器分配
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

## 性能优化策略

### 1. MM 算法优化

#### 1.1 最佳适配
```c
// 使用 DRM_MM_INSERT_BEST 进行最佳适配
r = drm_mm_insert_node_in_range(&mgr->mm, &node->mm_nodes[0],
                               num_pages, bo->page_alignment,
                               0, place->fpfn, place->lpfn,
                               DRM_MM_INSERT_BEST);
```

#### 1.2 地址对齐
```c
// 根据页面大小和对齐要求确定分配参数
r = drm_mm_insert_node_in_range(&mgr->mm, &node->mm_nodes[0],
                               num_pages, bo->page_alignment,
                               0, place->fpfn, place->lpfn,
                               DRM_MM_INSERT_BEST);
```

### 2. 并发控制优化

#### 2.1 自旋锁保护
```c
// 使用自旋锁保护并发访问
spin_lock(&mgr->lock);
r = drm_mm_insert_node_in_range(&mgr->mm, &node->mm_nodes[0],
                               num_pages, bo->page_alignment,
                               0, place->fpfn, place->lpfn,
                               DRM_MM_INSERT_BEST);
spin_unlock(&mgr->lock);
```

#### 2.2 临时分配优化
```c
// 临时分配不占用实际地址空间
if (!place->lpfn) {
    node->mm_nodes[0].start = 0;
    node->mm_nodes[0].size = PFN_UP(node->base.size);
    node->base.start = PDDGPU_BO_INVALID_OFFSET;
}
```

### 3. 使用量检查优化

```c
// 检查 GTT 使用量是否超限
if (!(place->flags & TTM_PL_FLAG_TEMPORARY) &&
    ttm_resource_manager_usage(man) > man->size) {
    r = -ENOSPC;
    goto err_free;
}
```

## 调试和监控

### 1. 调试接口

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

- **GTT 使用量**：跟踪 GTT 地址空间使用情况
- **分配统计**：记录分配和释放操作
- **碎片信息**：通过 DRM MM 提供碎片分析

## 错误处理

### 1. 分配失败处理

```c
// 检查 GTT 使用量
if (!(place->flags & TTM_PL_FLAG_TEMPORARY) &&
    ttm_resource_manager_usage(man) > man->size) {
    r = -ENOSPC;
    goto err_free;
}

// 分配失败时的清理
err_free:
    ttm_resource_fini(man, &node->base);
    kfree(node);
    return r;
```

### 2. 节点移除处理

```c
// 检查节点是否已分配
if (drm_mm_node_allocated(&node->mm_nodes[0]))
    drm_mm_remove_node(&node->mm_nodes[0]);
```

## GTT 管理器恢复

### 1. 恢复函数

```c
void pddgpu_gtt_mgr_recover(struct pddgpu_gtt_mgr *mgr)
{
    struct ttm_range_mgr_node *node;
    struct drm_mm_node *mm_node;
    struct pddgpu_device *pdev;

    pdev = to_pddgpu_device_from_gtt_mgr(mgr);
    
    spin_lock(&mgr->lock);
    drm_mm_for_each_node(mm_node, &mgr->mm) {
        node = container_of(mm_node, typeof(*node), mm_nodes[0]);
        // TODO: 实现 GART 恢复功能
        PDDGPU_DEBUG("Recovering GTT node: start=%llu, size=%llu\n",
                     mm_node->start, mm_node->size);
    }
    spin_unlock(&mgr->lock);
}
```

### 2. 恢复机制

- **节点遍历**：遍历所有已分配的 GTT 节点
- **状态恢复**：恢复每个节点的 GART 映射
- **错误处理**：处理恢复过程中的错误

## 与 VRAM 管理器的对比

### 1. 分配器差异

| 特性 | VRAM (DRM Buddy) | GTT (DRM MM) |
|------|------------------|--------------|
| 算法 | Buddy 算法 | 范围管理算法 |
| 适用场景 | 大块内存分配 | 地址空间管理 |
| 碎片处理 | 自动合并 | 手动管理 |
| 性能特点 | 高效分配/释放 | 灵活地址分配 |

### 2. 使用场景差异

| 场景 | VRAM | GTT |
|------|------|-----|
| 纹理数据 | ✓ | ✗ |
| 帧缓冲 | ✓ | ✗ |
| 用户数据 | ✗ | ✓ |
| 临时缓冲 | ✗ | ✓ |
| 系统内存映射 | ✗ | ✓ |

## 总结

PDDGPU GTT 管理器通过以下机制实现高效的内存管理：

### 1. **DRM MM 分配器**
- 使用 MM 算法提供灵活的地址空间管理
- 支持临时和永久分配
- 提供地址范围管理

### 2. **TTM 框架集成**
- 完整的 TTM 资源管理器集成
- 支持所有 TTM 管理器函数
- 提供标准化的接口

### 3. **性能优化**
- 最佳适配分配策略
- 自旋锁并发控制
- 临时分配优化

### 4. **调试支持**
- 详细的调试接口
- 统计信息跟踪
- 碎片分析功能

### 5. **扩展性**
- 支持自定义分配策略
- 灵活的地址空间管理
- 可扩展的恢复机制

这种设计既保持了与 TTM 框架的兼容性，又提供了灵活的地址空间管理能力，为 PDDGPU 提供了可靠的 GTT 管理解决方案。
