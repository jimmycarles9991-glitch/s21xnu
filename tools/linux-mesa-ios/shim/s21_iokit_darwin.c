/* Real SweetMali IOKit transport for iOS. Not compiled on Linux. */
#include "s21_iokit.h"

#include <errno.h>
#include <string.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>

static io_connect_t conn = IO_OBJECT_NULL;

int
s21_iokit_open(void)
{
   if (conn != IO_OBJECT_NULL)
      return 0;
   io_service_t svc = IOServiceGetMatchingService(
      kIOMainPortDefault, IOServiceMatching("SweetMali"));
   if (svc == IO_OBJECT_NULL)
      return -ENODEV;
   kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
   IOObjectRelease(svc);
   if (kr != KERN_SUCCESS) {
      conn = IO_OBJECT_NULL;
      return -EIO;
   }
   return 0;
}

void
s21_iokit_close(void)
{
   if (conn != IO_OBJECT_NULL) {
      IOServiceClose(conn);
      conn = IO_OBJECT_NULL;
   }
}

int
s21_iokit_struct(uint32_t selector, const void *in, size_t in_len,
                 void *out, size_t *out_len)
{
   if (conn == IO_OBJECT_NULL)
      return -ENODEV;
   kern_return_t kr = IOConnectCallStructMethod(conn, selector, in, in_len,
                                                 out, out_len);
   return kr == KERN_SUCCESS ? 0 : -EIO;
}

int
s21_iokit_scalar(uint32_t selector, const uint64_t *in, uint32_t in_count,
                 uint64_t *out, uint32_t *out_count)
{
   if (conn == IO_OBJECT_NULL)
      return -ENODEV;
   kern_return_t kr = IOConnectCallScalarMethod(conn, selector, in, in_count,
                                                 out, out_count);
   return kr == KERN_SUCCESS ? 0 : -EIO;
}

int
s21_iokit_mixed(uint32_t selector, const uint64_t *scalar_in,
                uint32_t scalar_count, const void *struct_in,
                size_t struct_len, uint64_t *out, uint32_t *out_count)
{
   if (conn == IO_OBJECT_NULL)
      return -ENODEV;
   kern_return_t kr = IOConnectCallMethod(conn, selector, scalar_in,
                                          scalar_count, struct_in, struct_len,
                                          out, out_count, NULL, NULL);
   return kr == KERN_SUCCESS ? 0 : -EIO;
}

void *
s21_iokit_map(uint32_t handle, size_t size)
{
   (void)size;
   mach_vm_address_t addr = 0;
   mach_vm_size_t len = 0;
   /* Use the normal cached mapping. The kernel's SUBMIT path flushes every
    * mapped BO before and after the job (0112), so coherency is explicit and
    * we avoid the stale-line problem seen with write-combined mappings on the
    * Linux port. */
   kern_return_t kr = IOConnectMapMemory64(conn, handle, mach_task_self(),
                                            &addr, &len, kIOMapAnywhere);
   return kr == KERN_SUCCESS ? (void *)(uintptr_t)addr : NULL;
}

int
s21_iokit_unmap(uint32_t handle, void *addr)
{
   kern_return_t kr = IOConnectUnmapMemory64(conn, handle, mach_task_self(),
                                              (mach_vm_address_t)(uintptr_t)addr);
   return kr == KERN_SUCCESS ? 0 : -EIO;
}

void
s21_iokit_log(const char *line)
{
   if (conn == IO_OBJECT_NULL || line == NULL)
      return;
   IOConnectCallMethod(conn, S21_LOG, NULL, 0, line, strlen(line),
                       NULL, NULL, NULL, NULL);
}
