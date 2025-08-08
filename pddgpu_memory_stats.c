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
#include <linux/workqueue.h> // Added for workqueue functions
#include <linux/rcupdate.h> // Added for RCU protection

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
	
	/* 检查设备状态 */
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		PDDGPU_DEBUG("Device is shutting down, stopping leak monitor\n");
		return;
	}
	
	/* 获取当前内存统计信息 */
	pddgpu_memory_stats_get_info(pdev, &info);
	
	/* 检查是否超过泄漏阈值 */
	if (info.vram_used + info.gtt_used > pdev->memory_stats.leak_monitor.leak_threshold) {
		PDDGPU_ERROR("Memory leak detected! Total used: %llu MB\n", 
		             (info.vram_used + info.gtt_used) >> 20);
		
		/* 生成详细泄漏报告 - 使用RCU保护版本 */
		pddgpu_memory_stats_leak_report_rcu(pdev);
		
		pdev->memory_stats.leak_monitor.last_leak_report_time = current_time;
	}
	
	/* 执行常规泄漏检测 - 使用RCU保护版本 */
	pddgpu_memory_stats_leak_check_rcu(pdev);
	
	/* 重新调度工作队列 */
	if (pdev->memory_stats.leak_monitor.monitor_enabled && 
	    !(atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		schedule_delayed_work(&pdev->memory_stats.leak_monitor.leak_monitor_work,
		                     msecs_to_jiffies(5000)); /* 5秒间隔 */
	}
}

/* 内存统计模块初始化 */
int pddgpu_memory_stats_init(struct pddgpu_device *pdev)
{
	PDDGPU_DEBUG("Initializing memory statistics module\n");
	
	/* 设置设备状态为初始化中 */
	atomic_set(&pdev->device_state, PDDGPU_DEVICE_STATE_INITIALIZING);
	
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
	
	/* 设置设备状态为就绪 */
	atomic_set(&pdev->device_state, PDDGPU_DEVICE_STATE_READY);
	
	PDDGPU_DEBUG("Memory statistics module initialized successfully\n");
	return 0;
}

/* 内存统计模块清理 */
void pddgpu_memory_stats_fini(struct pddgpu_device *pdev)
{
	struct pddgpu_memory_leak_object *leak_obj, *temp;
	unsigned long flags;
	
	PDDGPU_DEBUG("Finalizing memory statistics module\n");
	
	/* 设置设备状态为关闭中 */
	atomic_set(&pdev->device_state, PDDGPU_DEVICE_STATE_SHUTDOWN);
	
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
	ktime_t start_time;
	
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	/* 确保之前的操作完成 */
	smp_mb();
	
	start_time = ktime_get();
	
	if (bo) {
		bo->allocation_start_time = start_time;
	}
	
	atomic64_inc(&pdev->memory_stats.debug.debug_allocations);
	
	PDDGPU_DEBUG("Memory allocation started: size=%llu, domain=%u\n", size, domain);
}

/* 内存分配统计结束 */
void pddgpu_memory_stats_alloc_end(struct pddgpu_device *pdev, 
                                   struct pddgpu_bo *bo, int result)
{
	ktime_t end_time, duration;
	u64 duration_ns, size = 0;
	u32 domain = 0;
	
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	end_time = ktime_get();
	
	if (bo && result == 0) {
		duration = ktime_sub(end_time, bo->allocation_start_time);
		duration_ns = ktime_to_ns(duration);
		size = bo->tbo.base.size;
		domain = bo->tbo.resource ? bo->tbo.resource->mem_type : 0;
		
		/* 更新性能统计 */
		atomic64_add(duration_ns, &pdev->memory_stats.performance.allocation_time_total);
		atomic64_inc(&pdev->memory_stats.performance.allocation_count);
		
		/* 更新内存使用统计 */
		if (domain == TTM_PL_VRAM) {
			atomic64_add(size, &pdev->memory_stats.vram_allocated);
		} else if (domain == TTM_PL_TT) {
			atomic64_add(size, &pdev->memory_stats.gtt_allocated);
		}
		
		/* 确保统计更新对其他CPU可见 */
		smp_wmb();
		
		atomic64_inc(&pdev->memory_stats.total_allocations);
		
		/* 添加到泄漏检测列表 */
		pddgpu_memory_stats_add_leak_object(pdev, bo);
	}
	
	PDDGPU_DEBUG("Memory allocation ended: result=%d, size=%llu\n", result, size);
}

