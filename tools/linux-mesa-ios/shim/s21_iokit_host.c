/* In-memory model of the SweetMali user client for Linux host tests. */
#include "s21_iokit.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define HOST_MAX_BOS 512              /* V21: match SM_MAX_BO */

struct host_bo {
   int used;
   int own_cpu;      /* create allocates; import_user borrows caller memory */
   uint64_t size;
   uint64_t va;
   void *cpu;
};

static struct {
   struct host_bo bo[HOST_MAX_BOS];
   unsigned opens;
   unsigned submits;
   unsigned pairs;
   unsigned flushes;
   uint32_t last_slot;
   uint64_t last_jc;
   uint64_t second_jc;
   size_t last_struct_len;
   uint32_t last_nread;
   uint32_t last_map_va;
   unsigned fail_map;
   unsigned fail_range;
   unsigned logs;
} H;

void
s21_host_reset(void)
{
   for (unsigned i = 0; i < HOST_MAX_BOS; i++) {
      if (H.bo[i].own_cpu)
         free(H.bo[i].cpu);
      memset(&H.bo[i], 0, sizeof(H.bo[i]));
   }
   memset(&H, 0, sizeof(H));
}

unsigned s21_host_opens(void) { return H.opens; }
unsigned s21_host_submits(void) { return H.submits; }
unsigned s21_host_flush_calls(void) { return H.flushes; }
uint32_t s21_host_last_slot(void) { return H.last_slot; }
uint64_t s21_host_last_jc(void) { return H.last_jc; }
size_t s21_host_last_struct_len(void) { return H.last_struct_len; }
uint32_t s21_host_last_nread(void) { return H.last_nread; }
uint32_t s21_host_live_bos(void)
{
   uint32_t n = 0;
   for (unsigned i = 0; i < HOST_MAX_BOS; i++)
      n += H.bo[i].used != 0;
   return n;
}
unsigned s21_host_pair_calls(void) { return H.pairs; }
uint64_t s21_host_last_second_jc(void) { return H.second_jc; }
uint32_t s21_host_last_map_va(void) { return H.last_map_va; }
void s21_host_fail_map(unsigned count) { H.fail_map = count; }
void s21_host_fail_submit_range(unsigned count) { H.fail_range = count; }
void s21_host_logs_reset(void) { H.logs = 0; }
unsigned s21_host_logs(void) { return H.logs; }

int
s21_iokit_open(void)
{
   H.opens++;
   return 0;
}

void
s21_iokit_close(void)
{
}

int
s21_iokit_struct(uint32_t selector, const void *in, size_t in_len,
                 void *out, size_t *out_len)
{
   (void)in;
   (void)in_len;
   if (selector != S21_GET_INFO || out == NULL || out_len == NULL ||
       *out_len < 6 * sizeof(uint32_t))
      return -EINVAL;
   const uint32_t info[6] = {
      0x92020010, /* GPU_ID: arch 9.2, product major 2 */
      0x00ee0cee, /* SHADER_PRESENT: 14 cores */
      0x00000001, /* TILER_PRESENT */
      0x00000001, /* L2_PRESENT */
      0x000000ff, /* AS_PRESENT */
      0x00000007, /* JS_PRESENT */
   };
   memcpy(out, info, sizeof(info));
   *out_len = sizeof(info);
   return 0;
}

