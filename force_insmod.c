/*
 * force_insmod — load a kernel module while ignoring symbol-version
 * (CRC) and vermagic checks.
 *
 * Built-in busybox insmod doesn't pass the IGNORE_MODVERSIONS /
 * IGNORE_VERMAGIC flags accepted by Linux's `finit_module` syscall —
 * which is what you want when your out-of-tree module was built
 * against a kernel tree whose .config doesn't exactly match the
 * running kernel's. CRCs computed by `genksyms` differ across config
 * permutations (struct visibility under #ifdef), even with matching
 * source — and you don't always have access to the running kernel's
 * .config. This wrapper bypasses both checks.
 *
 * Setting these flags taints the kernel (TAINT_FORCED_MODULE) but the
 * load itself succeeds.
 *
 * Usage: force_insmod /path/to/module.ko
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/syscall.h>

#ifndef SYS_finit_module
#define SYS_finit_module 273  /* aarch64 syscall number */
#endif

#define MODULE_INIT_IGNORE_MODVERSIONS	1
#define MODULE_INIT_IGNORE_VERMAGIC	2

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s /path/to/module.ko\n", argv[0]);
		return 2;
	}

	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s\n", argv[1], strerror(errno));
		return 1;
	}

	int flags = MODULE_INIT_IGNORE_MODVERSIONS | MODULE_INIT_IGNORE_VERMAGIC;
	long r = syscall(SYS_finit_module, fd, "", flags);
	if (r < 0) {
		fprintf(stderr, "finit_module: %s (errno=%d)\n",
			strerror(errno), errno);
		close(fd);
		return 1;
	}

	close(fd);
	fprintf(stderr, "loaded\n");
	return 0;
}
