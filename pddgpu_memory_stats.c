/*
 * PDDGPU 内存统计模块实现
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <drm/drm_drv.h>
#include <drm/drm_print.h>

#include "include/pddgpu_drv.h"
#include "include/pddgpu_memory_stats.h"

/* 默认泄漏检测间隔 (毫秒) */
#define PDDGPU_DEFAULT_LEAK_CHECK_INTERVAL 5000

/* 内存泄漏监控工作函数 */
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

/* 内存统计模块初始化 */
int pddgpu_memory_stats_init(struct pddgpu_device *pdev)
{
	PDDGPU_DEBUG("Initializing memory statistics module\n");
	
	/* 初始化内存使用统计 */
	atomic64_set(&pdev->memory_stats.vram_allocated, 0);
	atomic64_set(&pdev->memory_stats.vram_freed, 0);
	atomic64_set(&pdev->memory_stats.gtt_allocated, 0);
	atomic64_set(&pdev->memory_stats.gtt_freed, 0);
	atomic64_set(&pdev->memory_stats.total_allocations, 0);
	atomic64_set(&pdev->memory_stats.total_deallocations, 0);
	
	/* 初始化泄漏检测器 */
	spin_lock_init(&pdev->memory_stats.leak_detector.lock);
	INIT_LIST_HEAD(&pdev->memory_stats.leak_detector.allocated_objects);
	atomic64_set(&pdev->memory_stats.leak_detector.leak_suspicious_count, 0);
	atomic64_set(&pdev->memory_stats.leak_detector.leak_confirmed_count, 0);
	pdev->memory_stats.leak_detector.last_check_time = ktime_get_ns();
	pdev->memory_stats.leak_detector.check_interval = PDDGPU_DEFAULT_LEAK_CHECK_INTERVAL * 1000000; /* 转换为纳秒 */
	
	/* 初始化性能统计 */
	atomic64_set(&pdev->memory_stats.performance.allocation_time_total, 0);
	atomic64_set(&pdev->memory_stats.performance.allocation_count, 0);
	atomic64_set(&pdev->memory_stats.performance.deallocation_time_total, 0);
	atomic64_set(&pdev->memory_stats.performance.deallocation_count, 0);
	atomic64_set(&pdev->memory_stats.performance.move_operations, 0);
	atomic64_set(&pdev->memory_stats.performance.move_time_total, 0);
	
	/* 初始化调试统计 */
	atomic64_set(&pdev->memory_stats.debug.debug_allocations, 0);
	atomic64_set(&pdev->memory_stats.debug.debug_deallocations, 0);
	atomic64_set(&pdev->memory_stats.debug.debug_moves, 0);
	atomic64_set(&pdev->memory_stats.debug.debug_evictions, 0);
	
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
	
	PDDGPU_DEBUG("Memory statistics module initialized successfully\n");
	return 0;
}

/* 内存统计模块清理 */
void pddgpu_memory_stats_fini(struct pddgpu_device *pdev)
{
	struct pddgpu_memory_leak_object *leak_obj, *temp;
	unsigned long flags;
	
	PDDGPU_DEBUG("Finalizing memory statistics module\n");
	
#if PDDGPU_MEMORY_LEAK_MONITOR_ENABLED
	/* 停止监控进程 */
	pdev->memory_stats.leak_monitor.monitor_enabled = false;
	cancel_delayed_work_sync(&pdev->memory_stats.leak_monitor.leak_monitor_work);
	
	PDDGPU_DEBUG("Memory leak monitor stopped\n");
#endif
	
	/* 清理泄漏检测对象 */
	spin_lock_irqsave(&pdev->memory_stats.leak_detector.lock, flags);
	list_for_each_entry_safe(leak_obj, temp, 
	                        &pdev->memory_stats.leak_detector.allocated_objects, list) {
		list_del(&leak_obj->list);
		kfree(leak_obj);
	}
	spin_unlock_irqrestore(&pdev->memory_stats.leak_detector.lock, flags);
	
	PDDGPU_DEBUG("Memory statistics module finalized\n");
}

