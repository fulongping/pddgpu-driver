/*
 * PDDGPU 内存泄漏测试程序
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
#define TEST_ALLOCATION_SIZE (10 * 1024 * 1024) /* 10MB */
#define TEST_LEAK_COUNT 15 /* 分配15个BO，超过100MB阈值 */
#define TEST_DELAY_SECONDS 2

int main(int argc, char *argv[])
{
    int fd;
    struct pddgpu_gem_create create_req;
    struct pddgpu_gem_destroy destroy_req;
    struct pddgpu_gem_info info_req;
    int handles[TEST_LEAK_COUNT];
    int i, ret;
    
    printf("PDDGPU 内存泄漏测试程序\n");
    printf("========================\n");
    
    /* 打开设备 */
    fd = open("/dev/pddgpu", O_RDWR);
    if (fd < 0) {
        perror("Failed to open /dev/pddgpu");
        return -1;
    }
    
    printf("设备打开成功，开始内存泄漏测试...\n");
    
    /* 分配多个BO，模拟内存泄漏 */
    for (i = 0; i < TEST_LEAK_COUNT; i++) {
        memset(&create_req, 0, sizeof(create_req));
        create_req.size = TEST_ALLOCATION_SIZE;
        create_req.domain = PDDGPU_GEM_DOMAIN_VRAM; /* 使用VRAM */
        create_req.flags = 0;
        
        ret = ioctl(fd, PDDGPU_IOCTL_GEM_CREATE, &create_req);
        if (ret < 0) {
            perror("Failed to create BO");
            goto cleanup;
        }
        
        handles[i] = create_req.handle;
        printf("分配BO %d: handle=%d, size=%llu bytes\n", 
               i, handles[i], (unsigned long long)create_req.size);
    }
    
    printf("\n已分配 %d 个BO，总大小: %llu MB\n", 
           TEST_LEAK_COUNT, 
           (unsigned long long)(TEST_ALLOCATION_SIZE * TEST_LEAK_COUNT) / (1024 * 1024));
    printf("等待 %d 秒，让内存泄漏监控进程检测到泄漏...\n", TEST_DELAY_SECONDS);
    
    /* 等待一段时间，让监控进程检测到泄漏 */
    sleep(TEST_DELAY_SECONDS);
    
    printf("\n开始释放部分BO...\n");
    
    /* 只释放前5个BO，保留其他BO造成泄漏 */
    for (i = 0; i < 5; i++) {
        memset(&destroy_req, 0, sizeof(destroy_req));
        destroy_req.handle = handles[i];
        
        ret = ioctl(fd, PDDGPU_IOCTL_GEM_DESTROY, &destroy_req);
        if (ret < 0) {
            perror("Failed to destroy BO");
            goto cleanup;
        }
        
        printf("释放BO %d: handle=%d\n", i, handles[i]);
    }
    
    printf("\n保留了 %d 个BO未释放，模拟内存泄漏\n", TEST_LEAK_COUNT - 5);
    printf("等待 %d 秒，观察内存泄漏监控...\n", TEST_DELAY_SECONDS);
    
    /* 再次等待，观察泄漏监控 */
    sleep(TEST_DELAY_SECONDS);
    
    printf("\n测试完成，清理剩余BO...\n");
    
    /* 清理剩余的BO */
    for (i = 5; i < TEST_LEAK_COUNT; i++) {
        memset(&destroy_req, 0, sizeof(destroy_req));
        destroy_req.handle = handles[i];
        
        ret = ioctl(fd, PDDGPU_IOCTL_GEM_DESTROY, &destroy_req);
        if (ret < 0) {
            perror("Failed to destroy BO");
        } else {
            printf("释放BO %d: handle=%d\n", i, handles[i]);
        }
    }

cleanup:
    close(fd);
    printf("\n测试程序结束\n");
    return 0;
}
