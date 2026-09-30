/*
 * Jitter RNG: a FreeBSD kernel module that runs the library once on load
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
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

/*
 * The FreeBSD kernel is a target of its own in jitterentropy.h and in every
 * arch/ backend - malloc(9), mp_ncpus, kern_yield(9), arc4random_buf() from
 * libkern - and nothing else in this tree builds it. This module is that
 * build: the library sources compiled with the kernel's own flags, linked
 * into a .ko, and run once from the MOD_LOAD handler.
 *
 * It initializes the library, allocates a collector, generates 64 bytes,
 * prints them and the jent_status() document, and frees the collector. Any
 * failure is returned from MOD_LOAD, which makes kldload(8) fail - that, and
 * the PASS line on the console, is what CI checks.
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/systm.h>
#include <sys/malloc.h>

#include "jitterentropy.h"

#define JENT_KMOD_TEST_BYTES	64
#define JENT_KMOD_STATUS_LEN	8192

static MALLOC_DEFINE(M_JENT_KMOD_TEST, "jent_kmod_test",
		     "Jitter RNG kernel module test buffers");

static void jent_kmod_test_hex(const unsigned char *buf, size_t len)
{
	static const char hex[] = "0123456789abcdef";
	char line[2 * JENT_KMOD_TEST_BYTES + 1];
	size_t i;

	if (len > JENT_KMOD_TEST_BYTES)
		len = JENT_KMOD_TEST_BYTES;

	for (i = 0; i < len; i++) {
		line[2 * i] = hex[buf[i] >> 4];
		line[2 * i + 1] = hex[buf[i] & 0x0f];
	}
	line[2 * len] = '\0';

	printf("jitterentropy_test: output %s\n", line);
}

static int jent_kmod_test_run(void)
{
	unsigned char out[JENT_KMOD_TEST_BYTES];
	struct rand_data *ec;
	char *status;
	ssize_t ret;
	int err;

	printf("jitterentropy_test: library version %u\n", jent_version());

	err = jent_entropy_init();
	if (err) {
		printf("jitterentropy_test: FAIL jent_entropy_init() = %d\n",
		       err);
		return (EIO);
	}

	/*
	 * Every backend wipes on free, the kernel one through zfree(9), and
	 * adds wired memory by construction; a 0 here is a broken build.
	 */
	if (!jent_secure_memory_supported()) {
		printf("jitterentropy_test: FAIL no secure memory backend\n");
		return (EIO);
	}

	ec = jent_entropy_collector_alloc(0, 0);
	if (ec == NULL) {
		printf("jitterentropy_test: FAIL "
		       "jent_entropy_collector_alloc()\n");
		return (ENOMEM);
	}

	ret = jent_read_entropy_safe(&ec, (char *)out, sizeof(out));
	if (ret != (ssize_t)sizeof(out)) {
		printf("jitterentropy_test: FAIL jent_read_entropy_safe() = "
		       "%zd\n", ret);
		jent_entropy_collector_free(ec);
		return (EIO);
	}
	jent_kmod_test_hex(out, sizeof(out));

	/* The one caller of snprintf() in the library, run where it is libkern's. */
	status = malloc(JENT_KMOD_STATUS_LEN, M_JENT_KMOD_TEST,
			M_WAITOK | M_ZERO);
	err = jent_status(ec, status, JENT_KMOD_STATUS_LEN);
	if (err) {
		printf("jitterentropy_test: FAIL jent_status() = %d\n", err);
		free(status, M_JENT_KMOD_TEST);
		jent_entropy_collector_free(ec);
		return (EIO);
	}
	printf("jitterentropy_test: status %s\n", status);
	free(status, M_JENT_KMOD_TEST);

	jent_entropy_collector_free(ec);
	explicit_bzero(out, sizeof(out));

	printf("jitterentropy_test: PASS\n");
	return (0);
}

static int jent_kmod_test_modevent(module_t mod, int type, void *arg)
{
	(void)mod;
	(void)arg;

	switch (type) {
	case MOD_LOAD:
		return (jent_kmod_test_run());
	case MOD_UNLOAD:
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t jent_kmod_test_mod = {
	"jitterentropy_test",
	jent_kmod_test_modevent,
	NULL
};

DECLARE_MODULE(jitterentropy_test, jent_kmod_test_mod, SI_SUB_DRIVERS,
	       SI_ORDER_ANY);
MODULE_VERSION(jitterentropy_test, 1);
