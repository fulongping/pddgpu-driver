# PDDGPU 测试程序 Makefile
#
# Copyright (C) 2024 PDDGPU Project

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -pthread
LDFLAGS = -pthread

TARGETS = memory_leak_test concurrency_test
SOURCES = memory_leak_test.c concurrency_test.c

.PHONY: all clean

all: $(TARGETS)

memory_leak_test: memory_leak_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

concurrency_test: concurrency_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)

install: all
	@echo "编译完成，测试程序已生成"
	@echo "使用方法:"
	@echo "  sudo ./memory_leak_test    # 内存泄漏测试"
	@echo "  sudo ./concurrency_test    # 并发测试"
	@echo ""
	@echo "注意: 需要先加载 PDDGPU 驱动模块"
	@echo "  sudo insmod ../pddgpu.ko"
