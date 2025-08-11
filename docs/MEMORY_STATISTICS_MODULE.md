# PDDGPU 内存统计模块

## 概述

PDDGPU 内存统计模块提供全面的内存管理和监控功能，包括内存使用统计、性能监控和内存泄漏检测。该模块通过宏控制的内存泄漏监控进程，当内存泄漏超过100M时自动打印告警信息。

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

### 4. 内存泄漏监控进程
- 通过宏 `PDDGPU_MEMORY_LEAK_MONITOR_ENABLED` 控制
- 后台工作队列定期检查
- 超过100M阈值时自动告警

## 调用关系图

```
用户空间应用
    ↓
pddgpu_gem_create_ioctl()
    ↓
pddgpu_bo_create()
    ↓
pddgpu_memory_stats_alloc_start() → 记录分配开始时间
    ↓
ttm_bo_init_reserved()
    ↓
pddgpu_vram_mgr_alloc() / pddgpu_gtt_mgr_alloc()
    ↓
pddgpu_memory_stats_alloc_end() → 更新统计信息
    ↓
pddgpu_memory_stats_add_leak_object() → 添加到泄漏检测列表

BO销毁流程:
pddgpu_bo_destroy()
    ↓
pddgpu_memory_stats_free_start() → 记录释放开始时间
    ↓
ttm_bo_validate() / ttm_bo_move()
    ↓
pddgpu_memory_stats_free_end() → 更新统计信息
    ↓
pddgpu_memory_stats_remove_leak_object() → 从泄漏检测列表移除

内存移动流程:
pddgpu_bo_move()
    ↓
pddgpu_memory_stats_move_start() → 记录移动开始时间
    ↓
ttm_bo_move_memcpy()
    ↓
pddgpu_memory_stats_move_end() → 更新移动统计

泄漏监控进程:
pddgpu_memory_stats_init() → 初始化监控工作队列
    ↓
schedule_delayed_work() → 启动后台监控
    ↓
pddgpu_memory_leak_monitor_work() → 定期检查泄漏
    ↓
pddgpu_memory_stats_leak_check() → 检测可疑/确认泄漏
    ↓
pddgpu_memory_stats_leak_report() → 生成泄漏报告
```

## 数据结构

### 内存泄漏对象
```c
struct pddgpu_memory_leak_object {
    struct list_head list;
    struct pddgpu_bo *bo;
    u64 allocation_time;
    u64 size;
    u32 domain;
    u32 flags;
    char caller_info[64];
    pid_t pid;
    u64 timestamp;
};
```

### 内存统计信息
```c
struct pddgpu_memory_stats_info {
    u64 vram_total;
    u64 vram_used;
    u64 vram_free;
    u64 gtt_total;
    u64 gtt_used;
    u64 gtt_free;
    u64 total_allocations;
    u64 total_deallocations;
    u64 leak_suspicious;
    u64 leak_confirmed;
    u64 avg_allocation_time;
    u64 avg_deallocation_time;
    u64 avg_move_time;
};
```

### 设备内存统计结构
```c
struct pddgpu_device {
    // ... 其他成员 ...
    struct {
        /* 内存使用统计 */
        atomic64_t vram_allocated;
        atomic64_t vram_freed;
        atomic64_t gtt_allocated;
        atomic64_t gtt_freed;
        atomic64_t total_allocations;
        atomic64_t total_deallocations;
        
        /* 内存泄漏检测 */
        struct {
            spinlock_t lock;
            struct list_head allocated_objects;
            atomic64_t leak_suspicious_count;
            atomic64_t leak_confirmed_count;
            u64 last_check_time;
            u64 check_interval;
        } leak_detector;
        
        /* 性能统计 */
        struct {
            atomic64_t allocation_time_total;
            atomic64_t allocation_count;
            atomic64_t deallocation_time_total;
            atomic64_t deallocation_count;
            atomic64_t move_operations;
            atomic64_t move_time_total;
        } performance;
        
        /* 调试统计 */
        struct {
            atomic64_t debug_allocations;
            atomic64_t debug_deallocations;
            atomic64_t debug_moves;
            atomic64_t debug_evictions;
        } debug;
        
#if PDDGPU_MEMORY_LEAK_MONITOR_ENABLED
        /* 内存泄漏监控工作队列 */
        struct {
            struct delayed_work leak_monitor_work;
            u64 last_leak_report_time;
            u64 leak_threshold;
            bool monitor_enabled;
        } leak_monitor;
#endif
    } memory_stats;
};
```

