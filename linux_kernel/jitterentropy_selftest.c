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
#include <linux/mutex.h>
#include <linux/workqueue.h>

#include "jitterentropy.h"
#include "jitterentropy_selftest.h"

/* Shared with the other interfaces, see jitterentropy_mod.c. */
extern unsigned int verbose;

/*
 * Seconds between two runs of the known answer tests of each instance. 0 by
 * default, leaving the library's run at load and no timers: repeating the tests
 * bounds the window in which a conditioning component that broke after the
 * load keeps handing out data, which is an audit regime's call to make rather
 * than every system's. Where it is wanted, one hour is a sensible value - a
 * run costs two Keccak computations.
 *
 * Read-only at runtime and reported in /proc/jitterentropy/statistics.
 */
static unsigned int selftest_interval;
module_param(selftest_interval, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(selftest_interval,
		 "Seconds between two runs of the cryptographic self test of each instance (default 0, no periodic run)");

/*
 * The upper bound exists so the delay computation below stays within an
 * unsigned long on 32 bit systems: 30 days at HZ=1000 is 2.6e9 jiffies, which
 * still fits, and no periodic self test regime asks for a longer interval.
 */
#define JENT_SELFTEST_INTERVAL_MAX (30U * 24U * 60U * 60U)

/*
 * Statistics across all instances, so what
 * /proc/jitterentropy/statistics reports is every run that happened.
 */
static atomic64_t jent_selftest_runs = ATOMIC64_INIT(0);
static atomic64_t jent_selftest_failures = ATOMIC64_INIT(0);
static unsigned long jent_selftest_last_run;

void jent_selftest_get_stats(struct jent_selftest_stats *stats)
{
	stats->interval = selftest_interval;
	stats->runs = (u64)atomic64_read(&jent_selftest_runs);
	stats->failures = (u64)atomic64_read(&jent_selftest_failures);

	/*
	 * The subtraction is jiffies arithmetic and therefore correct across
	 * the counter wrap; the division turns it into seconds without the
	 * 49-day range limit that jiffies_to_msecs() has on 32 bit.
	 */
	stats->seconds_since_last_run = stats->runs ?
		(u64)((jiffies - READ_ONCE(jent_selftest_last_run)) / HZ) : 0;
}

/*
 * One run of the known answer tests, bound to @ec or unbound with NULL,
 * shared by the per-instance timers and the on-demand runs of the
 * JENT_IOCSELFTEST ioctl. Called with the instance lock held when @ec is
 * bound.
 *
 * A failing known answer test means the conditioning component no longer
 * computes what it is specified to compute. Under fips=1 the entire kernel
 * acts as a FIPS 140 module and must panic on it, as it does on a permanent
 * health test failure (see jitterentropy_error.h). Otherwise a bound run
 * marks its instance (the library's JENT_ERR_SELFTEST gate), which from then
 * on refuses output - every other instance keeps delivering.
 */
static int jent_selftest_execute(struct rand_data *ec)
{
	/* " (instance " UUID ")", or empty for a run that cannot be named. */
	char instance[JENT_UUID_STRLEN + 12];
	char uuid[JENT_UUID_STRLEN];
	int ret = jent_selftest(ec);

	/*
	 * Name the instance in every verdict logged - an unbound run carries
	 * no UUID and goes unnamed.
	 */
	if (ec && !jent_uuid(ec, uuid, sizeof(uuid)) && uuid[0])
		snprintf(instance, sizeof(instance), " (instance %s)", uuid);
	else
		instance[0] = '\0';

	/*
	 * The timestamp first: a reader that sees a non-zero run count then
	 * always sees the timestamp belonging to it rather than the initial 0.
	 */
	WRITE_ONCE(jent_selftest_last_run, jiffies);
	atomic64_inc(&jent_selftest_runs);

	if (!ret) {
		if (verbose)
			pr_info("jitterentropy: cryptographic self test passed%s\n",
				instance);

		return 0;
	}

	atomic64_inc(&jent_selftest_failures);

	if (fips_enabled)
		panic("jitterentropy: cryptographic self test failed: %d%s\n",
		      ret, instance);

	if (instance[0])
		pr_err("jitterentropy: cryptographic self test failed: %d%s - the instance delivers no further entropy\n",
		       ret, instance);
	else
		pr_err("jitterentropy: cryptographic self test failed: %d\n",
		       ret);

	return -EFAULT;
}

static void jent_selftest_instance_schedule(struct jent_selftest_instance *st)
{
	/*
	 * A periodic timer with no deadline of its own, so it should not be
	 * what wakes an idle CPU on a kernel in power-efficient mode. That
	 * workqueue always exists; without the mode it is the per-CPU system
	 * workqueue.
	 */
	queue_delayed_work(system_power_efficient_wq, &st->work,
			   (unsigned long)selftest_interval * HZ);
}

static void jent_selftest_instance_work_fn(struct work_struct *work)
{
	struct jent_selftest_instance *st =
		container_of(to_delayed_work(work),
			     struct jent_selftest_instance, work);
	bool failed;

	/*
	 * The instance lock is what pauses this instance's output for the
	 * duration of its run - the delivery paths generate under it - and
	 * pins the collector pointer against reallocation. No other instance
	 * is touched.
	 */
	mutex_lock(st->lock);
	if (!st->failed && jent_selftest_execute(*st->entropy_collector))
		st->failed = true;
	failed = st->failed;
	mutex_unlock(st->lock);

	/*
	 * Not rescheduled after a failure (of this run, or of an on-demand
	 * run since this one was queued): the instance is out of service and
	 * further runs could not lift that.
	 */
	if (!failed)
		jent_selftest_instance_schedule(st);
}

void jent_selftest_instance_init(struct jent_selftest_instance *st,
				 struct mutex *lock,
				 struct rand_data **entropy_collector)
{
	st->lock = lock;
	st->entropy_collector = entropy_collector;
	st->failed = false;

	/*
	 * Initialized even when the periodic run is disabled, so the exit
	 * path can cancel unconditionally.
	 */
	INIT_DELAYED_WORK(&st->work, jent_selftest_instance_work_fn);

	if (selftest_interval)
		jent_selftest_instance_schedule(st);
}

void jent_selftest_instance_exit(struct jent_selftest_instance *st)
{
	/*
	 * Also handles the work requeueing itself: on return it is neither
	 * pending nor executing.
	 */
	cancel_delayed_work_sync(&st->work);
}

int jent_selftest_instance_run(struct jent_selftest_instance *st)
{
	int ret;

	if (mutex_lock_interruptible(st->lock))
		return -ERESTARTSYS;

	/*
	 * The failure is sticky, so another run could only confirm it.
	 * Report it without spending the work or repeating the log message.
	 */
	if (st->failed) {
		ret = -EFAULT;
	} else {
		ret = jent_selftest_execute(*st->entropy_collector);
		if (ret)
			st->failed = true;
	}

	mutex_unlock(st->lock);

	return ret;
}

int jent_selftest_run_now(void)
{
	return jent_selftest_execute(NULL);
}

void __init jent_selftest_init(void)
{
	/*
	 * No run of the module's own at load: jent_entropy_init_ex() has just
	 * run the same known answer tests from the library's initialization,
	 * refusing the load on failure, and every instance runs them for
	 * itself from then on. Only the interval the instances schedule with
	 * is validated here, before any of them exists.
	 */
	if (!selftest_interval) {
		pr_info("jitterentropy: periodic cryptographic self test disabled\n");
		return;
	}

	if (selftest_interval > JENT_SELFTEST_INTERVAL_MAX) {
		pr_warn("jitterentropy: selftest_interval %u too large, using %u seconds\n",
			selftest_interval, JENT_SELFTEST_INTERVAL_MAX);
		selftest_interval = JENT_SELFTEST_INTERVAL_MAX;
	}

	pr_info("jitterentropy: cryptographic self test scheduled every %u seconds for every instance\n",
		selftest_interval);
}
