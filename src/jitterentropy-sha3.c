/* Jitter RNG: SHA-3 Implementation
 *
 * Copyright (C) 2021 - 2025, Stephan Mueller <smueller@chronox.de>
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

#include "jitterentropy-sha3.h"
#include "jitterentropy-internal.h"

/***************************************************************************
 * Message Digest Implementation
 ***************************************************************************/

/*
 * Conversion of Little-Endian representations in byte streams - the data
 * representation in the integer values is the host representation.
 */
static inline uint32_t ptr_to_le32(const uint8_t *p)
{
	return (uint32_t)p[0]       | (uint32_t)p[1] << 8 |
	       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint64_t ptr_to_le64(const uint8_t *p)
{
	return (uint64_t)ptr_to_le32(p) | (uint64_t)ptr_to_le32(p + 4) << 32;
}

static inline void le32_to_ptr(uint8_t *p, const uint32_t value)
{
	p[0] = (uint8_t)(value);
	p[1] = (uint8_t)(value >> 8);
	p[2] = (uint8_t)(value >> 16);
	p[3] = (uint8_t)(value >> 24);
}

static inline void le64_to_ptr(uint8_t *p, const uint64_t value)
{
	le32_to_ptr(p + 4, (uint32_t)(value >> 32));
	le32_to_ptr(p,     (uint32_t)(value));
}

/*********************************** Keccak ***********************************/
/* state[x + y*5] */
#define A(x, y) (x + 5 * y)

static inline void jent_keccakp_theta(uint64_t s[25])
{
	uint64_t C[5], D[5];

	/* Step 1 */
	C[0] = s[A(0, 0)] ^ s[A(0, 1)] ^ s[A(0, 2)] ^ s[A(0, 3)] ^ s[A(0, 4)];
	C[1] = s[A(1, 0)] ^ s[A(1, 1)] ^ s[A(1, 2)] ^ s[A(1, 3)] ^ s[A(1, 4)];
	C[2] = s[A(2, 0)] ^ s[A(2, 1)] ^ s[A(2, 2)] ^ s[A(2, 3)] ^ s[A(2, 4)];
	C[3] = s[A(3, 0)] ^ s[A(3, 1)] ^ s[A(3, 2)] ^ s[A(3, 3)] ^ s[A(3, 4)];
	C[4] = s[A(4, 0)] ^ s[A(4, 1)] ^ s[A(4, 2)] ^ s[A(4, 3)] ^ s[A(4, 4)];

	/* Step 2 */
	D[0] = C[4] ^ rol64(C[1], 1);
	D[1] = C[0] ^ rol64(C[2], 1);
	D[2] = C[1] ^ rol64(C[3], 1);
	D[3] = C[2] ^ rol64(C[4], 1);
	D[4] = C[3] ^ rol64(C[0], 1);

	/* Step 3 */
	s[A(0, 0)] ^= D[0];
	s[A(1, 0)] ^= D[1];
	s[A(2, 0)] ^= D[2];
	s[A(3, 0)] ^= D[3];
	s[A(4, 0)] ^= D[4];

	s[A(0, 1)] ^= D[0];
	s[A(1, 1)] ^= D[1];
	s[A(2, 1)] ^= D[2];
	s[A(3, 1)] ^= D[3];
	s[A(4, 1)] ^= D[4];

	s[A(0, 2)] ^= D[0];
	s[A(1, 2)] ^= D[1];
	s[A(2, 2)] ^= D[2];
	s[A(3, 2)] ^= D[3];
	s[A(4, 2)] ^= D[4];

	s[A(0, 3)] ^= D[0];
	s[A(1, 3)] ^= D[1];
	s[A(2, 3)] ^= D[2];
	s[A(3, 3)] ^= D[3];
	s[A(4, 3)] ^= D[4];

	s[A(0, 4)] ^= D[0];
	s[A(1, 4)] ^= D[1];
	s[A(2, 4)] ^= D[2];
	s[A(3, 4)] ^= D[3];
	s[A(4, 4)] ^= D[4];
}

static inline void jent_keccakp_rho(uint64_t s[25])
{
	/* Step 1 */
	/* s[A(0, 0)] = s[A(0, 0)]; */

#define RHO_ROL(t)	(((t + 1) * (t + 2) / 2) % 64)
	/* Step 3 */
	s[A(1, 0)] = rol64(s[A(1, 0)], RHO_ROL(0));
	s[A(0, 2)] = rol64(s[A(0, 2)], RHO_ROL(1));
	s[A(2, 1)] = rol64(s[A(2, 1)], RHO_ROL(2));
	s[A(1, 2)] = rol64(s[A(1, 2)], RHO_ROL(3));
	s[A(2, 3)] = rol64(s[A(2, 3)], RHO_ROL(4));
	s[A(3, 3)] = rol64(s[A(3, 3)], RHO_ROL(5));
	s[A(3, 0)] = rol64(s[A(3, 0)], RHO_ROL(6));
	s[A(0, 1)] = rol64(s[A(0, 1)], RHO_ROL(7));
	s[A(1, 3)] = rol64(s[A(1, 3)], RHO_ROL(8));
	s[A(3, 1)] = rol64(s[A(3, 1)], RHO_ROL(9));
	s[A(1, 4)] = rol64(s[A(1, 4)], RHO_ROL(10));
	s[A(4, 4)] = rol64(s[A(4, 4)], RHO_ROL(11));
	s[A(4, 0)] = rol64(s[A(4, 0)], RHO_ROL(12));
	s[A(0, 3)] = rol64(s[A(0, 3)], RHO_ROL(13));
	s[A(3, 4)] = rol64(s[A(3, 4)], RHO_ROL(14));
	s[A(4, 3)] = rol64(s[A(4, 3)], RHO_ROL(15));
	s[A(3, 2)] = rol64(s[A(3, 2)], RHO_ROL(16));
	s[A(2, 2)] = rol64(s[A(2, 2)], RHO_ROL(17));
	s[A(2, 0)] = rol64(s[A(2, 0)], RHO_ROL(18));
	s[A(0, 4)] = rol64(s[A(0, 4)], RHO_ROL(19));
	s[A(4, 2)] = rol64(s[A(4, 2)], RHO_ROL(20));
	s[A(2, 4)] = rol64(s[A(2, 4)], RHO_ROL(21));
	s[A(4, 1)] = rol64(s[A(4, 1)], RHO_ROL(22));
	s[A(1, 1)] = rol64(s[A(1, 1)], RHO_ROL(23));
}

static inline void jent_keccakp_pi(uint64_t s[25])
{
	uint64_t t = s[A(4, 4)];

	/* Step 1 */
	/* s[A(0, 0)] = s[A(0, 0)]; */
	s[A(4, 4)] = s[A(1, 4)];
	s[A(1, 4)] = s[A(3, 1)];
	s[A(3, 1)] = s[A(1, 3)];
	s[A(1, 3)] = s[A(0, 1)];
	s[A(0, 1)] = s[A(3, 0)];
	s[A(3, 0)] = s[A(3, 3)];
	s[A(3, 3)] = s[A(2, 3)];
	s[A(2, 3)] = s[A(1, 2)];
	s[A(1, 2)] = s[A(2, 1)];
	s[A(2, 1)] = s[A(0, 2)];
	s[A(0, 2)] = s[A(1, 0)];
	s[A(1, 0)] = s[A(1, 1)];
	s[A(1, 1)] = s[A(4, 1)];
	s[A(4, 1)] = s[A(2, 4)];
	s[A(2, 4)] = s[A(4, 2)];
	s[A(4, 2)] = s[A(0, 4)];
	s[A(0, 4)] = s[A(2, 0)];
	s[A(2, 0)] = s[A(2, 2)];
	s[A(2, 2)] = s[A(3, 2)];
	s[A(3, 2)] = s[A(4, 3)];
	s[A(4, 3)] = s[A(3, 4)];
	s[A(3, 4)] = s[A(0, 3)];
	s[A(0, 3)] = s[A(4, 0)];
	s[A(4, 0)] = t;
}

static inline void jent_keccakp_chi(uint64_t s[25])
{
	uint64_t t0[5], t1[5];

	t0[0] = s[A(0, 0)];
	t0[1] = s[A(0, 1)];
	t0[2] = s[A(0, 2)];
	t0[3] = s[A(0, 3)];
	t0[4] = s[A(0, 4)];

	t1[0] = s[A(1, 0)];
	t1[1] = s[A(1, 1)];
	t1[2] = s[A(1, 2)];
	t1[3] = s[A(1, 3)];
	t1[4] = s[A(1, 4)];

	s[A(0, 0)] ^= ~s[A(1, 0)] & s[A(2, 0)];
	s[A(0, 1)] ^= ~s[A(1, 1)] & s[A(2, 1)];
	s[A(0, 2)] ^= ~s[A(1, 2)] & s[A(2, 2)];
	s[A(0, 3)] ^= ~s[A(1, 3)] & s[A(2, 3)];
	s[A(0, 4)] ^= ~s[A(1, 4)] & s[A(2, 4)];

	s[A(1, 0)] ^= ~s[A(2, 0)] & s[A(3, 0)];
	s[A(1, 1)] ^= ~s[A(2, 1)] & s[A(3, 1)];
	s[A(1, 2)] ^= ~s[A(2, 2)] & s[A(3, 2)];
	s[A(1, 3)] ^= ~s[A(2, 3)] & s[A(3, 3)];
	s[A(1, 4)] ^= ~s[A(2, 4)] & s[A(3, 4)];

	s[A(2, 0)] ^= ~s[A(3, 0)] & s[A(4, 0)];
	s[A(2, 1)] ^= ~s[A(3, 1)] & s[A(4, 1)];
	s[A(2, 2)] ^= ~s[A(3, 2)] & s[A(4, 2)];
	s[A(2, 3)] ^= ~s[A(3, 3)] & s[A(4, 3)];
	s[A(2, 4)] ^= ~s[A(3, 4)] & s[A(4, 4)];

	s[A(3, 0)] ^= ~s[A(4, 0)] & t0[0];
	s[A(3, 1)] ^= ~s[A(4, 1)] & t0[1];
	s[A(3, 2)] ^= ~s[A(4, 2)] & t0[2];
	s[A(3, 3)] ^= ~s[A(4, 3)] & t0[3];
	s[A(3, 4)] ^= ~s[A(4, 4)] & t0[4];

	s[A(4, 0)] ^= ~t0[0] & t1[0];
	s[A(4, 1)] ^= ~t0[1] & t1[1];
	s[A(4, 2)] ^= ~t0[2] & t1[2];
	s[A(4, 3)] ^= ~t0[3] & t1[3];
	s[A(4, 4)] ^= ~t0[4] & t1[4];
}

static const uint64_t jent_keccakp_iota_vals[] = {
	0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
	0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
	0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
	0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
	0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
	0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
	0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
	0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};

static inline void jent_keccakp_iota(uint64_t s[25], unsigned int round)
{
	s[0] ^= jent_keccakp_iota_vals[round];
}

static inline void jent_keccakp_1600(uint64_t s[25])
{
	unsigned int round;

	for (round = 0; round < 24; round++) {
		jent_keccakp_theta(s);
		jent_keccakp_rho(s);
		jent_keccakp_pi(s);
		jent_keccakp_chi(s);
		jent_keccakp_iota(s, round);
	}
}

/*********************************** SHA-3 ************************************/

static inline void jent_sha3_init(struct jent_sha_ctx *ctx)
{
	unsigned int i;

	for (i = 0; i < 25; i++)
		ctx->state[i] = 0;
	ctx->msg_len = 0;
}

void jent_sha3_512_init(struct jent_sha_ctx *ctx)
{
	jent_sha3_init(ctx);
	ctx->r = JENT_SHA3_512_SIZE_BLOCK;
	ctx->rword = JENT_SHA3_512_SIZE_BLOCK / sizeof(uint64_t);
	ctx->digestsize = JENT_SHA3_512_SIZE_DIGEST;
	ctx->padding = 0x06;
}

void jent_shake256_init(struct jent_sha_ctx *ctx)
{
	jent_sha3_init(ctx);

	ctx->r = JENT_SHA3_256_SIZE_BLOCK;
	ctx->rword = JENT_SHA3_256_SIZE_BLOCK / sizeof(uint64_t);
	ctx->digestsize = 0;
	ctx->padding = 0x1f;
}

static inline void jent_sha3_fill_state(struct jent_sha_ctx *ctx,
					const uint8_t *in)
{
	unsigned int i;

	for (i = 0; i < ctx->rword; i++) {
		ctx->state[i]  ^= ptr_to_le64(in);
		in += 8;
	}
}

void jent_sha3_update(struct jent_sha_ctx *ctx, const uint8_t *in, size_t inlen)
{
	size_t partial = ctx->msg_len % ctx->r;

	ctx->msg_len += inlen;

	/* Sponge absorbing phase */

	/* Check if we have a partial block stored */
	if (partial) {
		size_t todo = ctx->r - partial;

		/*
		 * If the provided data is small enough to fit in the partial
		 * buffer, copy it and leave it unprocessed.
		 */
		if (inlen < todo) {
			memcpy(ctx->partial + partial, in, inlen);
			return;
		}

		/*
		 * The input data is large enough to fill the entire partial
		 * block buffer. Thus, we fill it and transform it.
		 */
		memcpy(ctx->partial + partial, in, todo);
		inlen -= todo;
		in += todo;

		jent_sha3_fill_state(ctx, ctx->partial);
		jent_keccakp_1600(ctx->state);
	}

	/* Perform a transformation of full block-size messages */
	for (; inlen >= ctx->r; inlen -= ctx->r, in += ctx->r) {
		jent_sha3_fill_state(ctx, in);
		jent_keccakp_1600(ctx->state);
	}

	/* If we have data left, copy it into the partial block buffer */
	memcpy(ctx->partial, in, inlen);
}

void jent_sha3_final(struct jent_sha_ctx *ctx, uint8_t *digest)
{
	size_t partial = ctx->msg_len % ctx->r;
	unsigned int i;

	/* Final round in sponge absorbing phase */

	/* Fill the unused part of the partial buffer with zeros */
	memset(ctx->partial + partial, 0, ctx->r - partial);

	/*
	 * Add the leading and trailing bit as well as the 01 bits for the
	 * SHA-3 suffix.
	 */
	ctx->partial[partial] = ctx->padding;
	ctx->partial[ctx->r - 1] |= 0x80;

	/* Final transformation */
	jent_sha3_fill_state(ctx, ctx->partial);
	jent_keccakp_1600(ctx->state);

	/*
	 * Sponge squeeze phase - This squeeze implementation is deliberately
	 * short for the use in XDRBG-like constructions as it has the following
	 * caveats compared to a general-purpose squeeze:
	 *
	 * * the digest size is always smaller than the rate size r as we do not
	 *   have a loop around the jent_keccakp_1600 / copy out loop below
	 *
	 * * the requested digest size must be multiples of uint64_t
	 */
	for (i = 0; i < ctx->digestsize / 8; i++, digest += 8)
		le64_to_ptr(digest, ctx->state[i]);

	memset(ctx->partial, 0, ctx->r);
	jent_sha3_init(ctx);
}

int jent_sha3_alloc(void **hash_state)
{
	struct sha_ctx *tmp;

	tmp = jent_zalloc(JENT_SHA_MAX_CTX_SIZE);
	if (!tmp)
		return 1;

	*hash_state = tmp;

	return 0;
}

void jent_sha3_dealloc(void *hash_state)
{
	struct sha_ctx *ctx = (struct sha_ctx *)hash_state;

	jent_zfree(ctx, JENT_SHA_MAX_CTX_SIZE);
}

/*********************************** XDRBG ************************************/

#define JENT_XDRBG_DRNG_ENCODE_N(x) (x * 85)

/*
 * This operation implements XDRBG-256 as defined in [1].
 *
 * The output size is [0:256] bits.
 *
 * [1] https://leancrypto.org/papers/xdrbg.pdf
 */
static void jent_xdrbg256_generate_block(struct jent_sha_ctx *ctx, uint8_t *dst,
					 size_t dst_len)
{
	/*
	 * XDRBG:
	 * 512 Bit for next state (internal memory) || 256 Bit output for user
	 */
	uint8_t jent_block_next_state[JENT_XDRBG_SIZE_STATE +
				      JENT_SHA3_256_SIZE_DIGEST];
	uint8_t encode;

	/* Checking the output size */
	BUILD_BUG_ON(JENT_SHA3_256_SIZE_DIGEST != ((DATA_SIZE_BITS / 8)));
	/*
	 * The XOF implementation only allows the generation of up to one
	 * rate-size block. See the comments in the squeeze operation for
	 * details
	 */
	BUILD_BUG_ON(JENT_SHA3_256_SIZE_BLOCK < sizeof(jent_block_next_state));
	/*
	 * The squeeze operation is limited to return multiples of uint64_t -
	 * verify all set_digestsize values.
	 */
	BUILD_BUG_ON(JENT_XDRBG_SIZE_STATE % sizeof(uint64_t));
	BUILD_BUG_ON(sizeof(jent_block_next_state) % sizeof(uint64_t));

	/* The final operation automatically re-initializes the ->hash_state */


	/*
	 * SHA3-512 XDRBG-like: finalize seeding
	 *
	 * seed is inserted with SHA3-512 update
	 *
	 * initial seeding:
	 * V ← XOF( encode(( seed ), α, 0), |V| )
	 *
	 * reseeding:
	 * V ← XOF( encode(( V' || seed ), α, 1), |V| )
	 *
	 * The insertion of the V' is done at the end of this function for the
	 * next finalization of the reseeding. α is defined to be empty.
	 */
	encode = JENT_XDRBG_DRNG_ENCODE_N(ctx->initially_seeded);
	ctx->initially_seeded = 1;
	jent_sha3_update(ctx, &encode, 1);
	jent_shake256_set_digestsize(ctx, JENT_XDRBG_SIZE_STATE);
	jent_sha3_final(ctx, jent_block_next_state);

	/*
	 * XDRBG: generate
	 *
	 * ℓ = dst_len which is at maximum 256 bits
	 * T ← XOF( encode(V', α, 2), ℓ + |V| )
	 * V ← first |V| bits of T
	 * Σ ← last ℓ bits of T
	 */
	jent_sha3_update(ctx, jent_block_next_state, JENT_XDRBG_SIZE_STATE);
	encode = JENT_XDRBG_DRNG_ENCODE_N(2);
	jent_sha3_update(ctx, &encode, 1);
	/*
	 * Request a full block irrespective of the output size due to
	 * Keccak squeeze implementation limitation.
	 */
	jent_shake256_set_digestsize(ctx, sizeof(jent_block_next_state));
	jent_sha3_final(ctx, jent_block_next_state);

	/* Return Σ to the caller truncated to the requested size */
	if (dst_len)  {
		/* Safety measure to not overflow the generated buffer */
		if (dst_len > JENT_SHA3_256_SIZE_DIGEST)
			dst_len = JENT_SHA3_256_SIZE_DIGEST;

		memcpy(dst, jent_block_next_state + JENT_XDRBG_SIZE_STATE,
		       dst_len);
	}

	/*
	 * XDRBG: reseed
	 * Set the V into the state.
	 */
	jent_sha3_update(ctx, jent_block_next_state, JENT_XDRBG_SIZE_STATE);
	jent_memset_secure(jent_block_next_state,
			   sizeof(jent_block_next_state));
}

/*
 * This operation follows the guidelines of XDRBG as defined in [1] using
 * SHA3-512 instead of SHAKE-256.
 *
 * In addition to the use of a different hash, the XDRBG state V is defined to
 * be of size 512 bits during the seeding operation and 256 bits during the
 * generate operation. See the code below marking the place where the V
 * value is handled differently than specified in XDRBG.
 *
 * The output size is [0:256] bits.
 *
 * [1] https://leancrypto.org/papers/xdrbg.pdf
 */
static void jent_xdrbg_sha3_512_generate_block(struct jent_sha_ctx *ctx,
					       uint8_t *dst, size_t dst_len)
{
	/* 256 Bit for next state (internal memory) || 256 Bit output for user */
	uint8_t jent_block_next_state[JENT_SHA3_512_SIZE_DIGEST];
	uint8_t encode;

	BUILD_BUG_ON(JENT_SHA3_512_SIZE_DIGEST != ((DATA_SIZE_BITS / 8) * 2));

	/* The final operation automatically re-initializes the ->hash_state */

	/*
	 * XDRBG: finalize seeding operation
	 *
	 * seed is inserted with SHAKE update
	 *
	 * initial seeding:
	 * V ← SHA3-512( encode(( seed ), α, 0), |V| )
	 *
	 * reseeding:
	 * V ← SHA3-512( encode(( V' || seed ), α, 1), |V| )
	 *
	 * The insertion of the V' is done at the end of this function for the
	 * next finalization of the reseeding. α is defined to be empty.
	 */
	encode = JENT_XDRBG_DRNG_ENCODE_N(ctx->initially_seeded);
	ctx->initially_seeded = 1;
	jent_sha3_update(ctx, &encode, 1);
	jent_sha3_final(ctx, jent_block_next_state);

	/*
	 * XDRBG: generate
	 *
	 * ℓ = dst_len which is at maximum 256 bits
	 * T ← SHA3-512( encode(V', α, 2), ℓ + |V| )
	 * V ← first 256 bits of T
	 * Σ ← last 256 bits of T truncated to ℓ
	 */
	jent_sha3_update(ctx, jent_block_next_state,
			 sizeof(jent_block_next_state));
	encode = JENT_XDRBG_DRNG_ENCODE_N(2);
	jent_sha3_update(ctx, &encode, 1);
	jent_sha3_final(ctx, jent_block_next_state);

	/* Return Σ to the caller  truncated to the requested size */
	if (dst_len) {
		/* Safety measure to not overflow the generated buffer */
		if (dst_len > DATA_SIZE_BITS / 8)
			dst_len = DATA_SIZE_BITS / 8;

		memcpy(dst, jent_block_next_state + (DATA_SIZE_BITS / 8),
		       dst_len);
	}

	/*
	 * XDRBG: reseed
	 * Set the V into the state.
	 *
	 * This invocation is the difference to XDRBG: the V state is 256 bits
	 * instead of 512 bits as defined in XDRBG.
	 */
	jent_sha3_update(ctx, jent_block_next_state, DATA_SIZE_BITS / 8);
	jent_memset_secure(jent_block_next_state, sizeof(jent_block_next_state));
}

void jent_drbg_generate_block(struct jent_sha_ctx *ctx, uint8_t *dst,
			      size_t dst_len)
{
	if (ctx->r == JENT_SHA3_512_SIZE_BLOCK)
		jent_xdrbg_sha3_512_generate_block(ctx, dst, dst_len);
	else
		jent_xdrbg256_generate_block(ctx, dst, dst_len);
}

/********************************** Selftest **********************************/

/*
 * The SHAKE-256 support is only needed to support the XDRBG. Therefore, it is
 * implicitly self-tested with the XDRBG-256 self test. Yet, this self-test
 * code for SHAKE-256 is left in here to allow implementors to activate it at
 * their discretion. Furthermore it provides an example how to invoke the
 * Keccak operation as a SHAKE-256 for testing and analysis.
 */
#if 0
static int jent_shake256_tester(void)
{
	HASH_CTX_ON_STACK(ctx);
	static const uint8_t msg[] = { 0x6C, 0x9E, 0xC8, 0x5C, 0xBA, 0xBA, 0x62,
				       0xF5, 0xBC, 0xFE, 0xA1, 0x9E, 0xB9, 0xC9,
				       0x20, 0x52, 0xD8, 0xFF, 0x18, 0x81, 0x52,
				       0xE9, 0x61, 0xC1, 0xEC, 0x5C, 0x75, 0xBF,
				       0xC3, 0xC9, 0x1C, 0x8D };
	static const uint8_t exp[] = { 0x7d, 0x6a, 0x09, 0x6e, 0x13, 0x66, 0x1d,
				       0x9d, 0x0e, 0xca, 0xf5, 0x38, 0x30, 0xa1,
				       0x92, 0x87, 0xe0, 0xb3, 0x6e, 0xce, 0x48,
				       0x82, 0xeb, 0x58, 0x0b, 0x78, 0x5c, 0x1d,
				       0xef, 0x2d, 0xe5, 0xaa, 0x6c };
	uint8_t act[sizeof(exp)] = { 0 };
	unsigned int i;

	jent_shake256_init(&ctx);
	jent_sha3_update(&ctx, msg, sizeof(msg));
	jent_shake256_set_digestsize(&ctx, sizeof(exp));
	jent_sha3_final(&ctx, act);

	for (i = 0; i < sizeof(exp); i++) {
		if (exp[i] != act[i])
			return 1;
	}

	return 0;
}
#endif

static int jent_xdrbg256_tester(void)
{
	HASH_CTX_ON_STACK(ctx);
	/*
	 * Test vectors are generated using the leancrypto XDRBG implementation.
	 */
	uint8_t seed[] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	};
	static const uint8_t exp[] = {
		0x51, 0xe4, 0x3c, 0xf6, 0x4b, 0xa2, 0x80, 0x77,
		0x33, 0x1a, 0x47, 0xe3, 0xf8, 0xb4, 0x1a, 0x42,
		0xad, 0xd3, 0xa0, 0xf2, 0x53, 0x97, 0x10, 0xdd,
		0x6e, 0xa1, 0x16, 0x1d, 0x37, 0x8a, 0x6f, 0xb6
	};
	uint8_t act[sizeof(exp)] = { 0 };
	unsigned int i;

	BUILD_BUG_ON(JENT_SHA3_256_SIZE_DIGEST != sizeof(exp));

	jent_shake256_init(&ctx);
	/* Initial seed */
	jent_sha3_update(&ctx, seed, sizeof(seed));
	jent_xdrbg256_generate_block(&ctx, act, sizeof(act));
	/* Reseeding */
	jent_sha3_update(&ctx, seed, sizeof(seed));
	jent_xdrbg256_generate_block(&ctx, act, sizeof(act));

	for (i = 0; i < sizeof(exp); i++) {
		if (exp[i] != act[i])
			return 1;
	}

	return 0;
}

