# PDDGPU 内存统计模块

## 概述

PDDGPU 内存统计模块提供全面的内存管理和监控功能，包括内存使用统计、性能监控和内存泄漏检测。

## 主要功能

### 1. 内存使用统计
- VRAM 分配/释放统计
- GTT 分配/释放统计
- 总体内存使用情况

### 2. 性能监控
- 内存分配平均时间
- 内存释放平均时间
- 内存移动平均时间

### 3. 内存泄漏检测
- 实时监控已分配对象
- 可疑泄漏检测（30秒）
- 确认泄漏检测（5分钟）

## API 接口

### 初始化
```c
int pddgpu_memory_stats_init(struct pddgpu_device *pdev);
void pddgpu_memory_stats_fini(struct pddgpu_device *pdev);
```

### 统计函数
```c
void pddgpu_memory_stats_alloc_start(struct pddgpu_device *pdev, 
                                     struct pddgpu_bo *bo, u64 size, u32 domain);
void pddgpu_memory_stats_alloc_end(struct pddgpu_device *pdev, 
                                   struct pddgpu_bo *bo, int result);
void pddgpu_memory_stats_free_start(struct pddgpu_device *pdev, 
                                    struct pddgpu_bo *bo);
void pddgpu_memory_stats_free_end(struct pddgpu_device *pdev, 
                                  struct pddgpu_bo *bo);
```

### 泄漏检测
```c
void pddgpu_memory_stats_leak_check(struct pddgpu_device *pdev);
void pddgpu_memory_stats_leak_report(struct pddgpu_device *pdev);
```

### 信息获取
```c
void pddgpu_memory_stats_get_info(struct pddgpu_device *pdev, 
                                  struct pddgpu_memory_stats_info *info);
void pddgpu_memory_stats_debug_print(struct pddgpu_device *pdev);
```

## 集成方式

### 1. 设备初始化
```c
int pddgpu_device_init(struct pddgpu_device *pdev)
{
    /* 初始化内存统计模块 */
    ret = pddgpu_memory_stats_init(pdev);
    if (ret) return ret;
    
    /* 其他初始化... */
    return 0;
}
```

### 2. BO 创建集成
```c
int pddgpu_bo_create(struct pddgpu_device *pdev, struct pddgpu_bo_param *bp, 
                     struct pddgpu_bo **bo_ptr)
{
    /* 开始统计 */
    pddgpu_memory_stats_alloc_start(pdev, NULL, bp->size, bp->domain);
    
    /* BO 创建逻辑... */
    
    /* 完成统计 */
    pddgpu_memory_stats_alloc_end(pdev, bo, result);
    return result;
}
```

### 3. BO 销毁集成
```c
void pddgpu_bo_destroy(struct ttm_buffer_object *tbo)
{
    struct pddgpu_bo *bo = to_pddgpu_bo(tbo);
    struct pddgpu_device *pdev = pddgpu_ttm_pdev(tbo->bdev);
    
    /* 开始统计 */
    pddgpu_memory_stats_free_start(pdev, bo);
    
    /* BO 销毁逻辑... */
    
    /* 完成统计 */
    pddgpu_memory_stats_free_end(pdev, bo);
}
```

## 使用示例

### 获取统计信息
```c
struct pddgpu_memory_stats_info info;
pddgpu_memory_stats_get_info(pdev, &info);

PDDGPU_INFO("VRAM: Used=%llu MB, Free=%llu MB\n",
            info.vram_used >> 20, info.vram_free >> 20);
PDDGPU_INFO("Operations: Alloc=%llu, Dealloc=%llu\n",
            info.total_allocations, info.total_deallocations);
```

### 泄漏检测
```c
/* 定期检查 */
pddgpu_memory_stats_leak_check(pdev);

/* 生成报告 */
pddgpu_memory_stats_leak_report(pdev);
```

## 配置选项

### 泄漏检测间隔
```c
/* 设置检测间隔（毫秒） */
pddgpu_memory_stats_set_leak_check_interval(pdev, 5000);

/* 获取当前间隔 */
u64 interval = pddgpu_memory_stats_get_leak_check_interval(pdev);
```

### 重置统计
```c
/* 重置所有统计信息 */
pddgpu_memory_stats_reset(pdev);
```

## 性能特点

1. **原子操作**：使用 atomic64_t 进行无锁统计
2. **低开销**：最小化对正常操作的性能影响
3. **可配置**：支持自定义检测间隔和调试级别
4. **实时监控**：提供实时的内存使用和泄漏检测

## 总结

内存统计模块为 PDDGPU 驱动提供了强大的内存管理监控能力，帮助开发者：
- 监控内存使用情况
- 检测内存泄漏
- 分析性能瓶颈
- 优化内存管理策略
