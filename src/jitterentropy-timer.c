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

/* Timer-less entropy source */
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER

/***************************************************************************
 * Thread handler
 ***************************************************************************/

/*
 * CPU the counting thread pins itself to. When the caller has not set one
 * via jent_notime_set_cpu(), jent_notime_init() defaults to the
 * highest-numbered online CPU.
 */
static int jent_notime_cpu_configured = 0;
static unsigned long jent_notime_cpu = 0;

JENT_PRIVATE_STATIC
int jent_notime_init(void **ctx)
{
	struct jent_notime_ctx *thread_ctx;
	long ncpu = jent_ncpu();

	if (ncpu < 0)
		return (int)ncpu;

	/* We need at least two CPUs to enable the timer thread */
	if (ncpu < 2)
		return -ENOENT;

	thread_ctx = jent_zalloc(sizeof(struct jent_notime_ctx));
	if (!thread_ctx)
		return -ENOMEM;

	/*
	 * Pin the counting thread to a dedicated CPU - the caller-configured
	 * one, or by default the highest-numbered online CPU. The consumer
	 * thread is left unpinned, so on the >= 2 CPUs guaranteed above the
	 * scheduler keeps the two on separate cores and the counter keeps
	 * ticking while the consumer busy-waits.
	 */
	if (jent_notime_cpu_configured)
		thread_ctx->notime_cpu = jent_notime_cpu;
	else
		thread_ctx->notime_cpu = (unsigned long)(ncpu - 1);

	*ctx = thread_ctx;

	return 0;
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

static int jent_force_internal_timer = 0;
static int jent_notime_switch_blocked = 0;

void jent_notime_block_switch(void)
{
	jent_notime_switch_blocked = 1;
}

int jent_notime_set_cpu(unsigned long cpu)
{
	/* Configuration is only allowed before the library is initialized. */
	if (jent_notime_switch_blocked)
		return -EAGAIN;

	jent_notime_cpu = cpu;
	jent_notime_cpu_configured = 1;
	return 0;
}

static struct jent_notime_thread *notime_thread = &jent_notime_thread_builtin;

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

	/*
	 * Best-effort pin to a dedicated CPU; a failure here is ignored as
	 * the counter still ticks on whatever CPU the scheduler picks.
	 */
	if (thread_ctx)
		(void)jent_thread_pin_to_cpu(thread_ctx->notime_cpu);

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
	if (!ec->enable_notime || !notime_thread)
		return 0;

	ec->notime_interrupt = 0;
	ec->notime_prev_timer = 0;
	ec->notime_timer = 0;

	return notime_thread->jent_notime_start(ec->notime_thread_ctx,
					       jent_notime_sample_timer, ec);
}

void jent_notime_unsettick(struct rand_data *ec)
{
	if (!ec->enable_notime || !notime_thread)
		return;

	ec->notime_interrupt = 1;
	notime_thread->jent_notime_stop(ec->notime_thread_ctx);
}

void jent_get_nstime_internal(struct rand_data *ec, uint64_t *out)
{
	if (ec->enable_notime) {
		/*
		 * Allow the counting thread to be initialized and guarantee
		 * that it ticked since last time we looked.
		 *
		 * Note, we do not use an atomic operation here for reading
		 * jent_notime_timer since if this integer is garbled, it even
		 * adds to entropy. But on most architectures, read/write
		 * of an uint64_t should be atomic anyway.
		 */
		while (ec->notime_timer == ec->notime_prev_timer)
			jent_yield();

		ec->notime_prev_timer = ec->notime_timer;
		*out = ec->notime_prev_timer;
	} else {
		jent_get_nstime(out);
	}
}

static inline int jent_notime_enable_thread(struct rand_data *ec)
{
	if (notime_thread)
		return notime_thread->jent_notime_init(&ec->notime_thread_ctx);
	return 0;
}

void jent_notime_disable(struct rand_data *ec)
{
	if (notime_thread)
		notime_thread->jent_notime_fini(ec->notime_thread_ctx);
}

int jent_notime_enable(struct rand_data *ec, unsigned int flags)
{
	/* Use internal timer */
	if (jent_force_internal_timer || (flags & JENT_FORCE_INTERNAL_TIMER)) {
		/* Self test not run yet */
		if (!jent_force_internal_timer &&
		    jent_time_entropy_init(ec->osr,
					   flags | JENT_FORCE_INTERNAL_TIMER))
			return EHEALTH;

		ec->enable_notime = 1;
		return jent_notime_enable_thread(ec);
	}

	return 0;
}

int jent_notime_switch(struct jent_notime_thread *new_thread)
{
	if (jent_notime_switch_blocked)
		return -EAGAIN;
	notime_thread = new_thread;
	return 0;
}

void jent_notime_force(void)
{
	jent_force_internal_timer = 1;
}

int jent_notime_forced(void)
{
	return jent_force_internal_timer;
}

#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */
