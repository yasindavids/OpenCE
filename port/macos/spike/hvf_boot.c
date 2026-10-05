/*
HVF_BOOT.C

Phase 0 of port/macos/PROPOSAL.md: run the unchanged Android guest image
(build/android/halo_guest.elf) in a Hypervisor.framework VM.

- One host allocation is the guest space, 0x80000000-0xC0000000, mapped
  into the VM at the same guest physical addresses.
- The guest runs at EL1 with identity page tables of 2 MB blocks (4 KB
  granule). Addresses below 0x80000000 are not mapped, so a NULL pointer
  faults.
- The image's import table, which the Android host fills with 64-bit host
  addresses, is filled with the guest addresses of small trampolines:
  `hvc #index; ret`. The guest's import stubs (adrp/ldr/br x16) do not
  change.
- The host serves the imports that the start of the game needs, and stops
  at the first import or system call that it does not serve. It prints
  the number of calls of each import, the exits, and the time.

	clang -O2 -Wall -I../../android/include hvf_boot.c -o hvf_boot -framework Hypervisor
	codesign -s - --entitlements ../measurements/hypervisor.entitlements -f hvf_boot
	./hvf_boot ../../../build/android/halo_guest.elf [data folder [--bench [bench_lp64.elf]]]

With --bench, once the start stops, the host calls the image's monocypher
functions and times them. bench_native.c runs the same work natively.
Result on an M1 MacBook Air, macOS 26.6.1: refer to "Phase 0" in
port/macos/PROPOSAL.md.

bench_lp64.elf is the native (LP64) code of bench_native.c as an ELF image
at 0xB8000000, so the same instructions also run in the guest:

	F="--target=aarch64-none-elf -O2 -mcpu=cortex-a53 -ffp-contract=off -fno-stack-protector -ffreestanding -fno-pic"
	clang $F -c ../../third_party/monocypher/monocypher.c ../../third_party/monocypher/monocypher-ed25519.c bench_elf_runtime.c
	ld.lld -static -nostdlib --no-pie --image-base=0xb8000000 -z max-page-size=4096 -e 0 \
		monocypher.o monocypher-ed25519.o bench_elf_runtime.o -o bench_lp64.elf
*/

#include "halo_android_abi.h"

#include <Hypervisor/Hypervisor.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/qos.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x) do { hv_return_t r = (x); if (r != HV_SUCCESS) { printf(#x " failed %x\n", r); exit(1); } } while (0)

#define GUEST_BASE 0x80000000ULL
#define GUEST_SIZE 0x40000000ULL
#define HEAP_BASE 0x8a000000ULL /* anonymous mappings of the guest */
#define HEAP_END 0xb8000000ULL
#define LP64_IMAGE 0xb8000000ULL /* bench_lp64.elf, up to 0xbe000000 */
#define STACK_TOP 0xbf100000ULL
#define BOOT_DATA 0xbf800000ULL
#define TRAMPOLINES 0xbf900000ULL
#define VECTORS 0xbfa00000ULL
#define TABLES 0xbfc00000ULL
#define CALL_STACK_TOP 0xbf400000ULL

#include "bench_workload.h"

#define HVC_VECTOR 0xf000 /* hvc #0xf000+n: exception vector n */
#define HVC_RETURN 0xffff /* the guest returned from __guest_start */

/* Linux (arm64) system call numbers and values that the guest's musl uses */
enum
{
	LINUX_ioctl = 29, LINUX_openat = 56, LINUX_close = 57, LINUX_lseek = 62, LINUX_read = 63, LINUX_write = 64,
	LINUX_readv = 65, LINUX_writev = 66, LINUX_newfstatat = 79, LINUX_fstat = 80, LINUX_exit = 93,
	LINUX_exit_group = 94, LINUX_set_tid_address = 96, LINUX_futex = 98, LINUX_nanosleep = 101,
	LINUX_clock_gettime = 113, LINUX_clock_getres = 114, LINUX_sched_yield = 124, LINUX_sigaltstack = 132,
	LINUX_rt_sigaction = 134, LINUX_rt_sigprocmask = 135, LINUX_gettimeofday = 169, LINUX_getpid = 172,
	LINUX_gettid = 178, LINUX_brk = 214, LINUX_munmap = 215, LINUX_mremap = 216, LINUX_mmap = 222,
	LINUX_mprotect = 226, LINUX_madvise = 233, LINUX_getrandom = 278,
};
#define LINUX_MAP_FIXED 0x10
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_FIXED_NOREPLACE 0x100000
#define LINUX_ENOMEM 12
#define LINUX_EINVAL 22
#define LINUX_ENOTTY 25
#define LINUX_ENOSYS 38
#define LINUX_AT_FDCWD -100

