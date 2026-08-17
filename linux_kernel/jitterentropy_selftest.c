// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * Periodic cryptographic self test for the Jitter RNG kernel interfaces.
 *
 * See jitterentropy_selftest.h for what this is for.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/fips.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/workqueue.h>

#include "jitterentropy.h"
#include "jitterentropy_selftest.h"

/* Shared with the other interfaces, see jitterentropy_mod.c. */
extern unsigned int verbose;

/*
 * Seconds between two runs of the known answer tests. 0 by default, leaving
 * the run at module load and no timer: repeating the tests bounds the window
 * in which a conditioning component that broke after the load keeps handing
 * out data, which is an audit regime's call to make rather than every system's.
 * Where it is wanted, one hour is a sensible value - a run costs two Keccak
 * computations.
 *
 * Read-only at runtime and reported in /proc/jitterentropy/statistics.
 */
static unsigned int selftest_interval;
module_param(selftest_interval, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(selftest_interval,
		 "Seconds between two runs of the cryptographic self test (default 0, no periodic run)");

/*
 * The upper bound exists so the delay computation below stays within an
 * unsigned long on 32 bit systems: 30 days at HZ=1000 is 2.6e9 jiffies, which
 * still fits, and no periodic self test regime asks for a longer interval.
 */
#define JENT_SELFTEST_INTERVAL_MAX (30U * 24U * 60U * 60U)

static struct delayed_work jent_selftest_work;

/*
 * The error state. Written only by the work item below, and only ever from
 * false to true; read by the entropy delivery paths of all interfaces.
 */
static bool jent_selftest_error;

/* Statistics, written only by the work item, read by procfs. */
static atomic64_t jent_selftest_runs = ATOMIC64_INIT(0);
static unsigned long jent_selftest_last_run;

bool jent_selftest_failed(void)
{
	return READ_ONCE(jent_selftest_error);
}

void jent_selftest_get_stats(struct jent_selftest_stats *stats)
{
	stats->interval = selftest_interval;
	stats->runs = (u64)atomic64_read(&jent_selftest_runs);
	stats->failed = jent_selftest_failed();

	/*
	 * The subtraction is jiffies arithmetic and therefore correct across
	 * the counter wrap; the division turns it into seconds without the
	 * 49-day range limit that jiffies_to_msecs() has on 32 bit.
	 */
	stats->seconds_since_last_run = stats->runs ?
		(u64)((jiffies - READ_ONCE(jent_selftest_last_run)) / HZ) : 0;
}

static void jent_selftest_schedule(void)
{
	/*
	 * A periodic timer with no deadline of its own, so it should not be
	 * what wakes an idle CPU on a kernel in power-efficient mode. That
	 * workqueue always exists; without the mode it is the per-CPU system
	 * workqueue.
	 */
	queue_delayed_work(system_power_efficient_wq, &jent_selftest_work,
			   (unsigned long)selftest_interval * HZ);
}

/*
 * One run of the known answer tests, shared by the timer below and the
 * on-demand run of the JENT_IOCSELFTEST ioctl. Both count towards the
 * statistics, so what is reported there is every run that happened.
 */
static bool jent_selftest_execute(void)
{
	int ret = jent_crypto_selftest();

	/*
	 * The timestamp first: a reader that sees a non-zero run count then
	 * always sees the timestamp belonging to it rather than the initial 0.
	 */
	WRITE_ONCE(jent_selftest_last_run, jiffies);
	atomic64_inc(&jent_selftest_runs);

	if (!ret) {
		if (verbose)
			pr_info("jitterentropy: cryptographic self test passed\n");

		return true;
	}

	/*
	 * Enter the error state before reporting it, so no interface can
	 * deliver data between the log message and the flag.
	 */
	WRITE_ONCE(jent_selftest_error, true);

	/*
	 * A failing known answer test means the conditioning component no
	 * longer computes what it is specified to compute. Under fips=1 the
	 * entire kernel acts as a FIPS 140 module and must panic on it, as it
	 * does on a permanent health test failure (see jitterentropy_error.h).
	 */
	if (fips_enabled)
		panic("jitterentropy: cryptographic self test failed: %d\n",
		      ret);

	pr_err("jitterentropy: cryptographic self test failed: %d - no further entropy is delivered\n",
	       ret);

	return false;
}

int jent_selftest_run_now(void)
{
	/*
	 * The error state is sticky, so another run could only confirm it.
	 * Report it without spending the work or repeating the log message.
	 */
	if (jent_selftest_failed())
		return -EFAULT;

	return jent_selftest_execute() ? 0 : -EFAULT;
}

static void jent_selftest_work_fn(struct work_struct *work)
{
	/*
	 * An on-demand run may have entered the error state since this one was
	 * queued. Nothing left to establish, and nothing to reschedule.
	 */
	if (jent_selftest_failed())
		return;

	/*
	 * Not rescheduled on failure either: the state the run just entered is
	 * sticky, so further runs could not lift it.
	 */
	if (jent_selftest_execute())
		jent_selftest_schedule();
}

int __init jent_selftest_init(void)
{
	/*
	 * Initialized even when the periodic run is disabled, so the exit path
	 * can cancel unconditionally.
	 */
	INIT_DELAYED_WORK(&jent_selftest_work, jent_selftest_work_fn);

	/*
	 * The first run, before any interface is registered.
	 * jent_entropy_init_ex() has run the same tests, but not the error
	 * state, the statistics and the fips=1 handling kept here - two Keccak
	 * computations to have those rest on a run of this module's own. It
	 * happens whether or not the periodic run is enabled.
	 *
	 * On failure the module must not load: under fips=1 the run has
	 * panicked already, and a module that delivers nothing serves no one.
	 */
	if (!jent_selftest_execute())
		return -EFAULT;

	if (!selftest_interval) {
		pr_info("jitterentropy: periodic cryptographic self test disabled\n");
		return 0;
	}

	if (selftest_interval > JENT_SELFTEST_INTERVAL_MAX) {
		pr_warn("jitterentropy: selftest_interval %u too large, using %u seconds\n",
			selftest_interval, JENT_SELFTEST_INTERVAL_MAX);
		selftest_interval = JENT_SELFTEST_INTERVAL_MAX;
	}

	/* The run above was the first one, the next is one interval away. */
	jent_selftest_schedule();

	pr_info("jitterentropy: cryptographic self test scheduled every %u seconds\n",
		selftest_interval);

	return 0;
}

void jent_selftest_exit(void)
{
	cancel_delayed_work_sync(&jent_selftest_work);
}
