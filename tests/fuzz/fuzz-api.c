/*
 * Jitter RNG: libFuzzer harness misusing the public API of jitterentropy.h
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
 * The API as a hostile caller uses it: null pointers where an object is
 * expected, lengths of zero and of SIZE_MAX, oversampling rates far outside
 * the range the library clamps, flag words with every undefined bit set,
 * collectors freed twice, entropy read from an instance that was never
 * initialized, calls in an order no documented sequence produces.
 *
 * The library is linked rather than absorbed - what is under test is the
 * surface jitterentropy.h declares, which is what an application reaches, and
 * a harness that included the sources could reach past it.
 *
 * The input is read as a little program: one byte selects the call, the bytes
 * after it are its arguments, and the interpreter runs until the input is
 * exhausted or the operation budget is spent. Every argument is drawn through
 * a picker that folds the fuzzer's byte onto the boundaries of that argument's
 * range - 0, 1, the maximum, one past it - because a uniformly random 32-bit
 * oversampling rate is almost never the interesting one.
 *
 * What the harness asserts is the contract the header states, not the values
 * the RNG produces:
 *
 *   - no call writes outside the buffer it was given, which is checked with
 *     guard bytes around and behind the requested length rather than left to
 *     the sanitizer, so it holds in a build without one,
 *   - a read returns the full length asked for or one of the documented
 *     negative codes, never a partial one,
 *   - the misuse the header names - no collector, no buffer, a buffer too
 *     small - is reported rather than acted on, and
 *   - the startup and the self test report a documented code.
 *
 * A failed assertion aborts, which is what libFuzzer records as a crash. The
 * harness therefore uses assert() deliberately and must be built with NDEBUG
 * unset; the CMake build below states that.
 *
 * Three limits keep the runs finite. None hides a defect - the first two cap
 * how much work one call is asked to do, not which arguments reach it:
 *
 *   - the hash loop field of the flags is clamped. It is a multiplier on the
 *     conditioning performed for every single time delta, so the top setting
 *     of JENT_HASHLOOP_128 makes an allocation and a read a hundred times
 *     more expensive than the default - with the highest accepted oversampling
 *     rate, seconds per call in a plain build and over half a minute under the
 *     sanitizers. Left unclamped it does not merely slow the fuzzer down, it
 *     stops it: the search collapses onto inputs that time out, and a
 *     coverage-guided run measured under one execution per second. What is
 *     worth reaching is the decoding, and any non-default setting reaches it,
 *     so the ceiling is placed just above the default.
 *   - the memory size field of the flags is clamped, as the entropy pool is
 *     allocated and zeroed per collector: the 512 MB the field can ask for is
 *     a quarter of a second and half a gigabyte resident for one allocation,
 *     and the four an input may hold at once are past the RSS limit libFuzzer
 *     stops the run at. The size does not change how the pool is walked - the
 *     number of accesses is the same - so nothing but the footprint is given
 *     up here, and
 *   - JENT_FORCE_INTERNAL_TIMER is masked out of everything that could reach
 *     jent_entropy_init_ex(): forcing the internal timer is one-way
 *     process-wide state, so one input would put every later input in the
 *     fuzzer's process on the counting thread. The refusal paths of that flag
 *     are covered by unit-base-api and unit-notime, and the contradiction
 *     with JENT_DISABLE_INTERNAL_TIMER is still exercised through
 *     jent_entropy_collector_alloc(), which rejects it before anything is
 *     forced.
 */

/*
 * The harness states the API contract in assert(), so the assertions are the
 * test and not a debugging aid: a build that defines NDEBUG - any CMake
 * Release build does - would run the calls and check nothing. Undefined here
 * rather than argued about on the command line, before <assert.h> is reached.
 */
#ifdef NDEBUG
# undef NDEBUG
#endif

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jitterentropy.h>

/* The most negative documented return code of jent_read_entropy*(). */
#define FZ_ERR_LAST	JENT_ERR_SELFTEST
/* The largest documented return code of jent_entropy_init*(). */
#define FZ_INIT_LAST	EGCD

/* Collectors held at once, so that a run can free the wrong one, or one twice. */
#define FZ_SLOTS	4
/* Calls per input: the noise source measures real time, so runs are not free. */
#define FZ_MAX_OPS	8
/* The largest buffer any call is given, and the guard on each side of it. */
#define FZ_MAXLEN	1024
#define FZ_GUARD	16
#define FZ_FILL		0x5a

