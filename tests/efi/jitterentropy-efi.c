/*
 * Jitter RNG: an EFI application, for the build that has no operating system
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
 * The README says the Jitter RNG could run on baremetal, having no need of an
 * operating system: it reads a counter, measures how long a piece of work
 * takes, and hashes the variation. Nothing here tested that claim. Every other
 * program in this tree runs on Linux, Windows, a BSD, macOS or inside the
 * Linux kernel, and each of those supplies an allocator, a clock call, a CPU
 * count and a random pool that the arch/ backends reach for.
 *
 * This is that claim, executed. An EFI application is the smallest environment
 * that can still be booted and watched: firmware, one processor, no scheduler,
 * no threads, no libc, no /proc, no CSPRNG, and a hundred lines of boot
 * services underneath. It runs before an operating system exists.
 *
 * It is deliberately a whole application rather than a compile test. A
 * cross-compilation smoke build - which flake.nix already has several of -
 * proves the backends select and the sources translate. It cannot prove that
 * the counter the freestanding path reads actually moves, that the health
 * tests pass on what it measures, or that a collector can be built at all
 * where the only allocator is AllocatePool(). Those are properties of running,
 * and the failure they guard against is a Jitter RNG that initializes on such
 * a target and hands out something it did not measure.
 *
 * So it generates and it reports, and tests/efi/README.md says what a reader
 * of the output is entitled to conclude from it.
 *
 * What the library needs from its integrator here is six functions - the ones
 * a freestanding C implementation does not provide and the compiler emits
 * calls to regardless. gnu-efi supplies memcpy() and memset(); the other four
 * are below, on top of the EFI boot services. That list is the whole of the
 * porting interface, and jitterentropy.h states it beside the definition of
 * JENT_BAREMETAL.
 */

#include <efi.h>
#include <efilib.h>

#include <jitterentropy.h>

/* Bytes generated, which is what the test harness looks for in the output. */
#define JE_BYTES	32
/* Room for the whole status document; jent_status() refuses a short buffer. */
#define JE_STATUS_LEN	4096

/***************************************************************************
 * The porting interface: what a freestanding C implementation does not have.
 ***************************************************************************/

/*
 * The allocator. AllocatePool() is boot-services memory, which is what an
 * application has to use and what the firmware reclaims when it exits.
 *
 * The Jitter RNG asks for its entropy pool through this, and asks for it
 * zeroed - jent_zalloc() clears what it gets, so nothing here has to. It is
 * not secure memory in the sense the library means: there is no kernel to ask
 * to keep a page off a swap device, and there is no swap device either.
 * jent_secure_memory_supported() reports that, and a caller asking for
 * JENT_FORCE_SECURE_MEM is refused rather than quietly given ordinary memory.
 */
void *malloc(UINTN size);
void free(void *ptr);

void *malloc(UINTN size)
{
	return AllocatePool(size);
}

void free(void *ptr)
{
	if (ptr)
		FreePool(ptr);
}

UINTN strlen(const char *s);

UINTN strlen(const char *s)
{
	const char *p = s;

	while (*p)
		p++;

	return (UINTN)(p - s);
}

/*
 * snprintf(), for jent_status() and nothing else. Only the conversions that
 * source uses are implemented - %s, %d, %u, %ld and %llu - and an unknown one
 * is emitted verbatim rather than skipped, so a conversion added there shows
 * up in the output as itself instead of disappearing.
 *
 * The C99 return value is honoured: the length the whole document would have
 * had, whether or not it fit. jent_status() does not read it today, but a
 * shim that reports a truncated write as a complete one is the kind of thing
 * that is only ever discovered much later.
 */
struct je_out {
	char *buf;
	UINTN len;	/* what the caller gave, including the terminator */
	UINTN pos;	/* what would have been written */
};

static void je_putc(struct je_out *o, char c)
{
	/* One byte is always kept back for the terminator. */
	if (o->len && o->pos + 1 < o->len)
		o->buf[o->pos] = c;

	o->pos++;
}

static void je_puts(struct je_out *o, const char *s)
{
	while (*s)
		je_putc(o, *s++);
}

static void je_putu(struct je_out *o, UINT64 v)
{
	char tmp[24];
	int i = 0;

	if (!v) {
		je_putc(o, '0');
		return;
	}

	while (v && i < (int)sizeof(tmp)) {
		tmp[i++] = (char)('0' + (v % 10));
		v /= 10;
	}

	while (i--)
		je_putc(o, tmp[i]);
}

static void je_putd(struct je_out *o, INT64 v)
{
	if (v < 0) {
		je_putc(o, '-');
		/* Negated as unsigned, so the most negative value survives. */
		je_putu(o, (UINT64)(-(v + 1)) + 1);
		return;
	}

	je_putu(o, (UINT64)v);
}

int snprintf(char *buf, UINTN len, const char *fmt, ...);

