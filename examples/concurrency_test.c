/*
 * PDDGPU 并发测试程序
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>

/* PDDGPU IOCTL 定义 */
#define PDDGPU_IOCTL_BASE 0x50
#define PDDGPU_IOCTL_GEM_CREATE _IOWR(PDDGPU_IOCTL_BASE, 0, struct pddgpu_gem_create)
#define PDDGPU_IOCTL_GEM_DESTROY _IOW(PDDGPU_IOCTL_BASE, 1, struct pddgpu_gem_destroy)
#define PDDGPU_IOCTL_GEM_INFO _IOWR(PDDGPU_IOCTL_BASE, 2, struct pddgpu_gem_info)

/* 内存域定义 */
#define PDDGPU_GEM_DOMAIN_CPU 0
#define PDDGPU_GEM_DOMAIN_GTT 1
#define PDDGPU_GEM_DOMAIN_VRAM 2

/* GEM 创建参数 */
struct pddgpu_gem_create {
    __u64 size;
    __u32 domain;
    __u32 flags;
    __u32 handle;
    __u32 pad;
};

/* GEM 销毁参数 */
struct pddgpu_gem_destroy {
    __u32 handle;
    __u32 pad;
};

/* GEM 信息参数 */
struct pddgpu_gem_info {
    __u32 handle;
    __u32 pad;
    __u64 size;
    __u64 offset;
};

/* 测试配置 */
#define TEST_THREAD_COUNT 10
#define TEST_ITERATIONS 1000
#define TEST_ALLOCATION_SIZE (1024 * 1024) /* 1MB */
#define TEST_DELAY_US 1000 /* 1ms */

/* 全局变量 */
int g_device_fd = -1;
volatile int g_stop_test = 0;
pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;
struct {
    int total_allocations;
    int total_deallocations;
    int allocation_errors;
    int deallocation_errors;
} g_test_stats;

/* 信号处理函数 */
void signal_handler(int sig)
{
    printf("\n收到信号 %d，停止测试...\n", sig);
    g_stop_test = 1;
}

/* 分配线程函数 */
void *allocation_thread(void *arg)
{
    int thread_id = *(int *)arg;
    struct pddgpu_gem_create create_req;
    struct pddgpu_gem_destroy destroy_req;
    int handles[TEST_ITERATIONS];
    int handle_count = 0;
    int i, ret;
    
    printf("线程 %d 开始分配测试\n", thread_id);
    
    for (i = 0; i < TEST_ITERATIONS && !g_stop_test; i++) {
        /* 分配内存 */
        memset(&create_req, 0, sizeof(create_req));
        create_req.size = TEST_ALLOCATION_SIZE;
        create_req.domain = PDDGPU_GEM_DOMAIN_VRAM;
        create_req.flags = 0;
        
        ret = ioctl(g_device_fd, PDDGPU_IOCTL_GEM_CREATE, &create_req);
        if (ret < 0) {
            pthread_mutex_lock(&g_stats_mutex);
            g_test_stats.allocation_errors++;
            pthread_mutex_unlock(&g_stats_mutex);
            continue;
        }
        
        handles[handle_count++] = create_req.handle;
        
        pthread_mutex_lock(&g_stats_mutex);
        g_test_stats.total_allocations++;
        pthread_mutex_unlock(&g_stats_mutex);
        
        /* 随机延迟 */
        usleep(rand() % TEST_DELAY_US);
        
        /* 随机释放一些内存 */
        if (handle_count > 0 && (rand() % 10) == 0) {
            int idx = rand() % handle_count;
            memset(&destroy_req, 0, sizeof(destroy_req));
            destroy_req.handle = handles[idx];
            
            ret = ioctl(g_device_fd, PDDGPU_IOCTL_GEM_DESTROY, &destroy_req);
            if (ret < 0) {
                pthread_mutex_lock(&g_stats_mutex);
                g_test_stats.deallocation_errors++;
                pthread_mutex_unlock(&g_stats_mutex);
            } else {
                pthread_mutex_lock(&g_stats_mutex);
                g_test_stats.total_deallocations++;
                pthread_mutex_unlock(&g_stats_mutex);
            }
            
            /* 移除已释放的句柄 */
            handles[idx] = handles[--handle_count];
        }
    }
    
    /* 释放剩余的内存 */
    for (i = 0; i < handle_count; i++) {
        memset(&destroy_req, 0, sizeof(destroy_req));
        destroy_req.handle = handles[i];
        
        ret = ioctl(g_device_fd, PDDGPU_IOCTL_GEM_DESTROY, &destroy_req);
        if (ret < 0) {
            pthread_mutex_lock(&g_stats_mutex);
            g_test_stats.deallocation_errors++;
            pthread_mutex_unlock(&g_stats_mutex);
        } else {
            pthread_mutex_lock(&g_stats_mutex);
            g_test_stats.total_deallocations++;
            pthread_mutex_unlock(&g_stats_mutex);
        }
    }
    
    printf("线程 %d 完成分配测试\n", thread_id);
    return NULL;
}

