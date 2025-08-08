# PDDGPU VRAM和GTT管理器并发保护改进

## 概述

本文档总结了PDDGPU驱动中VRAM和GTT管理器的并发保护机制改进，包括竞态条件防护、错误处理机制和状态管理。

## VRAM管理器改进

### 1. 状态管理

#### 1.1 状态标志
```c
/* VRAM管理器状态标志 */
#define PDDGPU_VRAM_MGR_STATE_INITIALIZING	0x01
#define PDDGPU_VRAM_MGR_STATE_READY		0x02
#define PDDGPU_VRAM_MGR_STATE_SHUTDOWN		0x04
#define PDDGPU_VRAM_MGR_STATE_ERROR		0x08
```

#### 1.2 状态检查函数
```c
static inline bool pddgpu_vram_mgr_is_ready(struct pddgpu_vram_mgr *mgr)
{
	return (atomic_read(&mgr->state) & PDDGPU_VRAM_MGR_STATE_READY) &&
	       !(atomic_read(&mgr->state) & PDDGPU_VRAM_MGR_STATE_SHUTDOWN);
}
```

### 2. 错误处理机制

#### 2.1 错误状态设置
```c
static inline void pddgpu_vram_mgr_set_error(struct pddgpu_vram_mgr *mgr)
{
	atomic_or(PDDGPU_VRAM_MGR_STATE_ERROR, &mgr->state);
	PDDGPU_ERROR("VRAM manager entered error state\n");
}
```

#### 2.2 错误状态清除
```c
static inline void pddgpu_vram_mgr_clear_error(struct pddgpu_vram_mgr *mgr)
{
	atomic_and(~PDDGPU_VRAM_MGR_STATE_ERROR, &mgr->state);
}
```

### 3. 分配函数改进

#### 3.1 设备状态检查
```c
/* 检查设备状态 */
if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
	PDDGPU_DEBUG("Device is shutting down, skipping VRAM allocation\n");
	return -ENODEV;
}
```

#### 3.2 管理器状态检查
```c
/* 检查VRAM管理器状态 */
if (!pddgpu_vram_mgr_is_ready(mgr)) {
	PDDGPU_ERROR("VRAM manager is not ready\n");
	return -ENODEV;
}
```

#### 3.3 重试机制
```c
#define PDDGPU_VRAM_ALLOC_RETRY_COUNT	3
#define PDDGPU_VRAM_ALLOC_RETRY_DELAY	10 /* 毫秒 */

/* 重试机制 */
retry_alloc:
mutex_lock(&mgr->lock);

/* 再次检查状态（在锁内） */
if (!pddgpu_vram_mgr_is_ready(mgr)) {
	mutex_unlock(&mgr->lock);
	PDDGPU_ERROR("VRAM manager state changed during allocation\n");
	kfree(vres);
	return -ENODEV;
}

/* 分配失败时的重试 */
if (r) {
	/* 重试机制 */
	if (++retry_count < PDDGPU_VRAM_ALLOC_RETRY_COUNT) {
		PDDGPU_DEBUG("VRAM allocation failed, retrying (%d/%d)\n",
		             retry_count, PDDGPU_VRAM_ALLOC_RETRY_COUNT);
		msleep(PDDGPU_VRAM_ALLOC_RETRY_DELAY);
		goto retry_alloc;
	}
	
	PDDGPU_ERROR("VRAM allocation failed after %d retries\n", retry_count);
	return -ENOMEM;
}
```

### 4. 释放函数改进

#### 4.1 状态检查
```c
/* 检查设备状态 */
if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
	PDDGPU_DEBUG("Device is shutting down, skipping VRAM free\n");
	return;
}

/* 检查VRAM管理器状态 */
if (!pddgpu_vram_mgr_is_ready(mgr)) {
	PDDGPU_ERROR("VRAM manager is not ready during free\n");
	return;
}
```

