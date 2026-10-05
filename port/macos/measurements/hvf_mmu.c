/*
HVF_MMU.C

The guest side of the proposed macOS port (port/macos/PROPOSAL.md): a
Hypervisor.framework guest at EL1 with its own stage-1 page tables, as the
EL1 runtime of the port would set them up. It measures:

1. The ID registers that the vCPU reports (the stage-1 granules).
2. Memory latency: a chain of dependent 32-bit loads at random 64-byte
   lines, in working sets from 1 MB to 256 MB. The same instructions run
   natively in this process (16 KB pages) and in the guest, with the data
   mapped by 4 KB pages and by 2 MB blocks. The difference is the cost of
   the second stage of address translation and of the smaller pages.
3. CPU speed: a dependent multiply chain, natively and in the guest.
4. The watch of texture pages: a write to a read-only 4 KB guest page,
   handled by the guest's own EL1 fault handler with no exit, against a
   write to a read-only 16 KB page of this process, handled by a SIGBUS
   handler that calls mprotect, as the Android host does.

	clang -O2 hvf_mmu.c -o hvf_mmu -framework Hypervisor
	codesign -s - --entitlements hypervisor.entitlements -f hvf_mmu
	./hvf_mmu

Result on an M1 MacBook Air, macOS 26.6.1: refer to the tables in
port/macos/PROPOSAL.md.
*/

#include <Hypervisor/Hypervisor.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/qos.h>

#define CHECK(x) do { hv_return_t r = (x); if (r != HV_SUCCESS) { printf(#x " failed %x\n", r); exit(1); } } while (0)

#define LOW_IPA 0x80000000ULL /* code, vectors, page tables: one 2 MB block */
#define LOW_SIZE 0x200000ULL
#define VECTORS_OFFSET 0x800
#define TABLES_OFFSET 0x100000
#define DATA_IPA 0x90000000ULL
#define DATA_SIZE 0x10000000ULL /* 256 MB */

#define DESC_TABLE 3ULL
#define DESC_PAGE 3ULL
#define DESC_BLOCK 1ULL
#define DESC_AF (1ULL << 10)
#define DESC_SH_INNER (3ULL << 8)
#define DESC_AP_READ_ONLY (1ULL << 7)
#define DESC_NORMAL (DESC_AF | DESC_SH_INNER) /* AttrIndx 0: MAIR attribute 0 */

enum
{
	TEST_CHASE,
	TEST_COMPUTE,
	TEST_TOUCH,
};

/*
The guest code, copied to LOW_IPA. x3 selects the test. Each test ends
with hvc #0. The code uses only branches relative to itself.
*/
extern const uint32_t guest_code[];
extern const uint32_t guest_code_end[];
extern const uint32_t guest_vectors[];
uint32_t native_chase(const void *base, uint32_t start, uint64_t count);
uint64_t native_compute(uint64_t count);

