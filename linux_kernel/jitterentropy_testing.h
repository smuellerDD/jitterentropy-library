/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Test interface for Jitter RNG.
 *
 * Copyright (C) 2023 - 2026, Stephan Mueller <smueller@chronox.de>
 */

#ifdef CONFIG_EXTERNAL_JITTERENTROPY_TESTINTERFACE
void jent_testing_init(void);
void jent_testing_exit(void);
#else /* CONFIG_EXTERNAL_JITTERENTROPY_TESTINTERFACE */
static inline void jent_testing_init(void) { }
static inline void jent_testing_exit(void) { }
#endif /* CONFIG_EXTERNAL_JITTERENTROPY_TESTINTERFACE */
