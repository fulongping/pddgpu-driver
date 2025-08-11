# PDDGPU驱动 Makefile

obj-m := pddgpu.o

pddgpu-objs := pddgpu_drv.o \
                pddgpu_device.o \
                pddgpu_gem.o \
                pddgpu_object.o \
                pddgpu_vram_mgr.o \
                pddgpu_gtt_mgr.o \
                pddgpu_memory_stats.o


# 内核源码路径
KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

# 编译标志
ccflags-y := -I$(src)/include -DCONFIG_DRM_PDDGPU=1

# 默认目标
all: modules

# 编译模块
modules:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

# 清理
clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

# 安装模块
install:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules_install

# 卸载模块
uninstall:
	rm -f /lib/modules/$(shell uname -r)/extra/pddgpu.ko

# 帮助
help:
	@echo "可用目标:"
	@echo "  all      - 编译模块"
	@echo "  clean    - 清理编译文件"
	@echo "  install  - 安装模块"
	@echo "  uninstall- 卸载模块"
	@echo "  help     - 显示此帮助"

.PHONY: all modules clean install uninstall help
