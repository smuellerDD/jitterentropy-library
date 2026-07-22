/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Hardware RNG (hwrng) interface for Jitter RNG.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 */

#ifndef _JITTERENTROPY_HWRNG_H
#define _JITTERENTROPY_HWRNG_H

#ifdef CONFIG_EXTERNAL_JITTERENTROPY_HWRNG

#include <linux/init.h>

int __init jent_hwrng_init(void);
void jent_hwrng_exit(void);

#else /* CONFIG_EXTERNAL_JITTERENTROPY_HWRNG */

static inline int jent_hwrng_init(void)
{
	return 0;
}

static inline void jent_hwrng_exit(void)
{
}

#endif /* CONFIG_EXTERNAL_JITTERENTROPY_HWRNG */

#endif /* _JITTERENTROPY_HWRNG_H */