#### 4.2 锁内状态检查
```c
mutex_lock(&mgr->lock);

/* 再次检查状态（在锁内） */
if (!pddgpu_vram_mgr_is_ready(mgr)) {
	mutex_unlock(&mgr->lock);
	PDDGPU_ERROR("VRAM manager state changed during free\n");
	return;
}
```

### 5. 恢复机制

#### 5.1 管理器恢复函数
```c
int pddgpu_vram_mgr_recover(struct pddgpu_vram_mgr *mgr)
{
	struct pddgpu_device *pdev = to_pddgpu_device(mgr);
	int r;

	PDDGPU_DEBUG("Recovering VRAM manager\n");

	/* 检查设备状态 */
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		PDDGPU_ERROR("Device is shutting down, cannot recover VRAM manager\n");
		return -ENODEV;
	}

	/* 清除错误状态 */
	pddgpu_vram_mgr_clear_error(mgr);

	/* 重新初始化DRM Buddy分配器 */
	mutex_lock(&mgr->lock);
	r = drm_buddy_init(&mgr->mm, mgr->size, mgr->default_page_size);
	mutex_unlock(&mgr->lock);

	if (r) {
		PDDGPU_ERROR("Failed to recover DRM Buddy: %d\n", r);
		pddgpu_vram_mgr_set_error(mgr);
		return r;
	}

	/* 重置统计信息 */
	atomic64_set(&mgr->used, 0);
	atomic64_set(&mgr->vis_usage, 0);

	/* 设置就绪状态 */
	atomic_set(&mgr->state, PDDGPU_VRAM_MGR_STATE_READY);

	PDDGPU_INFO("VRAM manager recovered successfully\n");

	return 0;
}
```

### 6. 统计信息

#### 6.1 统计结构
```c
struct pddgpu_vram_stats {
	u64 total_size;
	u64 used_size;
	u64 visible_used;
	u32 state;
	bool is_healthy;
};
```

#### 6.2 统计获取函数
```c
void pddgpu_vram_mgr_get_stats(struct pddgpu_vram_mgr *mgr,
                                struct pddgpu_vram_stats *stats)
{
	if (!mgr || !stats)
		return;

	stats->total_size = mgr->size;
	stats->used_size = atomic64_read(&mgr->used);
	stats->visible_used = atomic64_read(&mgr->vis_usage);
	stats->state = atomic_read(&mgr->state);
	stats->is_healthy = pddgpu_vram_mgr_is_healthy(mgr);
}
```

## GTT管理器改进

### 1. 状态管理

#### 1.1 状态标志
```c
/* GTT管理器状态标志 */
#define PDDGPU_GTT_MGR_STATE_INITIALIZING	0x01
#define PDDGPU_GTT_MGR_STATE_READY		0x02
#define PDDGPU_GTT_MGR_STATE_SHUTDOWN		0x04
#define PDDGPU_GTT_MGR_STATE_ERROR		0x08
```

#### 1.2 状态检查函数
```c
static inline bool pddgpu_gtt_mgr_is_ready(struct pddgpu_gtt_mgr *mgr)
{
	return (atomic_read(&mgr->state) & PDDGPU_GTT_MGR_STATE_READY) &&
	       !(atomic_read(&mgr->state) & PDDGPU_GTT_MGR_STATE_SHUTDOWN);
}
```

### 2. 错误处理机制

#### 2.1 错误状态设置
```c
static inline void pddgpu_gtt_mgr_set_error(struct pddgpu_gtt_mgr *mgr)
{
	atomic_or(PDDGPU_GTT_MGR_STATE_ERROR, &mgr->state);
	PDDGPU_ERROR("GTT manager entered error state\n");
}
```

#### 2.2 错误状态清除
```c
static inline void pddgpu_gtt_mgr_clear_error(struct pddgpu_gtt_mgr *mgr)
{
	atomic_and(~PDDGPU_GTT_MGR_STATE_ERROR, &mgr->state);
}
```