/* 内存分配统计开始 */
void pddgpu_memory_stats_alloc_start(struct pddgpu_device *pdev, 
                                     struct pddgpu_bo *bo, u64 size, u32 domain)
{
	ktime_t start_time = ktime_get();
	
	/* 记录分配开始时间 */
	bo->allocation_start_time = start_time;
	
	/* 更新调试统计 */
	atomic64_inc(&pdev->memory_stats.debug.debug_allocations);
	
	PDDGPU_DEBUG("Memory allocation started: size=%llu, domain=%u\n", size, domain);
}

/* 内存分配统计结束 */
void pddgpu_memory_stats_alloc_end(struct pddgpu_device *pdev, 
                                   struct pddgpu_bo *bo, int result)
{
	ktime_t end_time = ktime_get();
	ktime_t duration;
	u64 duration_ns;
	
	if (result == 0) {
		/* 分配成功 */
		duration = ktime_sub(end_time, bo->allocation_start_time);
		duration_ns = ktime_to_ns(duration);
		
		/* 更新性能统计 */
		atomic64_add(duration_ns, &pdev->memory_stats.performance.allocation_time_total);
		atomic64_inc(&pdev->memory_stats.performance.allocation_count);
		
		/* 更新内存使用统计 */
		if (bo->tbo.resource) {
			u32 domain = bo->tbo.resource->mem_type;
			u64 size = bo->tbo.base.size;
			
			if (domain == TTM_PL_VRAM) {
				atomic64_add(size, &pdev->memory_stats.vram_allocated);
			} else if (domain == TTM_PL_TT) {
				atomic64_add(size, &pdev->memory_stats.gtt_allocated);
			}
		}
		
		atomic64_inc(&pdev->memory_stats.total_allocations);
		
		/* 添加到泄漏检测 */
		pddgpu_memory_stats_add_leak_object(pdev, bo);
		
		PDDGPU_DEBUG("Memory allocation completed: size=%llu, duration=%llu ns\n", 
		             bo->tbo.base.size, duration_ns);
	} else {
		PDDGPU_DEBUG("Memory allocation failed: result=%d\n", result);
	}
}

/* 内存释放统计开始 */
void pddgpu_memory_stats_free_start(struct pddgpu_device *pdev, 
                                    struct pddgpu_bo *bo)
{
	ktime_t start_time = ktime_get();
	
	/* 记录释放开始时间 */
	bo->deallocation_start_time = start_time;
	
	/* 更新调试统计 */
	atomic64_inc(&pdev->memory_stats.debug.debug_deallocations);
	
	PDDGPU_DEBUG("Memory deallocation started: size=%llu\n", bo->tbo.base.size);
}

/* 内存释放统计结束 */
void pddgpu_memory_stats_free_end(struct pddgpu_device *pdev, 
                                  struct pddgpu_bo *bo)
{
	ktime_t end_time = ktime_get();
	ktime_t duration;
	u64 duration_ns;
	
	duration = ktime_sub(end_time, bo->deallocation_start_time);
	duration_ns = ktime_to_ns(duration);
	
	/* 更新性能统计 */
	atomic64_add(duration_ns, &pdev->memory_stats.performance.deallocation_time_total);
	atomic64_inc(&pdev->memory_stats.performance.deallocation_count);
	
	/* 更新内存使用统计 */
	if (bo->tbo.resource) {
		u32 domain = bo->tbo.resource->mem_type;
		u64 size = bo->tbo.base.size;
		
		if (domain == TTM_PL_VRAM) {
			atomic64_add(size, &pdev->memory_stats.vram_freed);
		} else if (domain == TTM_PL_TT) {
			atomic64_add(size, &pdev->memory_stats.gtt_freed);
		}
	}
	
	atomic64_inc(&pdev->memory_stats.total_deallocations);
	
	/* 从泄漏检测中移除 */
	pddgpu_memory_stats_remove_leak_object(pdev, bo);
	
	PDDGPU_DEBUG("Memory deallocation completed: size=%llu, duration=%llu ns\n", 
	             bo->tbo.base.size, duration_ns);
}

