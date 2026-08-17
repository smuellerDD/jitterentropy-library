/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Periodic cryptographic self test for the Jitter RNG kernel interfaces.
 *
 * The known answer tests of the conditioning component, run at module load and
 * optionally repeated over the lifetime of a module that is loaded once and
 * then keeps running.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */

#ifndef _JITTERENTROPY_SELFTEST_H
#define _JITTERENTROPY_SELFTEST_H

#include <linux/init.h>
#include <linux/types.h>

/*
 * Start and stop the periodic run. The start performs the first run itself and
 * fails with -EFAULT when it fails - the module must not load then; beyond
 * that it only queues work, and none at all with selftest_interval=0. The stop
 * waits for a run in progress, so it must precede the teardown of anything the
 * run could touch.
 */
int __init jent_selftest_init(void);
void jent_selftest_exit(void);

/*
 * Run the known answer tests once, now, for the JENT_IOCSELFTEST ioctl of the
 * character device and the test interface. Returns 0 when they pass, -EFAULT
 * when they fail or have already failed - the error state below is sticky.
 *
 * A failure is handled as one of the timer is, panic under fips=1 included,
 * and the run is counted in the statistics with the periodic ones. It runs
 * synchronously in the caller's context - the tests allocate nothing and never
 * block - and takes no lock of any interface: the state is module-wide, not
 * that of the instance the ioctl arrived on.
 */
int jent_selftest_run_now(void);

/*
 * Has a self test failed? Once it has, the module is in its error state and
 * delivers no data through any interface: a broken conditioning component must
 * not be handing out output. The state is sticky - only reloading the module,
 * which runs the tests again, clears it.
 */
bool jent_selftest_failed(void);

/*
 * Snapshot of the periodic self test, reported via
 * /proc/jitterentropy/statistics.
 */
struct jent_selftest_stats {
	unsigned int interval;		/* Seconds between runs, 0 = disabled */
	u64 runs;			/* Completed runs since module load */
	u64 seconds_since_last_run;	/* Only meaningful when runs > 0 */
	bool failed;			/* The error state above */
};

void jent_selftest_get_stats(struct jent_selftest_stats *stats);

#endif /* _JITTERENTROPY_SELFTEST_H */