int
s21_iokit_scalar(uint32_t selector, const uint64_t *in, uint32_t in_count,
                 uint64_t *out, uint32_t *out_count)
{
   if (in == NULL)
      return -EINVAL;

   switch (selector) {
   case S21_CREATE_BO: {
      if (in_count < 1 || out == NULL || out_count == NULL || *out_count < 2 ||
          in[0] == 0 || in[0] > (15ULL << 20))
         return -EINVAL;
      unsigned slot = HOST_MAX_BOS;
      for (unsigned i = 0; i < HOST_MAX_BOS; i++) {
         if (!H.bo[i].used) {
            slot = i;
            break;
         }
      }
      if (slot == HOST_MAX_BOS)
         return -ENOMEM;
      void *cpu = calloc(1, (size_t)in[0]);
      if (cpu == NULL)
         return -ENOMEM;
      H.bo[slot].used = 1;
      H.bo[slot].own_cpu = 1;
      H.bo[slot].size = in[0];
      H.bo[slot].cpu = cpu;
      out[0] = slot + 1;
      out[1] = 0x10000000ULL + (uint64_t)slot * 0x100000ULL; /* fake PA */
      *out_count = 2;
      return 0;
   }
   case S21_MAP_BO: {
      if (in_count < 2 || in[0] < 1 || in[0] > HOST_MAX_BOS ||
          !H.bo[in[0] - 1].used || (in[1] & 0xfff))
         return -EINVAL;
      if (H.fail_map) {
         H.fail_map--;
         return -ENOMEM;
      }
      H.bo[in[0] - 1].va = in[1];
      H.last_map_va = (uint32_t)in[1];
      return 0;
   }
   case S21_SUBMIT: {
      if (in_count < 3)
         return -EINVAL;
      H.submits++;
      H.last_slot = (uint32_t)in[0];
      H.last_jc = in[1];
      if (out != NULL && out_count != NULL && *out_count >= 1) {
         out[0] = 0; /* success */
         *out_count = 1;
      }
      return 0;
   }
   case S21_DESTROY_BO: {
      if (in_count < 1 || in[0] < 1 || in[0] > HOST_MAX_BOS ||
          !H.bo[in[0] - 1].used)
         return -EINVAL;
      if (H.bo[in[0] - 1].own_cpu)
         free(H.bo[in[0] - 1].cpu);
      memset(&H.bo[in[0] - 1], 0, sizeof(H.bo[in[0] - 1]));
      return 0;
   }
   case S21_IMPORT_USER: {
      if (in_count < 3 || out == NULL || out_count == NULL || *out_count < 1 ||
          in[0] == 0 || in[1] == 0 || in[1] > (15ULL << 20) ||
          (in[0] & 0xfff) || (in[1] & 0xfff) || (in[2] & 0xfff))
         return -EINVAL;
      unsigned slot = HOST_MAX_BOS;
      for (unsigned i = 0; i < HOST_MAX_BOS; i++) {
         if (!H.bo[i].used) {
            slot = i;
            break;
         }
      }
      if (slot == HOST_MAX_BOS)
         return -ENOMEM;
      H.bo[slot].used = 1;
      H.bo[slot].own_cpu = 0;
      H.bo[slot].size = in[1];
      H.bo[slot].va = in[2];
      H.bo[slot].cpu = (void *)(uintptr_t)in[0];
      out[0] = slot + 1;
      *out_count = 1;
      return 0;
   }
   case S21_FLUSH_RANGE:
   case S21_SUBMIT_RANGE:
      H.flushes++;
      return 0;
   default:
      return -ENOTSUP;
   }
}

int
s21_iokit_mixed(uint32_t selector, const uint64_t *scalar_in,
                uint32_t scalar_count, const void *struct_in,
                size_t struct_len, uint64_t *out, uint32_t *out_count)
{
   if (selector == S21_SUBMIT_RANGE) {
      if (scalar_in == NULL || scalar_count < 5 || struct_len % 16 != 0)
         return -EINVAL;
      if (H.fail_range) {
         H.fail_range--;
         return -ENOTSUP;
      }
      H.pairs++;
      H.submits++;
      H.last_slot = (uint32_t)scalar_in[0];
      H.last_jc = scalar_in[1];
      H.second_jc = scalar_in[4];
      H.last_struct_len = struct_len;
      H.last_nread = (uint32_t)(struct_len / 16);
      H.flushes++;       /* one selective pre/post flush pass for both jobs */
      if (out != NULL && out_count != NULL && *out_count >= 6) {
         out[0] = 0;
         out[5] = 0;
         *out_count = 6;
      }
      return 0;
   }
   if (selector == S21_SUBMIT_LIST) {
      if (scalar_in == NULL || scalar_count < 4 || struct_len % 8 != 0)
         return -EINVAL;
      H.submits++;
      H.last_slot = (uint32_t)scalar_in[0];
      H.last_jc = scalar_in[1];
      H.last_struct_len = struct_len;
      H.last_nread = (uint32_t)scalar_in[3];
      H.flushes++;       /* one selective pre/post flush pass in the kernel */
      if (out != NULL && out_count != NULL && *out_count >= 1) {
         out[0] = 0;
         *out_count = 1;
      }
      return 0;
   }
   if (struct_in != NULL) {
      size_t n = out_count != NULL ? *out_count : 0;
      int e = s21_iokit_struct(selector, struct_in, struct_len, out, &n);
      if (out_count != NULL)
         *out_count = (uint32_t)n;
      return e;
   }
   return s21_iokit_scalar(selector, scalar_in, scalar_count, out, out_count);
}

void *
s21_iokit_map(uint32_t handle, size_t size)
{
   (void)size;
   if (handle < 1 || handle > HOST_MAX_BOS || !H.bo[handle - 1].used)
      return NULL;
   return H.bo[handle - 1].cpu;
}

int
s21_iokit_unmap(uint32_t handle, void *addr)
{
   (void)handle;
   (void)addr;
   return 0;
}

void
s21_iokit_log(const char *line)
{
   (void)line;
   H.logs++;
}
