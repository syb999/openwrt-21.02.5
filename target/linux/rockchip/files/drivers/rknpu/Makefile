# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_ROCKCHIP_RKNPU) += rknpu.o

ccflags-y += -I$(srctree)/$(src)/include
ccflags-y += -I$(src)/include
# Temporarily disabled while the driver is being back-ported to 5.4 so that the
# full warning list is visible; re-enabled before the final build.
#ccflags-y += -Werror

rknpu-y += rknpu_drv.o
rknpu-y += rknpu_reset.o
rknpu-y += rknpu_job.o
rknpu-y += rknpu_debugger.o
# rknpu_iommu.o implements the IOMMU path against the Rockchip 5.10 dma-iommu
# internals, which do not exist in this 5.4 kernel; the driver therefore runs
# in its non-IOMMU mode and uses the small compat layer instead.
rknpu-y += rknpu_iommu_compat.o
# rknpu_devfreq.o needs the vendor OPP/DVFS + IPA + system-monitor frameworks
# (leakage and pvtpll based voltage selection), none of which are part of this
# port yet.  The locking/suspend helpers the rest of the driver calls are
# provided by rknpu_devfreq_compat.o instead.  Without devfreq the NPU runs at
# the clock rate the CRU provides and with whatever voltage the board rail is
# set to.
#rknpu-$(CONFIG_PM_DEVFREQ) += rknpu_devfreq.o
rknpu-y += rknpu_devfreq_compat.o
rknpu-$(CONFIG_ROCKCHIP_RKNPU_SRAM) += rknpu_mm.o
rknpu-$(CONFIG_ROCKCHIP_RKNPU_FENCE) += rknpu_fence.o
rknpu-$(CONFIG_ROCKCHIP_RKNPU_DRM_GEM) += rknpu_gem.o
rknpu-$(CONFIG_ROCKCHIP_RKNPU_DMA_HEAP) += rknpu_mem.o