/* 内存移动统计开始 */
void pddgpu_memory_stats_move_start(struct pddgpu_device *pdev, 
                                    struct pddgpu_bo *bo)
{
	ktime_t start_time = ktime_get();
	
	/* 记录移动开始时间 */
	bo->move_start_time = start_time;
	
	/* 更新调试统计 */
	atomic64_inc(&pdev->memory_stats.debug.debug_moves);
	
	PDDGPU_DEBUG("Memory move started: size=%llu\n", bo->tbo.base.size);
}

/* 内存移动统计结束 */
void pddgpu_memory_stats_move_end(struct pddgpu_device *pdev, 
                                  struct pddgpu_bo *bo)
{
	ktime_t end_time = ktime_get();
	ktime_t duration;
	u64 duration_ns;
	
	duration = ktime_sub(end_time, bo->move_start_time);
	duration_ns = ktime_to_ns(duration);
	
	/* 更新性能统计 */
	atomic64_add(duration_ns, &pdev->memory_stats.performance.move_time_total);
	atomic64_inc(&pdev->memory_stats.performance.move_operations);
	
	PDDGPU_DEBUG("Memory move completed: size=%llu, duration=%llu ns\n", 
	             bo->tbo.base.size, duration_ns);
}

/* 内存泄漏检测 */
void pddgpu_memory_stats_leak_check(struct pddgpu_device *pdev)
{
	struct pddgpu_memory_leak_object *leak_obj, *temp;
	unsigned long flags;
	u64 current_time = ktime_get_ns();
	u64 check_interval = pdev->memory_stats.leak_detector.check_interval;
	
	/* 检查是否需要执行泄漏检测 */
	if (current_time - pdev->memory_stats.leak_detector.last_check_time < check_interval) {
		return;
	}
	
	spin_lock_irqsave(&pdev->memory_stats.leak_detector.lock, flags);
	
	/* 检查长时间未释放的对象 */
	list_for_each_entry_safe(leak_obj, temp, 
	                        &pdev->memory_stats.leak_detector.allocated_objects, list) {
		u64 age = current_time - leak_obj->allocation_time;
		
		/* 如果对象存在超过30秒，标记为可疑泄漏 */
		if (age > 30 * 1000000000ULL) { /* 30秒 */
			atomic64_inc(&pdev->memory_stats.leak_detector.leak_suspicious_count);
			
			PDDGPU_DEBUG("Suspicious memory leak detected: size=%llu, age=%llu ns, pid=%d\n",
			             leak_obj->size, age, leak_obj->pid);
		}
		
		/* 如果对象存在超过5分钟，标记为确认泄漏 */
		if (age > 5 * 60 * 1000000000ULL) { /* 5分钟 */
			atomic64_inc(&pdev->memory_stats.leak_detector.leak_confirmed_count);
			
			PDDGPU_ERROR("Confirmed memory leak detected: size=%llu, age=%llu ns, pid=%d\n",
			             leak_obj->size, age, leak_obj->pid);
		}
	}
	
	pdev->memory_stats.leak_detector.last_check_time = current_time;
	spin_unlock_irqrestore(&pdev->memory_stats.leak_detector.lock, flags);
}

/* 内存泄漏报告 */
void pddgpu_memory_stats_leak_report(struct pddgpu_device *pdev)
{
	struct pddgpu_memory_leak_object *leak_obj;
	unsigned long flags;
	u64 suspicious_count, confirmed_count;
	u64 total_leaked_size = 0;
	int leak_count = 0;
	
	suspicious_count = atomic64_read(&pdev->memory_stats.leak_detector.leak_suspicious_count);
	confirmed_count = atomic64_read(&pdev->memory_stats.leak_detector.leak_confirmed_count);
	
	spin_lock_irqsave(&pdev->memory_stats.leak_detector.lock, flags);
	
	list_for_each_entry(leak_obj, &pdev->memory_stats.leak_detector.allocated_objects, list) {
		total_leaked_size += leak_obj->size;
		leak_count++;
	}
	
	spin_unlock_irqrestore(&pdev->memory_stats.leak_detector.lock, flags);
	
	PDDGPU_INFO("Memory leak report:\n");
	PDDGPU_INFO("  Total allocated objects: %d\n", leak_count);
	PDDGPU_INFO("  Total leaked size: %llu bytes\n", total_leaked_size);
	PDDGPU_INFO("  Suspicious leaks: %llu\n", suspicious_count);
	PDDGPU_INFO("  Confirmed leaks: %llu\n", confirmed_count);
}

