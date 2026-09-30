/*
 * libjitterentropy-record in one translation unit
 *
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
 * The recording reaches internals a shared libjitterentropy does not export,
 * so the library carries a copy of libjitterentropy of its own. It is compiled
 * here, with the recording, as a private copy (JENT_PRIVATE_COMPILE in
 * jitterentropy.h): every function of the copy is static, and the object
 * defines the functions of jitterentropy-record.h and nothing else. That is
 * what lets it link beside libjitterentropy, shared or static, and it is a
 * property of the compilation - no linker has to hide anything afterwards.
 */
#define JENT_PRIVATE_COMPILE

/*
 * The copy calls only part of the library. JENT_PRIVATE_STATIC marks it unused
 * for GCC and Clang; MSVC reports each unreferenced static function at /W4.
 */
#ifdef _MSC_VER
# pragma warning(disable: 4505)
#endif

/*
 * jent_entropy_collector_alloc_raw() of jitterentropy-base.c below, for the
 * recording of jitterentropy-record.c - which defines it as well, but only
 * once the library sources are behind it.
 */
#define JENT_RAW_COLLECTOR

/*
 * The feature-test macros, ahead of the first system header of the
 * translation unit, which the first source below includes: the ones each
 * arch/ backend states would come too late for all but the first. _GNU_SOURCE
 * is what the two affinity backends and the recording's CPU pinning set.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE
#endif
#include "../../../arch/jitterentropy-arch-compat.h"

/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "../../../arch/jitterentropy-arch-atomic.c"

#include "../../../src/jitterentropy-sha3.c"
#include "../../../src/jitterentropy-gcd.c"
#include "../../../src/jitterentropy-health.c"
#include "../../../src/jitterentropy-noise.c"
#include "../../../src/jitterentropy-timer.c"
#include "../../../src/jitterentropy-base.c"
#include "../../../src/jitterentropy-uuid.c"
#include "../../../src/jitterentropy-status.c"

#include "../../../arch/jitterentropy-arch-cache.c"
#include "../../../arch/jitterentropy-arch-fips.c"
#include "../../../arch/jitterentropy-arch-memory.c"
#include "../../../arch/jitterentropy-arch-ncpu.c"
#include "../../../arch/jitterentropy-arch-sched.c"
#include "../../../arch/jitterentropy-arch-thread.c"
#include "../../../arch/jitterentropy-arch-timer.c"
#include "../../../arch/jitterentropy-arch-random.c"

#include "jitterentropy-record.c"