/* 内存释放统计开始 */
void pddgpu_memory_stats_free_start(struct pddgpu_device *pdev, 
                                    struct pddgpu_bo *bo)
{
	ktime_t start_time;
	
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	/* 确保之前的操作完成 */
	smp_mb();
	
	start_time = ktime_get();
	
	if (bo) {
		bo->deallocation_start_time = start_time;
	}
	
	atomic64_inc(&pdev->memory_stats.debug.debug_deallocations);
	
	PDDGPU_DEBUG("Memory deallocation started\n");
}

/* 内存释放统计结束 */
void pddgpu_memory_stats_free_end(struct pddgpu_device *pdev, 
                                  struct pddgpu_bo *bo)
{
	ktime_t end_time, duration;
	u64 duration_ns, size = 0;
	u32 domain = 0;
	
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	end_time = ktime_get();
	
	if (bo) {
		duration = ktime_sub(end_time, bo->deallocation_start_time);
		duration_ns = ktime_to_ns(duration);
		size = bo->tbo.base.size;
		domain = bo->tbo.resource ? bo->tbo.resource->mem_type : 0;
		
		/* 更新性能统计 */
		atomic64_add(duration_ns, &pdev->memory_stats.performance.deallocation_time_total);
		atomic64_inc(&pdev->memory_stats.performance.deallocation_count);
		
		/* 更新内存使用统计 */
		if (domain == TTM_PL_VRAM) {
			atomic64_add(size, &pdev->memory_stats.vram_freed);
		} else if (domain == TTM_PL_TT) {
			atomic64_add(size, &pdev->memory_stats.gtt_freed);
		}
		
		/* 确保统计更新对其他CPU可见 */
		smp_wmb();
		
		atomic64_inc(&pdev->memory_stats.total_deallocations);
		
		/* 从泄漏检测列表移除 */
		pddgpu_memory_stats_remove_leak_object(pdev, bo);
	}
	
	PDDGPU_DEBUG("Memory deallocation ended: size=%llu\n", size);
}

/* 内存移动统计开始 */
void pddgpu_memory_stats_move_start(struct pddgpu_device *pdev, 
                                    struct pddgpu_bo *bo)
{
	ktime_t start_time;
	
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	/* 确保之前的操作完成 */
	smp_mb();
	
	start_time = ktime_get();
	
	if (bo) {
		bo->move_start_time = start_time;
	}
	
	atomic64_inc(&pdev->memory_stats.debug.debug_moves);
	
	PDDGPU_DEBUG("Memory move started\n");
}

/* 内存移动统计结束 */
void pddgpu_memory_stats_move_end(struct pddgpu_device *pdev, 
                                  struct pddgpu_bo *bo)
{
	ktime_t end_time, duration;
	u64 duration_ns;
	
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	end_time = ktime_get();
	
	if (bo) {
		duration = ktime_sub(end_time, bo->move_start_time);
		duration_ns = ktime_to_ns(duration);
		
		/* 更新性能统计 */
		atomic64_add(duration_ns, &pdev->memory_stats.performance.move_time_total);
		atomic64_inc(&pdev->memory_stats.performance.move_operations);
	}
	
	PDDGPU_DEBUG("Memory move ended\n");
}