/* Linux (arm64) system call numbers of the files */
enum
{
	LINUX_getcwd = 17, LINUX_fcntl = 25, LINUX_mkdirat = 34, LINUX_unlinkat = 35, LINUX_renameat = 38,
	LINUX_ftruncate = 46, LINUX_faccessat = 48, LINUX_getdents64 = 61, LINUX_pread64 = 67, LINUX_pwrite64 = 68,
	LINUX_readlinkat = 78, LINUX_fsync = 82,
};

static uint8_t *guest_memory;
static hv_vcpu_t cpu;
static hv_vcpu_exit_t *exit_info;
static mach_timebase_info_data_t timebase;
static const struct halo_guest_header *header;
static const char **import_names;
static uint32_t import_count;
static uint64_t *import_calls;
static uint64_t syscall_calls[512];
static uint64_t exits;
static uint64_t heap_next = HEAP_BASE;
static uint32_t thread_pointer;
static int stopped;

static void *host_address(uint64_t address)
{
	if (address < GUEST_BASE || address >= GUEST_BASE + GUEST_SIZE)
		return NULL;
	return guest_memory + (address - GUEST_BASE);
}

static uint64_t reg(int index)
{
	uint64_t value;

	CHECK(hv_vcpu_get_reg(cpu, HV_REG_X0 + index, &value));
	return value;
}

static void stop(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	printf("stop: ");
	vprintf(format, arguments);
	printf("\n");
	va_end(arguments);
	stopped = 1;
}

static void print_frames(void)
{
	uint64_t pc, lr, frame = reg(29);
	int depth;

	CHECK(hv_vcpu_get_reg(cpu, HV_REG_PC, &pc));
	CHECK(hv_vcpu_get_reg(cpu, HV_REG_LR, &lr));
	printf("  pc %08llx lr %08llx (llvm-symbolizer --obj=build/android/halo_guest.elf <address>)\n", pc, lr);
	for (depth = 0; depth < 16 && host_address(frame) && frame; depth++)
	{
		uint64_t *record = host_address(frame);

		printf("  frame %08llx return %08llx\n", frame, record[1]);
		frame = record[0];
	}
}

/* ---------- guest memory layout */

static void build_page_tables(void)
{
	uint64_t *level1 = host_address(TABLES);
	uint64_t *level2 = host_address(TABLES + 0x1000);
	uint64_t index;

	memset(level1, 0, 0x2000);
	level1[2] = (TABLES + 0x1000) | 3; /* 0x80000000-0xBFFFFFFF */
	for (index = 0; index < 512; index++)
		level2[index] = (GUEST_BASE + (index << 21)) | (1ULL << 10) | (3ULL << 8) | 1; /* AF, inner shareable, normal */
}

static void build_trampolines(void)
{
	uint32_t *code = host_address(TRAMPOLINES);
	uint32_t *vectors = host_address(VECTORS);
	uint64_t *table = host_address(header->import_table);
	uint32_t index;

	for (index = 0; index < import_count; index++)
	{
		code[index * 2] = 0xd4000002 | (index << 5); /* hvc #index */
		code[index * 2 + 1] = 0xd65f03c0; /* ret */
		table[index] = TRAMPOLINES + index * 8;
	}
	code[import_count * 2] = 0xd4000002 | (HVC_RETURN << 5);
	for (index = 0; index < 16; index++)
		vectors[index * 32] = 0xd4000002 | ((HVC_VECTOR + index) << 5);
}

static uint8_t *image_file;

