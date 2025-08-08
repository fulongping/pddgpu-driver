/*
 * PDDGPU简单测试程序
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

/* PDDGPU IOCTL定义 */
#define PDDGPU_GEM_CREATE    0x00
#define PDDGPU_GEM_MAP       0x01
#define PDDGPU_GEM_INFO      0x02
#define PDDGPU_GEM_DESTROY   0x03

#define PDDGPU_GEM_DOMAIN_CPU    0x1
#define PDDGPU_GEM_DOMAIN_GTT    0x2
#define PDDGPU_GEM_DOMAIN_VRAM   0x4

/* IOCTL结构体 */
struct drm_pddgpu_gem_create {
    uint64_t size;
    uint32_t alignment;
    uint32_t domains;
    uint32_t flags;
    uint32_t handle;
    uint64_t pad;
};

struct drm_pddgpu_gem_map {
    uint32_t handle;
    uint32_t pad;
    uint64_t offset;
    uint64_t size;
    uint64_t flags;
};

struct drm_pddgpu_gem_info {
    uint32_t handle;
    uint32_t pad;
    uint64_t size;
    uint64_t offset;
    uint32_t domain;
    uint32_t flags;
};

/* DRM IOCTL宏 */
#define DRM_IOCTL_BASE 'P'
#define DRM_IOWR(nr, type) _IOWR(DRM_IOCTL_BASE, nr, type)
#define DRM_IOW(nr, type) _IOW(DRM_IOCTL_BASE, nr, type)

#define DRM_IOCTL_PDDGPU_GEM_CREATE  DRM_IOWR(PDDGPU_GEM_CREATE, struct drm_pddgpu_gem_create)
#define DRM_IOCTL_PDDGPU_GEM_MAP     DRM_IOWR(PDDGPU_GEM_MAP, struct drm_pddgpu_gem_map)
#define DRM_IOCTL_PDDGPU_GEM_INFO    DRM_IOWR(PDDGPU_GEM_INFO, struct drm_pddgpu_gem_info)
#define DRM_IOCTL_PDDGPU_GEM_DESTROY DRM_IOW(PDDGPU_GEM_DESTROY, struct drm_pddgpu_gem_create)

int main(int argc, char *argv[])
{
    int fd;
    struct drm_pddgpu_gem_create create = {};
    struct drm_pddgpu_gem_map map = {};
    struct drm_pddgpu_gem_info info = {};
    void *mapped_addr;
    int ret;
    
    printf("PDDGPU简单测试程序\n");
    
    /* 打开PDDGPU设备 */
    fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) {
        perror("Failed to open PDDGPU device");
        return -1;
    }
    
    printf("成功打开PDDGPU设备\n");
    
    /* 创建VRAM缓冲区 */
    create.size = 1024 * 1024;  // 1MB
    create.alignment = 4096;     // 4KB对齐
    create.domains = PDDGPU_GEM_DOMAIN_VRAM;
    create.flags = 0;
    
    ret = ioctl(fd, DRM_IOCTL_PDDGPU_GEM_CREATE, &create);
    if (ret < 0) {
        perror("Failed to create GEM object");
        close(fd);
        return -1;
    }
    
    printf("成功创建GEM对象: handle=%u, size=%llu\n", 
           create.handle, create.size);
    
    /* 映射缓冲区 */
    map.handle = create.handle;
    map.flags = 0;
    
    ret = ioctl(fd, DRM_IOCTL_PDDGPU_GEM_MAP, &map);
    if (ret < 0) {
        perror("Failed to map GEM object");
        goto cleanup;
    }
    
    printf("成功映射GEM对象: offset=%llu, size=%llu\n", 
           map.offset, map.size);
    
    /* 映射到用户空间 */
    mapped_addr = mmap(NULL, map.size, PROT_READ | PROT_WRITE, 
                       MAP_SHARED, fd, map.offset);
    if (mapped_addr == MAP_FAILED) {
        perror("Failed to mmap GEM object");
        goto cleanup;
    }
    
    printf("成功映射到用户空间: addr=%p\n", mapped_addr);
    
    /* 写入测试数据 */
    memset(mapped_addr, 0xAA, 1024);
    printf("写入测试数据完成\n");
    
    /* 读取并验证数据 */
    unsigned char *data = (unsigned char *)mapped_addr;
    int i;
    for (i = 0; i < 16; i++) {
        printf("%02x ", data[i]);
    }
    printf("\n数据验证完成\n");
    
    /* 获取GEM信息 */
    info.handle = create.handle;
    
    ret = ioctl(fd, DRM_IOCTL_PDDGPU_GEM_INFO, &info);
    if (ret < 0) {
        perror("Failed to get GEM info");
    } else {
        printf("GEM信息: size=%llu, offset=%llu, domain=0x%x, flags=0x%x\n",
               info.size, info.offset, info.domain, info.flags);
    }
    
    /* 清理 */
    munmap(mapped_addr, map.size);
    
cleanup:
    /* 销毁GEM对象 */
    create.handle = create.handle;  // 重新设置handle
    
    ret = ioctl(fd, DRM_IOCTL_PDDGPU_GEM_DESTROY, &create);
    if (ret < 0) {
        perror("Failed to destroy GEM object");
    } else {
        printf("成功销毁GEM对象\n");
    }
    
    close(fd);
    printf("测试完成\n");
    
    return 0;
}
