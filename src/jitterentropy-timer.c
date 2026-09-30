/* Jitter RNG: Internal timer implementation
 *
 * Copyright (C) 2021 - 2026, Stephan Mueller <smueller@chronox.de>
 *
 * License: see LICENSE file in root directory
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include "jitterentropy-base.h"
#include "jitterentropy-timer.h"
#include "arch/jitterentropy-arch-thread.h"

/* As in jitterentropy-base.c: the entropy core must not be optimized. */
#ifdef __OPTIMIZE__
 #error "The CPU Jitter random number generator must not be compiled with optimizations. See documentation. Use the compiler switch -O0 for compiling the entropy core."
#endif

/* Timer-less entropy source */
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER

/***************************************************************************
 * Thread handler
 ***************************************************************************/

/* CPU the counting thread pins itself to, see jent_notime_set_cpu(). */
static int jent_notime_cpu_configured = 0;
static unsigned long jent_notime_cpu = 0;

/* No CPU configured: the counting thread picks the highest it may run on. */
#define JENT_NOTIME_CPU_DEFAULT	(~0UL)

/*
 * Whether the builtin handler has a thread backend to run its counting
 * thread on. Only a hosted build (POSIX or Win32 threads) has one; on the
 * freestanding ones (Linux and FreeBSD kernel, bare metal) it is a stub, and
 * only a handler registered through jent_entropy_switch_notime_impl() can
 * provide the internal timer.
 */
#if defined(JENT_ARCH_THREAD_HOSTED)
# define JENT_NOTIME_BUILTIN_BACKEND	1
#else
# define JENT_NOTIME_BUILTIN_BACKEND	0
#endif

static int jent_notime_init_flags(void **ctx, unsigned int flags)
{
	struct jent_notime_ctx *thread_ctx;
	long ncpu = jent_ncpu();

	if (ncpu < 0)
		return (int)ncpu;

	/*
	 * We need at least two CPUs to enable the timer thread - and a thread
	 * backend: freestanding builds have only the stub that fails to start
	 * one (see jitterentropy-arch-thread.c), whatever the CPU count.
	 */
	if (ncpu < 2 || !JENT_NOTIME_BUILTIN_BACKEND)
		return -ENOENT;

	thread_ctx = jent_zalloc(sizeof(struct jent_notime_ctx), flags);
	if (!thread_ctx)
		return -ENOMEM;

	thread_ctx->notime_cpu = jent_notime_cpu_configured ?
				 jent_notime_cpu : JENT_NOTIME_CPU_DEFAULT;

	*ctx = thread_ctx;

	return 0;
}

/* The handler interface has no flags: an external handler gets the defaults. */
JENT_PRIVATE_STATIC
int jent_notime_init(void **ctx)
{
	return jent_notime_init_flags(ctx, 0);
}

JENT_PRIVATE_STATIC
void jent_notime_fini(void *ctx)
{
	struct jent_notime_ctx *thread_ctx = (struct jent_notime_ctx *)ctx;

	if (thread_ctx)
		jent_zfree(thread_ctx, sizeof(struct jent_notime_ctx));
}

#else /* JENT_CONF_ENABLE_INTERNAL_TIMER */

int jent_notime_init(void **ctx) { (void)ctx; return 0; }
void jent_notime_fini(void *ctx) { (void)ctx; }

#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER

static int jent_notime_start(void *ctx,
			    jent_notime_start_routine start_routine,
			    void *arg)
{
	struct jent_notime_ctx *thread_ctx = (struct jent_notime_ctx *)ctx;

	if (!thread_ctx)
		return -EINVAL;

	return jent_notime_thread_create(thread_ctx, start_routine, arg);
}

static void jent_notime_stop(void *ctx)
{
	struct jent_notime_ctx *thread_ctx = (struct jent_notime_ctx *)ctx;

	/* defensive check */
	if (ctx == NULL)
		return;

	jent_notime_thread_join(thread_ctx);
}

static struct jent_notime_thread jent_notime_thread_builtin = {
	.jent_notime_init  = jent_notime_init,
	.jent_notime_fini  = jent_notime_fini,
	.jent_notime_start = jent_notime_start,
	.jent_notime_stop  = jent_notime_stop
};

