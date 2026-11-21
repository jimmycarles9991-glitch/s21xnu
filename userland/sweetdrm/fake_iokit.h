/* Minimal in-memory SweetMali user client for the Linux host test. */
#ifndef S21_FAKE_IOKIT_H
#define S21_FAKE_IOKIT_H
#include <stddef.h>
#include <stdint.h>

typedef int kern_return_t;
typedef uint32_t mach_port_t, io_object_t, io_service_t, io_connect_t, IOOptionBits;
typedef uint64_t mach_vm_address_t, mach_vm_size_t;
typedef const char *FakeMatching;

#define kIOReturnSuccess       0
#define kIOReturnBadArgument   ((kern_return_t)0xe00002c2)
#define kIOReturnUnsupported   ((kern_return_t)0xe00002c7)
#define kIOReturnNoMemory      ((kern_return_t)0xe00002bd)
#define kIOReturnNoResources   ((kern_return_t)0xe00002be)
#define kIOReturnNotFound      ((kern_return_t)0xe00002f0)
#define kIOReturnBusy          ((kern_return_t)0xe00002d5)
#define kIOReturnTimeout       ((kern_return_t)0xe00002d6)
#define kIOMainPortDefault     0
#define kIOMapAnywhere         1u
#define kIOMapWriteCombineCache 0x400u
#define mach_task_self()       ((mach_port_t)1)

FakeMatching IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t port, FakeMatching m);
kern_return_t IOServiceOpen(io_service_t svc, mach_port_t task, uint32_t type, io_connect_t *c);
kern_return_t IOServiceClose(io_connect_t c);
kern_return_t IOObjectRelease(io_object_t o);
kern_return_t IOConnectCallMethod(io_connect_t c, uint32_t sel,
    const uint64_t *in, uint32_t nin, const void *ins, size_t nins,
    uint64_t *out, uint32_t *nout, void *outs, size_t *nouts);
kern_return_t IOConnectMapMemory64(io_connect_t c, uint32_t type,
    mach_port_t task, mach_vm_address_t *addr, mach_vm_size_t *size, IOOptionBits opt);
kern_return_t IOConnectUnmapMemory64(io_connect_t c, uint32_t type,
    mach_port_t task, mach_vm_address_t addr);

struct fake_call {
	uint32_t sel, nin, nout;
	uint64_t in[4], out[4];
};

struct fake_state {
	int service_present;
	uint32_t gpu_id;
	int open_result;
	int fail_sel;                 /* selector to fail once (0 = none) */
	unsigned opens, closes;
	unsigned creates, maps, submits, destroys, imports, unmaps;
	uint32_t next_handle;
	struct {
		uint32_t handle;
		uint64_t size, va, cpu;
		int mapped;
	} bo[512];
	unsigned nbo;
	struct fake_call calls[4096];
	unsigned ncalls;
	uint32_t last_slot;
	uint64_t last_jc;
	uint32_t last_timeout;
	uint64_t last_import_uva, last_import_len, last_import_va;
};

extern struct fake_state fake;
void fake_reset(void);
unsigned fake_count(uint32_t sel);
const struct fake_call *fake_last(uint32_t sel);
#endif
