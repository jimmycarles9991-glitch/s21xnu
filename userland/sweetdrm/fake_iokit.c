/* In-memory SweetMali user client, mimicking xnu/patches/0112's selectors. */
#include <string.h>

#include "fake_iokit.h"

struct fake_state fake;

void
fake_reset(void)
{
	memset(&fake, 0, sizeof(fake));
	fake.service_present = 1;
	fake.gpu_id = 0x92020010;
	fake.next_handle = 1;
}

unsigned
fake_count(uint32_t sel)
{
	unsigned n = 0;
	for (unsigned i = 0; i < fake.ncalls; i++) {
		if (fake.calls[i].sel == sel) {
			n++;
		}
	}
	return n;
}

const struct fake_call *
fake_last(uint32_t sel)
{
	for (unsigned i = fake.ncalls; i != 0; i--) {
		if (fake.calls[i - 1].sel == sel) {
			return &fake.calls[i - 1];
		}
	}
	return NULL;
}

static void
record(uint32_t sel, const uint64_t *in, uint32_t nin, const uint64_t *out, uint32_t nout)
{
	if (fake.ncalls >= sizeof(fake.calls) / sizeof(fake.calls[0])) {
		return;
	}
	struct fake_call *c = &fake.calls[fake.ncalls++];
	c->sel = sel;
	c->nin = nin;
	c->nout = nout;
	for (uint32_t i = 0; i < nin && i < 4; i++) {
		c->in[i] = in[i];
	}
	for (uint32_t i = 0; i < nout && i < 4; i++) {
		c->out[i] = out[i];
	}
}

FakeMatching
IOServiceMatching(const char *name)
{
	return strcmp(name, "SweetMali") == 0 ? "SweetMali" : NULL;
}

io_service_t
IOServiceGetMatchingService(mach_port_t port, FakeMatching m)
{
	(void)port;
	return fake.service_present && m != NULL ? 1u : 0u;
}

kern_return_t
IOServiceOpen(io_service_t svc, mach_port_t task, uint32_t type, io_connect_t *c)
{
	(void)task;
	(void)type;
	fake.opens++;
	if (!svc) {
		return kIOReturnNotFound;
	}
	if (fake.open_result) {
		return fake.open_result;
	}
	*c = 7;
	return kIOReturnSuccess;
}

kern_return_t
IOServiceClose(io_connect_t c)
{
	(void)c;
	fake.closes++;
	return kIOReturnSuccess;
}

kern_return_t
IOObjectRelease(io_object_t o)
{
	(void)o;
	return kIOReturnSuccess;
}

