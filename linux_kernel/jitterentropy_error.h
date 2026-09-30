/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Shared error mapping for the Jitter RNG kernel interfaces.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
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
 * The permanent failures - the SP800-90B permanent health test failures and a
 * failed self test bound to the instance - are sticky for the affected
 * instance, and under fips=1 the whole kernel must panic on them. The
 * intermittent ones reach here only after jent_read_entropy_safe() failed to
 * recover them. A later read retries the recovery, which can succeed if it
 * failed on an allocation or the startup test, but never once the instance
 * runs at the maximum oversampling rate. So they map to -EIO rather than the
 * upstream kernel Jitter RNG's -EAGAIN, which invites an immediate retry: a
 * nonblocking /dev/hwrng reader would spin on it, and the DRBG under fips=1
 * tolerates it on reseed, i.e. would go on without this noise source for
 * good. The hwrng core hands any error to read(2), and its fill thread sleeps
 * on any error alike.
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
		return -EIO;
	case JENT_ERR_EINVAL:
	case JENT_ERR_NOTIME:
		/* Generic errors */
		return -EINVAL;
	default:
		/* Unexpected errors */
		return -EFAULT;
	}
}

/*
 * jent_map_read_error() for the interfaces whose errors reach read(2): the
 * character device and /dev/hwrng, to which the hwrng core hands the error of
 * the .read callback unchanged. -EFAULT above follows the upstream crypto API
 * convention, but to a read(2) caller it means a bad buffer; there it is an
 * I/O error.
 *
 * @ret: negative return code from jent_read_entropy_safe()
 * Return: kernel errno (always negative)
 */
static inline int jent_map_user_read_error(ssize_t ret)
{
	int err = jent_map_read_error(ret);

	return err == -EFAULT ? -EIO : err;
}

#endif /* _JITTERENTROPY_ERROR_H */
