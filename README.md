# PDDGPU 驱动

一个基于AMDGPU架构的简单GPU驱动实现，用于学习和研究目的。

## 特性

- 基于DRM (Direct Rendering Manager) 框架
- 支持TTM (Translation Table Manager) 内存管理
- 实现GEM (Graphics Execution Manager) 接口
- PCI设备管理和MMIO寄存器访问
- VRAM内存分配和管理
- 用户空间IOCTL接口

## 目录结构

```
pddgpu_driver/
├── include/                 # 头文件
│   ├── pddgpu_drv.h        # 主要驱动头文件
│   └── pddgpu_regs.h       # 寄存器定义
├── pddgpu_drv.c            # 主驱动文件
├── pddgpu_device.c         # 设备初始化
├── pddgpu_gem.c            # GEM对象管理
├── examples/               # 示例代码
│   └── simple_test.c       # 简单测试程序
├── docs/                   # 文档
│   └── ARCHITECTURE.md     # 架构文档
├── Makefile                # 构建系统
├── Kconfig                 # 内核配置
└── README.md               # 本文件
```

## 编译

### 前置条件

- Linux内核源码树
- GCC编译器
- Make构建工具

### 编译步骤

```bash
# 进入内核源码目录
cd /path/to/linux/source

# 编译模块
make M=/path/to/pddgpu_driver

# 或者使用Kbuild系统
make modules SUBDIRS=/path/to/pddgpu_driver
```

## 安装

```bash
# 安装模块
sudo make M=/path/to/pddgpu_driver modules_install

# 加载模块
sudo modprobe pddgpu

# 检查模块状态
lsmod | grep pddgpu
```

## 使用示例

```c
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <drm/drm.h>

int main() {
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) {
        perror("Failed to open DRM device");
        return -1;
    }
    
    // 创建GEM对象
    struct drm_pddgpu_gem_create create = {
        .size = 1024 * 1024,  // 1MB
        .domains = PDDGPU_GEM_DOMAIN_VRAM,
        .flags = 0
    };
    
    if (ioctl(fd, DRM_IOCTL_PDDGPU_GEM_CREATE, &create) < 0) {
        perror("Failed to create GEM object");
        return -1;
    }
    
    printf("Created GEM object with handle: %u\n", create.handle);
    close(fd);
    return 0;
}
```

## 调试

启用调试输出：

```bash
# 加载模块时启用调试
sudo modprobe pddgpu debug=1

# 查看内核日志
dmesg | grep PDDGPU
```

## 许可证

本项目采用GPL v2许可证。详见LICENSE文件。

## 贡献

欢迎提交Issue和Pull Request！

## GitHub仓库设置

要将此项目推送到GitHub：

1. 在GitHub上创建新仓库
2. 添加远程仓库：
   ```bash
   git remote add origin https://github.com/yourusername/pddgpu-driver.git
   ```
3. 推送代码：
   ```bash
   git push -u origin master
   ```

## 注意事项

- 这是一个学习和研究项目，不建议在生产环境中使用
- 需要根据实际的GPU硬件调整寄存器定义和内存管理
- 建议在虚拟机或测试环境中进行开发和测试
# pddgpu-driver
