/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Character device interface for Jitter RNG.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 */

#ifndef _JITTERENTROPY_CHARDEV_H
#define _JITTERENTROPY_CHARDEV_H

#ifdef CONFIG_EXTERNAL_JITTERENTROPY_CHARDEV

int jent_chardev_init(void);
void jent_chardev_exit(void);

#else /* CONFIG_EXTERNAL_JITTERENTROPY_CHARDEV */

static inline int jent_chardev_init(void)
{
	return 0;
}

static inline void jent_chardev_exit(void)
{
}

#endif /* CONFIG_EXTERNAL_JITTERENTROPY_CHARDEV */

#endif /* _JITTERENTROPY_CHARDEV_H */