/* 获取内存统计信息 */
void pddgpu_memory_stats_get_info(struct pddgpu_device *pdev, 
                                  struct pddgpu_memory_stats_info *info)
{
	u64 vram_allocated, vram_freed, gtt_allocated, gtt_freed;
	u64 allocation_count, deallocation_count;
	u64 allocation_time_total, deallocation_time_total, move_time_total;
	u64 move_operations;
	
	/* 获取内存使用统计 */
	vram_allocated = atomic64_read(&pdev->memory_stats.vram_allocated);
	vram_freed = atomic64_read(&pdev->memory_stats.vram_freed);
	gtt_allocated = atomic64_read(&pdev->memory_stats.gtt_allocated);
	gtt_freed = atomic64_read(&pdev->memory_stats.gtt_freed);
	
	/* 获取性能统计 */
	allocation_count = atomic64_read(&pdev->memory_stats.performance.allocation_count);
	deallocation_count = atomic64_read(&pdev->memory_stats.performance.deallocation_count);
	allocation_time_total = atomic64_read(&pdev->memory_stats.performance.allocation_time_total);
	deallocation_time_total = atomic64_read(&pdev->memory_stats.performance.deallocation_time_total);
	move_time_total = atomic64_read(&pdev->memory_stats.performance.move_time_total);
	move_operations = atomic64_read(&pdev->memory_stats.performance.move_operations);
	
	/* 填充统计信息 */
	info->vram_total = pdev->vram_size;
	info->vram_used = vram_allocated - vram_freed;
	info->vram_free = pdev->vram_size - info->vram_used;
	info->gtt_total = pdev->gtt_size;
	info->gtt_used = gtt_allocated - gtt_freed;
	info->gtt_free = pdev->gtt_size - info->gtt_used;
	info->total_allocations = atomic64_read(&pdev->memory_stats.total_allocations);
	info->total_deallocations = atomic64_read(&pdev->memory_stats.total_deallocations);
	info->leak_suspicious = atomic64_read(&pdev->memory_stats.leak_detector.leak_suspicious_count);
	info->leak_confirmed = atomic64_read(&pdev->memory_stats.leak_detector.leak_confirmed_count);
	
	/* 计算平均时间 */
	info->avg_allocation_time = allocation_count > 0 ? allocation_time_total / allocation_count : 0;
	info->avg_deallocation_time = deallocation_count > 0 ? deallocation_time_total / deallocation_count : 0;
	info->avg_move_time = move_operations > 0 ? move_time_total / move_operations : 0;
}

/* 调试打印 */
void pddgpu_memory_stats_debug_print(struct pddgpu_device *pdev)
{
	struct pddgpu_memory_stats_info info;
	
	pddgpu_memory_stats_get_info(pdev, &info);
	
	PDDGPU_INFO("=== PDDGPU Memory Statistics ===\n");
	PDDGPU_INFO("VRAM: Total=%llu MB, Used=%llu MB, Free=%llu MB\n",
	             info.vram_total >> 20, info.vram_used >> 20, info.vram_free >> 20);
	PDDGPU_INFO("GTT:  Total=%llu MB, Used=%llu MB, Free=%llu MB\n",
	             info.gtt_total >> 20, info.gtt_used >> 20, info.gtt_free >> 20);
	PDDGPU_INFO("Operations: Allocations=%llu, Deallocations=%llu\n",
	             info.total_allocations, info.total_deallocations);
	PDDGPU_INFO("Performance: Avg_Alloc=%llu ns, Avg_Dealloc=%llu ns, Avg_Move=%llu ns\n",
	             info.avg_allocation_time, info.avg_deallocation_time, info.avg_move_time);
	PDDGPU_INFO("Leaks: Suspicious=%llu, Confirmed=%llu\n",
	             info.leak_suspicious, info.leak_confirmed);
	PDDGPU_INFO("================================\n");
}

