/*
 * PDDGPU寄存器定义
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#ifndef __PDDGPU_REGS_H__
#define __PDDGPU_REGS_H__

#include <linux/types.h>

/* PDDGPU芯片ID */
#define PDDGPU_CHIP_ID_PDD1000    0x1000
#define PDDGPU_CHIP_ID_PDD2000    0x2000
#define PDDGPU_CHIP_ID_PDD3000    0x3000

/* PDDGPU PCI ID */
#define PDDGPU_VENDOR_ID          0x1002
#define PDDGPU_DEVICE_ID_PDD1000  0x1000
#define PDDGPU_DEVICE_ID_PDD2000  0x2000
#define PDDGPU_DEVICE_ID_PDD3000  0x3000

/* PDDGPU MMIO偏移 */
#define PDDGPU_MMIO_BASE          0x00000000
#define PDDGPU_MMIO_SIZE          0x1000000

/* 寄存器偏移 */
#define PDDGPU_REG_CHIP_ID        0x0000
#define PDDGPU_REG_CHIP_REV       0x0004
#define PDDGPU_REG_VRAM_SIZE      0x0008
#define PDDGPU_REG_GTT_SIZE       0x0010
#define PDDGPU_REG_STATUS         0x0014
#define PDDGPU_REG_CONTROL        0x0018

/* 寄存器位定义 */
#define PDDGPU_STATUS_READY       (1 << 0)
#define PDDGPU_STATUS_BUSY        (1 << 1)
#define PDDGPU_STATUS_ERROR       (1 << 2)

#define PDDGPU_CTRL_ENABLE        (1 << 0)
#define PDDGPU_CTRL_RESET         (1 << 1)
#define PDDGPU_CTRL_IRQ_ENABLE    (1 << 2)

/* 寄存器读写宏 */
#define PDDGPU_READ32(addr)       readl((addr))
#define PDDGPU_WRITE32(addr, val) writel((val), (addr))

#define PDDGPU_READ64(addr)       readq((addr))
#define PDDGPU_WRITE64(addr, val) writeq((val), (addr))

/* 寄存器字段宏 */
#define PDDGPU_REG_FIELD(reg, field) \
	(((reg) >> PDDGPU_##field##_SHIFT) & PDDGPU_##field##_MASK)

#define PDDGPU_REG_SET_FIELD(reg, field, val) \
	(((reg) & ~(PDDGPU_##field##_MASK << PDDGPU_##field##_SHIFT)) | \
	 (((val) & PDDGPU_##field##_MASK) << PDDGPU_##field##_SHIFT))

#endif /* __PDDGPU_REGS_H__ */