static uint8_t *read_file(const char *path)
{
	FILE *file = fopen(path, "rb");
	uint8_t *result;
	long size;

	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	result = malloc((size_t)size);
	if (fread(result, 1, (size_t)size, file) != (size_t)size || memcmp(result, "\177ELF", 4))
	{
		free(result);
		result = NULL;
	}
	fclose(file);
	return result;
}

/* copies the PT_LOAD segments of an ELF file to their guest addresses */
static int load_segments(const uint8_t *file)
{
	uint64_t program_offset;
	uint16_t program_size, program_count, index;

	memcpy(&program_offset, file + 0x20, 8);
	memcpy(&program_size, file + 0x36, 2);
	memcpy(&program_count, file + 0x38, 2);
	for (index = 0; index < program_count; index++)
	{
		struct { uint32_t type, flags; uint64_t offset, address, physical, file_size, memory_size, align; } segment;

		memcpy(&segment, file + program_offset + (uint64_t)index * program_size, sizeof(segment));
		if (segment.type != 1)
			continue;
		if (!host_address(segment.address) || !host_address(segment.address + segment.memory_size - 1))
		{
			printf("segment %llx outside the guest space\n", segment.address);
			return -1;
		}
		memcpy(host_address(segment.address), file + segment.offset, segment.file_size);
	}
	return 0;
}

/* the address of a symbol of an ELF file (its .symtab) */
static uint64_t elf_symbol(const uint8_t *file, const char *wanted)
{
	struct section { uint32_t name, type; uint64_t flags, address, offset, size; uint32_t link, info; uint64_t align, entry_size; };
	uint64_t section_offset;
	uint16_t section_count, index;

	memcpy(&section_offset, file + 0x28, 8);
	memcpy(&section_count, file + 0x3c, 2);
	for (index = 0; index < section_count; index++)
	{
		const struct section *symbols = (const struct section *)(file + section_offset) + index;
		const struct section *strings;
		uint64_t symbol;

		if (symbols->type != 2)
			continue;
		strings = (const struct section *)(file + section_offset) + symbols->link;
		for (symbol = 0; symbol < symbols->size / 24; symbol++)
		{
			const uint8_t *entry = file + symbols->offset + symbol * 24;
			uint32_t name;
			uint64_t value;

			memcpy(&name, entry, 4);
			memcpy(&value, entry + 8, 8);
			if (!strcmp((const char *)file + strings->offset + name, wanted))
				return value;
		}
	}
	printf("no symbol %s\n", wanted);
	exit(1);
}

static int load_image(const char *path)
{
	const char *name;
	uint32_t index;

	image_file = read_file(path);
	if (!image_file || load_segments(image_file) != 0)
	{
		printf("cannot load %s\n", path);
		return -1;
	}
	header = host_address(HALO_GUEST_IMAGE_BASE);
	if (header->magic != HALO_GUEST_MAGIC || header->abi_version != HALO_GUEST_ABI_VERSION)
	{
		printf("the image header does not match\n");
		return -1;
	}
	import_count = *(uint32_t *)host_address(header->import_count);
	import_names = calloc(import_count, sizeof(*import_names));
	import_calls = calloc(import_count, sizeof(*import_calls));
	name = host_address(header->import_names);
	for (index = 0; index < import_count; index++)
	{
		import_names[index] = name;
		name += strlen(name) + 1;
	}
	printf("image %08x-%08x, %u imports, entry %08x\n", HALO_GUEST_IMAGE_BASE, header->image_end, import_count, header->start);
	return 0;
}

static uint32_t guest_strings(uint64_t *cursor, const char **strings, int count)
{
	uint32_t *array;
	uint32_t result;
	int index;

	result = (uint32_t)*cursor;
	array = host_address(*cursor);
	*cursor += (uint64_t)(count + 1) * 4;
	for (index = 0; index < count; index++)
	{
		size_t length = strlen(strings[index]) + 1;

		memcpy(host_address(*cursor), strings[index], length);
		array[index] = (uint32_t)*cursor;
		*cursor += (length + 3) & ~3ULL;
	}
	array[count] = 0;
	return result;
}

/* ---------- system calls */

struct guest_timespec
{
	int32_t seconds;
	int32_t nanoseconds;
};