/* 重置统计 */
void pddgpu_memory_stats_reset(struct pddgpu_device *pdev)
{
	PDDGPU_DEBUG("Resetting memory statistics\n");
	
	/* 重置内存使用统计 */
	atomic64_set(&pdev->memory_stats.vram_allocated, 0);
	atomic64_set(&pdev->memory_stats.vram_freed, 0);
	atomic64_set(&pdev->memory_stats.gtt_allocated, 0);
	atomic64_set(&pdev->memory_stats.gtt_freed, 0);
	atomic64_set(&pdev->memory_stats.total_allocations, 0);
	atomic64_set(&pdev->memory_stats.total_deallocations, 0);
	
	/* 重置性能统计 */
	atomic64_set(&pdev->memory_stats.performance.allocation_time_total, 0);
	atomic64_set(&pdev->memory_stats.performance.allocation_count, 0);
	atomic64_set(&pdev->memory_stats.performance.deallocation_time_total, 0);
	atomic64_set(&pdev->memory_stats.performance.deallocation_count, 0);
	atomic64_set(&pdev->memory_stats.performance.move_operations, 0);
	atomic64_set(&pdev->memory_stats.performance.move_time_total, 0);
	
	/* 重置调试统计 */
	atomic64_set(&pdev->memory_stats.debug.debug_allocations, 0);
	atomic64_set(&pdev->memory_stats.debug.debug_deallocations, 0);
	atomic64_set(&pdev->memory_stats.debug.debug_moves, 0);
	atomic64_set(&pdev->memory_stats.debug.debug_evictions, 0);
	
	/* 重置泄漏检测 */
	atomic64_set(&pdev->memory_stats.leak_detector.leak_suspicious_count, 0);
	atomic64_set(&pdev->memory_stats.leak_detector.leak_confirmed_count, 0);
	
	PDDGPU_DEBUG("Memory statistics reset completed\n");
}

/* 设置泄漏检测间隔 */
void pddgpu_memory_stats_set_leak_check_interval(struct pddgpu_device *pdev, u64 interval)
{
	pdev->memory_stats.leak_detector.check_interval = interval * 1000000; /* 转换为纳秒 */
	PDDGPU_DEBUG("Leak check interval set to %llu ms\n", interval);
}

/* 获取泄漏检测间隔 */
u64 pddgpu_memory_stats_get_leak_check_interval(struct pddgpu_device *pdev)
{
	return pdev->memory_stats.leak_detector.check_interval / 1000000; /* 转换为毫秒 */
}

/* 性能统计开始 */
void pddgpu_memory_stats_performance_start(struct pddgpu_device *pdev, ktime_t *start_time)
{
	*start_time = ktime_get();
}

/* 性能统计结束 */
void pddgpu_memory_stats_performance_end(struct pddgpu_device *pdev, ktime_t start_time, 
                                        atomic64_t *time_total, atomic64_t *count)
{
	ktime_t end_time = ktime_get();
	ktime_t duration = ktime_sub(end_time, start_time);
	u64 duration_ns = ktime_to_ns(duration);
	
	atomic64_add(duration_ns, time_total);
	atomic64_inc(count);
}

/* 内存使用统计更新 */
void pddgpu_memory_stats_update_usage(struct pddgpu_device *pdev, u32 domain, u64 size, bool alloc)
{
	if (alloc) {
		if (domain == TTM_PL_VRAM) {
			atomic64_add(size, &pdev->memory_stats.vram_allocated);
		} else if (domain == TTM_PL_TT) {
			atomic64_add(size, &pdev->memory_stats.gtt_allocated);
		}
		atomic64_inc(&pdev->memory_stats.total_allocations);
	} else {
		if (domain == TTM_PL_VRAM) {
			atomic64_add(size, &pdev->memory_stats.vram_freed);
		} else if (domain == TTM_PL_TT) {
			atomic64_add(size, &pdev->memory_stats.gtt_freed);
		}
		atomic64_inc(&pdev->memory_stats.total_deallocations);
	}
}