__asm__(
	"	.text\n"
	"	.p2align 4\n"
	"	.globl _guest_code\n"
	"_guest_code:\n"
	"	tlbi vmalle1\n"
	"	dsb ish\n"
	"	isb\n"
	"	cmp x3, #0\n"
	"	b.eq 1f\n"
	"	cmp x3, #1\n"
	"	b.eq 2f\n"
	"	cmp x3, #2\n"
	"	b.eq 3f\n"
	"	hvc #3\n"
	"1:	ldr w1, [x0, w1, uxtw]\n"
	"	subs x2, x2, #1\n"
	"	b.ne 1b\n"
	"	hvc #0\n"
	"2:	madd x4, x4, x5, x6\n"
	"	subs x2, x2, #1\n"
	"	b.ne 2b\n"
	"	hvc #0\n"
	"3:	strb wzr, [x0]\n"
	"	add x0, x0, #4096\n"
	"	subs x2, x2, #1\n"
	"	b.ne 3b\n"
	"	hvc #0\n"
	"	.globl _guest_code_end\n"
	"_guest_code_end:\n"
	"\n"
	/*
	The EL1 vectors, copied to LOW_IPA + VECTORS_OFFSET. The synchronous
	exception of the current EL (offset 0x200) is the watch: a permission
	fault in the data makes its 4 KB page writable and counts it in x22.
	x20 is the guest address of the data, x21 the guest address of its
	level 3 table. Other exceptions exit with hvc #1 or hvc #2.
	*/
	"	.p2align 11\n"
	"	.globl _guest_vectors\n"
	"_guest_vectors:\n"
	"	.rept 4\n"
	"	hvc #2\n"
	"	.balign 0x80\n"
	"	.endr\n"
	"	mrs x9, esr_el1\n"
	"	lsr x10, x9, #26\n"
	"	cmp x10, #0x25\n"
	"	b.ne 9f\n"
	"	and x10, x9, #0x3c\n"
	"	cmp x10, #0x0c\n"
	"	b.ne 9f\n"
	"	mrs x9, far_el1\n"
	"	sub x10, x9, x20\n"
	"	lsr x10, x10, #12\n"
	"	add x11, x21, x10, lsl #3\n"
	"	ldr x12, [x11]\n"
	"	bic x12, x12, #0x80\n"
	"	str x12, [x11]\n"
	"	dsb ishst\n"
	"	lsr x9, x9, #12\n"
	"	tlbi vale1, x9\n"
	"	dsb ish\n"
	"	isb\n"
	"	add x22, x22, #1\n"
	"	eret\n"
	"9:	hvc #1\n"
	"	.balign 0x80\n"
	"	.rept 11\n"
	"	hvc #2\n"
	"	.balign 0x80\n"
	"	.endr\n"
	"\n"
	"	.p2align 4\n"
	"	.globl _native_chase\n"
	"_native_chase:\n"
	"1:	ldr w1, [x0, w1, uxtw]\n"
	"	subs x2, x2, #1\n"
	"	b.ne 1b\n"
	"	mov w0, w1\n"
	"	ret\n"
	"\n"
	"	.globl _native_compute\n"
	"_native_compute:\n"
	"	mov x2, x0\n"
	"	mov x4, #1\n"
	"	mov x5, #3\n"
	"	mov x6, #7\n"
	"2:	madd x4, x4, x5, x6\n"
	"	subs x2, x2, #1\n"
	"	b.ne 2b\n"
	"	mov x0, x4\n"
	"	ret\n");

static uint8_t *low;
static uint8_t *data;
static hv_vcpu_t cpu;
static hv_vcpu_exit_t *exit_info;
static mach_timebase_info_data_t timebase;

static double elapsed_ns(uint64_t start, uint64_t end)
{
	return (double)(end - start) * timebase.numer / timebase.denom;
}

/* Identity page tables: the low 2 MB block, and the data by 4 KB pages or 2 MB blocks. */
static void map_guest(int small_pages, uint64_t read_only_bytes)
{
	uint64_t *level1 = (uint64_t *)(low + TABLES_OFFSET);
	uint64_t *level2 = level1 + 512;
	uint64_t *level3 = level2 + 512;
	uint64_t index;

	memset(level1, 0, 0x2000);
	level1[(LOW_IPA >> 30) & 3] = (LOW_IPA + TABLES_OFFSET + 0x1000) | DESC_TABLE;
	level2[0] = LOW_IPA | DESC_NORMAL | DESC_BLOCK;
	for (index = 0; index < DATA_SIZE >> 21; index++)
	{
		uint64_t block = DATA_IPA + (index << 21);
		uint64_t page;

		if (!small_pages)
		{
			level2[((DATA_IPA - LOW_IPA) >> 21) + index] = block | DESC_NORMAL | DESC_BLOCK;
			continue;
		}
		level2[((DATA_IPA - LOW_IPA) >> 21) + index] =
			(LOW_IPA + TABLES_OFFSET + 0x2000 + (index << 12)) | DESC_TABLE;
		for (page = 0; page < 512; page++)
		{
			uint64_t address = block + (page << 12);
			uint64_t descriptor = address | DESC_NORMAL | DESC_PAGE;

			if (address - DATA_IPA < read_only_bytes)
				descriptor |= DESC_AP_READ_ONLY;
			level3[(index << 9) + page] = descriptor;
		}
	}
}

