/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Placeholder for the vendor kernel's include/linux/version_compat_defs.h.
 *
 * Only rknpu_gem.c includes that header, and none of the compatibility
 * definitions it carries (bitmap_get_value8, read_poll_timeout_atomic,
 * kbase_*, mali_sysfs_emit, ...) are referenced by any RKNPU source file -
 * they exist for other Rockchip drivers in the vendor tree.  Living in the
 * driver's own include directory keeps the kernel tree untouched and the
 * back-ported sources unmodified.
 */
#ifndef _VERSION_COMPAT_DEFS_H_
#define _VERSION_COMPAT_DEFS_H_

#endif /* _VERSION_COMPAT_DEFS_H_ */
