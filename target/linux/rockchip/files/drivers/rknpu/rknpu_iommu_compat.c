// SPDX-License-Identifier: GPL-2.0
/*
 * Non-IOMMU compatibility layer for the RKNPU driver back-ported to this 5.4
 * kernel.
 *
 * The vendor driver's own rknpu_iommu.c is built on the DMA-IOMMU internals
 * that the Rockchip 5.10 tree patches into drivers/iommu/dma-iommu.c
 * (struct iommu_domain::iova_cookie, iommu_domain_num(), iommu_domain_refcount(),
 * ...).  None of that exists in mainline 5.4, so this driver is built and run
 * in its non-IOMMU mode instead: the board DTS does not point the NPU at an
 * IOMMU and the NPU uses plain DMA/physical addresses.
 *
 * In that mode the vendor code only ever needs the two trivial halves of its
 * mapping helpers - it literally falls back to dma_map_sg()/dma_unmap_sg()
 * whenever the caller asks for an aligned IOVA - plus a set of domain helpers.
 * Most of those are guarded with rknpu_dev->iommu_en and therefore never run
 * here; rknpu_iommu_domain_get_and_switch() is the exception, it is called
 * unconditionally from the submit path and must succeed as a no-op (see the
 * comment on its definition).
 *
 * Reintegrating the IOMMU path later means porting the vendor's dma-iommu
 * changes and dropping this file in favour of drivers/rknpu/rknpu_iommu.c.
 */

#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/slab.h>

#include "rknpu_iommu.h"

int rknpu_iommu_dma_map_sg(struct device *dev, struct scatterlist *sg,
			   int nents, enum dma_data_direction dir,
			   bool iova_aligned)
{
	/* No IOMMU domain: map straight through to the DMA API. */
	return dma_map_sg(dev, sg, nents, dir);
}

void rknpu_iommu_dma_unmap_sg(struct device *dev, struct scatterlist *sg,
			      int nents, enum dma_data_direction dir,
			      bool iova_aligned)
{
	dma_unmap_sg(dev, sg, nents, dir);
}

/*
 * The remaining helpers only ever run in IOMMU mode.  They are defined here so
 * that the build stays warning-free and a future IOMMU port shows up as a
 * clear failure rather than a silent one.
 */
dma_addr_t rknpu_iommu_dma_alloc_iova(struct iommu_domain *domain, size_t size,
				      u64 dma_limit, struct device *dev,
				      bool size_aligned)
{
	return 0;
}

void rknpu_iommu_dma_free_iova(struct rknpu_iommu_dma_cookie *cookie,
			       dma_addr_t iova, size_t size, bool size_aligned)
{
}

int rknpu_iommu_init_domain(struct rknpu_device *rknpu_dev)
{
	return -EOPNOTSUPP;
}

int rknpu_iommu_switch_domain(struct rknpu_device *rknpu_dev, int domain_id)
{
	return -EOPNOTSUPP;
}

void rknpu_iommu_free_domains(struct rknpu_device *rknpu_dev)
{
}

int rknpu_iommu_domain_get_and_switch(struct rknpu_device *rknpu_dev,
				      int domain_id)
{
	/*
	 * This round-trip is needed to acquire a reference on the IOMMU
	 * domain a job wants to run against, which only exists in IOMMU mode.
	 *
	 * It is *not* IOMMU-specific: rknpu_job_schedule() calls it
	 * unconditionally and treats a non-zero return value as a fatal
	 * submission error, and rknpu_drv.c's RKNPU_SET_IOMMU_DOMAIN_ID action
	 * calls it as well.  The vendor's own non-IOMMU build
	 * (drivers/rknpu/rknpu_iommu.c, "#else" branch) just returns 0 here
	 * for exactly that reason.  Returning -EOPNOTSUPP instead made
	 * rknpu_job_schedule() bail out with job->ret = -EINVAL before the
	 * job was ever queued or committed, so the commit never ran, the
	 * driver logged "job commit failed"/"job abort, ret: -22" and RKNN
	 * surfaced the failure as "rknn run error -1".
	 *
	 * Without an IOMMU there is nothing to switch to; the caller's
	 * matching rknpu_iommu_domain_put() is a no-op too.
	 */
	return 0;
}

int rknpu_iommu_domain_put(struct rknpu_device *rknpu_dev)
{
	return 0;
}
