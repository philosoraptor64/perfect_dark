// PD_VMPROBE=1: address-space probe for the GoldenEye port (G1 on the agent board), run inside an app that is
// already installed on the phone. Tries to reserve GoldenEye's four fixed regions at candidate bases without
// clobbering anything (VM_FLAGS_FIXED, no OVERWRITE), logs which bases fit, frees them and quits.
#include <stdio.h>
#include <stdlib.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/vm_map.h>

void vmprobeRunIfRequested(void)
{
	const char *v = getenv("PD_VMPROBE");
	if (!v || !v[0] || v[0] == '0') {
		return;
	}

	static const struct { unsigned long long off, size; } regions[] = {
		{ 0x10000000ULL, 64ULL << 20 },
		{ 0x70000000ULL, 8ULL << 20 },
		{ 0x80000000ULL, 8ULL << 20 },
		{ 0xA0000000ULL, 144ULL << 20 },
	};
	char path[1024];
	snprintf(path, sizeof(path), "%s/Documents/vmprobe.txt", getenv("HOME") ? getenv("HOME") : ".");
	FILE *f = fopen(path, "w");
	if (!f) f = stderr;

	// the process's own mappings, for context
	vm_address_t addr = 0;
	for (int n = 0; n < 4000; n++) {
		vm_size_t size = 0;
		vm_region_basic_info_data_64_t info;
		mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
		mach_port_t obj;
		if (vm_region_64(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &obj) != KERN_SUCCESS) {
			break;
		}
		fprintf(f, "map %016llx-%016llx\n", (unsigned long long)addr, (unsigned long long)(addr + size));
		addr += size;
	}

	int ok = 0;
	static unsigned long long bases[80];
	int nb = 0;
	for (unsigned long long b = 0x100000000ULL; b <= 0x3F00000000ULL && nb < 79; b += 0x100000000ULL) bases[nb++] = b;
	bases[nb++] = 0x100000000000ULL; // the macOS/Simulator default (known good on the Mac)
	for (int bi = 0; bi < nb; bi++) {
		unsigned long long base = bases[bi];
		kern_return_t lastkr = KERN_SUCCESS;
		int failed = -1;
		vm_address_t got[4] = {0};
		int fits = 1;
		for (int i = 0; i < 4; i++) {
			vm_address_t a = (vm_address_t)(base + regions[i].off);
			kern_return_t kr = vm_map(mach_task_self(), &a, (vm_size_t)regions[i].size, 0, VM_FLAGS_FIXED, MACH_PORT_NULL, 0,
				FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_DEFAULT);
			if (kr != KERN_SUCCESS) {
				fits = 0;
				lastkr = kr;
				failed = i;
				break;
			}
			got[i] = a;
		}
		for (int i = 0; i < 4; i++) {
			if (got[i]) vm_deallocate(mach_task_self(), got[i], (vm_size_t)regions[i].size);
		}
		fprintf(f, "base %016llx %s (region %d kr %d)\n", base, fits ? "FITS" : "no", failed, lastkr);
		ok += fits;
	}
	fprintf(f, "done: %d bases fit\n", ok);
	if (f != stderr) fclose(f);
	exit(0);
}
#else
void vmprobeRunIfRequested(void) {}
#endif
