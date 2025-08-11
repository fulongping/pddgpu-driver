/*
 * PDDGPU寄存器定义
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#ifndef __PDDGPU_REGS_H__
#define __PDDGPU_REGS_H__

#include <linux/types.h>

/* 芯片ID定义 */
#define PDDGPU_CHIP_ID_PDD1000    0x1000
#define PDDGPU_CHIP_ID_PDD2000    0x2000
#define PDDGPU_CHIP_ID_PDD3000    0x3000

/* 硬件寄存器偏移 */
#define PDDGPU_REG_CHIP_ID        0x0000   // 芯片ID寄存器
#define PDDGPU_REG_CHIP_REV       0x0004   // 芯片版本寄存器
#define PDDGPU_REG_VRAM_SIZE      0x0010   // VRAM大小寄存器
#define PDDGPU_REG_GTT_SIZE       0x0018   // GTT大小寄存器
#define PDDGPU_REG_VRAM_START     0x0020   // VRAM起始地址寄存器
#define PDDGPU_REG_VRAM_END       0x0028   // VRAM结束地址寄存器
#define PDDGPU_REG_GTT_START      0x0030   // GTT起始地址寄存器
#define PDDGPU_REG_GTT_END        0x0038   // GTT结束地址寄存器

/* 内存控制器寄存器 */
#define PDDGPU_REG_MC_VRAM_CTRL   0x0100   // VRAM控制寄存器
#define PDDGPU_REG_MC_GTT_CTRL    0x0104   // GTT控制寄存器
#define PDDGPU_REG_MC_FB_CTRL     0x0108   // 帧缓冲区控制寄存器

/* 中断控制器寄存器 */
#define PDDGPU_REG_IH_RING_BASE   0x0200   // 中断处理环基地址
#define PDDGPU_REG_IH_RING_SIZE   0x0204   // 中断处理环大小
#define PDDGPU_REG_IH_RING_WPTR   0x0208   // 中断处理环写指针
#define PDDGPU_REG_IH_RING_RPTR   0x020C   // 中断处理环读指针

/* 命令处理器寄存器 */
#define PDDGPU_REG_CP_RING_BASE   0x0300   // 命令处理器环基地址
#define PDDGPU_REG_CP_RING_SIZE   0x0304   // 命令处理器环大小
#define PDDGPU_REG_CP_RING_WPTR   0x0308   // 命令处理器环写指针
#define PDDGPU_REG_CP_RING_RPTR   0x030C   // 命令处理器环读指针

/* 寄存器位定义 */
#define PDDGPU_MC_VRAM_CTRL_ENABLE    0x00000001
#define PDDGPU_MC_VRAM_CTRL_ECC       0x00000002
#define PDDGPU_MC_GTT_CTRL_ENABLE     0x00000001
#define PDDGPU_MC_FB_CTRL_ENABLE      0x00000001

/* 内存域定义 */
#define PDDGPU_GEM_DOMAIN_CPU         0x00000001
#define PDDGPU_GEM_DOMAIN_GTT         0x00000002
#define PDDGPU_GEM_DOMAIN_VRAM        0x00000004

/* 内存标志定义 */
#define PDDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED    0x00000001
#define PDDGPU_GEM_CREATE_NO_CPU_ACCESS          0x00000002
#define PDDGPU_GEM_CREATE_CP_MQD_GFX             0x00000004
#define PDDGPU_GEM_CREATE_FLAG_NO_DEFER          0x00000008
#define PDDGPU_GEM_CREATE_VRAM_CLEARED           0x00000010
#define PDDGPU_GEM_CREATE_VM_ALWAYS_VALID        0x00000020
#define PDDGPU_GEM_CREATE_EXPLICIT_SYNC          0x00000040

/* 内存类型定义 */
#define PDDGPU_PL_SYSTEM             0
#define PDDGPU_PL_TT                 1
#define PDDGPU_PL_VRAM               2

/* 寄存器读写宏 */
#define PDDGPU_READ32(pdev, reg)          readl((pdev)->rmmio + (reg))
#define PDDGPU_WRITE32(pdev, reg, val)    writel((val), (pdev)->rmmio + (reg))
#define PDDGPU_READ64(pdev, reg)          readq((pdev)->rmmio + (reg))
#define PDDGPU_WRITE64(pdev, reg, val)    writeq((val), (pdev)->rmmio + (reg))

/* 寄存器字段定义 */
#define PDDGPU_REG_FIELD_MASK(reg, field) \
    (((1ULL << PDDGPU_##reg##_##field##_SIZE) - 1) << PDDGPU_##reg##_##field##_SHIFT)

#define PDDGPU_REG_FIELD_GET(reg, field, val) \
    (((val) & PDDGPU_REG_FIELD_MASK(reg, field)) >> PDDGPU_##reg##_##field##_SHIFT)

#define PDDGPU_REG_FIELD_SET(reg, field, val) \
    (((val) << PDDGPU_##reg##_##field##_SHIFT) & PDDGPU_REG_FIELD_MASK(reg, field))

/* 内存对齐定义 */
#define PDDGPU_PAGE_SIZE             4096
#define PDDGPU_PAGE_SHIFT            12
#define PDDGPU_PAGE_MASK             0xFFFFFFFFFFFFF000ULL

/* 内存限制定义 */
#define PDDGPU_MAX_VRAM_SIZE         (8ULL << 30)  // 8GB
#define PDDGPU_MAX_GTT_SIZE          (4ULL << 30)  // 4GB
#define PDDGPU_MAX_BO_SIZE           (1ULL << 30)  // 1GB
#define PDDGPU_MAX_ALIGNMENT         (1 << 20)     // 1MB

/* 内存统计定义 */
#define PDDGPU_VRAM_USAGE_SIZE       (256ULL << 20)  // 256MB
#define PDDGPU_GTT_USAGE_SIZE        (128ULL << 20)  // 128MB

#endif /* __PDDGPU_REGS_H__ */
