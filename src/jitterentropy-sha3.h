/*
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

#ifndef JITTERENTROPY_SHA3_H
#define JITTERENTROPY_SHA3_H

#include "jitterentropy-internal.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* struct jent_sha_ctx is defined in jitterentropy-internal.h. */
#define JENT_SHA_MAX_CTX_SIZE	(sizeof(struct jent_sha_ctx))
#define HASH_CTX_ON_STACK(name)						       \
	struct jent_sha_ctx name

static inline unsigned int jent_sha3_rate(void *hash_state)
{
	struct jent_sha_ctx *ctx = hash_state;

	return ctx->r;
}

JENT_INTERNAL
void jent_sha3_256_init(struct jent_sha_ctx *ctx);
JENT_INTERNAL
void jent_sha3_update(struct jent_sha_ctx *ctx, const uint8_t *in,
		      size_t inlen);
JENT_INTERNAL
void jent_sha3_final(struct jent_sha_ctx *ctx, uint8_t *digest);
JENT_INTERNAL
int jent_sha3_tester(void);

JENT_INTERNAL
void jent_shake256_init(struct jent_sha_ctx *ctx);
static inline void jent_shake256_set_digestsize(struct jent_sha_ctx *ctx,
						unsigned int digestsize)
{
	ctx->digestsize = (uint8_t)digestsize;
}
JENT_INTERNAL
void jent_drbg_generate_block(struct jent_sha_ctx *ctx, uint8_t *dst,
			      size_t dst_len);

#ifdef __cplusplus
}
#endif

#endif /* JITTERENTROPY_SHA3_H */