/* 内存泄漏检测 */
void pddgpu_memory_stats_leak_check(struct pddgpu_device *pdev)
{
	struct pddgpu_memory_leak_object *leak_obj, *temp;
	unsigned long flags;
	u64 current_time = ktime_get_ns();
	u64 check_interval = pdev->memory_stats.leak_detector.check_interval;
	
	/* 检查设备状态 */
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	/* 检查是否需要执行泄漏检测 */
	if (current_time - pdev->memory_stats.leak_detector.last_check_time < check_interval) {
		return;
	}
	
	/* 尝试获取锁，如果失败则稍后重试 */
	if (!spin_trylock_irqsave(&pdev->memory_stats.leak_detector.lock, flags)) {
		return;
	}
	
	/* 检查长时间未释放的对象 */
	list_for_each_entry_safe(leak_obj, temp, 
	                        &pdev->memory_stats.leak_detector.allocated_objects, list) {
		u64 age = current_time - leak_obj->allocation_time;
		
		/* 检查对象有效性 */
		if (!leak_obj->bo || !leak_obj->bo->tbo.base.resv) {
			/* 对象已被释放，从列表中移除 */
			list_del(&leak_obj->list);
			kfree(leak_obj);
			continue;
		}
		
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

/* RCU保护的只读泄漏检测 */
void pddgpu_memory_stats_leak_check_rcu(struct pddgpu_device *pdev)
{
	struct pddgpu_memory_leak_object *leak_obj;
	u64 current_time = ktime_get_ns();
	u64 check_interval = pdev->memory_stats.leak_detector.check_interval;
	
	/* 检查设备状态 */
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	/* 检查是否需要执行泄漏检测 */
	if (current_time - pdev->memory_stats.leak_detector.last_check_time < check_interval) {
		return;
	}
	
	/* 使用RCU保护进行只读访问 */
	rcu_read_lock();
	list_for_each_entry_rcu(leak_obj, &pdev->memory_stats.leak_detector.allocated_objects, list) {
		u64 age = current_time - leak_obj->allocation_time;
		
		/* 检查对象有效性 */
		if (!leak_obj->bo || !leak_obj->bo->tbo.base.resv) {
			continue; /* 跳过无效对象，不修改列表 */
		}
		
		/* 如果对象存在超过30秒，标记为可疑泄漏 */
		if (age > 30 * 1000000000ULL) { /* 30秒 */
			atomic64_inc(&pdev->memory_stats.leak_detector.leak_suspicious_count);
			
			PDDGPU_DEBUG("Suspicious memory leak detected (RCU): size=%llu, age=%llu ns, pid=%d\n",
			             leak_obj->size, age, leak_obj->pid);
		}
		
		/* 如果对象存在超过5分钟，标记为确认泄漏 */
		if (age > 5 * 60 * 1000000000ULL) { /* 5分钟 */
			atomic64_inc(&pdev->memory_stats.leak_detector.leak_confirmed_count);
			
			PDDGPU_ERROR("Confirmed memory leak detected (RCU): size=%llu, age=%llu ns, pid=%d\n",
			             leak_obj->size, age, leak_obj->pid);
		}
	}
	rcu_read_unlock();
	
	pdev->memory_stats.leak_detector.last_check_time = current_time;
}

/* 内存泄漏报告 */
void pddgpu_memory_stats_leak_report(struct pddgpu_device *pdev)
{
	struct pddgpu_memory_leak_object *leak_obj;
	unsigned long flags;
	u64 suspicious_count, confirmed_count;
	u64 total_leaked_size = 0;
	int leak_count = 0;
	
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	suspicious_count = atomic64_read(&pdev->memory_stats.leak_detector.leak_suspicious_count);
	confirmed_count = atomic64_read(&pdev->memory_stats.leak_detector.leak_confirmed_count);
	
	spin_lock_irqsave(&pdev->memory_stats.leak_detector.lock, flags);
	
	list_for_each_entry(leak_obj, &pdev->memory_stats.leak_detector.allocated_objects, list) {
		/* 检查对象有效性 */
		if (leak_obj->bo && leak_obj->bo->tbo.base.resv) {
			total_leaked_size += leak_obj->size;
			leak_count++;
		}
	}
	
	spin_unlock_irqrestore(&pdev->memory_stats.leak_detector.lock, flags);
	
	PDDGPU_INFO("Memory leak report:\n");
	PDDGPU_INFO("  Total allocated objects: %d\n", leak_count);
	PDDGPU_INFO("  Total leaked size: %llu bytes\n", total_leaked_size);
	PDDGPU_INFO("  Suspicious leaks: %llu\n", suspicious_count);
	PDDGPU_INFO("  Confirmed leaks: %llu\n", confirmed_count);
}

/* RCU保护的只读泄漏报告 */
void pddgpu_memory_stats_leak_report_rcu(struct pddgpu_device *pdev)
{
	struct pddgpu_memory_leak_object *leak_obj;
	u64 suspicious_count, confirmed_count;
	u64 total_leaked_size = 0;
	int leak_count = 0;
	
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	suspicious_count = atomic64_read(&pdev->memory_stats.leak_detector.leak_suspicious_count);
	confirmed_count = atomic64_read(&pdev->memory_stats.leak_detector.leak_confirmed_count);
	
	/* 使用RCU保护进行只读访问 */
	rcu_read_lock();
	list_for_each_entry_rcu(leak_obj, &pdev->memory_stats.leak_detector.allocated_objects, list) {
		/* 检查对象有效性 */
		if (leak_obj->bo && leak_obj->bo->tbo.base.resv) {
			total_leaked_size += leak_obj->size;
			leak_count++;
		}
	}
	rcu_read_unlock();
	
	PDDGPU_INFO("Memory leak report (RCU):\n");
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
	
	if (!pdev || !info || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	/* 使用原子操作获取一致性快照 */
	do {
		vram_allocated = atomic64_read(&pdev->memory_stats.vram_allocated);
		vram_freed = atomic64_read(&pdev->memory_stats.vram_freed);
	} while (atomic64_read(&pdev->memory_stats.vram_allocated) != vram_allocated);
	
	do {
		gtt_allocated = atomic64_read(&pdev->memory_stats.gtt_allocated);
		gtt_freed = atomic64_read(&pdev->memory_stats.gtt_freed);
	} while (atomic64_read(&pdev->memory_stats.gtt_allocated) != gtt_allocated);
	
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
	
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	pddgpu_memory_stats_get_info(pdev, &info);
	
	PDDGPU_INFO("Memory Statistics Debug Info:\n");
	PDDGPU_INFO("  VRAM: Total=%llu MB, Used=%llu MB, Free=%llu MB\n",
	            info.vram_total >> 20, info.vram_used >> 20, info.vram_free >> 20);
	PDDGPU_INFO("  GTT:  Total=%llu MB, Used=%llu MB, Free=%llu MB\n",
	            info.gtt_total >> 20, info.gtt_used >> 20, info.gtt_free >> 20);
	PDDGPU_INFO("  Operations: Alloc=%llu, Dealloc=%llu\n",
	            info.total_allocations, info.total_deallocations);
	PDDGPU_INFO("  Performance: Avg_Alloc=%llu ns, Avg_Dealloc=%llu ns, Avg_Move=%llu ns\n",
	            info.avg_allocation_time, info.avg_deallocation_time, info.avg_move_time);
	PDDGPU_INFO("  Leaks: Suspicious=%llu, Confirmed=%llu\n",
	            info.leak_suspicious, info.leak_confirmed);
}

/* 重置统计 */
void pddgpu_memory_stats_reset(struct pddgpu_device *pdev)
{
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
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
	
	/* 重置泄漏检测统计 */
	atomic64_set(&pdev->memory_stats.leak_detector.leak_suspicious_count, 0);
	atomic64_set(&pdev->memory_stats.leak_detector.leak_confirmed_count, 0);
	
	PDDGPU_DEBUG("Memory statistics reset\n");
}

/* 设置泄漏检测间隔 */
void pddgpu_memory_stats_set_leak_check_interval(struct pddgpu_device *pdev, u64 interval)
{
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	pdev->memory_stats.leak_detector.check_interval = interval * 1000000; /* 转换为纳秒 */
}

/* 获取泄漏检测间隔 */
u64 pddgpu_memory_stats_get_leak_check_interval(struct pddgpu_device *pdev)
{
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return 0;
	}
	
	return pdev->memory_stats.leak_detector.check_interval / 1000000; /* 转换为毫秒 */
}

/* 性能统计辅助函数 */
void pddgpu_memory_stats_performance_start(struct pddgpu_device *pdev, ktime_t *start_time)
{
	if (!pdev || !start_time || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	*start_time = ktime_get();
}

void pddgpu_memory_stats_performance_end(struct pddgpu_device *pdev, ktime_t start_time, 
                                        atomic64_t *time_total, atomic64_t *count)
{
	ktime_t end_time, duration;
	u64 duration_ns;
	
	if (!pdev || !time_total || !count || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	end_time = ktime_get();
	duration = ktime_sub(end_time, start_time);
	duration_ns = ktime_to_ns(duration);
	
	atomic64_add(duration_ns, time_total);
	atomic64_inc(count);
}

/* 内存使用统计更新 */
void pddgpu_memory_stats_update_usage(struct pddgpu_device *pdev, u32 domain, u64 size, bool alloc)
{
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		return;
	}
	
	if (domain == TTM_PL_VRAM) {
		if (alloc) {
			atomic64_add(size, &pdev->memory_stats.vram_allocated);
			atomic64_inc(&pdev->memory_stats.total_allocations);
		} else {
			atomic64_add(size, &pdev->memory_stats.vram_freed);
			atomic64_inc(&pdev->memory_stats.total_deallocations);
		}
	} else if (domain == TTM_PL_TT) {
		if (alloc) {
			atomic64_add(size, &pdev->memory_stats.gtt_allocated);
			atomic64_inc(&pdev->memory_stats.total_allocations);
		} else {
			atomic64_add(size, &pdev->memory_stats.gtt_freed);
			atomic64_inc(&pdev->memory_stats.total_deallocations);
		}
	}
}

/* RCU回调函数 - 延迟释放泄漏对象 */
static void pddgpu_memory_leak_object_rcu_free(struct rcu_head *rcu)
{
	struct pddgpu_memory_leak_object *leak_obj = 
		container_of(rcu, struct pddgpu_memory_leak_object, rcu);
	kfree(leak_obj);
}

/* 内存泄漏检测辅助函数 - 使用RCU保护 */
static inline void pddgpu_memory_stats_add_leak_object_rcu(struct pddgpu_device *pdev, 
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
	list_add_tail_rcu(&leak_obj->list, &pdev->memory_stats.leak_detector.allocated_objects);
	spin_unlock_irqrestore(&pdev->memory_stats.leak_detector.lock, flags);
}

static inline void pddgpu_memory_stats_remove_leak_object_rcu(struct pddgpu_device *pdev, 
                                                              struct pddgpu_bo *bo)
{
	struct pddgpu_memory_leak_object *leak_obj, *temp;
	unsigned long flags;
	
	spin_lock_irqsave(&pdev->memory_stats.leak_detector.lock, flags);
	list_for_each_entry_safe(leak_obj, temp, 
	                        &pdev->memory_stats.leak_detector.allocated_objects, list) {
		if (leak_obj->bo == bo) {
			list_del_rcu(&leak_obj->list);
			call_rcu(&leak_obj->rcu, pddgpu_memory_leak_object_rcu_free);
			break;
		}
	}
	spin_unlock_irqrestore(&pdev->memory_stats.leak_detector.lock, flags);
}

/* 批量操作接口 */
void pddgpu_memory_stats_batch_update(struct pddgpu_device *pdev,
                                     struct pddgpu_memory_stats_batch *batch)
{
	if (!pdev || !batch) {
		PDDGPU_ERROR("Invalid parameters for batch update\n");
		return;
	}

	/* 检查设备状态 */
	if (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN) {
		PDDGPU_DEBUG("Device is shutting down, skipping batch update\n");
		return;
	}

	/* 确保之前的操作完成 */
	smp_mb();

	/* 批量更新统计信息 */
	if (batch->vram_allocated > 0)
		atomic64_add(batch->vram_allocated, &pdev->memory_stats.vram_allocated);
	if (batch->vram_freed > 0)
		atomic64_add(batch->vram_freed, &pdev->memory_stats.vram_freed);
	if (batch->gtt_allocated > 0)
		atomic64_add(batch->gtt_allocated, &pdev->memory_stats.gtt_allocated);
	if (batch->gtt_freed > 0)
		atomic64_add(batch->gtt_freed, &pdev->memory_stats.gtt_freed);
	if (batch->total_allocations > 0)
		atomic64_add(batch->total_allocations, &pdev->memory_stats.total_allocations);
	if (batch->total_deallocations > 0)
		atomic64_add(batch->total_deallocations, &pdev->memory_stats.total_deallocations);
	if (batch->move_operations > 0)
		atomic64_add(batch->move_operations, &pdev->memory_stats.performance.move_operations);
	if (batch->move_time_total > 0)
		atomic64_add(batch->move_time_total, &pdev->memory_stats.performance.move_time_total);

	/* 确保统计更新对其他CPU可见 */
	smp_wmb();

	PDDGPU_DEBUG("Batch update completed: VRAM=%llu/%llu, GTT=%llu/%llu\n",
	             batch->vram_allocated, batch->vram_freed,
	             batch->gtt_allocated, batch->gtt_freed);
}

/* 改进的错误处理接口 */
int pddgpu_memory_stats_leak_check_safe(struct pddgpu_device *pdev)
{
	unsigned long flags;
	int ret = 0;

	if (!pdev) {
		PDDGPU_ERROR("Invalid device parameter\n");
		return -EINVAL;
	}

	/* 检查设备状态 */
	if (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN) {
		PDDGPU_DEBUG("Device is shutting down, skipping leak check\n");
		return -ENODEV;
	}

	/* 尝试获取锁，如果失败则返回错误 */
	if (!spin_trylock_irqsave(&pdev->memory_stats.leak_detector.lock, flags)) {
		PDDGPU_DEBUG("Leak detector lock is busy, skipping check\n");
		return -EBUSY;
	}

	/* 再次检查设备状态（在锁内） */
	if (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN) {
		spin_unlock_irqrestore(&pdev->memory_stats.leak_detector.lock, flags);
		PDDGPU_DEBUG("Device state changed during leak check\n");
		return -ENODEV;
	}

	/* 执行泄漏检测 */
	pddgpu_memory_stats_leak_check(pdev);

	spin_unlock_irqrestore(&pdev->memory_stats.leak_detector.lock, flags);

	return ret;
}

int pddgpu_memory_stats_leak_report_safe(struct pddgpu_device *pdev)
{
	unsigned long flags;
	int ret = 0;

	if (!pdev) {
		PDDGPU_ERROR("Invalid device parameter\n");
		return -EINVAL;
	}

	/* 检查设备状态 */
	if (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN) {
		PDDGPU_DEBUG("Device is shutting down, skipping leak report\n");
		return -ENODEV;
	}

	/* 尝试获取锁，如果失败则返回错误 */
	if (!spin_trylock_irqsave(&pdev->memory_stats.leak_detector.lock, flags)) {
		PDDGPU_DEBUG("Leak detector lock is busy, skipping report\n");
		return -EBUSY;
	}

	/* 再次检查设备状态（在锁内） */
	if (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN) {
		spin_unlock_irqrestore(&pdev->memory_stats.leak_detector.lock, flags);
		PDDGPU_DEBUG("Device state changed during leak report\n");
		return -ENODEV;
	}

	/* 执行泄漏报告 */
	pddgpu_memory_stats_leak_report(pdev);

	spin_unlock_irqrestore(&pdev->memory_stats.leak_detector.lock, flags);

	return ret;
}

/* 无锁操作接口 */
void pddgpu_memory_stats_add_leak_object_lockfree(struct pddgpu_device *pdev, 
                                                  struct pddgpu_bo *bo)
{
	struct pddgpu_memory_leak_object *leak_obj;
	unsigned long flags;

	if (!pdev || !bo) {
		PDDGPU_ERROR("Invalid parameters for lockfree leak object addition\n");
		return;
	}

	/* 检查设备状态 */
	if (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN) {
		PDDGPU_DEBUG("Device is shutting down, skipping lockfree addition\n");
		return;
	}

	leak_obj = kzalloc(sizeof(*leak_obj), GFP_KERNEL);
	if (!leak_obj) {
		PDDGPU_ERROR("Failed to allocate leak object\n");
		return;
	}

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

	/* 使用RCU保护添加对象 */
	spin_lock_irqsave(&pdev->memory_stats.leak_detector.lock, flags);
	list_add_tail_rcu(&leak_obj->list, &pdev->memory_stats.leak_detector.allocated_objects);
	spin_unlock_irqrestore(&pdev->memory_stats.leak_detector.lock, flags);

	PDDGPU_DEBUG("Lockfree leak object added: size=%llu, pid=%d\n",
	             leak_obj->size, leak_obj->pid);
}

void pddgpu_memory_stats_remove_leak_object_lockfree(struct pddgpu_device *pdev, 
                                                     struct pddgpu_bo *bo)
{
	struct pddgpu_memory_leak_object *leak_obj, *temp;
	unsigned long flags;

	if (!pdev || !bo) {
		PDDGPU_ERROR("Invalid parameters for lockfree leak object removal\n");
		return;
	}

	/* 检查设备状态 */
	if (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN) {
		PDDGPU_DEBUG("Device is shutting down, skipping lockfree removal\n");
		return;
	}

	spin_lock_irqsave(&pdev->memory_stats.leak_detector.lock, flags);
	list_for_each_entry_safe(leak_obj, temp, 
	                        &pdev->memory_stats.leak_detector.allocated_objects, list) {
		if (leak_obj->bo == bo) {
			list_del_rcu(&leak_obj->list);
			call_rcu(&leak_obj->rcu, pddgpu_memory_leak_object_rcu_free);
			break;
		}
	}
	spin_unlock_irqrestore(&pdev->memory_stats.leak_detector.lock, flags);

	PDDGPU_DEBUG("Lockfree leak object removed: bo=%p\n", bo);
}