static double run_guest(uint64_t test, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t *result)
{
	uint64_t start, end, syndrome, value;

	CHECK(hv_vcpu_set_reg(cpu, HV_REG_PC, LOW_IPA));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_CPSR, 0x3c5)); /* EL1h, interrupts masked */
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_X0, x0));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_X1, x1));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_X2, x2));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_X3, test));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_X4, 1));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_X5, 3));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_X6, 7));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_X20, DATA_IPA));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_X21, LOW_IPA + TABLES_OFFSET + 0x2000));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_X22, 0));
	start = mach_absolute_time();
	for (;;)
	{
		CHECK(hv_vcpu_run(cpu));
		if (exit_info->reason == HV_EXIT_REASON_EXCEPTION)
			break;
	}
	end = mach_absolute_time();
	syndrome = exit_info->exception.syndrome;
	if ((syndrome >> 26) != 0x16 || (syndrome & 0xffff) != 0)
	{
		uint64_t esr, far, elr;

		hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_ESR_EL1, &esr);
		hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_FAR_EL1, &far);
		hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_ELR_EL1, &elr);
		printf("guest stopped: syndrome %llx, ESR_EL1 %llx, FAR_EL1 %llx, ELR_EL1 %llx\n",
			syndrome, esr, far, elr);
		exit(1);
	}
	CHECK(hv_vcpu_get_reg(cpu, test == TEST_TOUCH ? HV_REG_X22 : test == TEST_COMPUTE ? HV_REG_X4 : HV_REG_X1, &value));
	if (result)
		*result = value;
	return elapsed_ns(start, end);
}

/* One cycle through `size` bytes at random 64-byte lines (Sattolo's algorithm). Each line holds the offset of the next. */
static void make_chain(uint64_t size)
{
	uint32_t lines = (uint32_t)(size / 64);
	uint32_t *order = malloc(sizeof(uint32_t) * lines);
	uint64_t state = 0x9e3779b97f4a7c15ULL;
	uint32_t index;

	for (index = 0; index < lines; index++)
		order[index] = index;
	for (index = lines - 1; index > 0; index--)
	{
		uint32_t other, swap;

		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		other = (uint32_t)(state % index);
		swap = order[index];
		order[index] = order[other];
		order[other] = swap;
	}
	for (index = 0; index < lines; index++)
		*(uint32_t *)(data + (uint64_t)order[index] * 64) = order[(index + 1) % lines] * 64;
	free(order);
}

static void watch_handler(int signal_number, siginfo_t *info, void *context)
{
	uintptr_t page = (uintptr_t)info->si_addr & ~(uintptr_t)16383;

	(void)signal_number;
	(void)context;
	mprotect((void *)page, 16384, PROT_READ | PROT_WRITE);
}

static void test_native_watch(void)
{
	uint64_t size = 64ULL << 20;
	uint64_t pages = size / 16384;
	uint8_t *memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	struct sigaction action;
	uint64_t start, end, index;
	double base_ns, fault_ns;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = watch_handler;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGBUS, &action, NULL);
	sigaction(SIGSEGV, &action, NULL);
	memset(memory, 1, size);

	start = mach_absolute_time();
	for (index = 0; index < pages; index++)
		((volatile uint8_t *)memory)[index * 16384] = 0;
	end = mach_absolute_time();
	base_ns = elapsed_ns(start, end);

	mprotect(memory, size, PROT_READ);
	start = mach_absolute_time();
	for (index = 0; index < pages; index++)
		((volatile uint8_t *)memory)[index * 16384] = 0;
	end = mach_absolute_time();
	fault_ns = elapsed_ns(start, end);
	printf("native watch (SIGBUS + mprotect, 16 KB page): %.0f ns per fault (%llu faults)\n",
		(fault_ns - base_ns) / pages, pages);
	munmap(memory, size);
}

