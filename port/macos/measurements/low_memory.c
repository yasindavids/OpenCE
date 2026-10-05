/*
LOW_MEMORY.C

Can a macOS process map memory at the Xbox window (0x80000000)? The Android
port puts the guest there (port/android/include/halo_android_abi.h).

	clang -O2 low_memory.c -o low_memory && ./low_memory
	clang -O2 low_memory.c -o low_memory_small -Wl,-pagezero_size,0x4000 && ./low_memory_small
	clang -arch x86_64 -O2 low_memory.c -o low_memory_x64 -Wl,-pagezero_size,0x1000 && ./low_memory_x64

Results on an M1 MacBook Air, macOS 26.6.1 (port/macos/PROPOSAL.md):
- arm64: page size 16384, the fixed mapping fails (ENOMEM).
- arm64 with a small __PAGEZERO: the kernel kills the process (exit 137).
- x86_64 under Rosetta 2: page size 4096, the fixed mapping succeeds.
*/

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
	void *want = (void *)0x80000000ULL;
	void *fixed;
	void *hint;

	printf("pagesize %ld\n", sysconf(_SC_PAGESIZE));
	fixed = mmap(want, 0x08000000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
	if (fixed == MAP_FAILED)
		printf("FIXED 0x80000000: FAIL errno=%d %s\n", errno, strerror(errno));
	else
	{
		*(volatile int *)fixed = 42;
		printf("FIXED 0x80000000: OK %p\n", fixed);
	}
	hint = mmap(want, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	printf("hint 0x80000000 -> %p\n", hint);
	return 0;
}