static clockid_t clock_of(uint64_t linux_clock)
{
	return linux_clock == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC;
}

static int64_t guest_mmap(uint64_t address, uint64_t size, int flags)
{
	uint64_t length = (size + 0xfff) & ~0xfffULL;

	if (!(flags & LINUX_MAP_ANONYMOUS))
	{
		stop("mmap of a file");
		return -LINUX_ENOSYS;
	}
	if (flags & (LINUX_MAP_FIXED | LINUX_MAP_FIXED_NOREPLACE))
	{
		if (!host_address(address) || !host_address(address + length - 1))
			return -LINUX_ENOMEM;
		memset(host_address(address), 0, length);
		return (int64_t)address;
	}
	if (heap_next + length > HEAP_END)
		return -LINUX_ENOMEM;
	address = heap_next;
	heap_next += length;
	return (int64_t)address;
}

/* ---------- files: Linux values to Darwin values */

static int64_t linux_error(int error)
{
	switch (error)
	{
	case EAGAIN: return -11;
	case EDEADLK: return -35;
	case ENAMETOOLONG: return -36;
	case ENOSYS: return -38;
	case ENOTEMPTY: return -39;
	case ELOOP: return -40;
	case ENOTSUP: return -95;
	case ETIMEDOUT: return -110;
	default: return error <= 34 ? -error : -LINUX_EINVAL;
	}
}

static int64_t result_of(int64_t value)
{
	return value < 0 ? linux_error(errno) : value;
}

static int directory_of(int64_t linux_fd)
{
	return (int)linux_fd == LINUX_AT_FDCWD ? AT_FDCWD : (int)linux_fd;
}

static int open_flags(uint64_t linux_flags)
{
	static const struct { int linux_flag, darwin_flag; } flags[] = {
		{ 0100, O_CREAT }, { 0200, O_EXCL }, { 0400, O_NOCTTY }, { 01000, O_TRUNC }, { 02000, O_APPEND },
		{ 04000, O_NONBLOCK }, { 040000, O_DIRECTORY }, { 0100000, O_NOFOLLOW }, { 02000000, O_CLOEXEC },
	};
	int result = (int)(linux_flags & 3);
	unsigned index;

	for (index = 0; index < sizeof(flags) / sizeof(flags[0]); index++)
		if (linux_flags & flags[index].linux_flag)
			result |= flags[index].darwin_flag;
	return result;
}

static int at_flags(uint64_t linux_flags)
{
	return ((linux_flags & 0x100) ? AT_SYMLINK_NOFOLLOW : 0) | ((linux_flags & 0x200) ? AT_REMOVEDIR : 0);
}

/* the AArch64 kernel's struct stat (port/android/guest/libc/arch/arm64_32/kstat.h) */
struct linux_stat
{
	uint64_t dev, ino;
	uint32_t mode, nlink, uid, gid;
	uint64_t rdev, pad;
	int64_t size;
	int32_t blksize, pad2;
	int64_t blocks, atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
	uint32_t unused[2];
};

static int64_t stat_out(int result, const struct stat *value, uint64_t address)
{
	struct linux_stat *out = host_address(address);

	if (result != 0)
		return linux_error(errno);
	memset(out, 0, sizeof(*out));
	out->dev = (uint64_t)value->st_dev;
	out->ino = value->st_ino;
	out->mode = value->st_mode;
	out->nlink = value->st_nlink;
	out->uid = value->st_uid;
	out->gid = value->st_gid;
	out->rdev = (uint64_t)value->st_rdev;
	out->size = value->st_size;
	out->blksize = value->st_blksize;
	out->blocks = value->st_blocks;
	out->atime = value->st_atimespec.tv_sec;
	out->atime_nsec = value->st_atimespec.tv_nsec;
	out->mtime = value->st_mtimespec.tv_sec;
	out->mtime_nsec = value->st_mtimespec.tv_nsec;
	out->ctime = value->st_ctimespec.tv_sec;
	out->ctime_nsec = value->st_ctimespec.tv_nsec;
	return 0;
}