int main(void)
{
	static const uint64_t sizes[] = { 1ULL << 20, 4ULL << 20, 16ULL << 20, 64ULL << 20, 256ULL << 20 };
	uint64_t loads = 4000000;
	uint64_t mmfr0, result, faults;
	unsigned size_index;
	int run;

	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	mach_timebase_info(&timebase);

	low = mmap(NULL, LOW_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	data = mmap(NULL, DATA_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	memcpy(low, guest_code, (const uint8_t *)guest_code_end - (const uint8_t *)guest_code);
	memcpy(low + VECTORS_OFFSET, guest_vectors, 0x800);

	CHECK(hv_vm_create(NULL));
	CHECK(hv_vm_map(low, LOW_IPA, LOW_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
	CHECK(hv_vm_map(data, DATA_IPA, DATA_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE));
	CHECK(hv_vcpu_create(&cpu, &exit_info, NULL));

	CHECK(hv_vcpu_get_sys_reg(cpu, HV_SYS_REG_ID_AA64MMFR0_EL1, &mmfr0));
	printf("ID_AA64MMFR0_EL1 %016llx: PARange %llu, TGran4 %llx, TGran16 %llx, TGran64 %llx (0 or 1: supported, f: not)\n",
		mmfr0, mmfr0 & 0xf, (mmfr0 >> 28) & 0xf, (mmfr0 >> 20) & 0xf, (mmfr0 >> 24) & 0xf);

	CHECK(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_MAIR_EL1, 0xff)); /* attribute 0: normal, write-back */
	/* T0SZ 32 (4 GB), 4 KB granule, inner shareable write-back walks, no TTBR1, 36-bit IPS. */
	CHECK(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_TCR_EL1, 32 | (1 << 8) | (1 << 10) | (3 << 12) | (1 << 23) | (1ULL << 32)));
	CHECK(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_TTBR0_EL1, LOW_IPA + TABLES_OFFSET));
	CHECK(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_VBAR_EL1, LOW_IPA + VECTORS_OFFSET));
	CHECK(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_CPACR_EL1, 3 << 20));
	CHECK(hv_vcpu_set_sys_reg(cpu, HV_SYS_REG_SCTLR_EL1, 0x30d00800 | (1 << 12) | (1 << 2) | 1)); /* MMU, caches */

	printf("\nmemory latency, ns per dependent load (best of 3, %llu loads)\n", loads);
	printf("%-12s %12s %12s %12s\n", "working set", "native 16K", "guest 4K", "guest 2M");
	for (size_index = 0; size_index < sizeof(sizes) / sizeof(sizes[0]); size_index++)
	{
		double best[3] = { 1e30, 1e30, 1e30 };
		uint64_t native_end = 0;

		make_chain(sizes[size_index]);
		for (run = 0; run < 3; run++)
		{
			uint64_t start = mach_absolute_time();
			double ns;

			native_end = native_chase(data, 0, loads);
			ns = elapsed_ns(start, mach_absolute_time());
			if (ns < best[0])
				best[0] = ns;
			map_guest(1, 0);
			ns = run_guest(TEST_CHASE, DATA_IPA, 0, loads, &result);
			if (result != native_end)
				printf("guest chain differs: %llx %llx\n", result, native_end);
			if (ns < best[1])
				best[1] = ns;
			map_guest(0, 0);
			ns = run_guest(TEST_CHASE, DATA_IPA, 0, loads, NULL);
			if (ns < best[2])
				best[2] = ns;
		}
		printf("%9llu MB %12.1f %12.1f %12.1f\n", sizes[size_index] >> 20,
			best[0] / loads, best[1] / loads, best[2] / loads);
	}

	{
		uint64_t count = 500000000;
		double native_best = 1e30, guest_best = 1e30;

		for (run = 0; run < 3; run++)
		{
			uint64_t start = mach_absolute_time();
			uint64_t native_result = native_compute(count);
			double ns = elapsed_ns(start, mach_absolute_time());

			if (ns < native_best)
				native_best = ns;
			ns = run_guest(TEST_COMPUTE, 0, 0, count, &result);
			if (result != native_result)
				printf("guest compute differs\n");
			if (ns < guest_best)
				guest_best = ns;
		}
		printf("\ndependent multiply chain, ns per iteration: native %.3f, guest %.3f (%+.1f%%)\n",
			native_best / count, guest_best / count, (guest_best / native_best - 1) * 100);
	}

	{
		uint64_t pages = (64ULL << 20) / 4096;
		double base_ns, fault_ns;

		map_guest(1, 0);
		base_ns = run_guest(TEST_TOUCH, DATA_IPA, 0, pages, &faults);
		map_guest(1, 64ULL << 20);
		fault_ns = run_guest(TEST_TOUCH, DATA_IPA, 0, pages, &faults);
		printf("\nguest watch (EL1 handler, 4 KB page, no exit): %.0f ns per fault (%llu faults of %llu writes)\n",
			(fault_ns - base_ns) / pages, faults, pages);
		test_native_watch();
	}
	return 0;
}