## API 接口

### 初始化接口
```c
/* 内存统计模块初始化 */
int pddgpu_memory_stats_init(struct pddgpu_device *pdev);
/* 内存统计模块清理 */
void pddgpu_memory_stats_fini(struct pddgpu_device *pdev);
```

### 统计函数
```c
/* 内存分配统计 */
void pddgpu_memory_stats_alloc_start(struct pddgpu_device *pdev, 
                                     struct pddgpu_bo *bo, u64 size, u32 domain);
void pddgpu_memory_stats_alloc_end(struct pddgpu_device *pdev, 
                                   struct pddgpu_bo *bo, int result);

/* 内存释放统计 */
void pddgpu_memory_stats_free_start(struct pddgpu_device *pdev, 
                                    struct pddgpu_bo *bo);
void pddgpu_memory_stats_free_end(struct pddgpu_device *pdev, 
                                  struct pddgpu_bo *bo);

/* 内存移动统计 */
void pddgpu_memory_stats_move_start(struct pddgpu_device *pdev, 
                                    struct pddgpu_bo *bo);
void pddgpu_memory_stats_move_end(struct pddgpu_device *pdev, 
                                  struct pddgpu_bo *bo);
```

### 泄漏检测接口
```c
/* 内存泄漏检测 */
void pddgpu_memory_stats_leak_check(struct pddgpu_device *pdev);
/* 内存泄漏报告 */
void pddgpu_memory_stats_leak_report(struct pddgpu_device *pdev);
/* 内存泄漏监控工作函数 */
void pddgpu_memory_leak_monitor_work(struct work_struct *work);
```

### 信息获取接口
```c
/* 获取内存统计信息 */
void pddgpu_memory_stats_get_info(struct pddgpu_device *pdev, 
                                  struct pddgpu_memory_stats_info *info);
/* 调试打印 */
void pddgpu_memory_stats_debug_print(struct pddgpu_device *pdev);
```

## 集成方式

### 1. 设备初始化集成
```c
int pddgpu_device_init(struct pddgpu_device *pdev)
{
    int ret;
    
    /* 初始化内存统计模块 */
    ret = pddgpu_memory_stats_init(pdev);
    if (ret) {
        PDDGPU_ERROR("Failed to initialize memory statistics module\n");
        goto err_unmap_mmio;
    }
    
    /* 其他初始化... */
    return 0;

err_memory_stats_fini:
    pddgpu_memory_stats_fini(pdev);
err_unmap_mmio:
    pci_iounmap(pci_dev, pdev->rmmio);
    return ret;
}
```

### 2. BO 创建集成
```c
int pddgpu_bo_create(struct pddgpu_device *pdev, struct pddgpu_bo_param *bp, 
                     struct pddgpu_bo **bo_ptr)
{
    int ret;
    
    /* 开始内存分配统计 */
    pddgpu_memory_stats_alloc_start(pdev, NULL, bp->size, bp->domain);
    
    /* 验证大小和域 */
    if (!pddgpu_bo_validate_size(pdev, bp->size, bp->domain)) {
        pddgpu_memory_stats_alloc_end(pdev, NULL, -ENOMEM);
        return -ENOMEM;
    }
    
    /* BO 创建逻辑... */
    ret = ttm_bo_init_reserved(&pdev->mman.bdev, &bo->tbo, bp->type,
                               &bo->placement, page_align, &ctx, NULL,
                               bp->resv, bp->destroy);
    if (unlikely(ret != 0)) {
        pddgpu_memory_stats_alloc_end(pdev, bo, ret);
        return ret;
    }
    
    /* 完成内存分配统计 */
    pddgpu_memory_stats_alloc_end(pdev, bo, 0);
    
    *bo_ptr = bo;
    return 0;
}
```

