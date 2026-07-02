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
 * SP800-90B permanent health-test failures (-6/-7/-8/-10) as well as
 * intermittent failures that could not be recovered even after raising the OSR
 * to its maximum (-2/-3/-5/-9, returned by jent_read_entropy_safe() when
 * jent_health_failure_reset() gave up) are unrecoverable. If the kernel was
 * booted with fips=1 the entire kernel acts as a FIPS 140 module, so such a
 * failure is a module error and must panic. This is handled identically here
 * for all interfaces (crypto API, hwrng, character device) so the behaviour
 * does not depend on which interface observed the failure.
 *
 * @ret: negative return code from jent_read_entropy_safe()
 * Return: kernel errno (always negative)
 */
static inline int jent_map_read_error(ssize_t ret)
{
	switch (ret) {
	case -2:
	case -3:
	case -5:
	case -6:
	case -7:
	case -8:
	case -9:
	case -10:
		/* (Un)recoverable permanent health test error */
		if (fips_enabled)
			panic("Jitter RNG permanent health test failure\n");

		pr_err("Jitter RNG permanent health test failure\n");
		return -EFAULT;
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
