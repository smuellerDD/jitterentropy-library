/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Kernel crypto API interface for Jitter RNG.
 *
 * Copyright (C) 2023 - 2026, Stephan Mueller <smueller@chronox.de>
 */

#ifndef _JITTERENTROPY_KCAPI_H
#define _JITTERENTROPY_KCAPI_H

#ifdef CONFIG_EXTERNAL_JITTERENTROPY_KCAPI

int jent_kcapi_init(void);
void jent_kcapi_exit(void);

#else /* CONFIG_EXTERNAL_JITTERENTROPY_KCAPI */

static inline int jent_kcapi_init(void)
{
	return 0;
}

static inline void jent_kcapi_exit(void)
{
}

#endif /* CONFIG_EXTERNAL_JITTERENTROPY_KCAPI */

#endif /* _JITTERENTROPY_KCAPI_H */