static int jent_xdrbg_sha3_512_tester(void)
{
	HASH_CTX_ON_STACK(ctx);
	uint8_t seed[] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	};
	static const uint8_t exp[] = {
		0x05, 0xc7, 0x63, 0xb5, 0x89, 0x42, 0xba, 0xe4,
		0x00, 0xb9, 0xa8, 0x95, 0xff, 0xaf, 0x71, 0x9a,
		0x8e, 0x18, 0x99, 0x0b, 0xb6, 0x6d, 0x59, 0xd6,
		0x3e, 0x20, 0x5a, 0xde, 0xb5, 0x0c, 0x70, 0x3c
	};
	uint8_t act[sizeof(exp)] = { 0 };
	unsigned int i;

	BUILD_BUG_ON(JENT_SHA3_256_SIZE_DIGEST != sizeof(exp));

	jent_sha3_512_init(&ctx);
	/* Initial seed */
	jent_sha3_update(&ctx, seed, sizeof(seed));
	jent_xdrbg_sha3_512_generate_block(&ctx, act, sizeof(act));
	/* Reseeding */
	jent_sha3_update(&ctx, seed, sizeof(seed));
	jent_xdrbg_sha3_512_generate_block(&ctx, act, sizeof(act));

	for (i = 0; i < sizeof(exp); i++) {
		if (exp[i] != act[i])
			return 1;
	}

	return 0;
}