### 3. BO 销毁集成
```c
void pddgpu_bo_destroy(struct ttm_buffer_object *tbo)
{
    struct pddgpu_bo *bo = to_pddgpu_bo(tbo);
    struct pddgpu_device *pdev = pddgpu_ttm_pdev(tbo->bdev);
    
    PDDGPU_DEBUG("Destroying BO: %p\n", bo);
    
    /* 开始内存释放统计 */
    pddgpu_memory_stats_free_start(pdev, bo);
    
    /* 清理映射 */
    if (bo->kmap.bo)
        ttm_bo_kunmap(&bo->kmap);
    
    /* 清理通知器 */
#ifdef CONFIG_MMU_NOTIFIER
    if (bo->notifier.ops)
        mmu_interval_notifier_remove(&bo->notifier);
#endif
    
    /* 完成内存释放统计 */
    pddgpu_memory_stats_free_end(pdev, bo);
    
    /* 释放BO结构 */
    kvfree(bo);
}
```

### 4. 内存移动集成
```c
static int pddgpu_bo_move(struct ttm_buffer_object *bo, bool evict,
                          struct ttm_operation_ctx *ctx,
                          struct ttm_resource *new_mem,
                          struct ttm_place *hop)
{
    struct pddgpu_bo *abo = to_pddgpu_bo(bo);
    struct pddgpu_device *pdev = pddgpu_ttm_pdev(bo->bdev);
    int ret;
    
    PDDGPU_DEBUG("Moving BO: size=%lu, new_mem=%p\n", bo->base.size, new_mem);
    
    /* 开始内存移动统计 */
    pddgpu_memory_stats_move_start(pdev, abo);
    
    ret = ttm_bo_move_memcpy(bo, evict, ctx, new_mem);
    if (ret) {
        PDDGPU_ERROR("Failed to move BO: %d\n", ret);
        return ret;
    }
    
    /* 完成内存移动统计 */
    pddgpu_memory_stats_move_end(pdev, abo);
    
    /* 更新BO信息 */
    abo->domain = new_mem->mem_type;
    abo->size = bo->base.size;
    
    return 0;
}
```

## 内存泄漏监控进程

### 配置宏
```c
/* 内存泄漏监控宏 */
#define PDDGPU_MEMORY_LEAK_MONITOR_ENABLED 1
#define PDDGPU_MEMORY_LEAK_THRESHOLD (100 * 1024 * 1024) /* 100MB */
```

### 监控进程实现
```c
void pddgpu_memory_leak_monitor_work(struct work_struct *work)
{
    struct pddgpu_device *pdev = container_of(work, struct pddgpu_device,
                                             memory_stats.leak_monitor.leak_monitor_work.work);
    struct pddgpu_memory_stats_info info;
    u64 current_time = ktime_get_ns();
    
    /* 获取当前内存统计信息 */
    pddgpu_memory_stats_get_info(pdev, &info);
    
    /* 检查是否超过泄漏阈值 */
    if (info.vram_used + info.gtt_used > pdev->memory_stats.leak_monitor.leak_threshold) {
        PDDGPU_ERROR("Memory leak detected! Total used: %llu MB\n", 
                     (info.vram_used + info.gtt_used) >> 20);
        
        /* 生成详细泄漏报告 */
        pddgpu_memory_stats_leak_report(pdev);
        
        pdev->memory_stats.leak_monitor.last_leak_report_time = current_time;
    }
    
    /* 执行常规泄漏检测 */
    pddgpu_memory_stats_leak_check(pdev);
    
    /* 重新调度工作队列 */
    if (pdev->memory_stats.leak_monitor.monitor_enabled) {
        schedule_delayed_work(&pdev->memory_stats.leak_monitor.leak_monitor_work,
                             msecs_to_jiffies(5000)); /* 5秒间隔 */
    }
}
```

