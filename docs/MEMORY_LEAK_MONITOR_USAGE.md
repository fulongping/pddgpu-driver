# PDDGPU 内存泄漏监控使用指南

## 概述

PDDGPU 驱动集成了内存泄漏监控功能，通过宏控制的后台进程自动检测内存泄漏，当泄漏超过100M时自动打印告警信息。

## 功能特性

### 1. 自动监控
- 后台工作队列定期检查内存使用情况
- 每5秒检查一次内存泄漏
- 自动检测可疑泄漏（30秒）和确认泄漏（5分钟）

### 2. 阈值告警
- 默认阈值：100MB
- 可通过宏 `PDDGPU_MEMORY_LEAK_THRESHOLD` 调整
- 超过阈值时自动打印告警信息

### 3. 详细报告
- 内存使用统计
- 泄漏对象列表
- 性能分析数据

## 配置选项

### 启用/禁用监控
```c
/* 在 pddgpu_drv.h 中 */
#define PDDGPU_MEMORY_LEAK_MONITOR_ENABLED 1  /* 启用 */
#define PDDGPU_MEMORY_LEAK_MONITOR_ENABLED 0  /* 禁用 */
```

### 调整泄漏阈值
```c
/* 在 pddgpu_drv.h 中 */
#define PDDGPU_MEMORY_LEAK_THRESHOLD (100 * 1024 * 1024) /* 100MB */
#define PDDGPU_MEMORY_LEAK_THRESHOLD (200 * 1024 * 1024) /* 200MB */
```

### 运行时控制
```c
/* 启用/禁用监控 */
pdev->memory_stats.leak_monitor.monitor_enabled = true/false;

/* 设置自定义阈值 */
pdev->memory_stats.leak_monitor.leak_threshold = 150 * 1024 * 1024; /* 150MB */
```

## 使用方法

### 1. 编译驱动
```bash
cd pddgpu_driver
make
```

### 2. 加载驱动
```bash
sudo insmod pddgpu.ko
```

### 3. 运行测试程序
```bash
cd examples
make
sudo ./memory_leak_test
```

### 4. 查看内核日志
```bash
# 查看实时日志
dmesg -w

# 查看 PDDGPU 相关日志
dmesg | grep PDDGPU
```

## 测试程序说明

### memory_leak_test.c
该测试程序演示了内存泄漏监控功能：

1. **分配阶段**：分配15个10MB的BO，总计150MB
2. **泄漏模拟**：只释放前5个BO，保留10个BO造成泄漏
3. **监控观察**：等待监控进程检测到泄漏并告警

### 运行结果示例
```
PDDGPU 内存泄漏测试程序
========================
设备打开成功，开始内存泄漏测试...
分配BO 0: handle=1, size=10485760 bytes
分配BO 1: handle=2, size=10485760 bytes
...
已分配 15 个BO，总大小: 150 MB
等待 2 秒，让内存泄漏监控进程检测到泄漏...

开始释放部分BO...
释放BO 0: handle=1
释放BO 1: handle=2
...
保留了 10 个BO未释放，模拟内存泄漏
等待 2 秒，观察内存泄漏监控...

测试完成，清理剩余BO...
释放BO 5: handle=6
...
测试程序结束
```

### 内核日志输出示例
```
[  123.456789] PDDGPU: Memory leak monitor started
[  125.123456] PDDGPU: Memory leak detected! Total used: 104 MB
[  125.123457] PDDGPU: Memory leak report:
[  125.123458] PDDGPU:   Total allocated objects: 10
[  125.123459] PDDGPU:   Total leaked size: 104857600 bytes
[  125.123460] PDDGPU:   Suspicious leaks: 10
[  125.123461] PDDGPU:   Confirmed leaks: 0
```

## 监控进程工作原理

### 1. 初始化流程
```
pddgpu_device_init()
    ↓
pddgpu_memory_stats_init()
    ↓
INIT_DELAYED_WORK() + schedule_delayed_work()
    ↓
pddgpu_memory_leak_monitor_work() (每5秒执行)
```

### 2. 监控检查流程
```
pddgpu_memory_leak_monitor_work()
    ↓
pddgpu_memory_stats_get_info() → 获取内存统计
    ↓
检查 (vram_used + gtt_used) > threshold
    ↓
pddgpu_memory_stats_leak_report() → 生成告警
    ↓
pddgpu_memory_stats_leak_check() → 常规泄漏检测
    ↓
schedule_delayed_work() → 重新调度
```

### 3. 泄漏检测算法
- **可疑泄漏**：对象存在超过30秒
- **确认泄漏**：对象存在超过5分钟
- **阈值告警**：总内存使用超过100MB

## 调试和故障排除

### 1. 启用调试输出
```c
/* 在驱动代码中启用调试 */
#define PDDGPU_DEBUG(fmt, ...) pr_debug("PDDGPU: " fmt, ##__VA_ARGS__)
```

### 2. 查看详细统计
```bash
# 查看内存统计信息
cat /sys/kernel/debug/pddgpu/memory_stats

# 查看泄漏检测对象
cat /sys/kernel/debug/pddgpu/leak_objects
```

### 3. 手动触发检查
```bash
# 通过 sysfs 接口手动触发泄漏检查
echo 1 > /sys/kernel/debug/pddgpu/trigger_leak_check
```

### 4. 常见问题

#### 问题1：监控进程未启动
**症状**：内核日志中没有 "Memory leak monitor started"
**解决**：检查 `PDDGPU_MEMORY_LEAK_MONITOR_ENABLED` 宏是否设置为1

#### 问题2：告警阈值不准确
**症状**：告警时机不对
**解决**：检查 `PDDGPU_MEMORY_LEAK_THRESHOLD` 设置，确保单位正确

#### 问题3：监控进程占用CPU过高
**症状**：系统负载增加
**解决**：调整检查间隔，默认5秒可以改为10秒或更长

## 性能影响

### 1. 内存开销
- 每个泄漏对象：约100字节
- 工作队列：约1KB
- 总体开销：< 1MB

### 2. CPU开销
- 检查频率：每5秒一次
- 单次检查：< 1ms
- CPU占用：< 0.1%

### 3. 优化建议
- 生产环境：可以增加检查间隔到10-30秒
- 调试环境：可以缩短间隔到1-2秒
- 内存紧张：可以减少泄漏对象跟踪数量

## 总结

PDDGPU 内存泄漏监控功能提供了：

1. **自动化监控**：无需人工干预的持续监控
2. **及时告警**：超过阈值时立即告警
3. **详细报告**：提供完整的泄漏分析信息
4. **可配置性**：支持自定义阈值和检查间隔
5. **低开销**：对系统性能影响极小

该功能特别适用于：
- 开发和调试阶段的内存泄漏检测
- 生产环境的内存使用监控
- 性能分析和优化
- 系统稳定性保障