static int jent_sha3_512_tester(void)
{
	HASH_CTX_ON_STACK(ctx);
	static const uint8_t msg[] = { 0x5E, 0x5E, 0xD6 };
	static const uint8_t exp[] = {
		0x73, 0xDE, 0xE5, 0x10, 0x3A, 0xE5, 0xC1, 0x7E, 0x38, 0xFA,
		0x2C, 0xE2, 0xF4, 0x4B, 0x6F, 0x4C, 0xCA, 0x67, 0x99, 0x1B,
		0xDC, 0x9E, 0x9A, 0x9E, 0x23, 0x19, 0xF9, 0xC5, 0x9A, 0x23,
		0x3A, 0x9A, 0xE8, 0x59, 0xB2, 0x83, 0xE1, 0xF2, 0x03, 0x10,
		0xF5, 0x96, 0x04, 0x0A, 0x7D, 0x6A, 0x2C, 0xC9, 0xA5, 0x49,
		0xDE, 0x80, 0x09, 0x38, 0x4B, 0xB7, 0x0B, 0x0B, 0xE5, 0xA5,
		0x55, 0x66, 0x6A, 0xD7
	};

	uint8_t act[sizeof(exp)] = { 0 };
	unsigned int i;

	jent_sha3_512_init(&ctx);
	jent_sha3_update(&ctx, msg, sizeof(msg));
	jent_sha3_final(&ctx, act);

	for (i = 0; i < sizeof(exp); i++) {
		if (exp[i] != act[i])
			return 1;
	}

	return 0;
}

int jent_sha3_tester(unsigned int sha3_512)
{
	if (jent_sha3_512_tester())
		return 1;
	if (sha3_512)
		return jent_xdrbg_sha3_512_tester();

	return jent_xdrbg256_tester();
}