### 初始化监控进程
```c
int pddgpu_memory_stats_init(struct pddgpu_device *pdev)
{
    // ... 现有初始化代码 ...
    
#if PDDGPU_MEMORY_LEAK_MONITOR_ENABLED
    /* 初始化内存泄漏监控 */
    INIT_DELAYED_WORK(&pdev->memory_stats.leak_monitor.leak_monitor_work,
                      pddgpu_memory_leak_monitor_work);
    pdev->memory_stats.leak_monitor.last_leak_report_time = 0;
    pdev->memory_stats.leak_monitor.leak_threshold = PDDGPU_MEMORY_LEAK_THRESHOLD;
    pdev->memory_stats.leak_monitor.monitor_enabled = true;
    
    /* 启动监控进程 */
    schedule_delayed_work(&pdev->memory_stats.leak_monitor.leak_monitor_work,
                         msecs_to_jiffies(5000));
    
    PDDGPU_DEBUG("Memory leak monitor started\n");
#endif
    
    return 0;
}
```

## 使用示例

### 获取统计信息
```c
struct pddgpu_memory_stats_info info;
pddgpu_memory_stats_get_info(pdev, &info);

PDDGPU_INFO("VRAM: Used=%llu MB, Free=%llu MB\n",
            info.vram_used >> 20, info.vram_free >> 20);
PDDGPU_INFO("GTT:  Used=%llu MB, Free=%llu MB\n",
            info.gtt_used >> 20, info.gtt_free >> 20);
PDDGPU_INFO("Operations: Alloc=%llu, Dealloc=%llu\n",
            info.total_allocations, info.total_deallocations);
PDDGPU_INFO("Performance: Avg_Alloc=%llu ns, Avg_Dealloc=%llu ns\n",
            info.avg_allocation_time, info.avg_deallocation_time);
PDDGPU_INFO("Leaks: Suspicious=%llu, Confirmed=%llu\n",
            info.leak_suspicious, info.leak_confirmed);
```

### 手动泄漏检测
```c
/* 定期检查 */
pddgpu_memory_stats_leak_check(pdev);

/* 生成报告 */
pddgpu_memory_stats_leak_report(pdev);

/* 调试打印 */
pddgpu_memory_stats_debug_print(pdev);
```

### 控制监控进程
```c
/* 启用/禁用监控 */
pdev->memory_stats.leak_monitor.monitor_enabled = true/false;

/* 设置泄漏阈值 */
pdev->memory_stats.leak_monitor.leak_threshold = 200 * 1024 * 1024; /* 200MB */

/* 手动触发监控检查 */
schedule_delayed_work(&pdev->memory_stats.leak_monitor.leak_monitor_work, 0);
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
5. **后台进程**：通过工作队列实现非阻塞的泄漏监控
6. **阈值告警**：自动检测超过100M的内存泄漏并告警

## 调试支持

### 调试宏
```c
#define PDDGPU_DEBUG(fmt, ...) pr_debug("PDDGPU: " fmt, ##__VA_ARGS__)
#define PDDGPU_INFO(fmt, ...)  pr_info("PDDGPU: " fmt, ##__VA_ARGS__)
#define PDDGPU_ERROR(fmt, ...) pr_err("PDDGPU: " fmt, ##__VA_ARGS__)
```

### 调试接口
```c
/* 打印详细调试信息 */
pddgpu_memory_stats_debug_print(pdev);

/* 重置所有统计 */
pddgpu_memory_stats_reset(pdev);

/* 获取统计信息 */
struct pddgpu_memory_stats_info info;
pddgpu_memory_stats_get_info(pdev, &info);
```

## 总结

内存统计模块为 PDDGPU 驱动提供了强大的内存管理监控能力，包括：

- **实时监控**：内存使用情况和性能统计
- **泄漏检测**：自动检测和报告内存泄漏
- **后台监控**：通过宏控制的后台进程持续监控
- **阈值告警**：当内存泄漏超过100M时自动告警
- **性能分析**：详细的操作时间统计
- **调试支持**：丰富的调试接口和日志输出

该模块帮助开发者：
- 监控内存使用情况
- 检测内存泄漏
- 分析性能瓶颈
- 优化内存管理策略
- 及时发现和处理内存问题
