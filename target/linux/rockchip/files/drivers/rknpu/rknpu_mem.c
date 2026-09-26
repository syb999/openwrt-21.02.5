// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co.Ltd
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 */

#include <linux/version.h>
#include <linux/uaccess.h>   /* copy_from_user/copy_to_user: only reached when the DMA-HEAP backend is selected */
#include <linux/rk-dma-heap.h>

#if KERNEL_VERSION(5, 10, 0) <= LINUX_VERSION_CODE
#include <linux/dma-map-ops.h>
#endif

#include "rknpu_drv.h"
#include "rknpu_ioctl.h"
#include "rknpu_mem.h"

#ifdef CONFIG_ROCKCHIP_RKNPU_DMA_HEAP

int rknpu_mem_create_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			   unsigned int cmd, unsigned long data)
{
	struct rknpu_mem_create args;
	int ret = -EINVAL;
	struct dma_buf_attachment *attachment;
	struct sg_table *table;
	struct scatterlist *sgl;
	dma_addr_t phys;
	struct dma_buf *dmabuf;
	struct page **pages;
	struct page *page;
	struct rknpu_mem_object *rknpu_obj = NULL;
	struct rknpu_session *session = NULL;
	int i, fd;
	unsigned int length, page_count;
	unsigned int in_size = _IOC_SIZE(cmd);
	unsigned int k_size = sizeof(struct rknpu_mem_create);
	char *k_data = (char *)&args;

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_create *)data,
				    in_size))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	if (k_size > in_size)
		memset(k_data + in_size, 0, k_size - in_size);

	if (args.flags & RKNPU_MEM_NON_CONTIGUOUS) {
		LOG_ERROR("%s: malloc iommu memory unsupported in current!\n",
			  __func__);
		ret = -EINVAL;
		return ret;
	}

	rknpu_obj = kzalloc(sizeof(*rknpu_obj), GFP_KERNEL);
	if (!rknpu_obj)
		return -ENOMEM;

	if (args.handle > 0) {
		fd = args.handle;

		dmabuf = dma_buf_get(fd);
		if (IS_ERR(dmabuf)) {
			ret = PTR_ERR(dmabuf);
			goto err_free_obj;
		}

		rknpu_obj->dmabuf = dmabuf;
		rknpu_obj->owner = 0;
	} else {
		/* Start test kernel alloc/free dma buf */
		dmabuf = rk_dma_heap_buffer_alloc(rknpu_dev->heap, args.size,
						  O_CLOEXEC | O_RDWR, 0x0,
						  dev_name(rknpu_dev->dev));
		if (IS_ERR(dmabuf)) {
			LOG_ERROR("dmabuf alloc failed, args.size = %llu\n",
				  args.size);
			ret = PTR_ERR(dmabuf);
			goto err_free_obj;
		}

		rknpu_obj->dmabuf = dmabuf;
		rknpu_obj->owner = 1;

		fd = dma_buf_fd(dmabuf, O_CLOEXEC | O_RDWR);
		if (fd < 0) {
			LOG_ERROR("dmabuf fd get failed\n");
			ret = -EFAULT;
			goto err_free_dma_buf;
		}
	}

	attachment = dma_buf_attach(dmabuf, rknpu_dev->dev);
	if (IS_ERR(attachment)) {
		LOG_ERROR("dma_buf_attach failed\n");
		ret = PTR_ERR(attachment);
		goto err_free_dma_buf;
	}

	table = dma_buf_map_attachment(attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR(table)) {
		LOG_ERROR("dma_buf_attach failed\n");
		dma_buf_detach(dmabuf, attachment);
		ret = PTR_ERR(table);
		goto err_free_dma_buf;
	}

	for_each_sgtable_sg(table, sgl, i) {
		phys = sg_dma_address(sgl);
		page = sg_page(sgl);
		length = sg_dma_len(sgl);
		LOG_DEBUG("%s, %d, phys: %pad, length: %u\n", __func__,
			  __LINE__, &phys, length);
	}

	if (args.flags & RKNPU_MEM_KERNEL_MAPPING) {
		page_count = length >> PAGE_SHIFT;
		pages = vmalloc(page_count * sizeof(struct page));
		if (!pages) {
			LOG_ERROR("alloc pages failed\n");
			ret = -ENOMEM;
			goto err_detach_dma_buf;
		}

		for (i = 0; i < page_count; i++)
			pages[i] = &page[i];

		rknpu_obj->kv_addr =
			vmap(pages, page_count, VM_MAP, PAGE_KERNEL);
		if (!rknpu_obj->kv_addr) {
			LOG_ERROR("vmap pages addr failed\n");
			ret = -ENOMEM;
			goto err_free_pages;
		}
		vfree(pages);
		pages = NULL;
	}

	rknpu_obj->size = PAGE_ALIGN(args.size);
	rknpu_obj->dma_addr = phys;
	rknpu_obj->sgt = table;
	/*
	 * Keep the attachment and its mapping for the whole lifetime of the
	 * object instead of tearing them down right here.  The sg_table that
	 * dma_buf_map_attachment() returned (and the dma_addr derived from it)
	 * is owned by the exporter per attachment: unmapping/detaching now
	 * would free that sg_table and leave rknpu_obj->sgt dangling.  NPU
	 * jobs submitted from userspace keep using this dma_addr (written to
	 * the hardware as the task/base address) for as long as the buffer
	 * object exists, so the mapping must stay valid until
	 * rknpu_mem_destroy_ioctl() releases it.
	 */
	rknpu_obj->attachment = attachment;

	args.size = rknpu_obj->size;
	args.obj_addr = (__u64)(uintptr_t)rknpu_obj;
	args.dma_addr = rknpu_obj->dma_addr;
	args.handle = fd;

	LOG_DEBUG(
		"args.handle: %d, args.size: %lld, rknpu_obj: %#llx, rknpu_obj->dma_addr: %#llx\n",
		args.handle, args.size, (__u64)(uintptr_t)rknpu_obj,
		(__u64)rknpu_obj->dma_addr);

	if (unlikely(copy_to_user((struct rknpu_mem_create *)data, &args,
				  in_size))) {
		LOG_ERROR("%s: copy_to_user failed\n", __func__);
		ret = -EFAULT;
		goto err_unmap_kv_addr;
	}

	spin_lock(&rknpu_dev->lock);

	session = file->private_data;
	if (!session) {
		spin_unlock(&rknpu_dev->lock);
		ret = -EFAULT;
		goto err_unmap_kv_addr;
	}
	list_add_tail(&rknpu_obj->head, &session->list);

	spin_unlock(&rknpu_dev->lock);

	return 0;