static int64_t guest_syscall(uint64_t number, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
	(void)e;
	(void)f;
	if (number < 512)
		syscall_calls[number]++;
	switch (number)
	{
	case LINUX_write:
		if (a == 1 || a == 2)
		{
			fwrite(host_address(b), 1, (size_t)(uint32_t)c, stdout);
			return (uint32_t)c;
		}
		return result_of(write((int)a, host_address(b), (size_t)(uint32_t)c));
	case LINUX_openat:
		return result_of(openat(directory_of((int64_t)a), host_address(b), open_flags(c), (int)d));
	case LINUX_close:
		return result_of(close((int)a));
	case LINUX_read:
		return result_of(read((int)a, host_address(b), (size_t)(uint32_t)c));
	case LINUX_pread64:
		return result_of(pread((int)a, host_address(b), (size_t)(uint32_t)c, (off_t)d));
	case LINUX_pwrite64:
		return result_of(pwrite((int)a, host_address(b), (size_t)(uint32_t)c, (off_t)d));
	case LINUX_lseek:
		return result_of(lseek((int)a, (off_t)b, (int)c));
	case LINUX_fstat:
	{
		struct stat value;

		return stat_out(fstat((int)a, &value), &value, b);
	}
	case LINUX_newfstatat:
	{
		struct stat value;
		const char *path = host_address(b);

		if (path && !*path && (d & 0x1000))
			return stat_out(fstat((int)a, &value), &value, c);
		return stat_out(fstatat(directory_of((int64_t)a), path, &value, at_flags(d)), &value, c);
	}
	case LINUX_fcntl:
		if (b == F_GETFD || b == F_SETFD)
			return result_of(fcntl((int)a, (int)b, (int)c));
		break;
	case LINUX_mkdirat:
		return result_of(mkdirat(directory_of((int64_t)a), host_address(b), (mode_t)c));
	case LINUX_unlinkat:
		return result_of(unlinkat(directory_of((int64_t)a), host_address(b), at_flags(c)));
	case LINUX_renameat:
		return result_of(renameat(directory_of((int64_t)a), host_address(b), directory_of((int64_t)c), host_address(d)));
	case LINUX_faccessat:
		return result_of(faccessat(directory_of((int64_t)a), host_address(b), (int)c, 0));
	case LINUX_ftruncate:
		return result_of(ftruncate((int)a, (off_t)b));
	case LINUX_fsync:
		return result_of(fsync((int)a));
	case LINUX_getcwd:
		return getcwd(host_address(a), (size_t)b) ? (int64_t)strlen(host_address(a)) + 1 : linux_error(errno);
	case LINUX_readv:
	case LINUX_writev:
	{
		const uint32_t *vector = host_address(b);
		struct iovec host_vector[64];
		int64_t total = 0;
		uint64_t index;

		if (c > 64)
			return -LINUX_EINVAL;
		for (index = 0; index < c; index++)
		{
			host_vector[index].iov_base = host_address(vector[index * 2]);
			host_vector[index].iov_len = vector[index * 2 + 1];
			total += vector[index * 2 + 1];
		}
		if (number == LINUX_writev && (a == 1 || a == 2))
		{
			for (index = 0; index < c; index++)
				fwrite(host_vector[index].iov_base, 1, host_vector[index].iov_len, stdout);
			return total;
		}
		if (number == LINUX_readv)
			return result_of(readv((int)a, host_vector, (int)c));
		return result_of(writev((int)a, host_vector, (int)c));
	}
	case LINUX_clock_gettime:
	case LINUX_clock_getres:
	{
		struct timespec value;
		struct guest_timespec *result = host_address(b);

		if (number == LINUX_clock_gettime)
			clock_gettime(clock_of(a), &value);
		else
			clock_getres(clock_of(a), &value);
		if (result)
		{
			result->seconds = (int32_t)value.tv_sec;
			result->nanoseconds = (int32_t)value.tv_nsec;
		}
		return 0;
	}
	case LINUX_gettimeofday:
	{
		struct timespec value;
		struct guest_timespec *result = host_address(a);

		clock_gettime(CLOCK_REALTIME, &value);
		if (result)
		{
			result->seconds = (int32_t)value.tv_sec;
			result->nanoseconds = (int32_t)(value.tv_nsec / 1000);
		}
		return 0;
	}
	case LINUX_nanosleep:
	{
		const struct guest_timespec *request = host_address(a);
		struct timespec value = { request->seconds, request->nanoseconds };

		nanosleep(&value, NULL);
		return 0;
	}
	case LINUX_mmap:
		return guest_mmap(a, b, (int)d);
	case LINUX_munmap:
	case LINUX_mprotect:
	case LINUX_madvise:
		return 0;
	case LINUX_brk:
	case LINUX_mremap:
		return -LINUX_ENOMEM;
	case LINUX_set_tid_address:
	case LINUX_getpid:
	case LINUX_gettid:
		return 1;
	case LINUX_rt_sigaction:
	case LINUX_rt_sigprocmask:
	case LINUX_sigaltstack:
	case LINUX_sched_yield:
		return 0;
	case LINUX_ioctl:
		return -LINUX_ENOTTY;
	case LINUX_getrandom:
		arc4random_buf(host_address(a), (size_t)b);
		return (int64_t)b;
	case LINUX_exit:
	case LINUX_exit_group:
		stop("the guest exited with %d", (int)a);
		return 0;
	}
	stop("system call %llu is not served (%llx, %llx, %llx, %llx)", number, a, b, c, d);
	return -LINUX_ENOSYS;
}

