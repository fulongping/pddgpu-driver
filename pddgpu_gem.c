/*
 * PDDGPU GEM接口
 *
 * Copyright (C) 2024 PDDGPU Project
 */

#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>

#include "include/pddgpu_drv.h"
#include "pddgpu_object.h"

/* GEM创建对象 */
struct drm_gem_object *pddgpu_gem_create_object(struct drm_device *dev, size_t size)
{
	struct pddgpu_bo *bo;
	
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return NULL;
	
	bo->base.base.size = size;
	bo->base.base.funcs = &pddgpu_gem_object_funcs;
	
	return &bo->base.base;
}

/* GEM对象函数 */
static const struct drm_gem_object_funcs pddgpu_gem_object_funcs = {
	.free = pddgpu_gem_free_object,
	.print_info = pddgpu_gem_print_info,
	.export = pddgpu_gem_prime_export,
	.vmap = pddgpu_gem_prime_vmap,
	.vunmap = pddgpu_gem_prime_vunmap,
	.mmap = pddgpu_gem_prime_mmap,
};

/* GEM创建IOCTL */
int pddgpu_gem_create_ioctl(struct drm_device *dev, void *data,
                            struct drm_file *filp)
{
	struct pddgpu_device *pdev = to_pddgpu_device(dev);
	struct drm_pddgpu_gem_create *args = data;
	struct pddgpu_bo_param bp = {};
	struct pddgpu_bo *bo;
	struct drm_gem_object *gobj;
	int ret;
	
	PDDGPU_DEBUG("GEM create: size=%llu, alignment=%u, domains=0x%x, flags=0x%x\n",
	             args->size, args->alignment, args->domains, args->flags);
	
	/* 验证参数 */
	if (args->size == 0 || args->size > PDDGPU_MAX_BO_SIZE) {
		PDDGPU_ERROR("Invalid buffer size: %llu\n", args->size);
		return -EINVAL;
	}
	
	if (args->alignment > PDDGPU_MAX_ALIGNMENT) {
		PDDGPU_ERROR("Invalid alignment: %u\n", args->alignment);
		return -EINVAL;
	}
	
	/* 设置创建参数 */
	bp.size = args->size;
	bp.alignment = args->alignment;
	bp.domain = args->domains;
	bp.flags = args->flags;
	bp.type = ttm_bo_type_device;
	bp.resv = NULL;
	bp.bo_ptr_size = sizeof(struct pddgpu_bo);
	bp.destroy = pddgpu_bo_destroy;
	
	/* 创建缓冲区对象 */
	ret = pddgpu_bo_create(pdev, &bp, &bo);
	if (ret) {
		PDDGPU_ERROR("Failed to create BO: %d\n", ret);
		return ret;
	}
	
	gobj = &bo->base.base;
	
	/* 创建句柄 */
	ret = drm_gem_handle_create(filp, gobj, &args->handle);
	if (ret) {
		PDDGPU_ERROR("Failed to create handle: %d\n", ret);
		pddgpu_bo_unref(&bo);
		return ret;
	}
	
	PDDGPU_DEBUG("GEM created: handle=%u, size=%llu\n", args->handle, args->size);
	
	return 0;
}

/* GEM映射IOCTL */
int pddgpu_gem_map_ioctl(struct drm_device *dev, void *data,
                          struct drm_file *filp)
{
	struct drm_pddgpu_gem_map *args = data;
	struct drm_gem_object *gobj;
	struct pddgpu_bo *bo;
	int ret;
	
	PDDGPU_DEBUG("GEM map: handle=%u, offset=%llu, size=%llu, flags=0x%llx\n",
	             args->handle, args->offset, args->size, args->flags);
	
	/* 获取GEM对象 */
	gobj = drm_gem_object_lookup(filp, args->handle);
	if (!gobj) {
		PDDGPU_ERROR("Invalid handle: %u\n", args->handle);
		return -ENOENT;
	}
	
	bo = to_pddgpu_bo(gobj);
	
	/* 验证参数 */
	if (args->offset + args->size > gobj->size) {
		PDDGPU_ERROR("Invalid mapping range\n");
		drm_gem_object_put(gobj);
		return -EINVAL;
	}
	
	/* 映射BO */
	ret = ttm_bo_kmap(&bo->tbo, args->offset, args->size, &bo->kptr);
	if (ret) {
		PDDGPU_ERROR("Failed to map BO: %d\n", ret);
		drm_gem_object_put(gobj);
		return ret;
	}
	
	/* 返回映射地址 */
	args->offset = (u64)bo->kptr;
	
	drm_gem_object_put(gobj);
	
	PDDGPU_DEBUG("GEM mapped: addr=0x%llx\n", args->offset);
	
	return 0;
}

/* GEM信息IOCTL */
int pddgpu_gem_info_ioctl(struct drm_device *dev, void *data,
                           struct drm_file *filp)
{
	struct drm_pddgpu_gem_info *args = data;
	struct drm_gem_object *gobj;
	struct pddgpu_bo *bo;
	
	PDDGPU_DEBUG("GEM info: handle=%u\n", args->handle);
	
	/* 获取GEM对象 */
	gobj = drm_gem_object_lookup(filp, args->handle);
	if (!gobj) {
		PDDGPU_ERROR("Invalid handle: %u\n", args->handle);
		return -ENOENT;
	}
	