### 3. 分配函数改进

#### 3.1 设备状态检查
```c
/* 检查设备状态 */
if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
	PDDGPU_DEBUG("Device is shutting down, skipping GTT allocation\n");
	return -ENODEV;
}
```

#### 3.2 管理器状态检查
```c
/* 检查GTT管理器状态 */
if (!pddgpu_gtt_mgr_is_ready(mgr)) {
	PDDGPU_ERROR("GTT manager is not ready\n");
	return -ENODEV;
}
```

#### 3.3 重试机制
```c
#define PDDGPU_GTT_ALLOC_RETRY_COUNT	3
#define PDDGPU_GTT_ALLOC_RETRY_DELAY	5 /* 毫秒 */

/* 重试机制 */
retry_alloc:
spin_lock(&mgr->lock);

/* 再次检查状态（在锁内） */
if (!pddgpu_gtt_mgr_is_ready(mgr)) {
	spin_unlock(&mgr->lock);
	PDDGPU_ERROR("GTT manager state changed during allocation\n");
	r = -ENODEV;
	goto err_free;
}

/* 分配失败时的重试 */
if (unlikely(r)) {
	/* 重试机制 */
	if (++retry_count < PDDGPU_GTT_ALLOC_RETRY_COUNT) {
		PDDGPU_DEBUG("GTT allocation failed, retrying (%d/%d)\n",
		             retry_count, PDDGPU_GTT_ALLOC_RETRY_COUNT);
		msleep(PDDGPU_GTT_ALLOC_RETRY_DELAY);
		goto retry_alloc;
	}
	
	PDDGPU_ERROR("GTT allocation failed after %d retries: %d\n", retry_count, r);
	goto err_free;
}
```

### 4. 释放函数改进

#### 4.1 状态检查
```c
/* 检查设备状态 */
if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
	PDDGPU_DEBUG("Device is shutting down, skipping GTT free\n");
	return;
}

/* 检查GTT管理器状态 */
if (!pddgpu_gtt_mgr_is_ready(mgr)) {
	PDDGPU_ERROR("GTT manager is not ready during free\n");
	return;
}
```

#### 4.2 锁内状态检查
```c
spin_lock(&mgr->lock);

/* 再次检查状态（在锁内） */
if (!pddgpu_gtt_mgr_is_ready(mgr)) {
	spin_unlock(&mgr->lock);
	PDDGPU_ERROR("GTT manager state changed during free\n");
	return;
}
```

### 5. 恢复机制

#### 5.1 管理器恢复函数
```c
int pddgpu_gtt_mgr_recover(struct pddgpu_gtt_mgr *mgr)
{
	struct pddgpu_device *pdev = container_of(mgr, struct pddgpu_device, mman.gtt_mgr);
	int r;

	PDDGPU_DEBUG("Recovering GTT manager\n");

	/* 检查设备状态 */
	if (!pdev || (atomic_read(&pdev->device_state) & PDDGPU_DEVICE_STATE_SHUTDOWN)) {
		PDDGPU_ERROR("Device is shutting down, cannot recover GTT manager\n");
		return -ENODEV;
	}

	/* 清除错误状态 */
	pddgpu_gtt_mgr_clear_error(mgr);

	/* 重新初始化DRM MM分配器 */
	spin_lock(&mgr->lock);
	r = drm_mm_init(&mgr->mm, 0, mgr->mm.size);
	spin_unlock(&mgr->lock);

	if (r) {
		PDDGPU_ERROR("Failed to recover DRM MM: %d\n", r);
		pddgpu_gtt_mgr_set_error(mgr);
		return r;
	}

	/* 设置就绪状态 */
	atomic_set(&mgr->state, PDDGPU_GTT_MGR_STATE_READY);

	PDDGPU_INFO("GTT manager recovered successfully\n");

	return 0;
}
```

### 6. 统计信息