/*
 * The memory size the flags may ask for. Higher fields are folded onto this
 * one rather than dropped, so the decoding of a too-large field is still
 * reached - jent_memsize() clamps it, and that clamp is what this leaves
 * exercised.
 */
#define FZ_MAX_MEMSIZE_FIELD	13	/* JENT_MAX_MEMSIZE_4MB */

/*
 * The hash loop the flags may ask for, folded the same way and for the same
 * reason - except that this one multiplies the work of every time delta rather
 * than the cost of one allocation, which is why the ceiling sits one step
 * above the default rather than anywhere near the JENT_HASHLOOP_128 the field
 * can express. See the head of the file.
 */
#define FZ_MAX_HASHLOOP_FIELD	1	/* JENT_HASHLOOP_2 */

struct fz_state {
	const uint8_t *data;
	size_t len;
	size_t pos;
};

/* The input as a stream. Exhausted is a state, not an error: it ends the run. */
static int fz_eof(const struct fz_state *s)
{
	return s->pos >= s->len;
}

static uint8_t fz_u8(struct fz_state *s)
{
	return fz_eof(s) ? 0 : s->data[s->pos++];
}

static uint32_t fz_u32(struct fz_state *s)
{
	uint32_t v = 0;
	unsigned int i;

	for (i = 0; i < 4; i++)
		v = (v << 8) | fz_u8(s);

	return v;
}

/*
 * The oversampling rate. JENT_MIN_OSR and JENT_MAX_OSR are build-time bounds
 * the library clamps to and does not publish, so what is drawn here is the
 * range around them: nothing, the smallest sensible value, values inside the
 * documented range, and the far end of unsigned int where a clamp that
 * computes rather than compares overflows.
 */
static unsigned int fz_osr(struct fz_state *s)
{
	uint8_t pick = fz_u8(s);

	switch (pick % 9) {
	case 0:
		return 0;			/* "the default", not a rate */
	case 1:
		return 1;			/* below JENT_MIN_OSR, clamped up */
	case 2:
		return 3;			/* JENT_MIN_OSR, the documented default */
	case 3:
		/*
		 * JENT_MAX_OSR, the highest rate an allocation accepts - and
		 * the most expensive one, as it is the number of times every
		 * measurement is repeated.
		 */
		return 20;
	case 4:
		return 21;			/* one past it, to be refused */
	case 5:
		return UINT_MAX - 1;
	case 6:
		return UINT_MAX;
	case 7:
		return (unsigned int)pick;
	default:
		return fz_u32(s);
	}
}

/*
 * The flag word: the defined bits, the fields, and the bits between them that
 * no flag uses and every caller is free to get wrong.
 */
static unsigned int fz_flags(struct fz_state *s)
{
	unsigned int flags = fz_u32(s);
	unsigned int memsize = (flags & JENT_MAX_MEMSIZE_MASK) >>
			       JENT_FLAGS_TO_MEMSIZE_SHIFT;
	unsigned int hashloop = JENT_FLAGS_TO_HASHLOOP(flags);

	if (memsize > FZ_MAX_MEMSIZE_FIELD) {
		memsize %= (FZ_MAX_MEMSIZE_FIELD + 1);
		flags = (flags & ~(unsigned int)JENT_MAX_MEMSIZE_MASK) |
			(memsize << JENT_FLAGS_TO_MEMSIZE_SHIFT);
	}

	if (hashloop > FZ_MAX_HASHLOOP_FIELD) {
		hashloop %= (FZ_MAX_HASHLOOP_FIELD + 1);
		flags = (flags & ~(unsigned int)JENT_MAX_HASHLOOP_MASK) |
			JENT_HASHLOOP_TO_FLAGS(hashloop);
	}

	return flags;
}

/* The same, for the calls that must not force the internal timer. See above. */
static unsigned int fz_flags_no_force(struct fz_state *s)
{
	return fz_flags(s) & ~(unsigned int)JENT_FORCE_INTERNAL_TIMER;
}

/* A length, folded onto the boundaries of what the buffer below can hold. */
static size_t fz_len(struct fz_state *s)
{
	uint8_t pick = fz_u8(s);

	switch (pick % 8) {
	case 0:
		return 0;
	case 1:
		return 1;
	case 2:
		return 63;
	case 3:
		return 64;			/* one XDRBG block */
	case 4:
		return 65;
	case 5:
		return FZ_MAXLEN - 1;
	case 6:
		return FZ_MAXLEN;
	default:
		return (size_t)pick % (FZ_MAXLEN + 1);
	}
}

/*
 * The buffer every call writes into: the requested length sits between two
 * guard regions, and the rest of it is filled as well, so that a write past
 * the length - not only past the allocation - is caught.
 */