int snprintf(char *buf, UINTN len, const char *fmt, ...)
{
	struct je_out o;
	va_list ap;

	o.buf = buf;
	o.len = len;
	o.pos = 0;

	va_start(ap, fmt);

	while (*fmt) {
		const char *spec = fmt;
		int longs = 0;

		if (*fmt != '%') {
			je_putc(&o, *fmt++);
			continue;
		}

		fmt++;
		while (*fmt == 'l') {
			longs++;
			fmt++;
		}

		switch (*fmt) {
		case '%':
			je_putc(&o, '%');
			break;
		case 's':
			je_puts(&o, va_arg(ap, const char *));
			break;
		case 'u':
			if (longs >= 2)
				je_putu(&o, va_arg(ap, UINT64));
			else if (longs == 1)
				je_putu(&o, (UINT64)va_arg(ap, unsigned long));
			else
				je_putu(&o, (UINT64)va_arg(ap, unsigned int));
			break;
		case 'd':
		case 'i':
			if (longs >= 2)
				je_putd(&o, va_arg(ap, INT64));
			else if (longs == 1)
				je_putd(&o, (INT64)va_arg(ap, long));
			else
				je_putd(&o, (INT64)va_arg(ap, int));
			break;
		default:
			/* Not understood: say so by copying it through. */
			while (spec <= fmt)
				je_putc(&o, *spec++);
			break;
		}

		if (*fmt)
			fmt++;
	}

	va_end(ap);

	if (len)
		o.buf[o.pos + 1 < len ? o.pos : len - 1] = '\0';

	return (int)o.pos;
}

/***************************************************************************
 * Output. The console takes UTF-16 and wants a carriage return of its own.
 ***************************************************************************/

static void je_print_ascii(const char *s)
{
	CHAR16 line[2];

	line[1] = L'\0';

	for (; *s; s++) {
		if (*s == '\n') {
			Print(L"\r\n");
			continue;
		}
		line[0] = (CHAR16)*s;
		Print(line);
	}
}

static void je_print_hex(const unsigned char *buf, UINTN len)
{
	static const char hex[] = "0123456789abcdef";
	UINTN i;

	for (i = 0; i < len; i++) {
		CHAR16 pair[3];

		pair[0] = (CHAR16)hex[(buf[i] >> 4) & 0xf];
		pair[1] = (CHAR16)hex[buf[i] & 0xf];
		pair[2] = L'\0';
		Print(pair);
	}
}

/*
 * Every line the harness matches on carries this, so that the assertions are
 * made against this program's output and not against something the firmware
 * happened to print on the same console.
 */
#define JE_TAG	L"jitterentropy-efi: "

/***************************************************************************
 * The application. je_run() does the work and efi_main() reports what came of
 * it and stops the machine, so that every outcome ends the run the same way.
 ***************************************************************************/

/*
 * One collector, from its allocation to its release: build it, generate from
 * it, print what came out and print what jent_status() says it settled on.
 * Every configuration goes through this same sequence, so the three documents
 * in the transcript differ only in what the library made of the flags.
 *
 * @required says whether a refusal ends the run. It is set for the default
 * configuration, where nothing may go wrong, and clear for the compliance
 * modes: both carry tighter health test cutoffs than the common one - NTG.1
 * markedly so - and a startup firing on what a particular firmware and
 * processor produce is a legitimate outcome rather than a defect in the
 * library. So those two are attempted, and whichever answer comes back is
 * reported.
 *
 * Both compliance modes also imply JENT_FORCE_SECURE_MEM, and here that is
 * satisfied rather than waived: there is no swap device, no second process and
 * no core dump, which is the same ground the Linux kernel backend claims
 * secure memory on. jent_status() says "secureMemory": true accordingly.
 */
static EFI_STATUS je_collector(const CHAR16 *name, unsigned int flags,
			       int required)
{
	unsigned char buf[JE_BYTES];
	struct rand_data *ec;
	char *status;
	ssize_t rc;

	ec = jent_entropy_collector_alloc(0, flags);
	if (!ec) {
		if (!required) {
			Print(JE_TAG L"%s refused, which its health test "
			      L"cutoffs allow\r\n", name);
			return EFI_SUCCESS;
		}

		Print(JE_TAG L"FAIL: no %s collector could be allocated\r\n",
		      name);
		return EFI_OUT_OF_RESOURCES;
	}
	Print(JE_TAG L"%s collector allocated\r\n", name);

	rc = jent_read_entropy(ec, (char *)buf, sizeof(buf));
	if (rc != (ssize_t)sizeof(buf)) {
		Print(JE_TAG L"%s%s generation returns %d\r\n",
		      required ? L"FAIL: the " : L"", name, (int)rc);
		jent_entropy_collector_free(ec);
		return required ? EFI_DEVICE_ERROR : EFI_SUCCESS;
	}

	Print(JE_TAG L"%s entropy ", name);
	je_print_hex(buf, sizeof(buf));
	Print(L"\r\n");

	/*
	 * The status document, which says what this collector settled on: the
	 * memory size, the oversampling rate, the flags, whether the platform
	 * granted secure memory, and how many blocks it has produced. It is
	 * also what exercises the snprintf() above, this being the only caller
	 * of it in the library.
	 */
	status = malloc(JE_STATUS_LEN);
	if (!status) {
		Print(JE_TAG L"FAIL: no room for the %s status document\r\n",
		      name);
		jent_entropy_collector_free(ec);
		return EFI_OUT_OF_RESOURCES;
	}

	if (jent_status(ec, status, JE_STATUS_LEN)) {
		Print(JE_TAG L"FAIL: the %s status call was refused\r\n",
		      name);
		free(status);
		jent_entropy_collector_free(ec);
		return EFI_DEVICE_ERROR;
	}

	Print(JE_TAG L"%s status\r\n", name);
	je_print_ascii(status);
	Print(L"\r\n");

	free(status);
	jent_entropy_collector_free(ec);

	return EFI_SUCCESS;
}

