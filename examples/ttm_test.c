/*
 * PDDGPU TTM测试程序
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

#include "../include/pddgpu_drv.h"

#define DEVICE_PATH "/dev/dri/card0"
#define TEST_SIZE (1024 * 1024)  /* 1MB */

int main(int argc, char *argv[])
{
	int fd;
	struct drm_pddgpu_gem_create create_args = {};
	struct drm_pddgpu_gem_map map_args = {};
	struct drm_pddgpu_gem_info info_args = {};
	void *mapped_addr;
	int ret;

	printf("PDDGPU TTM测试程序\n");
	printf("==================\n");

	/* 打开设备 */
	fd = open(DEVICE_PATH, O_RDWR);
	if (fd < 0) {
		perror("Failed to open device");
		return -1;
	}

	printf("设备已打开: %s\n", DEVICE_PATH);

	/* 创建GEM对象 */
	create_args.size = TEST_SIZE;
	create_args.alignment = 4096;
	create_args.domains = PDDGPU_GEM_DOMAIN_VRAM | PDDGPU_GEM_DOMAIN_GTT;
	create_args.flags = PDDGPU_GEM_CREATE_VRAM_CLEARED;

	ret = ioctl(fd, DRM_IOCTL_PDDGPU_GEM_CREATE, &create_args);
	if (ret < 0) {
		perror("Failed to create GEM object");
		close(fd);
		return -1;
	}

	printf("GEM对象已创建: handle=%u, size=%llu\n", 
	       create_args.handle, create_args.size);

	/* 获取GEM对象信息 */
	info_args.handle = create_args.handle;
	ret = ioctl(fd, DRM_IOCTL_PDDGPU_GEM_INFO, &info_args);
	if (ret < 0) {
		perror("Failed to get GEM info");
		close(fd);
		return -1;
	}

	printf("GEM对象信息:\n");
	printf("  大小: %llu bytes\n", info_args.size);
	printf("  GPU地址: 0x%llx\n", info_args.offset);
	printf("  域: 0x%x\n", info_args.domain);
	printf("  标志: 0x%x\n", info_args.flags);

	/* 映射GEM对象 */
	map_args.handle = create_args.handle;
	map_args.offset = 0;
	map_args.size = TEST_SIZE;
	map_args.flags = 0;

	ret = ioctl(fd, DRM_IOCTL_PDDGPU_GEM_MAP, &map_args);
	if (ret < 0) {
		perror("Failed to map GEM object");
		close(fd);
		return -1;
	}

	printf("GEM对象已映射: 地址=0x%llx\n", map_args.offset);

	/* 测试写入数据 */
	mapped_addr = (void *)map_args.offset;
	if (mapped_addr != MAP_FAILED) {
		/* 写入测试数据 */
		memset(mapped_addr, 0xAA, TEST_SIZE);
		printf("已写入测试数据到映射地址\n");

		/* 验证数据 */
		unsigned char *data = (unsigned char *)mapped_addr;
		int i;
		for (i = 0; i < 16; i++) {
			printf("%02x ", data[i]);
		}
		printf("...\n");
	} else {
		printf("映射地址无效\n");
	}

	/* 清理 */
	printf("清理资源...\n");
	close(fd);

	printf("测试完成\n");
	return 0;
}