/***************************************************************************
 * Timer-less timer replacement
 *
 * If there is no high-resolution hardware timer available, we create one
 * ourselves. This logic is only used when the initialization identifies
 * that no suitable time source is available.
 ***************************************************************************/

/*
 * Both are process-wide and one-way: the first is set by a startup that had to
 * fall back to the counting thread, the second by any startup at all, and every
 * later caller reads them from whichever thread it runs on. Atomic for the
 * reason given in arch/jitterentropy-arch-atomic.h - the store releases the
 * decision and the load acquires it, so a thread told that the internal timer
 * is forced also sees the state the forcing thread built.
 */
static int jent_force_internal_timer = 0;
static int jent_notime_switch_blocked = 0;

void jent_notime_block_switch(void)
{
	jent_atomic_store_int(&jent_notime_switch_blocked, 1);
}

int jent_notime_set_cpu(unsigned long cpu)
{
	/* Configuration is only allowed before the library is initialized. */
	if (jent_atomic_load_int(&jent_notime_switch_blocked))
		return -EAGAIN;

	jent_notime_cpu = cpu;
	jent_notime_cpu_configured = 1;
	return 0;
}

static struct jent_notime_thread *notime_thread = &jent_notime_thread_builtin;

/*
 * The registered handler, copied: the caller's struct may be a temporary or
 * change later, and is read only while it is registered.
 */
static struct jent_notime_thread jent_notime_thread_external;

/**
 * Timer-replacement loop
 *
 * @brief The measurement loop triggers the read of the value from the
 * counter function. It conceptually acts as the low resolution
 * samples timer from a ring oscillator.
 */
#ifdef JENT_PTHREAD
static void *jent_notime_sample_timer(void *arg)
#else
static int jent_notime_sample_timer(void *arg)
#endif
{
	struct rand_data *ec = (struct rand_data *)arg;
	struct jent_notime_ctx *thread_ctx =
		(struct jent_notime_ctx *)ec->notime_thread_ctx;

	/* Best-effort pin; only the builtin handler's ctx has this layout. */
	if (thread_ctx && notime_thread == &jent_notime_thread_builtin) {
		unsigned long cpu = thread_ctx->notime_cpu;

		/*
		 * Asked here, of the affinity inherited from the reader that
		 * started this thread, not of the allocating thread's.
		 */
		if (cpu == JENT_NOTIME_CPU_DEFAULT) {
			long highest = jent_cpu_highest();

			if (highest >= 0)
				(void)jent_thread_pin_to_cpu(
					(unsigned long)highest);
		} else {
			(void)jent_thread_pin_to_cpu(cpu);
		}
	}

	ec->notime_timer = 0;

	while (1) {
		if (ec->notime_interrupt)
			goto out;

		ec->notime_timer++;
	}

out:
#ifdef JENT_PTHREAD
	return NULL;
#else
	return 0;
#endif
}

/*
 * Enable the clock: spawn a new thread that holds a counter.
 *
 * Note, although creating a thread is expensive, we do that every time a
 * caller wants entropy from us and terminate the thread afterwards. This
 * is to ensure an attacker cannot easily identify the ticking thread.
 */
int jent_notime_settick(struct rand_data *ec)
{
	int ret;

	if (!ec->enable_notime || !notime_thread)
		return 0;

	ec->notime_interrupt = 0;
	ec->notime_prev_timer = 0;
	ec->notime_timer = 0;

	ret = notime_thread->jent_notime_start(ec->notime_thread_ctx,
					      jent_notime_sample_timer, ec);
	if (!ret)
		ec->notime_running = 1;

	return ret;
}

/* Stops only a thread a start created, and only once. */
void jent_notime_unsettick(struct rand_data *ec)
{
	if (!ec->enable_notime || !notime_thread || !ec->notime_running)
		return;

	ec->notime_interrupt = 1;
	notime_thread->jent_notime_stop(ec->notime_thread_ctx);
	ec->notime_running = 0;
}

/*
 * One read of the counter. Where a uint64_t takes two loads, a read may be
 * torn by a carry into the upper word: a reader's tear shows as two reads
 * disagreeing, a writer preempted between its stores (high word first) shows
 * a low word of all ones. Both are read again.
 */