/*
 * The internal timer, which this build does not have and must therefore
 * refuse. It is a counting thread, and there is no thread here to run it on -
 * JENT_CONF_ENABLE_INTERNAL_TIMER is left undefined, and the collector that
 * would drive it must not come into existence.
 *
 * A refusal is the whole of what is checked, on both entry points that can be
 * asked for it. The failure this guards against is the one that matters on a
 * target with no scheduler: an allocation that accepts the flag and hands back
 * a collector whose clock is a counter nothing increments, which would then
 * spin forever on the first measurement rather than return an error.
 *
 * Run last, and that is not arbitrary. A startup that fails clears the
 * process-wide latch recording that the self tests have run, so the next
 * allocation repeats them; harmless, but it would happen underneath the three
 * configurations above and they are what this program is for.
 */
static EFI_STATUS je_no_internal_timer(void)
{
	struct rand_data *ec;
	int ret;

	ret = jent_entropy_init_ex(0, JENT_FORCE_INTERNAL_TIMER);
	if (!ret) {
		Print(JE_TAG L"FAIL: the startup accepted the internal timer "
		      L"in a build without one\r\n");
		return EFI_DEVICE_ERROR;
	}

	ec = jent_entropy_collector_alloc(0, JENT_FORCE_INTERNAL_TIMER);
	if (ec) {
		Print(JE_TAG L"FAIL: a collector was built on an internal "
		      L"timer that cannot run\r\n");
		jent_entropy_collector_free(ec);
		return EFI_DEVICE_ERROR;
	}

	Print(JE_TAG L"internal timer refused, the startup reporting %d\r\n",
	      ret);

	return EFI_SUCCESS;
}

static EFI_STATUS je_run(void)
{
	EFI_STATUS status;
	int ret;

	Print(JE_TAG L"start, library version %u\r\n", jent_version());

	/*
	 * The startup, which is where the claim is actually tested: it
	 * measures the platform clock, establishes the common divisor of the
	 * deltas and runs the conditioning known answer tests. On a machine
	 * whose counter does not move it fails here, and that is the answer
	 * this program exists to obtain.
	 *
	 * No flags: the internal timer is compiled out of this build - there
	 * is no thread to run a counter on - so nothing has to disable it.
	 */
	ret = jent_entropy_init_ex(0, 0);
	if (ret) {
		Print(JE_TAG L"FAIL: the startup reports %d\r\n", ret);
		return EFI_DEVICE_ERROR;
	}
	Print(JE_TAG L"startup passed\r\n");

	/* The default configuration, where nothing is allowed to go wrong. */
	status = je_collector(L"default", 0, 1);
	if (status != EFI_SUCCESS)
		return status;

	/* The two compliance modes, which are allowed to be refused. */
	status = je_collector(L"FIPS", JENT_FORCE_FIPS, 0);
	if (status != EFI_SUCCESS)
		return status;

	status = je_collector(L"NTG.1", JENT_NTG1, 0);
	if (status != EFI_SUCCESS)
		return status;

	return je_no_internal_timer();
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
	EFI_STATUS status;

	InitializeLib(image, systab);

	status = je_run();

	Print(status == EFI_SUCCESS ? JE_TAG L"done\r\n"
				    : JE_TAG L"failed\r\n");

	/*
	 * Power the machine off rather than returning. Returning hands control
	 * back to whatever loaded the application, which under the harness is
	 * the firmware's boot manager with nothing else to boot: it would draw
	 * a menu and wait, and the run would end on the harness's timeout with
	 * every outcome looking alike. Shutting down ends it on this program's
	 * verdict instead, and a run that does not reach here is a hang, which
	 * is a result too.
	 *
	 * ResetSystem() does not return. The line after it is for a firmware
	 * that has no power management to do it with, which is not QEMU but
	 * may be the machine somebody tries this on.
	 */
	RT->ResetSystem(EfiResetShutdown, status, 0, NULL);

	return status;
}