static unsigned char fz_area[FZ_GUARD + FZ_MAXLEN + FZ_GUARD];

static char *fz_buf(void)
{
	memset(fz_area, FZ_FILL, sizeof(fz_area));

	return (char *)(fz_area + FZ_GUARD);
}

static void fz_check_buf(size_t len)
{
	size_t i;

	assert(len <= FZ_MAXLEN);

	for (i = 0; i < FZ_GUARD; i++) {
		assert(fz_area[i] == FZ_FILL);
		assert(fz_area[FZ_GUARD + FZ_MAXLEN + i] == FZ_FILL);
	}

	/* Behind what was asked for: still the fill, never the RNG's output. */
	for (i = len; i < FZ_MAXLEN; i++)
		assert(fz_area[FZ_GUARD + i] == FZ_FILL);
}

/* The string a call claims to have written stays inside the buffer it got. */
static void fz_check_string(size_t buflen, size_t len)
{
	assert(buflen <= FZ_MAXLEN);
	assert(len < buflen);
	fz_check_buf(len + 1);
}

static void fz_check_read(ssize_t ret, size_t len)
{
	/* The full length or a documented failure - never a partial read. */
	if (ret >= 0)
		assert((size_t)ret == len);
	else
		assert(ret >= FZ_ERR_LAST);

	fz_check_buf(ret > 0 ? (size_t)ret : 0);
}

/* The calls the interpreter below dispatches to. */
enum fz_op {
	FZ_OP_ALLOC = 0,
	FZ_OP_FREE,
	FZ_OP_READ,
	FZ_OP_READ_NULL,
	FZ_OP_READ_SAFE,
	FZ_OP_READ_SAFE_NULL,
	FZ_OP_STATUS,
	FZ_OP_UUID,
	FZ_OP_SELFTEST,
	FZ_OP_INIT,
	FZ_OP_ALLOC_CONTRADICTION,
	FZ_OP_MISC,
	FZ_OP_LAST
};

static void fz_op_alloc(struct fz_state *s, struct rand_data **slots)
{
	unsigned int slot = fz_u8(s) % FZ_SLOTS;
	unsigned int osr = fz_osr(s);
	unsigned int flags = fz_flags_no_force(s);
	struct rand_data *ec = jent_entropy_collector_alloc(osr, flags);

	/*
	 * Overwriting a slot that still holds a collector leaks it, so the old
	 * one goes first - the double free is op FZ_OP_FREE's business, and a
	 * leak would be reported against the wrong call.
	 */
	if (ec) {
		jent_entropy_collector_free(slots[slot]);
		slots[slot] = ec;
	}
}

static void fz_op_free(struct fz_state *s, struct rand_data **slots)
{
	unsigned int slot = fz_u8(s) % FZ_SLOTS;

	jent_entropy_collector_free(slots[slot]);

	/*
	 * Whether the slot is cleared is the fuzzer's choice: leaving it set
	 * is a use-after-free the harness would perform itself, so the pointer
	 * is dropped either way and only the "free a slot that holds nothing"
	 * case is kept - jent_entropy_collector_free(NULL) is a documented
	 * no-op, and it is the one an application repeats by accident.
	 */
	slots[slot] = NULL;
	if (fz_u8(s) & 1)
		jent_entropy_collector_free(slots[slot]);
}

static void fz_op_read(struct fz_state *s, struct rand_data **slots)
{
	unsigned int slot = fz_u8(s) % FZ_SLOTS;
	size_t len = fz_len(s);
	char *buf = fz_buf();

	fz_check_read(jent_read_entropy(slots[slot], buf, len), len);
}

/* The arguments the header names as misuse: no collector, no buffer. */
static void fz_op_read_null(struct fz_state *s, struct rand_data **slots)
{
	unsigned int slot = fz_u8(s) % FZ_SLOTS;
	size_t len = fz_len(s);
	char *buf = fz_buf();

	assert(jent_read_entropy(NULL, buf, len) == JENT_ERR_EINVAL);
	fz_check_buf(0);

	/*
	 * A length with no buffer to put it in, up to the largest one there
	 * is: rejected on the argument, so no memory is needed to ask for it.
	 */
	assert(jent_read_entropy(slots[slot], NULL, len ? len : SIZE_MAX) ==
	       JENT_ERR_EINVAL);
	assert(jent_read_entropy(NULL, NULL, SIZE_MAX) == JENT_ERR_EINVAL);
}

