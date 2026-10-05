/*
HVF_EXIT.C

The cost of one guest-to-host call through Hypervisor.framework: a guest
loop of `hvc #0` at guest address 0x80000000, run 200000 times. Each
iteration is one exit to the host and one return to the guest, the path
that a host import of the proposed macOS port takes (port/macos/PROPOSAL.md).

	clang -O2 hvf_exit.c -o hvf_exit -framework Hypervisor
	codesign -s - --entitlements hypervisor.entitlements -f hvf_exit
	./hvf_exit

Result on an M1 MacBook Air, macOS 26.6.1: 710 to 860 ns over three runs.
*/

#include <Hypervisor/Hypervisor.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <sys/mman.h>

#define CHECK(x) do { hv_return_t r = (x); if (r != HV_SUCCESS) { printf(#x " failed %x\n", r); return 1; } } while (0)

int main(void)
{
	size_t size = 0x4000;
	uint32_t *code = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	hv_vcpu_t cpu;
	hv_vcpu_exit_t *exit_info;
	mach_timebase_info_data_t timebase;
	uint64_t start, end;
	int count = 200000;
	int index;

	CHECK(hv_vm_create(NULL));
	code[0] = 0xd4000002; /* hvc #0 */
	code[1] = 0x17ffffff; /* b .-4 */
	CHECK(hv_vm_map(code, 0x80000000ULL, size, HV_MEMORY_READ | HV_MEMORY_EXEC));
	CHECK(hv_vcpu_create(&cpu, &exit_info, NULL));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_PC, 0x80000000ULL));
	CHECK(hv_vcpu_set_reg(cpu, HV_REG_CPSR, 0x3c5)); /* EL1h, MMU off: VA == IPA */

	mach_timebase_info(&timebase);
	for (index = 0; index < 1000; index++)
		hv_vcpu_run(cpu);
	start = mach_absolute_time();
	for (index = 0; index < count; index++)
	{
		CHECK(hv_vcpu_run(cpu));
		if (exit_info->reason != HV_EXIT_REASON_EXCEPTION)
		{
			printf("unexpected exit reason %d\n", exit_info->reason);
			return 1;
		}
	}
	end = mach_absolute_time();
	printf("HVF guest->host->guest round trip (hvc exit): %.0f ns\n",
		(double)(end - start) * timebase.numer / timebase.denom / count);
	return 0;
}
