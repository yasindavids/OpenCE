/*
HVF_LIMITS.C

The limits of Hypervisor.framework on this Mac: the number of vCPUs (one for
each guest thread) and the guest physical address size.

	clang hvf_limits.c -o hvf_limits -framework Hypervisor
	codesign -s - --entitlements hypervisor.entitlements -f hvf_limits
	./hvf_limits

Result on an M1 MacBook Air, macOS 26.6.1: 64 vCPUs, 36-bit IPA.
*/

#include <Hypervisor/Hypervisor.h>
#include <stdio.h>

int main(void)
{
	uint32_t vcpus = 0, ipa_maximum = 0, ipa_default = 0;

	hv_vm_get_max_vcpu_count(&vcpus);
	hv_vm_config_get_max_ipa_size(&ipa_maximum);
	hv_vm_config_get_default_ipa_size(&ipa_default);
	printf("max vcpus %u, ipa default %u bits, max %u bits\n", vcpus, ipa_default, ipa_maximum);
	return 0;
}