static void fz_op_read_safe(struct fz_state *s, struct rand_data **slots)
{
	unsigned int slot = fz_u8(s) % FZ_SLOTS;
	size_t len = fz_len(s);
	char *buf = fz_buf();

	/*
	 * The reallocating entry point takes the collector by reference and
	 * may replace it, so the slot is handed over as it stands - including
	 * when it holds nothing.
	 */
	fz_check_read(jent_read_entropy_safe(&slots[slot], buf, len), len);
}

static void fz_op_read_safe_null(struct fz_state *s, struct rand_data **slots)
{
	unsigned int slot = fz_u8(s) % FZ_SLOTS;
	size_t len = fz_len(s);
	char *buf = fz_buf();

	assert(jent_read_entropy_safe(NULL, buf, len) == JENT_ERR_EINVAL);
	fz_check_buf(0);

	assert(jent_read_entropy_safe(&slots[slot], NULL,
				      len ? len : SIZE_MAX) == JENT_ERR_EINVAL);
}

static void fz_op_status(struct fz_state *s, struct rand_data **slots)
{
	unsigned int slot = fz_u8(s) % FZ_SLOTS;
	size_t buflen = fz_len(s);
	char *buf = fz_buf();
	int ret;

	assert(jent_status(slots[slot], NULL, buflen) == -1);
	assert(jent_status(slots[slot], buf, 0) == -1);
	fz_check_buf(0);

	buf = fz_buf();
	ret = jent_status(slots[slot], buf, buflen);
	if (!buflen) {
		assert(ret == -1);
		fz_check_buf(0);
	} else {
		/*
		 * Whatever the verdict, the document is terminated inside the
		 * buffer: a truncated status is still a C string, and the
		 * bytes behind it are the caller's.
		 */
		fz_check_string(buflen, strlen(buf));
	}
}

static void fz_op_uuid(struct fz_state *s, struct rand_data **slots)
{
	unsigned int slot = fz_u8(s) % FZ_SLOTS;
	size_t buflen = fz_len(s);
	char *buf = fz_buf();
	int ret;

	assert(jent_uuid(slots[slot], NULL, buflen) == -1);
	assert(jent_uuid(NULL, buf, buflen) == -1);
	assert(jent_uuid(slots[slot], buf, 0) == -1);
	fz_check_buf(0);

	buf = fz_buf();
	ret = jent_uuid(slots[slot], buf, buflen);
	if (ret) {
		/* A refused call writes nothing at all. */
		fz_check_buf(0);
	} else {
		assert(buflen >= JENT_UUID_STRLEN);
		assert(strlen(buf) == JENT_UUID_STRLEN - 1);
		fz_check_string(buflen, strlen(buf));
	}
}

static void fz_op_selftest(struct fz_state *s, struct rand_data **slots)
{
	unsigned int slot = fz_u8(s) % FZ_SLOTS;
	int ret;

	/* Without an instance to bind the verdict to, and with one. */
	ret = jent_selftest(NULL);
	assert(ret == 0 || ret == EHASH);

	ret = jent_selftest(slots[slot]);
	assert(ret == 0 || ret == EHASH);
}

static void fz_op_init(struct fz_state *s, struct rand_data **slots)
{
	unsigned int osr = fz_osr(s);
	unsigned int flags = fz_flags_no_force(s);
	int ret;

	(void)slots;

	ret = jent_entropy_init_ex(osr, flags);
	assert(ret >= 0 && ret <= FZ_INIT_LAST);

	if (fz_u8(s) & 1) {
		ret = jent_entropy_init();
		assert(ret >= 0 && ret <= FZ_INIT_LAST);
	}
}

/*
 * Asking for the internal timer and forbidding it in the same call. Refused at
 * the allocation, before anything is forced, which is why this reaches the
 * flag the other operations mask out.
 */
static void fz_op_alloc_contradiction(struct fz_state *s,
				      struct rand_data **slots)
{
	unsigned int osr = fz_osr(s);
	unsigned int flags = fz_flags(s) | JENT_FORCE_INTERNAL_TIMER |
			     JENT_DISABLE_INTERNAL_TIMER;

	(void)slots;

	assert(jent_entropy_collector_alloc(osr, flags) == NULL);
}

