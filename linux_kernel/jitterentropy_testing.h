/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Test interface for Jitter RNG.
 *
 * Copyright (C) 2023 - 2026, Stephan Mueller <smueller@chronox.de>
 */

#ifndef _JITTERENTROPY_TESTING_H
#define _JITTERENTROPY_TESTING_H

#ifdef CONFIG_EXTERNAL_JITTERENTROPY_TESTINTERFACE
#include <linux/init.h>

/*
 * Create/remove the debugfs test interface. A creation failure is returned
 * as an error after removing anything already created; a kernel without
 * debugfs or with lockdown active skips the interface and succeeds.
 */
int __init jent_testing_init(void);
void jent_testing_exit(void);
#else /* CONFIG_EXTERNAL_JITTERENTROPY_TESTINTERFACE */
static inline int jent_testing_init(void) { return 0; }
static inline void jent_testing_exit(void) { }
#endif /* CONFIG_EXTERNAL_JITTERENTROPY_TESTINTERFACE */

#endif /* _JITTERENTROPY_TESTING_H */
