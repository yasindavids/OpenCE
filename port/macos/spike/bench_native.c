/*
BENCH_NATIVE.C

The work of hvf_boot.c --bench (bench_workload.h), natively: the same
monocypher source as the guest image (port/third_party/monocypher), as a
64-bit macOS program. Build it with the guest's code generation options,
then with the M1's, to compare both with the guest:

	clang -O2 -mcpu=cortex-a53 -ffp-contract=off -fno-stack-protector -I../../third_party/monocypher \
		bench_native.c ../../third_party/monocypher/monocypher.c ../../third_party/monocypher/monocypher-ed25519.c \
		-o bench_native_a53
	clang -O2 -mcpu=apple-m1 -ffp-contract=off -fno-stack-protector -I../../third_party/monocypher \
		bench_native.c ../../third_party/monocypher/monocypher.c ../../third_party/monocypher/monocypher-ed25519.c \
		-o bench_native_m1
*/

#include "bench_workload.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"

#include <mach/mach_time.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/qos.h>

static mach_timebase_info_data_t timebase;

static double milliseconds(uint64_t ticks)
{
	return (double)ticks * timebase.numer / timebase.denom / 1e6;
}

static void print_hash(const char *label, const uint8_t *hash)
{
	printf("%s %02x%02x%02x%02x%02x%02x%02x%02x", label, hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7]);
}

int main(void)
{
	uint8_t *message = malloc(BENCH_MESSAGE_SIZE);
	uint8_t hash[64], scalar[32], point[32], out[32];
	uint64_t index, start;
	double best;
	int run, iteration;

	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	mach_timebase_info(&timebase);
	for (index = 0; index < BENCH_MESSAGE_SIZE; index++)
		message[index] = (uint8_t)((index * 2654435761u) >> 24);
	printf("native benchmark (wall clock), best of %d\n", BENCH_RUNS);

	best = 1e30;
	for (run = 0; run < BENCH_RUNS; run++)
	{
		start = mach_absolute_time();
		crypto_blake2b(hash, 64, message, BENCH_MESSAGE_SIZE);
		if (milliseconds(mach_absolute_time() - start) < best)
			best = milliseconds(mach_absolute_time() - start);
	}
	print_hash("  blake2b, 64 MB:", hash);
	printf("  %8.2f ms\n", best);

	best = 1e30;
	for (run = 0; run < BENCH_RUNS; run++)
	{
		start = mach_absolute_time();
		crypto_sha512(hash, message, BENCH_MESSAGE_SIZE);
		if (milliseconds(mach_absolute_time() - start) < best)
			best = milliseconds(mach_absolute_time() - start);
	}
	print_hash("  sha512, 64 MB: ", hash);
	printf("  %8.2f ms\n", best);

	best = 1e30;
	for (run = 0; run < BENCH_RUNS; run++)
	{
		for (index = 0; index < 32; index++)
		{
			scalar[index] = (uint8_t)(index + 1);
			point[index] = index == 0 ? 9 : 0;
		}
		start = mach_absolute_time();
		for (iteration = 0; iteration < BENCH_X25519_COUNT; iteration++)
		{
			crypto_x25519(out, scalar, point);
			memcpy(point, out, 32);
		}
		if (milliseconds(mach_absolute_time() - start) < best)
			best = milliseconds(mach_absolute_time() - start);
	}
	print_hash("  x25519, 2000:  ", point);
	printf("  %8.2f ms\n", best);
	return 0;
}
