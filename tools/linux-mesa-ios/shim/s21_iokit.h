/* IOKit transport used by the S21 drm shim. Two implementations:
 *  - s21_iokit_darwin.c: real SweetMali user client (iOS)
 *  - s21_iokit_host.c: in-memory model for the Linux host test
 */
#ifndef S21_IOKIT_H
#define S21_IOKIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 0 on success, negative errno on failure. */
int s21_iokit_open(void);
void s21_iokit_close(void);

/* SweetMali selectors (xnu/patches/0112-s21-mali-iokit.sh). */
enum {
   S21_GET_INFO = 0,
   S21_CREATE_BO = 1,
   S21_MAP_BO = 2,
   S21_SUBMIT = 3,
   S21_DESTROY_BO = 4,
   S21_LOG = 5,
   S21_MAP_FB = 6,
   S21_IMPORT_USER = 8,
   S21_KEEP = 9,
   S21_SUBMIT_RANGE = 12,
   S21_FLUSH_RANGE = 13,
};

int s21_iokit_struct(uint32_t selector, const void *in, size_t in_len,
                     void *out, size_t *out_len);
int s21_iokit_scalar(uint32_t selector, const uint64_t *in, uint32_t in_count,
                     uint64_t *out, uint32_t *out_count);

/* Map a kernel BO (handle = selector-1 index + 1) into this task. */
void *s21_iokit_map(uint32_t handle, size_t size);
int s21_iokit_unmap(uint32_t handle, void *addr);

/* Diagnostics: write a line to the kernel log (best effort). */
void s21_iokit_log(const char *line);

#ifdef S21_HOST_TEST
/* Host-model inspection helpers (Linux test only). */
void s21_host_reset(void);
unsigned s21_host_opens(void);
unsigned s21_host_submits(void);
unsigned s21_host_flush_calls(void);
uint32_t s21_host_last_slot(void);
uint64_t s21_host_last_jc(void);
uint32_t s21_host_live_bos(void);
uint32_t s21_host_last_map_va(void);
void s21_host_fail_map(unsigned count);   /* fail the next count MAP_BO calls */
#endif

#ifdef __cplusplus
}
#endif

#endif
