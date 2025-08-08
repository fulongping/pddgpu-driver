/*
 * PDDGPU 内存统计模块
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#ifndef __PDDGPU_MEMORY_STATS_H__
#define __PDDGPU_MEMORY_STATS_H__

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/atomic.h>
#include <linux/ktime.h>

struct pddgpu_device;
struct pddgpu_bo;

/* 内存泄漏检测对象 */
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

/* 内存统计信息 */
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

/* 内存统计模块初始化 */
int pddgpu_memory_stats_init(struct pddgpu_device *pdev);

/* 内存统计模块清理 */
void pddgpu_memory_stats_fini(struct pddgpu_device *pdev);

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

/* 内存泄漏检测 */
void pddgpu_memory_stats_leak_check(struct pddgpu_device *pdev);
void pddgpu_memory_stats_leak_report(struct pddgpu_device *pdev);

/* 获取内存统计信息 */
void pddgpu_memory_stats_get_info(struct pddgpu_device *pdev, 
                                  struct pddgpu_memory_stats_info *info);

/* 调试接口 */
void pddgpu_memory_stats_debug_print(struct pddgpu_device *pdev);
void pddgpu_memory_stats_reset(struct pddgpu_device *pdev);

/* 内存泄漏检测配置 */
void pddgpu_memory_stats_set_leak_check_interval(struct pddgpu_device *pdev, u64 interval);
u64 pddgpu_memory_stats_get_leak_check_interval(struct pddgpu_device *pdev);

/* 性能统计 */
void pddgpu_memory_stats_performance_start(struct pddgpu_device *pdev, ktime_t *start_time);
void pddgpu_memory_stats_performance_end(struct pddgpu_device *pdev, ktime_t start_time, 
                                        atomic64_t *time_total, atomic64_t *count);

/* 内存使用统计更新 */
void pddgpu_memory_stats_update_usage(struct pddgpu_device *pdev, u32 domain, u64 size, bool alloc);

/* 内存泄漏检测辅助函数 */
static inline void pddgpu_memory_stats_add_leak_object(struct pddgpu_device *pdev, 
                                                       struct pddgpu_bo *bo)
{
	struct pddgpu_memory_leak_object *leak_obj;
	unsigned long flags;
	
	leak_obj = kzalloc(sizeof(*leak_obj), GFP_KERNEL);
	if (!leak_obj)
		return;
	
	leak_obj->bo = bo;
	leak_obj->allocation_time = ktime_get_ns();
	leak_obj->size = bo->tbo.base.size;
	leak_obj->domain = bo->tbo.resource ? bo->tbo.resource->mem_type : 0;
	leak_obj->flags = bo->tbo.base.flags;
	leak_obj->pid = current->pid;
	leak_obj->timestamp = ktime_get_ns();
	
	/* 获取调用者信息 */
	snprintf(leak_obj->caller_info, sizeof(leak_obj->caller_info), 
	         "PID:%d", current->pid);
	
	spin_lock_irqsave(&pdev->memory_stats.leak_detector.lock, flags);
	list_add_tail(&leak_obj->list, &pdev->memory_stats.leak_detector.allocated_objects);
	spin_unlock_irqrestore(&pdev->memory_stats.leak_detector.lock, flags);
}

static inline void pddgpu_memory_stats_remove_leak_object(struct pddgpu_device *pdev, 
                                                          struct pddgpu_bo *bo)
{
	struct pddgpu_memory_leak_object *leak_obj, *temp;
	unsigned long flags;
	
	spin_lock_irqsave(&pdev->memory_stats.leak_detector.lock, flags);
	list_for_each_entry_safe(leak_obj, temp, 
	                        &pdev->memory_stats.leak_detector.allocated_objects, list) {
		if (leak_obj->bo == bo) {
			list_del(&leak_obj->list);
			kfree(leak_obj);
			break;
		}
	}
	spin_unlock_irqrestore(&pdev->memory_stats.leak_detector.lock, flags);
}

#endif /* __PDDGPU_MEMORY_STATS_H__ */