	bo = to_pddgpu_bo(gobj);
	
	/* 填充信息 */
	args->size = gobj->size;
	args->offset = ttm_bo_gpu_offset(&bo->tbo);
	args->domain = bo->domain;
	args->flags = bo->flags;
	
	drm_gem_object_put(gobj);
	
	PDDGPU_DEBUG("GEM info: size=%llu, offset=0x%llx, domain=0x%x\n",
	             args->size, args->offset, args->domain);
	
	return 0;
}

/* GEM销毁IOCTL */
int pddgpu_gem_destroy_ioctl(struct drm_device *dev, void *data,
                              struct drm_file *filp)
{
	struct drm_pddgpu_gem_create *args = data;
	struct drm_gem_object *gobj;
	struct pddgpu_bo *bo;
	
	PDDGPU_DEBUG("GEM destroy: handle=%u\n", args->handle);
	
	/* 获取GEM对象 */
	gobj = drm_gem_object_lookup(filp, args->handle);
	if (!gobj) {
		PDDGPU_ERROR("Invalid handle: %u\n", args->handle);
		return -ENOENT;
	}
	
	bo = to_pddgpu_bo(gobj);
	
	/* 释放BO */
	pddgpu_bo_unref(&bo);
	
	/* 释放GEM对象 */
	drm_gem_object_put(gobj);
	
	PDDGPU_DEBUG("GEM destroyed: handle=%u\n", args->handle);
	
	return 0;
}

/* GEM对象打开 */
int pddgpu_gem_open_object(struct drm_gem_object *obj, struct drm_file *file)
{
	struct pddgpu_bo *bo = to_pddgpu_bo(obj);
	
	PDDGPU_DEBUG("GEM open object: %p\n", obj);
	
	/* 增加引用计数 */
	ttm_bo_get(&bo->tbo);
	
	return 0;
}

/* GEM对象关闭 */
void pddgpu_gem_close_object(struct drm_gem_object *obj, struct drm_file *file)
{
	struct pddgpu_bo *bo = to_pddgpu_bo(obj);
	
	PDDGPU_DEBUG("GEM close object: %p\n", obj);
	
	/* 减少引用计数 */
	ttm_bo_put(&bo->tbo);
}

/* GEM对象释放 */
void pddgpu_gem_free_object(struct drm_gem_object *obj)
{
	struct pddgpu_bo *bo = to_pddgpu_bo(obj);
	
	PDDGPU_DEBUG("GEM free object: %p\n", obj);
	
	/* 释放BO */
	pddgpu_bo_unref(&bo);
}

/* GEM对象信息打印 */
void pddgpu_gem_print_info(struct drm_printer *p, unsigned int indent,
                           const struct drm_gem_object *obj)
{
	struct pddgpu_bo *bo = to_pddgpu_bo(obj);
	
	drm_printf_indent(p, indent, "PDDGPU BO:\n");
	drm_printf_indent(p, indent + 1, "Size: %zu\n", obj->size);
	drm_printf_indent(p, indent + 1, "Domain: 0x%x\n", bo->domain);
	drm_printf_indent(p, indent + 1, "Flags: 0x%x\n", bo->flags);
	drm_printf_indent(p, indent + 1, "Pin count: %u\n", bo->pin_count);
}

/* GEM Prime导出 */
struct dma_buf *pddgpu_gem_prime_export(struct drm_gem_object *obj, int flags)
{
	struct pddgpu_bo *bo = to_pddgpu_bo(obj);
	
	PDDGPU_DEBUG("GEM prime export: %p\n", obj);
	
	/* 使用TTM的DMA-BUF导出 */
	return drm_gem_dmabuf_export(obj->dev, obj, flags);
}

/* GEM Prime vmap */
int pddgpu_gem_prime_vmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	struct pddgpu_bo *bo = to_pddgpu_bo(obj);
	int ret;
	
	PDDGPU_DEBUG("GEM prime vmap: %p\n", obj);
	
	/* 映射BO */
	ret = ttm_bo_kmap(&bo->tbo, 0, obj->size, &bo->kptr);
	if (ret) {
		PDDGPU_ERROR("Failed to map BO: %d\n", ret);
		return ret;
	}
	
	/* 设置映射信息 */
	iosys_map_set_vaddr(map, bo->kptr);
	
	return 0;
}

/* GEM Prime vunmap */
void pddgpu_gem_prime_vunmap(struct drm_gem_object *obj, struct iosys_map *map)
{
	struct pddgpu_bo *bo = to_pddgpu_bo(obj);
	
	PDDGPU_DEBUG("GEM prime vunmap: %p\n", obj);
	
	/* 取消映射 */
	if (bo->kptr) {
		ttm_bo_kunmap(&bo->tbo, 0);
		bo->kptr = NULL;
	}
}

/* GEM Prime mmap */
int pddgpu_gem_prime_mmap(struct drm_gem_object *obj, struct vm_area_struct *vma)
{
	struct pddgpu_bo *bo = to_pddgpu_bo(obj);
	int ret;
	
	PDDGPU_DEBUG("GEM prime mmap: %p\n", obj);
	
	/* 使用TTM的mmap */
	ret = ttm_bo_mmap(&bo->tbo, vma);
	if (ret) {
		PDDGPU_ERROR("Failed to mmap BO: %d\n", ret);
		return ret;
	}
	
	return 0;
}
