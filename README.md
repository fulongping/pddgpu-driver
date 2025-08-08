# PDDGPU 驱动

这是一个简单的GPU驱动示例，模仿AMDGPU的内存管理和PCI设备管理方式。

## 特性

- PCI设备管理
- VRAM内存管理（基于TTM框架）
- GEM对象管理
- 用户空间接口
- 内核空间接口

## 目录结构

```
pddgpu_driver/
├── README.md
├── Makefile
├── Kconfig
├── pddgpu_drv.c          # 主驱动文件
├── pddgpu_device.c       # 设备管理
├── pddgpu_gmc.c          # 图形内存控制器
├── pddgpu_gmc.h
├── pddgpu_object.c       # 缓冲区对象管理
├── pddgpu_object.h
├── pddgpu_gem.c          # GEM接口
├── pddgpu_gem.h
├── pddgpu_ttm.c          # TTM内存管理
├── pddgpu_ttm.h
├── pddgpu_vram_mgr.c     # VRAM管理器
├── pddgpu_vram_mgr.h
└── include/
    ├── pddgpu_drv.h      # 驱动头文件
    └── pddgpu_regs.h     # 寄存器定义
```

## 编译和安装

```bash
# 编译驱动
make

# 安装驱动
sudo insmod pddgpu.ko

# 卸载驱动
sudo rmmod pddgpu
```

## 使用示例

```c
// 用户空间创建缓冲区
#include <xf86drm.h>
#include <xf86drmMode.h>

int fd = drmOpen("pddgpu", NULL);
uint32_t handle;
uint64_t size = 1024 * 1024; // 1MB

struct drm_pddgpu_gem_create create = {
    .size = size,
    .domain = PDDGPU_GEM_DOMAIN_VRAM,
    .flags = 0
};

drmIoctl(fd, DRM_IOCTL_PDDGPU_GEM_CREATE, &create);
handle = create.handle;
```

## 许可证

GPL v2
