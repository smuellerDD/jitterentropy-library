/*
 * Copyright (C) 2019 - 2021, Stephan Mueller <smueller@chronox.de>
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

#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "jitterentropy.h"

int main(int argc, char * argv[])
{
	unsigned long size, rounds;
	int ret = 0;
	struct rand_data *ec_nostir;

	if (argc != 2 && argc != 3) {
		printf("%s <number of measurements>\n", argv[0]);
		return 1;
	}

	rounds = strtoul(argv[1], NULL, 10);
	if (rounds >= UINT_MAX)
		return 1;

	ret = jent_entropy_init();
	
	if (ret) {
		printf("The initialization failed with error code %d\n", ret);
		return ret;
	}

	ec_nostir = jent_entropy_collector_alloc(0, (argc == 3) ?
						JENT_FORCE_INTERNAL_TIMER : 0);
	if(!ec_nostir)
		return 1;

	for (size = 0; size < rounds; size++) {
		char tmp[32];

		if (0 > jent_read_entropy(ec_nostir, tmp, sizeof(tmp))) {
			fprintf(stderr, "FIPS 140-2 continuous test failed\n");
			return 1;
		}
		fwrite(&tmp, sizeof(tmp), 1, stdout);
	}

	jent_entropy_collector_free(ec_nostir);

	return 0;
}