/* 监控线程函数 */
void *monitor_thread(void *arg)
{
    struct timespec start_time, current_time;
    int last_allocations = 0, last_deallocations = 0;
    int last_errors = 0;
    
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    while (!g_stop_test) {
        sleep(1);
        
        pthread_mutex_lock(&g_stats_mutex);
        int current_allocations = g_test_stats.total_allocations;
        int current_deallocations = g_test_stats.total_deallocations;
        int current_errors = g_test_stats.allocation_errors + g_test_stats.deallocation_errors;
        pthread_mutex_unlock(&g_stats_mutex);
        
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        double elapsed = (current_time.tv_sec - start_time.tv_sec) + 
                        (current_time.tv_nsec - start_time.tv_nsec) / 1e9;
        
        int alloc_rate = current_allocations - last_allocations;
        int dealloc_rate = current_deallocations - last_deallocations;
        int error_rate = current_errors - last_errors;
        
        printf("[%.1fs] 分配: %d/s, 释放: %d/s, 错误: %d/s, 总计: 分配=%d, 释放=%d, 错误=%d\n",
               elapsed, alloc_rate, dealloc_rate, error_rate,
               current_allocations, current_deallocations, current_errors);
        
        last_allocations = current_allocations;
        last_deallocations = current_deallocations;
        last_errors = current_errors;
    }
    
    return NULL;
}

/* 压力测试函数 */
void *stress_thread(void *arg)
{
    int thread_id = *(int *)arg;
    struct pddgpu_gem_create create_req;
    struct pddgpu_gem_destroy destroy_req;
    int i, ret;
    
    printf("压力测试线程 %d 开始\n", thread_id);
    
    for (i = 0; i < 100 && !g_stop_test; i++) {
        /* 快速分配和释放 */
        memset(&create_req, 0, sizeof(create_req));
        create_req.size = TEST_ALLOCATION_SIZE;
        create_req.domain = PDDGPU_GEM_DOMAIN_VRAM;
        create_req.flags = 0;
        
        ret = ioctl(g_device_fd, PDDGPU_IOCTL_GEM_CREATE, &create_req);
        if (ret >= 0) {
            pthread_mutex_lock(&g_stats_mutex);
            g_test_stats.total_allocations++;
            pthread_mutex_unlock(&g_stats_mutex);
            
            /* 立即释放 */
            memset(&destroy_req, 0, sizeof(destroy_req));
            destroy_req.handle = create_req.handle;
            
            ret = ioctl(g_device_fd, PDDGPU_IOCTL_GEM_DESTROY, &destroy_req);
            if (ret >= 0) {
                pthread_mutex_lock(&g_stats_mutex);
                g_test_stats.total_deallocations++;
                pthread_mutex_unlock(&g_stats_mutex);
            } else {
                pthread_mutex_lock(&g_stats_mutex);
                g_test_stats.deallocation_errors++;
                pthread_mutex_unlock(&g_stats_mutex);
            }
        } else {
            pthread_mutex_lock(&g_stats_mutex);
            g_test_stats.allocation_errors++;
            pthread_mutex_unlock(&g_stats_mutex);
        }
        
        /* 最小延迟 */
        usleep(100);
    }
    
    printf("压力测试线程 %d 完成\n", thread_id);
    return NULL;
}

int main(int argc, char *argv[])
{
    pthread_t threads[TEST_THREAD_COUNT + 2]; /* +2 for monitor and stress threads */
    pthread_t monitor_thread_id, stress_thread_id;
    int thread_ids[TEST_THREAD_COUNT];
    int i, ret;
    
    printf("PDDGPU 并发测试程序\n");
    printf("==================\n");
    
    /* 设置信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* 打开设备 */
    g_device_fd = open("/dev/pddgpu", O_RDWR);
    if (g_device_fd < 0) {
        perror("Failed to open /dev/pddgpu");
        return -1;
    }
    
    printf("设备打开成功，开始并发测试...\n");
    printf("配置: %d 个线程, 每个线程 %d 次迭代, 分配大小 %d bytes\n",
           TEST_THREAD_COUNT, TEST_ITERATIONS, TEST_ALLOCATION_SIZE);
    
    /* 初始化随机数生成器 */
    srand(time(NULL));
    
    /* 创建监控线程 */
    ret = pthread_create(&monitor_thread_id, NULL, monitor_thread, NULL);
    if (ret != 0) {
        perror("Failed to create monitor thread");
        goto cleanup;
    }
    
    /* 创建压力测试线程 */
    ret = pthread_create(&stress_thread_id, NULL, stress_thread, &thread_ids[0]);
    if (ret != 0) {
        perror("Failed to create stress thread");
        goto cleanup;
    }
    
    /* 创建分配线程 */
    for (i = 0; i < TEST_THREAD_COUNT; i++) {
        thread_ids[i] = i;
        ret = pthread_create(&threads[i], NULL, allocation_thread, &thread_ids[i]);
        if (ret != 0) {
            perror("Failed to create allocation thread");
            goto cleanup;
        }
    }
    
    printf("所有线程已启动，按 Ctrl+C 停止测试\n");
    
    /* 等待所有线程完成 */
    for (i = 0; i < TEST_THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }
    
    /* 停止监控线程 */
    g_stop_test = 1;
    pthread_join(monitor_thread_id, NULL);
    pthread_join(stress_thread_id, NULL);
    
    /* 打印最终统计 */
    printf("\n=== 测试完成 ===\n");
    printf("总分配次数: %d\n", g_test_stats.total_allocations);
    printf("总释放次数: %d\n", g_test_stats.total_deallocations);
    printf("分配错误: %d\n", g_test_stats.allocation_errors);
    printf("释放错误: %d\n", g_test_stats.deallocation_errors);
    printf("成功率: %.2f%%\n", 
           (double)(g_test_stats.total_allocations + g_test_stats.total_deallocations) /
           (double)(g_test_stats.total_allocations + g_test_stats.total_deallocations + 
                   g_test_stats.allocation_errors + g_test_stats.deallocation_errors) * 100);

cleanup:
    if (g_device_fd >= 0) {
        close(g_device_fd);
    }
    
    pthread_mutex_destroy(&g_stats_mutex);
    
    printf("测试程序结束\n");
    return 0;
}
