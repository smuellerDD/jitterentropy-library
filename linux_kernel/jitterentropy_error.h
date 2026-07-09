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

/*
 * Map a jent_read_entropy_safe() return code to a kernel error code.
 *
 * Only SP800-90B permanent health-test failures (-6/-7/-8/-10) are a module
 * error: if the kernel was booted with fips=1 the entire kernel acts as a
 * FIPS 140 module and must panic on them.
 *
 * Intermittent health-test failures (-2/-3/-5/-9) are returned by
 * jent_read_entropy_safe() only after its recovery attempt gave up (the OSR
 * reached its maximum or the reallocation failed). They are not a permanent
 * failure in the SP800-90B sense and therefore never panic; they map to
 * -EAGAIN, matching the handling of intermittent failures in the upstream
 * kernel Jitter RNG.
 *
 * This is handled identically here for all interfaces (crypto API, hwrng,
 * character device) so the behaviour does not depend on which interface
 * observed the failure.
 *
 * @ret: negative return code from jent_read_entropy_safe()
 * Return: kernel errno (always negative)
 */
static inline int jent_map_read_error(ssize_t ret)
{
	switch (ret) {
	case -6:
	case -7:
	case -8:
	case -10:
		/* Permanent health test error */
		if (fips_enabled)
			panic("Jitter RNG permanent health test failure\n");

		pr_err("Jitter RNG permanent health test failure\n");
		return -EFAULT;
	case -2:
	case -3:
	case -5:
	case -9:
		/* Unrecovered intermittent health test error */
		pr_warn_ratelimited("Jitter RNG intermittent health test failure not recovered\n");
		return -EAGAIN;
	case -1:
	case -4:
		/* Generic errors */
		return -EINVAL;
	default:
		/* Unexpected errors */
		return -EFAULT;
	}
}

#endif /* _JITTERENTROPY_ERROR_H */