err_unmap_kv_addr:
	vunmap(rknpu_obj->kv_addr);
	rknpu_obj->kv_addr = NULL;

err_free_pages:
	vfree(pages);
	pages = NULL;

err_detach_dma_buf:
	dma_buf_unmap_attachment(attachment, table, DMA_BIDIRECTIONAL);
	dma_buf_detach(dmabuf, attachment);

err_free_dma_buf:
	if (rknpu_obj->owner)
		rk_dma_heap_buffer_free(dmabuf);
	else
		dma_buf_put(dmabuf);

err_free_obj:
	kfree(rknpu_obj);

	return ret;
}

int rknpu_mem_destroy_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			    unsigned long data)
{
	struct rknpu_mem_object *rknpu_obj, *entry, *q;
	struct rknpu_session *session = NULL;
	struct rknpu_mem_destroy args;
	int ret = -EFAULT;

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_destroy *)data,
				    sizeof(struct rknpu_mem_destroy)))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	if (!kern_addr_valid(args.obj_addr)) {
		LOG_ERROR("%s: invalid obj_addr: %#llx\n", __func__,
			  (__u64)(uintptr_t)args.obj_addr);
		ret = -EINVAL;
		return ret;
	}

	rknpu_obj = (struct rknpu_mem_object *)(uintptr_t)args.obj_addr;
	LOG_DEBUG(
		"free args.handle: %d, rknpu_obj: %#llx, rknpu_obj->dma_addr: %#llx\n",
		args.handle, (__u64)(uintptr_t)rknpu_obj,
		(__u64)rknpu_obj->dma_addr);

	spin_lock(&rknpu_dev->lock);
	session = file->private_data;
	if (!session) {
		spin_unlock(&rknpu_dev->lock);
		ret = -EFAULT;
		return ret;
	}
	list_for_each_entry_safe(entry, q, &session->list, head) {
		if (entry == rknpu_obj) {
			list_del(&entry->head);
			break;
		}
	}
	spin_unlock(&rknpu_dev->lock);

	if (rknpu_obj == entry) {
		vunmap(rknpu_obj->kv_addr);
		rknpu_obj->kv_addr = NULL;

		/*
		 * Release the mapping and the attachment that were kept alive
		 * since rknpu_mem_create_ioctl().  Must happen while the
		 * dmabuf is still referenced below.
		 */
		if (rknpu_obj->attachment && rknpu_obj->sgt)
			dma_buf_unmap_attachment(rknpu_obj->attachment,
						 rknpu_obj->sgt,
						 DMA_BIDIRECTIONAL);
		if (rknpu_obj->attachment)
			dma_buf_detach(rknpu_obj->dmabuf,
				       rknpu_obj->attachment);
		rknpu_obj->attachment = NULL;
		rknpu_obj->sgt = NULL;

		if (!rknpu_obj->owner)
			dma_buf_put(rknpu_obj->dmabuf);

		kfree(rknpu_obj);
	}

	return 0;
}

