/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Kernel crypto API interface for Jitter RNG.
 *
 * Copyright (C) 2023 - 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */


#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/hash.h>
#include <linux/hashtable.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid_namespace.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>
#include <linux/string.h>
#include <crypto/internal/rng.h>

#include "jitterentropy.h"
#include "jitterentropy_error.h"
#include "jitterentropy_kcapi.h"
#include "jitterentropy_selftest.h"
#include "jitterentropy_status.h"

/*
 * The OSR and flags used to allocate the per-tfm Jitter RNG instances and the
 * verbose logging switch are configurable via the module parameters of the
 * same name (see jitterentropy_mod.c).
 */
extern unsigned int jent_osr;
extern unsigned int jent_flags;
extern unsigned int jent_verbose;

/*
 * Concurrent tfms allowed each unprivileged user, 0 for unlimited. AF_ALG
 * lets any local user instantiate the RNG (bind to rng/jitter_rng, or to a
 * DRBG where that seeds from this module), and every tfm allocates a
 * collector of hundreds of kB, so it needs a bound like /dev/jitterentropy
 * (see max_instances in jitterentropy_chardev.c). The count is separate from
 * the character device one, so filling either interface does not starve the
 * other.
 *
 * Per user, not global: with CONFIG_BUILTIN_JITTERENTROPY the DRBG allocates
 * its jitterentropy_rng tfm in the context of whichever process instantiates
 * it, so a global count one user fills would refuse the DRBG to all others.
 * Within user namespaces the outermost owner that is not root is charged, so
 * the uids a user maps there do not each bring a limit of their own.
 *
 * A uid alone does not name a tenant: rootful containers all run as the same
 * global uids, root first. The bucket is therefore also keyed on the
 * outermost PID namespace below init_pid_ns that init_user_ns owns - one
 * created with real privilege, i.e. by a container runtime - or init_pid_ns
 * if there is none. Outermost, so namespaces nested inside a container do
 * not open new buckets. An unprivileged user cannot create such a namespace,
 * but a privileged helper can on the user's behalf (setuid firejail or bwrap,
 * PrivatePIDs=), and every such sandbox is a fresh bucket.
 */