/* ---------- imports */

static int64_t guest_import(uint32_t index)
{
	const char *name = import_names[index];

	import_calls[index]++;
	if (!strcmp(name, "host_syscall"))
		return guest_syscall(reg(0), reg(1), reg(2), reg(3), reg(4), reg(5), reg(6));
	if (!strcmp(name, "host_get_tp"))
		return thread_pointer;
	if (!strcmp(name, "host_set_tp"))
	{
		thread_pointer = (uint32_t)reg(0);
		return 0;
	}
	if (!strcmp(name, "host_errno"))
		return 0;
	/* the watch moves into the guest (PROPOSAL.md); here it records nothing */
	if (!strncmp(name, "host_memory_watch_", 18))
		return 0;
	if (!strcmp(name, "host_log"))
	{
		printf("guest log %d: %s\n", (int)reg(0), (const char *)host_address(reg(1)));
		return 0;
	}
	if (!strcmp(name, "host_abort"))
	{
		stop("host_abort: %s", (const char *)host_address(reg(0)));
		return 0;
	}
	if (!strcmp(name, "host_exit"))
	{
		stop("host_exit %d", (int)reg(0));
		return 0;
	}
	stop("import %s is not served (x0 %llx, x1 %llx)", name, reg(0), reg(1));
	return 0;
}

/* ---------- running the guest */

static double milliseconds(uint64_t ticks)
{
	return (double)ticks * timebase.numer / timebase.denom / 1e6;
}

/* runs the vCPU until the guest returns to the return trampoline (1) or stops (0) */
static int run_guest(int report_first_call)
{
	while (!stopped)
	{
		uint64_t syndrome;
		uint32_t immediate;

		CHECK(hv_vcpu_run(cpu));
		if (exit_info->reason != HV_EXIT_REASON_EXCEPTION)
			continue;
		exits++;
		syndrome = exit_info->exception.syndrome;
		immediate = (uint32_t)(syndrome & 0xffff);
		if ((syndrome >> 26) != 0x16)
		{
			stop("exit with syndrome %llx at guest physical %llx", syndrome, exit_info->exception.physical_address);
			print_frames();
			return 0;
		}
		if (report_first_call)
		{
			uint64_t pc;

			CHECK(hv_vcpu_get_reg(cpu, HV_REG_PC, &pc));
			printf("first host call: %s, from %08llx, pc after the exit %08llx\n",
				immediate < import_count ? import_names[immediate] : "?", reg(30), pc);
			report_first_call = 0;
		}
		if (immediate >= HVC_VECTOR && immediate < HVC_VECTOR + 16)
		{
			uint64_t esr, far, elr;

			hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_ESR_EL1, &esr);
			hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_FAR_EL1, &far);
			hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_ELR_EL1, &elr);
			stop("guest exception (vector %u): ESR %llx, FAR %llx, ELR %llx", immediate - HVC_VECTOR, esr, far, elr);
			CHECK(hv_vcpu_set_reg(cpu, HV_REG_PC, elr));
			print_frames();
			return 0;
		}
		if (immediate == HVC_RETURN)
			return 1;
		if (immediate >= import_count)
		{
			stop("hvc #%u", immediate);
			return 0;
		}
		CHECK(hv_vcpu_set_reg(cpu, HV_REG_X0, (uint64_t)guest_import(immediate)));
		if (stopped)
			print_frames();
	}
	return 0;
}