/* The calls that take no collector, and the two that take a pointer to one. */
static void fz_op_misc(struct fz_state *s, struct rand_data **slots)
{
	uint8_t pick = fz_u8(s);

	(void)slots;

	switch (pick % 4) {
	case 0:
		assert(jent_version() == JENT_VERSION);
		break;
	case 1: {
		int ret = jent_secure_memory_supported();

		assert(ret == 0 || ret == 1);
		break;
	}
	case 2: {
		/*
		 * Registering nothing is how a caller unregisters. The switch
		 * is refused with -EAGAIN once a collector in FIPS mode has
		 * bound the callback it runs with, which an earlier operation
		 * of this same input may have allocated - so both answers are
		 * the contract, and anything else is not.
		 */
		int ret = jent_set_fips_failure_callback(NULL);

		assert(ret == 0 || ret == -EAGAIN);
		break;
	}
	default:
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
		/*
		 * A CPU index no machine has: advisory, so the answer is a
		 * status and not a promise, and it must not be a stray value.
		 */
		assert(jent_entropy_set_notime_cpu((unsigned long)-1) != 1);

		/* No implementation at all, which has to be refused. */
		assert(jent_entropy_switch_notime_impl(NULL) != 0);
#endif
		break;
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct rand_data *slots[FZ_SLOTS] = { NULL };
	struct fz_state s;
	unsigned int op;

	s.data = data;
	s.len = size;
	s.pos = 0;

	for (op = 0; op < FZ_MAX_OPS && !fz_eof(&s); op++) {
		switch ((enum fz_op)(fz_u8(&s) % FZ_OP_LAST)) {
		case FZ_OP_ALLOC:
			fz_op_alloc(&s, slots);
			break;
		case FZ_OP_FREE:
			fz_op_free(&s, slots);
			break;
		case FZ_OP_READ:
			fz_op_read(&s, slots);
			break;
		case FZ_OP_READ_NULL:
			fz_op_read_null(&s, slots);
			break;
		case FZ_OP_READ_SAFE:
			fz_op_read_safe(&s, slots);
			break;
		case FZ_OP_READ_SAFE_NULL:
			fz_op_read_safe_null(&s, slots);
			break;
		case FZ_OP_STATUS:
			fz_op_status(&s, slots);
			break;
		case FZ_OP_UUID:
			fz_op_uuid(&s, slots);
			break;
		case FZ_OP_SELFTEST:
			fz_op_selftest(&s, slots);
			break;
		case FZ_OP_INIT:
			fz_op_init(&s, slots);
			break;
		case FZ_OP_ALLOC_CONTRADICTION:
			fz_op_alloc_contradiction(&s, slots);
			break;
		case FZ_OP_MISC:
		case FZ_OP_LAST:
			fz_op_misc(&s, slots);
			break;
		}
	}

	for (op = 0; op < FZ_SLOTS; op++)
		jent_entropy_collector_free(slots[op]);

	return 0;
}

#ifdef JENT_FUZZ_STANDALONE

/*
 * The same harness without libFuzzer, so that a build with any compiler can
 * run it: with arguments it replays the files it is given - a crash libFuzzer
 * found is reproduced by handing its input here - and without them it runs a
 * fixed sweep, which is what the suite registers as a test case.
 *
 * The sweep's inputs come from a counter through a mixing function rather than
 * from rand(): the point of a regression case is that it is the same on every
 * machine and in every run.
 */

static int fz_run_file(const char *path)
{
	unsigned char buf[4096];
	size_t len;
	FILE *f = fopen(path, "rb");

	if (!f) {
		fprintf(stderr, "fuzz-api: cannot open %s\n", path);
		return 1;
	}

	len = fread(buf, 1, sizeof(buf), f);
	fclose(f);

	LLVMFuzzerTestOneInput(buf, len);
	printf("fuzz-api: %s (%zu bytes) survived\n", path, len);

	return 0;
}

#define FZ_SWEEP_INPUTS		32
#define FZ_SWEEP_LEN		24

int main(int argc, char *argv[])
{
	unsigned char input[FZ_SWEEP_LEN];
	unsigned int i, j;
	int ret = 0;

	if (argc > 1) {
		for (i = 1; i < (unsigned int)argc; i++)
			ret |= fz_run_file(argv[i]);

		return ret;
	}

	for (i = 0; i < FZ_SWEEP_INPUTS; i++) {
		uint64_t x = 0x9e3779b97f4a7c15ULL * (i + 1);

		for (j = 0; j < FZ_SWEEP_LEN; j++) {
			x ^= x >> 30;
			x *= 0xbf58476d1ce4e5b9ULL;
			x ^= x >> 27;
			input[j] = (unsigned char)(x >> 24);
		}

		LLVMFuzzerTestOneInput(input, sizeof(input));
	}

	printf("fuzz-api: %u inputs survived\n", FZ_SWEEP_INPUTS);

	return 0;
}

#endif /* JENT_FUZZ_STANDALONE */
