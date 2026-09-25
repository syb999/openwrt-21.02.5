/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Compatibility shims for back-porting the vendor RKNPU driver (written for
 * 5.10) to this 5.4 kernel.  Included from rknpu_drv.h so that every driver
 * source file picks them up.
 *
 * Kept separate from the vendor sources on purpose: that way the diff against
 * rockchip-linux/kernel@develop-5.10 stays small and reviewable, and the
 * shims can be dropped one by one if the kernel tree is ever moved forward.
 */
#ifndef __RKNPU_COMPAT_H
#define __RKNPU_COMPAT_H

#include <linux/scatterlist.h>
#include <linux/version.h>
#include <linux/mm.h>
#include <linux/iommu.h>
#include <drm/drm_prime.h>

/*
 * iommu_flush_iotlb_all() is what iommu_flush_tlb_all() was renamed to in 5.8.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
#define iommu_flush_iotlb_all(domain)	iommu_flush_tlb_all(domain)
#endif

/*
 * vm_flags_set()/vm_flags_clear() helpers appeared in 6.1; before that the
 * flags were poked directly.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0)
static inline void vm_flags_set(struct vm_area_struct *vma, unsigned long flags)
{
	vma->vm_flags |= flags;
}

static inline void vm_flags_clear(struct vm_area_struct *vma, unsigned long flags)
{
	vma->vm_flags &= ~flags;
}
#endif

/*
 * for_each_sgtable_sg() was added in 5.10; on 5.4 the same iteration is done
 * straight over the scatterlist of the sg_table.
 */
#ifndef for_each_sgtable_sg
#define for_each_sgtable_sg(sgt, sg, i) \
	for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)
#endif

/*
 * drm_prime_sg_to_page_array() was split out of drm_prime_sg_to_page_addr_arrays()
 * in 5.9; the latter is what this kernel provides.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0)
static inline int drm_prime_sg_to_page_array(struct sg_table *sgt,
					     struct page **pages,
					     int max_entries)
{
	return drm_prime_sg_to_page_addr_arrays(sgt, pages, NULL, max_entries);
}
#endif

/*
 * The vendor tree also provides rockchip_nvmem_cell_read_u8() as a shorthand
 * for the nvmem consume API.  Only rknpu_get_invalid_core_mask() uses it, and
 * only when the board DT carries a "cores" nvmem cell (the multi-core RK3588
 * NPU); a plain lookup gives the same behaviour here.
 */
#include <linux/nvmem-consumer.h>

static inline int rockchip_nvmem_cell_read_u8(struct device_node *np,
					      const char *name, u8 *val)
{
	struct nvmem_cell *cell;
	void *buf;
	size_t len;
	int ret;

	cell = of_nvmem_cell_get(np, name);
	if (IS_ERR(cell))
		return PTR_ERR(cell);

	buf = nvmem_cell_read(cell, &len);
	nvmem_cell_put(cell);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	ret = len < sizeof(*val) ? -EINVAL : 0;
	if (!ret)
		*val = *(u8 *)buf;

	kfree(buf);

	return ret;
}

#endif /* __RKNPU_COMPAT_H */