/* calls a guest function on its own stack; returns the guest's execution time in ticks */
static uint64_t call_guest(uint64_t function, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
	uint64_t before, after;

	CHECK(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_SP_EL1, CALL_STACK_TOP));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_CPSR, 0x3c5));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_PC, function));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_X0, a));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_X1, b));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_X2, c));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_X3, d));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_LR, TRAMPOLINES + import_count * 8));
	hv_vcpu_get_exec_time(cpu, &before);
	if (!run_guest(0))
		exit(1);
	hv_vcpu_get_exec_time(cpu, &after);
	return after - before;
}

static uint64_t guest_allocate(uint64_t size)
{
	uint64_t address = heap_next;

	heap_next += (size + 0xfff) & ~0xfffULL;
	return address;
}

static void print_hash(const char *label, const uint8_t *hash)
{
	printf("%s %02x%02x%02x%02x%02x%02x%02x%02x", label, hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7]);
}

/*
The workload of bench_native.c, on the monocypher of an ELF file in the
guest: the game's image (arm64_32), or bench_lp64.elf (the native LP64
code, at LP64_IMAGE).
*/
static void benchmark(const uint8_t *file, const char *label)
{
	uint64_t message = guest_allocate(BENCH_MESSAGE_SIZE);
	uint64_t hash = guest_allocate(64), scalar = guest_allocate(32), point = guest_allocate(32), out = guest_allocate(32);
	uint8_t *bytes = host_address(message);
	uint64_t blake2b = elf_symbol(file, "crypto_blake2b"), sha512 = elf_symbol(file, "crypto_sha512");
	uint64_t x25519 = elf_symbol(file, "crypto_x25519");
	uint64_t index, wall, ticks;
	int run;

	for (index = 0; index < BENCH_MESSAGE_SIZE; index++)
		bytes[index] = (uint8_t)((index * 2654435761u) >> 24);
	printf("\nin the guest: %s (wall clock, and the guest's execution time), best of %d\n", label, BENCH_RUNS);
	{
		double best_wall = 1e30, best_guest = 1e30;

		for (run = 0; run < BENCH_RUNS; run++)
		{
			wall = mach_absolute_time();
			ticks = call_guest(blake2b, hash, 64, message, BENCH_MESSAGE_SIZE);
			wall = mach_absolute_time() - wall;
			if (milliseconds(wall) < best_wall)
				best_wall = milliseconds(wall);
			if (milliseconds(ticks) < best_guest)
				best_guest = milliseconds(ticks);
		}
		print_hash("  blake2b, 64 MB:", host_address(hash));
		printf("  %8.2f ms (guest %.2f ms)\n", best_wall, best_guest);
	}
	{
		double best_wall = 1e30, best_guest = 1e30;

		for (run = 0; run < BENCH_RUNS; run++)
		{
			wall = mach_absolute_time();
			ticks = call_guest(sha512, hash, message, BENCH_MESSAGE_SIZE, 0);
			wall = mach_absolute_time() - wall;
			if (milliseconds(wall) < best_wall)
				best_wall = milliseconds(wall);
			if (milliseconds(ticks) < best_guest)
				best_guest = milliseconds(ticks);
		}
		print_hash("  sha512, 64 MB: ", host_address(hash));
		printf("  %8.2f ms (guest %.2f ms)\n", best_wall, best_guest);
	}
	{
		double best_wall = 1e30, best_guest = 1e30;

		for (run = 0; run < BENCH_RUNS; run++)
		{
			uint8_t *scalar_bytes = host_address(scalar), *point_bytes = host_address(point);
			uint64_t total_ticks = 0;
			int iteration;

			for (index = 0; index < 32; index++)
			{
				scalar_bytes[index] = (uint8_t)(index + 1);
				point_bytes[index] = index == 0 ? 9 : 0;
			}
			wall = mach_absolute_time();
			for (iteration = 0; iteration < BENCH_X25519_COUNT; iteration++)
			{
				total_ticks += call_guest(x25519, out, scalar, point, 0);
				memcpy(point_bytes, host_address(out), 32);
			}
			wall = mach_absolute_time() - wall;
			if (milliseconds(wall) < best_wall)
				best_wall = milliseconds(wall);
			if (milliseconds(total_ticks) < best_guest)
				best_guest = milliseconds(total_ticks);
		}
		print_hash("  x25519, 2000:  ", host_address(point));
		printf("  %8.2f ms (guest %.2f ms)\n", best_wall, best_guest);
	}
}

