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
#include <linux/rcupdate.h> /* RCU保护 */
#include <linux/rwsem.h> /* 读写锁 */
#include <linux/sched.h> /* pid_t */

struct pddgpu_device;
struct pddgpu_bo;

/* 并发控制宏 */
#define PDDGPU_MEMORY_STATS_LOCK(pdev, flags) \
	spin_lock_irqsave(&(pdev)->memory_stats.leak_detector.lock, flags)

#define PDDGPU_MEMORY_STATS_UNLOCK(pdev, flags) \
	spin_unlock_irqrestore(&(pdev)->memory_stats.leak_detector.lock, flags)

#define PDDGPU_MEMORY_STATS_READ_LOCK(pdev) \
	down_read(&(pdev)->memory_stats.leak_detector_rwsem)

#define PDDGPU_MEMORY_STATS_READ_UNLOCK(pdev) \
	up_read(&(pdev)->memory_stats.leak_detector_rwsem)

#define PDDGPU_MEMORY_STATS_WRITE_LOCK(pdev) \
	down_write(&(pdev)->memory_stats.leak_detector_rwsem)

#define PDDGPU_MEMORY_STATS_WRITE_UNLOCK(pdev) \
	up_write(&(pdev)->memory_stats.leak_detector_rwsem)

/* 批量操作结构体 */
struct pddgpu_memory_stats_batch {
	u64 vram_allocated;
	u64 vram_freed;
	u64 gtt_allocated;
	u64 gtt_freed;
	u64 total_allocations;
	u64 total_deallocations;
	u64 move_operations;
	u64 move_time_total;
};

/* 无锁链表节点 */
struct pddgpu_memory_leak_object {
	struct list_head list;
	struct rcu_head rcu;  /* RCU保护 */
	struct pddgpu_bo *bo;
	u64 allocation_time;
	u64 size;
	u32 domain;
	u32 flags;
	char caller_info[64];
	pid_t pid;
	u64 timestamp;
	atomic_t ref_count; /* 引用计数 */
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

/* RCU保护的只读内存泄漏检测 */
void pddgpu_memory_stats_leak_check_rcu(struct pddgpu_device *pdev);
void pddgpu_memory_stats_leak_report_rcu(struct pddgpu_device *pdev);

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

/* 内存泄漏监控工作函数 */
void pddgpu_memory_leak_monitor_work(struct work_struct *work);

/* 内存使用统计更新 */
void pddgpu_memory_stats_update_usage(struct pddgpu_device *pdev, u32 domain, u64 size, bool alloc);

/* 批量操作接口 */
void pddgpu_memory_stats_batch_update(struct pddgpu_device *pdev,
                                     struct pddgpu_memory_stats_batch *batch);

/* 改进的错误处理接口 */
int pddgpu_memory_stats_leak_check_safe(struct pddgpu_device *pdev);
int pddgpu_memory_stats_leak_report_safe(struct pddgpu_device *pdev);

/* 无锁操作接口 */
void pddgpu_memory_stats_add_leak_object_lockfree(struct pddgpu_device *pdev, 
                                                  struct pddgpu_bo *bo);
void pddgpu_memory_stats_remove_leak_object_lockfree(struct pddgpu_device *pdev, 
                                                     struct pddgpu_bo *bo);

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
	atomic_set(&leak_obj->ref_count, 1);
	
	/* 获取调用者信息 */
	snprintf(leak_obj->caller_info, sizeof(leak_obj->caller_info), 
	         "PID:%d", current->pid);
	
	PDDGPU_MEMORY_STATS_LOCK(pdev, flags);
	list_add_tail(&leak_obj->list, &pdev->memory_stats.leak_detector.allocated_objects);
	PDDGPU_MEMORY_STATS_UNLOCK(pdev, flags);
}

static inline void pddgpu_memory_stats_remove_leak_object(struct pddgpu_device *pdev, 
                                                          struct pddgpu_bo *bo)
{
	struct pddgpu_memory_leak_object *leak_obj, *temp;
	unsigned long flags;
	
	PDDGPU_MEMORY_STATS_LOCK(pdev, flags);
	list_for_each_entry_safe(leak_obj, temp, 
	                        &pdev->memory_stats.leak_detector.allocated_objects, list) {
		if (leak_obj->bo == bo) {
			list_del(&leak_obj->list);
			kfree(leak_obj);
			break;
		}
	}
	PDDGPU_MEMORY_STATS_UNLOCK(pdev, flags);
}

/* 引用计数操作 */
static inline void pddgpu_memory_leak_object_get(struct pddgpu_memory_leak_object *leak_obj)
{
	if (leak_obj)
		atomic_inc(&leak_obj->ref_count);
}

static inline void pddgpu_memory_leak_object_put(struct pddgpu_memory_leak_object *leak_obj)
{
	if (leak_obj && atomic_dec_and_test(&leak_obj->ref_count))
		kfree(leak_obj);
}

#endif /* __PDDGPU_MEMORY_STATS_H__ */
