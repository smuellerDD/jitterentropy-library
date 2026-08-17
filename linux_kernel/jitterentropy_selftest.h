/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Periodic cryptographic self test for the Jitter RNG kernel interfaces.
 *
 * The known answer tests of the conditioning component, optionally repeated
 * per instance over the lifetime of a module that is loaded once and then
 * keeps running. At module load itself they run inside
 * jent_entropy_init_ex(), the library's initialization, which refuses the
 * load on failure - the module schedules no run of its own there.
 *
 * Every Jitter RNG instance drives its own self test: it queues its own work
 * item, runs the tests bound to its own collector (jent_selftest()) under its
 * own lock - so only that instance pauses while its test runs - and stops
 * only itself when a run fails, through the library's JENT_ERR_SELFTEST gate.
 * There is no module-wide self test state gating the interfaces and no
 * instance blocks another.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */

#ifndef _JITTERENTROPY_SELFTEST_H
#define _JITTERENTROPY_SELFTEST_H

#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct rand_data;

/*
 * The self test of one Jitter RNG instance. Every interface embeds one per
 * entropy collector.
 *
 * The collector is referenced indirectly because jent_read_entropy_safe()
 * reallocates it on health-test recovery; it only ever does so under the
 * instance lock named here, which the self test holds while running.
 */
struct jent_selftest_instance {
	struct delayed_work work;		/* the periodic run */
	struct mutex *lock;			/* the instance lock */
	struct rand_data **entropy_collector;	/* reallocated under lock */
	bool failed;				/* sticky; written under lock */
};

/*
 * Start and stop the self test of one instance. The init queues the periodic
 * run (none with selftest_interval=0) with the collector already allocated;
 * the exit cancels it and waits for a run in progress, so it must precede the
 * freeing of the collector and must not be called under the instance lock,
 * which the run takes.
 */
void jent_selftest_instance_init(struct jent_selftest_instance *st,
				 struct mutex *lock,
				 struct rand_data **entropy_collector);
void jent_selftest_instance_exit(struct jent_selftest_instance *st);

/*
 * Run the known answer tests of one instance now, for the JENT_IOCSELFTEST
 * ioctl of the character device. Takes the instance lock; the caller must not
 * hold it. Returns 0 when they pass, -EFAULT when they fail or a run of this
 * instance has already failed - the failure is sticky, as the library's
 * JENT_ERR_SELFTEST gate that stops the instance's output is - and
 * -ERESTARTSYS when interrupted waiting for the lock. A failure is a panic
 * under fips=1, as a permanent health test failure is, and every run is
 * counted in the statistics.
 */
int jent_selftest_instance_run(struct jent_selftest_instance *st);

/*
 * Run the known answer tests once, unbound to any instance, for the
 * JENT_IOCSELFTEST ioctl of the debugfs test interface - its instances record
 * raw noise that never passes the conditioning component, so there is nothing
 * to bind the verdict to. Returns 0 or -EFAULT; panics under fips=1 on
 * failure; counted in the statistics.
 */
int jent_selftest_run_now(void);

/*
 * Validate the selftest_interval module parameter the per-instance runs are
 * scheduled with, before any instance exists. No run of its own: at module
 * load jent_entropy_init_ex() has just run the same known answer tests from
 * the library's initialization, refusing the load on failure.
 */
void __init jent_selftest_init(void);

/*
 * Snapshot of the self test activity across all instances, reported via
 * /proc/jitterentropy/statistics.
 */
struct jent_selftest_stats {
	unsigned int interval;		/* Seconds between runs, 0 = disabled */
	u64 runs;			/* Completed runs since module load */
	u64 failures;			/* Failed runs since module load */
	u64 seconds_since_last_run;	/* Only meaningful when runs > 0 */
};

void jent_selftest_get_stats(struct jent_selftest_stats *stats);

#endif /* _JITTERENTROPY_SELFTEST_H */
