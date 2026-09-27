/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Compatibility shim for the Rockchip RGA multi-core driver.
 *
 * The vendor rga3 sources were written for the Rockchip develop-5.10 tree.
 * They are built unchanged against the OpenWrt linux-5.4.215 kernel; this
 * header supplies the few kernel helpers that were added after v5.4 and are
 * used without a LINUX_VERSION_CODE guard in the vendor code.
 *
 * It is force-included from the rga3 Makefile (ccflags-y -include ...), so the
 * definitions are available in every translation unit of the module.
 */
#ifndef __RGA_VERSION_COMPAT_H__
#define __RGA_VERSION_COMPAT_H__

#include <linux/version.h>

/*
 * for_each_sgtable_sg() / for_each_sgtable_dma_sg() were introduced in v5.9.
 * rga_dma_buf.c uses for_each_sgtable_sg() to sum the mapped segment lengths
 * of a scatterlist; the v5.4 equivalent is for_each_sg() over sgt->nents,
 * which is exactly what the v5.9 helper expands to.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0)
#ifndef for_each_sgtable_sg
#define for_each_sgtable_sg(sgt, sg, i) \
	for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)
#endif

#ifndef for_each_sgtable_dma_sg
#define for_each_sgtable_dma_sg(sgt, sg, i) \
	for_each_sg((sgt)->sgl, sg, (sgt)->nents, i)
#endif
#endif /* LINUX_VERSION_CODE < 5.9 */

#endif /* __RGA_VERSION_COMPAT_H__ */
