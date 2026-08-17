/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Shared error mapping for the Jitter RNG kernel interfaces.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 */

#ifndef _JITTERENTROPY_ERROR_H
#define _JITTERENTROPY_ERROR_H

#include <linux/errno.h>
#include <linux/fips.h>
#include <linux/kernel.h>	/* panic(), pr_err() */

#include "jitterentropy.h"	/* JENT_ERR_* */

/*
 * Map a jent_read_entropy_safe() return code to a kernel error code. Shared by
 * all interfaces, so the behaviour does not depend on which one observed the
 * failure.
 *
 * Only SP800-90B permanent health test failures are a module error, which
 * under fips=1 the whole kernel must panic on. The intermittent ones reach
 * here only after jent_read_entropy_safe() gave up recovering them, and map to
 * -EAGAIN as they do in the upstream kernel Jitter RNG.
 *
 * @ret: negative return code from jent_read_entropy_safe()
 * Return: kernel errno (always negative)
 */
static inline int jent_map_read_error(ssize_t ret)
{
	switch (ret) {
	case JENT_ERR_RCT_PERMANENT:
	case JENT_ERR_APT_PERMANENT:
	case JENT_ERR_LAG_PERMANENT:
	case JENT_ERR_RCT_MEM_PERMANENT:
	case JENT_ERR_SELFTEST:
		/* Permanent health test error */
		if (fips_enabled)
			panic("Jitter RNG permanent health test failure\n");

		/*
		 * Rate-limited: a permanent failure is sticky for the affected
		 * instance, and periodic retries (e.g. the hwrng core's fill
		 * thread, which retries every 10 seconds forever) would
		 * otherwise flood the log.
		 */
		pr_err_ratelimited("Jitter RNG permanent health test failure\n");
		return -EFAULT;
	case JENT_ERR_RCT:
	case JENT_ERR_APT:
	case JENT_ERR_LAG:
	case JENT_ERR_RCT_MEM:
		/* Unrecovered intermittent health test error */
		pr_warn_ratelimited("Jitter RNG intermittent health test failure not recovered\n");
		return -EAGAIN;
	case JENT_ERR_EINVAL:
	case JENT_ERR_NOTIME:
		/* Generic errors */
		return -EINVAL;
	default:
		/* Unexpected errors */
		return -EFAULT;
	}
}

#endif /* _JITTERENTROPY_ERROR_H */