#### 6.1 统计结构
```c
struct pddgpu_gtt_stats {
	u64 total_size;
	u64 used_size;
	u32 state;
	bool is_healthy;
};
```

#### 6.2 统计获取函数
```c
void pddgpu_gtt_mgr_get_stats(struct pddgpu_gtt_mgr *mgr,
                               struct pddgpu_gtt_stats *stats)
{
	if (!mgr || !stats)
		return;

	stats->total_size = mgr->mm.size << PAGE_SHIFT;
	stats->used_size = mgr->mm.allocated_size << PAGE_SHIFT;
	stats->state = atomic_read(&mgr->state);
	stats->is_healthy = pddgpu_gtt_mgr_is_healthy(mgr);
}
```

## 并发保护机制

### 1. 锁机制

#### 1.1 VRAM管理器锁
- 使用 `mutex` 保护DRM Buddy分配器
- 在锁内进行状态检查
- 确保分配和释放操作的原子性

#### 1.2 GTT管理器锁
- 使用 `spinlock` 保护DRM MM分配器
- 在锁内进行状态检查
- 确保地址空间分配的原子性

### 2. 状态管理

#### 2.1 原子操作
- 使用 `atomic_t` 管理状态
- 使用 `atomic64_t` 管理统计信息
- 确保状态检查的原子性

#### 2.2 状态转换
- 初始化：`INITIALIZING` → `READY`
- 错误：`READY` → `ERROR`
- 恢复：`ERROR` → `READY`
- 关闭：`READY` → `SHUTDOWN`

### 3. 错误处理

#### 3.1 错误检测
- 分配失败时设置错误状态
- 初始化失败时设置错误状态
- 状态不一致时设置错误状态

#### 3.2 错误恢复
- 提供恢复函数重新初始化分配器
- 清除错误状态
- 重置统计信息

### 4. 重试机制

#### 4.1 分配重试
- 分配失败时自动重试
- 可配置重试次数和延迟
- 避免临时资源竞争

#### 4.2 状态重试
- 状态检查失败时重试
- 确保状态一致性

## 性能优化

### 1. 锁优化

#### 1.1 锁粒度
- 最小化锁保护范围
- 避免长时间持有锁
- 使用适当的锁类型

#### 1.2 锁竞争
- 减少锁竞争
- 使用原子操作减少锁使用
- 优化锁的获取和释放

### 2. 内存管理

#### 2.1 分配优化
- 使用DRM Buddy分配器提高效率
- 支持大页分配
- 减少内存碎片

#### 2.2 统计优化
- 使用原子操作更新统计
- 避免锁竞争
- 提供实时统计信息

## 测试验证

### 1. 并发测试

#### 1.1 多线程分配
```c
void *alloc_thread(void *arg)
{
	struct pddgpu_device *pdev = arg;
	for (int i = 0; i < 1000; i++) {
		// 分配内存
		pddgpu_bo_create(pdev, &bp, &bo);
		// 释放内存
		pddgpu_bo_unref(&bo);
	}
	return NULL;
}
```

#### 1.2 压力测试
```c
for (int i = 0; i < 100; i++) {
	pthread_create(&threads[i], NULL, alloc_thread, pdev);
}
```

### 2. 错误测试

#### 2.1 状态错误测试
- 模拟管理器错误状态
- 验证错误恢复机制
- 测试状态转换

#### 2.2 资源耗尽测试
- 耗尽VRAM/GTT空间
- 验证错误处理
- 测试恢复机制

## 总结

通过实施这些改进，PDDGPU的VRAM和GTT管理器在并发处理方面得到了显著提升：

1. **安全性**：添加了完善的状态检查和错误处理机制
2. **可靠性**：实现了错误恢复和重试机制
3. **性能**：优化了锁机制和内存分配策略
4. **可观测性**：提供了详细的统计信息和调试接口
5. **可维护性**：改进了代码结构和错误处理流程

这些改进确保了PDDGPU驱动在高并发环境下的稳定性和可靠性。
