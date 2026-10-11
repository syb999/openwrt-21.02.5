// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
/*
 * linux/fitblk.h 是内核 uapi 头，由本 target 的补丁
 * 063-block-add-uImage-FIT-subimage-block-driver.patch 新增、只存在于内核树里。
 * 21.02.5 的 toolchain 头文件目录尚未包含它（要重编 toolchain 才会带上），
 * 因此这里"有则用、无则按内核同名头补定义"。
 */
#if defined(__has_include)
# if __has_include(<linux/fitblk.h>)
#  include <linux/fitblk.h>
# endif
#endif
#ifndef FITBLK_RELEASE
#define FITBLK_RELEASE 0x4600
#endif

static int fitblk_release(char *device)
{
	int fd, ret;

	fd = open(device, O_RDONLY);
	if (fd == -1)
		return errno;

	ret = ioctl(fd, FITBLK_RELEASE, NULL);
	close(fd);

	if (ret == -1)
		return errno;

	return 0;
}

int main(int argc, char *argp[])
{
	int ret;

	if (argc != 2) {
		fprintf(stderr, "Release uImage.FIT sub-image block device\n");
		fprintf(stderr, "Syntax: %s /dev/fitXXX\n", argp[0]);
		return -EINVAL;
	}

	ret = fitblk_release(argp[1]);
	if (ret)
		fprintf(stderr, "fitblk: error releasing %s: %s\n", argp[1],
			strerror(ret));
	else
		fprintf(stderr, "fitblk: %s released\n", argp[1]);

	return ret;
}