kern_return_t
IOConnectCallMethod(io_connect_t c, uint32_t sel,
    const uint64_t *in, uint32_t nin, const void *ins, size_t nins,
    uint64_t *out, uint32_t *nout, void *outs, size_t *nouts)
{
	(void)c;
	(void)ins;
	(void)nins;
	if (fake.fail_sel && (uint32_t)fake.fail_sel == sel) {
		fake.fail_sel = 0;
		return kIOReturnNoMemory;
	}
	uint64_t o[4] = { 0 };
	uint32_t no = 0;

	switch (sel) {
	case 0: { /* GET_INFO: structure out, 6 u32 */
		if (outs == NULL || nouts == NULL || *nouts < 24) {
			return kIOReturnBadArgument;
		}
		uint32_t info[6] = {
			fake.gpu_id, 0xee0cee, 1, 1, 0xff, 7
		};
		memcpy(outs, info, sizeof(info));
		*nouts = sizeof(info);
		break;
	}
	case 1: { /* CREATE_BO */
		if (nin < 1 || nout == NULL || *nout < 2 || fake.nbo >= 512) {
			return kIOReturnBadArgument;
		}
		uint64_t size = (in[0] + 4095) & ~4095ULL;
		if (size == 0) {
			return kIOReturnBadArgument;
		}
		uint32_t h = fake.next_handle++;
		memset(&fake.bo[fake.nbo], 0, sizeof(fake.bo[0]));
		fake.bo[fake.nbo].handle = h;
		fake.bo[fake.nbo].size = size;
		fake.nbo++;
		o[0] = h;
		o[1] = 0x10000000ULL + (uint64_t)h * 0x100000ULL;
		no = 2;
		fake.creates++;
		break;
	}
	case 2: { /* MAP_BO: handle, gpu_va */
		if (nin < 2) {
			return kIOReturnBadArgument;
		}
		int found = 0;
		for (unsigned i = 0; i < fake.nbo; i++) {
			if (fake.bo[i].handle == (uint32_t)in[0] && !fake.bo[i].mapped) {
				fake.bo[i].mapped = 1;
				fake.bo[i].va = in[1];
				found = 1;
				break;
			}
		}
		if (!found) {
			return kIOReturnBadArgument;
		}
		fake.maps++;
		break;
	}
	case 3: { /* SUBMIT: slot, jc, timeout -> status */
		if (nin < 3 || nout == NULL || *nout < 1) {
			return kIOReturnBadArgument;
		}
		fake.last_slot = (uint32_t)in[0];
		fake.last_jc = in[1];
		fake.last_timeout = (uint32_t)in[2];
		o[0] = 0;
		no = 1;
		fake.submits++;
		break;
	}
	case 4: { /* DESTROY_BO */
		if (nin < 1) {
			return kIOReturnBadArgument;
		}
		for (unsigned i = 0; i < fake.nbo; i++) {
			if (fake.bo[i].handle == (uint32_t)in[0]) {
				memmove(&fake.bo[i], &fake.bo[i + 1],
				    (fake.nbo - i - 1) * sizeof(fake.bo[0]));
				fake.nbo--;
				fake.destroys++;
				break;
			}
		}
		break;
	}
	case 8: { /* IMPORT_USER: uva, len, gpu_va -> handle */
		if (nin < 3 || nout == NULL || *nout < 1 || fake.nbo >= 512) {
			return kIOReturnBadArgument;
		}
		fake.last_import_uva = in[0];
		fake.last_import_len = in[1];
		fake.last_import_va = in[2];
		uint32_t h = fake.next_handle++;
		memset(&fake.bo[fake.nbo], 0, sizeof(fake.bo[0]));
		fake.bo[fake.nbo].handle = h;
		fake.bo[fake.nbo].size = in[1];
		fake.bo[fake.nbo].mapped = 1;
		fake.bo[fake.nbo].va = in[2];
		fake.nbo++;
		o[0] = h;
		no = 1;
		fake.imports++;
		break;
	}
	default:
		return kIOReturnUnsupported;
	}
	if (nout != NULL) {
		for (uint32_t i = 0; i < no && i < *nout; i++) {
			out[i] = o[i];
		}
		*nout = no;
	}
	record(sel, in, nin, o, no);
	return kIOReturnSuccess;
}

kern_return_t
IOConnectMapMemory64(io_connect_t c, uint32_t type, mach_port_t task,
    mach_vm_address_t *addr, mach_vm_size_t *size, IOOptionBits opt)
{
	(void)c;
	(void)task;
	(void)opt;
	for (unsigned i = 0; i < fake.nbo; i++) {
		if (fake.bo[i].handle == type) {
			if (!fake.bo[i].cpu) {
				fake.bo[i].cpu = 0x200000000ULL + (uint64_t)type * 0x100000ULL;
			}
			*addr = fake.bo[i].cpu;
			*size = fake.bo[i].size;
			fake.maps++;
			return kIOReturnSuccess;
		}
	}
	return kIOReturnNotFound;
}

kern_return_t
IOConnectUnmapMemory64(io_connect_t c, uint32_t type, mach_port_t task,
    mach_vm_address_t addr)
{
	(void)c;
	(void)type;
	(void)task;
	for (unsigned i = 0; i < fake.nbo; i++) {
		if (fake.bo[i].cpu == addr) {
			fake.bo[i].cpu = 0;
			fake.unmaps++;
			return kIOReturnSuccess;
		}
	}
	return kIOReturnNotFound;
}