static unsigned int max_kcapi_instances = 256;
module_param(max_kcapi_instances, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(max_kcapi_instances,
		 "Maximum concurrent kernel crypto API instances per unprivileged user and container (0: unlimited)");

struct jent_kcapi_user {
	struct hlist_node node;
	kuid_t uid;
	/* Referenced, so a freed namespace's address cannot alias a bucket. */
	struct pid_namespace *pid_ns;
	unsigned int count;
};

static DEFINE_HASHTABLE(jent_kcapi_users, 6);
static DEFINE_MUTEX(jent_kcapi_users_lock);

static unsigned long jent_kcapi_user_hash(kuid_t uid,
					  struct pid_namespace *pid_ns)
{
	return __kuid_val(uid) ^ hash_ptr(pid_ns, 32);
}

static kuid_t jent_kcapi_charged_uid(void)
{
	struct user_namespace *ns;
	kuid_t uid = current_uid();

	/*
	 * The outermost owner that is not root: a user's own namespaces all
	 * charge that user. In namespaces only root created (container
	 * runtimes, PrivateUsers=) the caller's uid is charged - a global uid
	 * already, mapped as root chose.
	 */
	for (ns = current_user_ns(); ns != &init_user_ns; ns = ns->parent)
		if (!uid_eq(ns->owner, GLOBAL_ROOT_UID))
			uid = ns->owner;

	return uid;
}

static struct pid_namespace *jent_kcapi_charged_pid_ns(void)
{
	struct pid_namespace *ns = task_active_pid_ns(current);
	struct pid_namespace *charged = &init_pid_ns;

	/* Every chain ends in init_pid_ns; a child pins its parent. */
	for (; ns && ns != &init_pid_ns; ns = ns->parent)
		if (ns->user_ns == &init_user_ns)
			charged = ns;

	return charged;
}

/*
 * Charge a new tfm to its bucket. Returns the bucket, ERR_PTR(-ENFILE) at the
 * limit or ERR_PTR(-ENOMEM) when a new bucket cannot be allocated - kept apart
 * so a caller hitting memory pressure is not told the limit is reached.
 */
static struct jent_kcapi_user *jent_kcapi_instance_get(void)
{
	struct pid_namespace *pid_ns = jent_kcapi_charged_pid_ns();
	kuid_t uid = jent_kcapi_charged_uid();
	unsigned long key = jent_kcapi_user_hash(uid, pid_ns);
	struct jent_kcapi_user *user;

	mutex_lock(&jent_kcapi_users_lock);

	hash_for_each_possible(jent_kcapi_users, user, node, key)
		if (uid_eq(user->uid, uid) && user->pid_ns == pid_ns)
			goto found;

	user = kzalloc(sizeof(*user), GFP_KERNEL);
	if (!user) {
		user = ERR_PTR(-ENOMEM);
		goto out;
	}
	user->uid = uid;
	user->pid_ns = get_pid_ns(pid_ns);
	hash_add(jent_kcapi_users, &user->node, key);

found:
	/*
	 * Exempt, so a user at the limit does not starve privileged callers
	 * or the kernel itself: kernel threads run with full capabilities.
	 * Only consulted once the limit would refuse, so a bind that fits
	 * needs no privilege: no LSM audit denial for every unprivileged
	 * caller, and no PF_SUPERPRIV on a privileged one.
	 */
	if (max_kcapi_instances && user->count >= max_kcapi_instances &&
	    !capable(CAP_SYS_RESOURCE)) {
		/* count >= 1: the bucket is in use and stays. */
		user = ERR_PTR(-ENFILE);
		goto out;
	}

	user->count++;

out:
	mutex_unlock(&jent_kcapi_users_lock);
	return user;
}

static void jent_kcapi_instance_put(struct jent_kcapi_user *user)
{
	struct pid_namespace *pid_ns = NULL;

	mutex_lock(&jent_kcapi_users_lock);
	if (!--user->count) {
		hash_del(&user->node);
		pid_ns = user->pid_ns;
		kfree(user);
	}
	mutex_unlock(&jent_kcapi_users_lock);

	if (pid_ns)
		put_pid_ns(pid_ns);
}

/***************************************************************************
 * Kernel crypto API interface
 ***************************************************************************/

struct jitterentropy {
	struct mutex jent_lock;
	struct rand_data *entropy_collector;
	/* The bucket the instance is counted against. */
	struct jent_kcapi_user *user;
	/* This instance's own periodic self test. */
	struct jent_selftest_instance selftest;
};

static void jent_kcapi_tfm_cleanup(struct crypto_tfm *tfm)
{
	struct jitterentropy *rng = crypto_tfm_ctx(tfm);

	/*
	 * Before the collector goes away, and outside jent_lock: stopping the
	 * self test waits for a run in progress, which takes jent_lock.
	 */
	jent_selftest_instance_exit(&rng->selftest);

	mutex_lock(&rng->jent_lock);

	if (rng->entropy_collector)
		jent_entropy_collector_free(rng->entropy_collector);
	rng->entropy_collector = NULL;

	mutex_unlock(&rng->jent_lock);
	mutex_destroy(&rng->jent_lock);

	/* Only called for a tfm whose jent_kcapi_tfm_init() succeeded. */
	jent_kcapi_instance_put(rng->user);
}

static int jent_kcapi_log(struct jitterentropy *rng)
{
	static DEFINE_RATELIMIT_STATE(jent_log_rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);

	if (!jent_verbose)
		return 0;

	/*
	 * Rate-limit the status as a whole, not per line, so an emitted status
	 * is never cut short. Asked before the document is rendered: a
	 * suppressed status is not worth serializing.
	 */
	if (!__ratelimit(&jent_log_rs))
		return 0;

	/* The collector pointer must not be read without the lock. */
	return jent_status_to_log(&rng->jent_lock, &rng->entropy_collector);
}

static int jent_kcapi_tfm_init(struct crypto_tfm *tfm)
{
	struct jitterentropy *rng = crypto_tfm_ctx(tfm);
	struct jent_kcapi_user *user;

	/*
	 * Taken before the collector is allocated. ENFILE at the limit as for
	 * the character device, ENOMEM if the bucket could not be allocated;
	 * crypto_alloc_rng() hands either through to the caller, e.g. the
	 * AF_ALG bind().
	 */
	user = jent_kcapi_instance_get();
	if (IS_ERR(user))
		return PTR_ERR(user);
	rng->user = user;

	mutex_init(&rng->jent_lock);

	rng->entropy_collector =
		jent_entropy_collector_alloc(jent_osr, jent_flags);
	if (!rng->entropy_collector) {
		/* As for the character device: startup or memory. */
		pr_warn_ratelimited("jitterentropy: no entropy collector for a crypto API instance: its startup failed or memory ran out\n");
		mutex_destroy(&rng->jent_lock);
		jent_kcapi_instance_put(rng->user);
		return -ENOMEM;
	}

	jent_selftest_instance_init(&rng->selftest, &rng->jent_lock,
				    &rng->entropy_collector);

	/*
	 * Best-effort verbose logging: a failure to allocate the status buffer
	 * must not fail the tfm initialization.
	 */
	jent_kcapi_log(rng);

	return 0;
}

static int jent_kcapi_random(struct crypto_rng *tfm,
			     const u8 *src, unsigned int slen,
			     u8 *rdata, unsigned int dlen)
{
	struct jitterentropy *rng = crypto_rng_ctx(tfm);
	struct rand_data *ec;
	bool reallocated;
	ssize_t rc;
	int ret;

	/*
	 * jent_lock also serializes against this instance's own self test:
	 * no data leaves while its run is in progress, and after a failed run
	 * the library refuses with JENT_ERR_SELFTEST, mapped below.
	 */
	mutex_lock(&rng->jent_lock);

	ec = rng->entropy_collector;
	rc = jent_read_entropy_safe(&rng->entropy_collector, rdata, dlen);

	/*
	 * Detect a collector reallocation on health-test recovery while still
	 * holding the lock; rng->entropy_collector must not be read unlocked.
	 */
	reallocated = (ec != rng->entropy_collector);

	if (rc >= 0) {
		/*
		 * Success: jent_read_entropy_safe() returns the number of
		 * generated bytes (== dlen). The crypto API expects 0 on
		 * success, so map any non-negative result accordingly.
		 */
		ret = 0;
	} else {
		/*
		 * Map the error (panicking under FIPS on a permanent
		 * health-test failure). Shared with the hwrng and character
		 * device interfaces, see jitterentropy_error.h.
		 */
		ret = jent_map_read_error(rc);
	}

	mutex_unlock(&rng->jent_lock);

	if (jent_verbose && reallocated) {
		/*
		 * The entropy collector was reallocated
		 *
		 * Do not honor the return code here: In case logging was
		 * unsuccessful, so be it.
		 */
		jent_kcapi_log(rng);
	}

	return ret;
}

static int jent_kcapi_reset(struct crypto_rng *tfm,
			    const u8 *seed, unsigned int slen)
{
	return 0;
}

/*
 * If the code is compiled as part of the kernel, use jitterentropy_rng as name.
 * Otherwise use "jitter_rng" as name as otherwise we have a name clash with
 * the existing old in-kernel variant.
 */
static struct rng_alg jent_alg = {
	.generate		= jent_kcapi_random,
	.seed			= jent_kcapi_reset,
	.seedsize		= 0,
	.base			= {
#ifdef CONFIG_BUILTIN_JITTERENTROPY
		.cra_name               = "jitterentropy_rng",
		.cra_driver_name        = "jitterentropy_rng",
#else
		.cra_name               = "jitter_rng",
		.cra_driver_name        = "jitter_rng",
#endif
		.cra_priority           = 100,
		.cra_ctxsize            = sizeof(struct jitterentropy),
		.cra_module             = THIS_MODULE,
		.cra_init               = jent_kcapi_tfm_init,
		.cra_exit               = jent_kcapi_tfm_cleanup,
	}
};

int __init jent_kcapi_init(void)
{
	return crypto_register_rng(&jent_alg);
}

void jent_kcapi_exit(void)
{
	crypto_unregister_rng(&jent_alg);
}