/* ---------- main */

int main(int argc, char **argv)
{
	static const char *guest_argv[] = { "halo" };
	static const char *guest_environment[] = { "HOME=/", "LANG=C" };
	uint64_t cursor = BOOT_DATA, start, end, exec_time;
	struct halo_guest_boot *boot;
	uint32_t boot_address;
	uint32_t index;
	int bench = argc > 3 && !strcmp(argv[3], "--bench");

	if (argc < 2)
	{
		printf("usage: hvf_boot <halo_guest.elf> [data folder [--bench [bench_lp64.elf]]]\n");
		return 1;
	}
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	mach_timebase_info(&timebase);
	guest_memory = mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
	if (guest_memory == MAP_FAILED || load_image(argv[1]) != 0)
		return 1;
	if (argc > 2 && chdir(argv[2]) != 0)
	{
		perror(argv[2]);
		return 1;
	}
	build_page_tables();
	build_trampolines();

	boot = host_address(cursor);
	boot_address = (uint32_t)cursor;
	cursor += sizeof(*boot);
	boot->argc = 1;
	boot->argv = guest_strings(&cursor, guest_argv, 1);
	boot->environment = guest_strings(&cursor, guest_environment, 2);
	boot->page_size = 4096;

	CHECK(hv_vm_create(NULL));
	CHECK(hv_vm_map(guest_memory, GUEST_BASE, GUEST_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
	CHECK(hv_vcpu_create(&cpu, &exit_info, NULL));
	CHECK(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_MAIR_EL1, 0xff));
	CHECK(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_TCR_EL1, 32 | (1 << 8) | (1 << 10) | (3 << 12) | (1 << 23) | (1ULL << 32)));
	CHECK(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_TTBR0_EL1, TABLES));
	CHECK(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_VBAR_EL1, VECTORS));
	CHECK(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_CPACR_EL1, 3 << 20));
	CHECK(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_SCTLR_EL1, 0x30d00800 | (1 << 12) | (1 << 2) | 1));
	CHECK(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_SP_EL1, STACK_TOP));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_CPSR, 0x3c5));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_PC, header->start));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_X0, boot_address));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_LR, TRAMPOLINES + import_count * 8));

	start = mach_absolute_time();
	if (run_guest(1))
		stop("the guest returned from __guest_start");
	end = mach_absolute_time();
	hv_vcpu_get_exec_time(cpu, &exec_time);

	printf("\n%llu exits in %.1f ms (%.1f ms in the guest)\n", exits, milliseconds(end - start), milliseconds(exec_time));
	printf("guest heap used: %llu KB\n", (heap_next - HEAP_BASE) / 1024);
	for (index = 0; index < import_count; index++)
		if (import_calls[index])
			printf("  %-40s %llu\n", import_names[index], import_calls[index]);
	for (index = 0; index < 512; index++)
		if (syscall_calls[index])
			printf("    system call %-27u %llu\n", index, syscall_calls[index]);
	if (bench)
	{
		uint8_t *lp64 = argc > 4 ? read_file(argv[4]) : NULL;
		uint64_t bench_heap = heap_next;

		stopped = 0;
		benchmark(image_file, "the image's monocypher (arm64_32)");
		if (lp64 && load_segments(lp64) == 0)
		{
			heap_next = bench_heap; /* the same buffers again */
			benchmark(lp64, argv[4]);
		}
	}
	return 0;
}