static uint64_t jent_notime_read(struct rand_data *ec)
{
#if defined(UINTPTR_MAX) && defined(UINT64_MAX) && UINTPTR_MAX >= UINT64_MAX
	return ec->notime_timer;
#else
	uint64_t a, b;

	for (;;) {
		a = ec->notime_timer;
		b = ec->notime_timer;

		/* The reader's own tear. */
		if (b < a || b - a > 0x80000000U)
			continue;

		/* Possibly the writer's, torn high: let it complete. */
		if ((b & UINT64_C(0xffffffff)) == UINT64_C(0xffffffff)) {
			jent_yield();
			continue;
		}

		return b;
	}
#endif
}

void jent_get_nstime_internal(struct rand_data *ec, uint64_t *out)
{
	if (ec->enable_notime) {
		uint64_t now;

		/*
		 * Allow the counting thread to be initialized and guarantee
		 * that it ticked since last time we looked. The value compared
		 * is the value returned, so a torn read cannot slip through.
		 */
		while ((now = jent_notime_read(ec)) <= ec->notime_prev_timer)
			jent_yield();

		ec->notime_prev_timer = now;
		*out = now;
	} else {
		jent_get_nstime(out);
	}
}

static inline int jent_notime_enable_thread(struct rand_data *ec,
					    unsigned int flags)
{
	int ret;

	if (!notime_thread)
		return 0;

	/* The builtin handler honors the collector flags for its context. */
	if (notime_thread == &jent_notime_thread_builtin)
		ret = jent_notime_init_flags(&ec->notime_thread_ctx, flags);
	else
		ret = notime_thread->jent_notime_init(&ec->notime_thread_ctx);

	/* A failed init's context is never handed to fini. */
	if (ret)
		ec->notime_thread_ctx = NULL;

	return ret;
}

void jent_notime_disable(struct rand_data *ec)
{
	/* Only a context an init handed back is torn down. */
	if (notime_thread && ec->notime_thread_ctx) {
		notime_thread->jent_notime_fini(ec->notime_thread_ctx);
		ec->notime_thread_ctx = NULL;
	}
}

int jent_notime_enable(struct rand_data *ec, unsigned int flags)
{
	/*
	 * Read once and used twice: the two tests below are the same question
	 * - has a startup already established the internal timer - and a
	 * second load could answer them differently if another thread forces
	 * it in between, which would run the startup a second time.
	 */
	int forced = jent_atomic_load_int(&jent_force_internal_timer);

	/* Use internal timer */
	if (forced || (flags & JENT_FORCE_INTERNAL_TIMER)) {
		/* Self test not run yet */
		if (!forced &&
		    jent_time_entropy_init(ec->osr,
					   flags | JENT_FORCE_INTERNAL_TIMER))
			return EHEALTH;

		ec->enable_notime = 1;
		return jent_notime_enable_thread(ec, flags);
	}

	return 0;
}

/*
 * Whether the builtin counting thread cannot run here for want of a second
 * CPU or of a thread backend - the -ENOENT of jent_notime_init_flags(),
 * which the collector allocation reports as a NULL like a failed
 * allocation. More memory, or another try, does not mend it. An external
 * handler brings its own rule.
 */
int jent_notime_cpus_missing(void)
{
	long ncpu;

	if (notime_thread != &jent_notime_thread_builtin)
		return 0;

	if (!JENT_NOTIME_BUILTIN_BACKEND)
		return 1;

	ncpu = jent_ncpu();
	return ncpu >= 0 && ncpu < 2;
}

int jent_notime_switch(struct jent_notime_thread *new_thread)
{
	if (jent_atomic_load_int(&jent_notime_switch_blocked))
		return -EAGAIN;

	/* Reject incomplete handlers, which would hang or crash on first use. */
	if (!new_thread || !new_thread->jent_notime_init ||
	    !new_thread->jent_notime_fini || !new_thread->jent_notime_start ||
	    !new_thread->jent_notime_stop)
		return -EINVAL;

	/* The builtin one is recognized by its address, so it is not copied. */
	if (new_thread == &jent_notime_thread_builtin) {
		notime_thread = &jent_notime_thread_builtin;
		return 0;
	}

	jent_notime_thread_external = *new_thread;
	notime_thread = &jent_notime_thread_external;
	return 0;
}

void jent_notime_force(void)
{
	jent_atomic_store_int(&jent_force_internal_timer, 1);
}

int jent_notime_forced(void)
{
	return jent_atomic_load_int(&jent_force_internal_timer);
}

#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */
