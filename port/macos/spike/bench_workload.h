/*
BENCH_WORKLOAD.H

The work that hvf_boot.c (in the guest) and bench_native.c (natively) time:
monocypher hashes of one buffer, and a chain of X25519 operations.
*/

#define BENCH_RUNS 5
#define BENCH_MESSAGE_SIZE (64ULL << 20)
#define BENCH_X25519_COUNT 2000
