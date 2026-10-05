/*
BENCH_ELF_RUNTIME.C

The C library functions that monocypher needs in bench_lp64.elf, a
static AArch64 (LP64) image of monocypher that hvf_boot.c --bench loads
into the guest and calls: the same code as bench_native.c, in the VM.
*/

#include <stddef.h>

__attribute__((no_builtin)) void *memcpy(void *destination, const void *source, size_t size)
{
	unsigned char *to = destination;
	const unsigned char *from = source;

	while (size--)
		*to++ = *from++;
	return destination;
}

__attribute__((no_builtin)) void *memset(void *destination, int value, size_t size)
{
	unsigned char *to = destination;

	while (size--)
		*to++ = (unsigned char)value;
	return destination;
}
