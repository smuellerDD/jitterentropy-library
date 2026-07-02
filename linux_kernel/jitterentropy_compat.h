/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Kernel-version compatibility handling for the Jitter RNG kernel interfaces.
 *
 * The integration supports Linux 5.10 and every newer release. 5.10 is a
 * conservative baseline: all of the kernel APIs consumed by these interfaces
 * predate it. The memory helpers kvmalloc()/kvzalloc()/kvfree() (4.12) and
 * kvfree_sensitive() (5.7, used by the character-device and test interfaces),
 * compat_ptr_ioctl() (5.4, used by the character-device ioctl), the crypto RNG
 * registration, the hw_random framework, misc devices, debugfs and
 * u64_to_user_ptr() are all stable across the whole 5.10..latest range.
 *
 * Reject older kernels with an explicit message instead of letting the build
 * fail later with confusing implicit-declaration errors.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 */

#ifndef _JITTERENTROPY_COMPAT_H
#define _JITTERENTROPY_COMPAT_H

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 10, 0)
#error "The Jitter RNG kernel module requires Linux 5.10 or newer"
#endif

#endif /* _JITTERENTROPY_COMPAT_H */