/*
 * Sync is delegated to the dma-buf exporter.
 *
 * This file used to contain an sg_table based helper
 * (rknpu_dma_buf_sync()) that pushed rknpu_obj->sgt / sg_dma_address()
 * into dma_sync_single_range_for_cpu()/dma_sync_single_range_for_device().
 * At the time that was unsafe, because rknpu_mem_create_ioctl() unmapped
 * and detached the dma_buf attachment right after importing the buffer:
 * exporters keep the mapping in a per-attachment sg_table (mainline
 * system_heap, rockchip rk_cma_heap) which is released by ->detach(), so
 * rknpu_obj->sgt was a dangling pointer and the stale sg_dma_address()
 * made dma_direct_sync_single_for_device() run cache maintenance on a
 * phys_to_virt() address that is not mapped, oopsing in
 * __clean_dcache_area_poc() ("Unable to handle kernel paging request") as
 * soon as RKNN synced a buffer allocated from /dev/dma_heap/system.
 *
 * The attachment is now kept alive for the whole object lifetime (see
 * rknpu_mem_create_ioctl()/rknpu_mem_destroy_ioctl()), so rknpu_obj->sgt
 * would be usable again.  Sync nevertheless goes through
 * dmabuf->ops->begin_cpu_access()/end_cpu_access() (vendor 5.10 uses the
 * ..._partial() variants): the exporter itself knows whether the buffer
 * needs cache maintenance and how it is mapped, so buffers that have no
 * NPU DMA mapping (mainline dma-heap) as well as the rockchip cma heap
 * path are both handled correctly, and it does not depend on the device
 * mapping being kept around.
 */

int rknpu_mem_sync_ioctl(struct rknpu_device *rknpu_dev, unsigned long data)
{
	struct rknpu_mem_object *rknpu_obj = NULL;
	struct rknpu_mem_sync args;
	struct dma_buf *dmabuf;
	int ret = -EFAULT;

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_sync *)data,
				    sizeof(struct rknpu_mem_sync)))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	if (!kern_addr_valid(args.obj_addr)) {
		LOG_ERROR("%s: invalid obj_addr: %#llx\n", __func__,
			  (__u64)(uintptr_t)args.obj_addr);
		ret = -EINVAL;
		return ret;
	}

	rknpu_obj = (struct rknpu_mem_object *)(uintptr_t)args.obj_addr;

	dmabuf = rknpu_obj->dmabuf;
	if (!dmabuf) {
		LOG_ERROR("%s: no dmabuf for obj_addr: %#llx\n", __func__,
			  (__u64)(uintptr_t)args.obj_addr);
		return -EINVAL;
	}

#ifndef CONFIG_DMABUF_PARTIAL
	/*
	 * 5.4 has no dma_buf_ops->begin_cpu_access_partial()/
	 * end_cpu_access_partial() (they arrived with the DMABUF_PARTIAL
	 * support, see 319-rockchip-dma-buf-heap.patch), so use the
	 * whole-buffer cpu access callbacks instead. They end up in the
	 * exporter of the buffer (rockchip cma heap, mainline
	 * /dev/dma_heap/system, ...) and are safe for buffers that are not
	 * DMA mapped to the NPU device. Without partial support
	 * args.offset/args.size cannot be expressed, so the buffer is synced
	 * as a whole.
	 */
	if (args.flags & RKNPU_MEM_SYNC_TO_DEVICE)
		dma_buf_end_cpu_access(dmabuf, DMA_TO_DEVICE);

	if (args.flags & RKNPU_MEM_SYNC_FROM_DEVICE)
		dma_buf_begin_cpu_access(dmabuf, DMA_FROM_DEVICE);
#else
	if (args.flags & RKNPU_MEM_SYNC_TO_DEVICE) {
		dmabuf->ops->end_cpu_access_partial(dmabuf, DMA_TO_DEVICE,
						    args.offset, args.size);
	}
	if (args.flags & RKNPU_MEM_SYNC_FROM_DEVICE) {
		dmabuf->ops->begin_cpu_access_partial(dmabuf, DMA_FROM_DEVICE,
						      args.offset, args.size);
	}
#endif

	return 0;
}

#endif
