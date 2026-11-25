/*
 * Fresh libdrm/Panfrost emulation over the S21 SweetMali user client.
 *
 * This file is new code for the Linux-Mesa port. It speaks the selectors in
 * xnu/patches/0112-s21-mali-iokit.sh and the Mesa 26.2.4 panfrost ioctl structs
 * from include/drm-uapi/panfrost_drm.h. It does not import anything from the
 * deleted Mesa tree.
 *
 * Coherency: selector 3 SUBMIT flushes every mapped BO. V22 uses selector 11
 * SUBMIT_LIST instead: it flushes only the large BOs named in each submit
 * (small descriptors are always flushed by the kernel), before and after the
 * job. We therefore still use cached CPU mappings (s21_iokit_map) and never
 * assume the Mali-G78 is coherent, without flushing hundreds of unrelated BOs.
 */
#include "s21_iokit.h"
#include "s21_drm.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>

#ifdef S21_IOS
#include <sys/sysctl.h>
#endif

#include "xf86drm.h"
#include "drm-uapi/drm.h"
#include "drm-uapi/panfrost_drm.h"

#define S21_CHUNK_MAX   (15ULL << 20)    /* SM_MAX_BO_BYTES in 0112 */
#define S21_TOTAL_MAX   (768ULL << 20)   /* SM_MAX_TOTAL in 0112 (V21: was 512 MiB) */
#define S21_XNU_PAGE    (16ULL << 10)    /* XNU page_size used by kSMCreateBO in 0112 */
#define S21_VA_BASE     0x30000000ULL
#define S21_VA_LIMIT    0x70000000ULL
#define S21_MAX_CHUNKS  128
#define S21_MAX_BO      512              /* V21: match the kernel's larger fBo table */
#define S21_PRIME_BASE  0x50000000        /* synthetic dma-buf fds; never passed to dup()/close() */
#define S21_SMALL_PAGES 16                /* kernel always flushes BOs this small */
#define S21_RANGE_SMALL 1U                /* kSMSubmitRange: also flush BOs <= 16 pages */
#define S21_RANGE_BOTH  ((1ULL << 62) | (1ULL << 63)) /* before and after both jobs */

/* Measured on this phone with the Linux Panfrost port; see ROADMAP-linux-mesa-port.md. */
#define P_GPU_ID        0x92020010ULL
#define P_SHADER        0x00ee0ceeULL
#define P_TILER         0x1ULL
#define P_L2            0x1ULL
#define P_AS            0xffULL
#define P_JS            0x7ULL
#define P_L2_FEATURES   0x07120206ULL
#define P_MEM_FEATURES  0x301ULL
#define P_MMU_FEATURES  0x2830ULL
#define P_THREAD_FEAT   0x48000ULL
#define P_MAX_THREADS   0x400ULL
#define P_WORKGROUP     0x200ULL
#define P_BARRIER       0x200ULL
#define P_TILER_FEAT    0x809ULL
#define P_TEX0          0xf7fe007eULL
#define P_TEX1          0xc3fff7ffULL
#define P_TEX2          0xbfe1ff9fULL
#define P_TEX3          0x000010c6ULL

struct s21_bo {
   bool used;
   uint32_t k[S21_MAX_CHUNKS];
   uint32_t nk;
   uint64_t va, size, cpu;
   bool heap;
   uint64_t seq;
};

struct s21_sync {
   bool used, signaled;
   uint64_t point;
};

/* V30: the V29 log path issued one IOKit call for every hot DRM line. The
 * observed boot had 96,104 per-BO lines alone, so logging is now opt-in and
 * the important totals/timings are reported by a few aggregate lines. */
struct s21_drm_stats {
   uint64_t submits, pairs, list, full, drains, deferred, errors, hot_skipped;
   uint64_t timing_n;
   uint64_t pre_ns, pre_max, job_ns, job_max;
   uint64_t post_ns, post_max, lock_ns, lock_max;
   bool first_pair, first_list;
};

/* Mesa's JM backend issues the tiler and fragment jobs of one batch back to
 * back. Keep only that pair here until kSMSubmitRange can run both chains under
 * one sm_lock section. Nothing is queued across a syncobj observation. */
struct s21_pending_submit {
   bool used;
   uint32_t slot;
   uint32_t ctx;
   uint64_t jc;
   unsigned nva;                          /* unique large BO VAs; the tail repeats them for write-after */
   uint64_t va[2 * (S21_MAX_BO / 2)];
};

struct va_seg {
   uint64_t lo, hi;
   bool used;
};

static struct {
   pthread_mutex_t lock;
   bool up;
   struct s21_bo bo[S21_MAX_BO];
   struct s21_sync sync[S21_MAX_BO];
   struct va_seg *va;
   unsigned nva, va_cap;
   uint64_t total;
   uint64_t seq;
   int sync_fd[S21_MAX_BO];   /* fd -> syncobj handle + 1, 0 = unused */
   int prime_fd[S21_MAX_BO];  /* BO handle - 1 -> synthetic dma-buf fd, 0 = unused */
   bool no_pair;              /* S21_NO_PAIR=1 keeps the old two-submit path */
   bool verbose;              /* drmdiag=1: V29-style hot logs plus selector-12 timings */
   bool stats_logged;
   struct s21_pending_submit pending;
   struct s21_drm_stats stats;
} C = {
   .lock = PTHREAD_MUTEX_INITIALIZER,
   .sync_fd = { 0 },
   .prime_fd = { 0 },
};

static void
log_bo_label(uint32_t handle, const char *label)
{
   char line[192];
   char buf[128];
   size_t n = 0;

   if (!C.verbose)
      return;
   if (label != NULL) {
      while (n + 1 < sizeof(buf) && label[n] != '\0') {
         unsigned char c = (unsigned char)label[n];
         buf[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
      }
   }
   buf[n] = '\0';
   snprintf(line, sizeof(line), "S21DRM: label h=%u label=%s", handle, buf);
   s21_iokit_log(line);
}

static unsigned
bootarg_uint(const char *args, const char *key)
{
   size_t kl;
   const char *p;

   if (args == NULL || key == NULL)
      return 0;
   kl = strlen(key);
   for (p = args; *p != '\0';) {
      const char *tok;
      size_t len;
      while (*p == ' ')
         p++;
      tok = p;
      while (*p != '\0' && *p != ' ')
         p++;
      len = (size_t)(p - tok);
      if (len > kl + 1 && strncmp(tok, key, kl) == 0 && tok[kl] == '=')
         return (unsigned)strtoul(tok + kl + 1, NULL, 10);
   }
   return 0;
}

/* Called with C.lock held. Boot args are the reliable source on iOS; the
 * environment variables also let the host test and local runs override them. */
static void
read_config_locked(void)
{
   char args[2048];
   const char *env;

   args[0] = '\0';
#ifdef S21_IOS
   {
      size_t n = sizeof(args) - 1;
      if (sysctlbyname("kern.bootargs", args, &n, NULL, 0) == 0)
         args[n < sizeof(args) - 1 ? n : sizeof(args) - 1] = '\0';
   }
#endif
   env = getenv("S21_DRM_DIAG");
   C.verbose = bootarg_uint(args, "drmdiag") != 0 ||
               (env != NULL && env[0] != '\0' && env[0] != '0');
   env = getenv("S21_NO_PAIR");
   C.no_pair = bootarg_uint(args, "S21_NO_PAIR") != 0 ||
               (env != NULL && env[0] == '1');
}

enum {
   S21_NOTE_LIST = 0,
   S21_NOTE_FULL,
   S21_NOTE_PAIR,
};

static void
stats_add_timing(uint64_t *sum, uint64_t *max, uint64_t v)
{
   *sum += v;
   if (v > *max)
      *max = v;
}

/* Record one IOKit submit and decide which low-volume log lines are needed.
 * A quiet boot gets one successful pair/list line plus periodic summaries;
 * errors are always logged. drmdiag=1 restores the V29-style per-submit log. */
static bool
stats_note(uint32_t kind, int e, uint32_t n, const uint64_t *out,
           bool *first, bool *bad)
{
   bool heartbeat = false;
   bool failure = e != 0;

   if (kind == S21_NOTE_PAIR) {
      if (n < 6 || out == NULL || out[0] != 0 || out[5] != 0)
         failure = true;
   } else if (n < 1 || out == NULL || out[0] != 0) {
      failure = true;
   }
   *bad = failure;

   pthread_mutex_lock(&C.lock);
   C.stats.submits++;
   if (kind == S21_NOTE_PAIR) {
      C.stats.pairs++;
      if (!failure) {
         stats_add_timing(&C.stats.pre_ns, &C.stats.pre_max, out[1]);
         stats_add_timing(&C.stats.job_ns, &C.stats.job_max, out[2]);
         stats_add_timing(&C.stats.post_ns, &C.stats.post_max, out[3]);
         stats_add_timing(&C.stats.lock_ns, &C.stats.lock_max, out[4]);
         C.stats.timing_n++;
      }
   } else if (kind == S21_NOTE_LIST) {
      C.stats.list++;
   } else {
      C.stats.full++;
   }
   if (failure)
      C.stats.errors++;

   *first = false;
   if (!C.verbose && !failure) {
      bool *flag = kind == S21_NOTE_PAIR ? &C.stats.first_pair
                                         : &C.stats.first_list;
      if (!*flag) {
         *flag = true;
         *first = true;
      }
   }
   if (!C.verbose && C.stats.submits >= 512 && C.stats.submits % 512 == 0)
      heartbeat = true;
   pthread_mutex_unlock(&C.lock);
   return heartbeat;
}

static void
stats_note_drain(void)
{
   pthread_mutex_lock(&C.lock);
   C.stats.drains++;
   pthread_mutex_unlock(&C.lock);
}

static void
stats_note_deferred(void)
{
   pthread_mutex_lock(&C.lock);
   C.stats.deferred++;
   pthread_mutex_unlock(&C.lock);
}

static void
stats_get(struct s21_drm_stats *dst)
{
   pthread_mutex_lock(&C.lock);
   *dst = C.stats;
   pthread_mutex_unlock(&C.lock);
}

static uint64_t
avg_u64(uint64_t sum, uint64_t n)
{
   return n != 0 ? sum / n : 0;
}

static void
log_stats(const struct s21_drm_stats *s)
{
   char line[256];

   snprintf(line, sizeof(line),
            "S21DRM: stats submits=%llu pairs=%llu list=%llu full=%llu "
            "drains=%llu deferred=%llu errors=%llu hot_skipped=%llu",
            (unsigned long long)s->submits, (unsigned long long)s->pairs,
            (unsigned long long)s->list, (unsigned long long)s->full,
            (unsigned long long)s->drains, (unsigned long long)s->deferred,
            (unsigned long long)s->errors,
            (unsigned long long)s->hot_skipped);
   s21_iokit_log(line);
   if (s->timing_n == 0)
      return;
   snprintf(line, sizeof(line),
            "S21DRM: timing samples=%llu pre avg=%llu max=%llu "
            "job avg=%llu max=%llu post avg=%llu max=%llu "
            "lock avg=%llu max=%llu ns",
            (unsigned long long)s->timing_n,
            (unsigned long long)avg_u64(s->pre_ns, s->timing_n),
            (unsigned long long)s->pre_max,
            (unsigned long long)avg_u64(s->job_ns, s->timing_n),
            (unsigned long long)s->job_max,
            (unsigned long long)avg_u64(s->post_ns, s->timing_n),
            (unsigned long long)s->post_max,
            (unsigned long long)avg_u64(s->lock_ns, s->timing_n),
            (unsigned long long)s->lock_max);
   s21_iokit_log(line);
}

static int
align_bo_size(uint64_t v, uint64_t *out)
{
   if (v == 0 || v > UINT64_MAX - (S21_XNU_PAGE - 1))
      return -EINVAL;
   *out = (v + S21_XNU_PAGE - 1) & ~(S21_XNU_PAGE - 1);
   return 0;
}

/* --- GPU VA allocation -------------------------------------------------- */

/* CoreAnimation fragments this table far past 128 segments: V16 hit 135 in
 * backboardd pid 58 and Mesa then failed every CREATE_BO. Let it grow. */
static int
va_grow_locked(void)
{
   unsigned cap = C.va_cap != 0 ? C.va_cap * 2 : 64;
   struct va_seg *va = realloc(C.va, (size_t)cap * sizeof(*va));
   if (va == NULL)
      return -ENOMEM;
   C.va = va;
   C.va_cap = cap;
   return 0;
}

static int
va_init_locked(void)
{
   if (C.nva == 0) {
      int e = va_grow_locked();
      if (e != 0)
         return e;
      C.va[0] = (struct va_seg){ .lo = S21_VA_BASE, .hi = S21_VA_LIMIT };
      C.nva = 1;
   }
   return 0;
}

static int
va_alloc_locked(uint64_t size, uint64_t *out)
{
   int e = va_init_locked();
   if (e != 0)
      return e;
   /* V21: best fit. First fit split the large early hole on every small BO
    * and left no contiguous space for the next tiler heap, even below the
    * total-size limit. Pick the smallest sufficient hole instead. */
   unsigned best = C.nva;
   for (unsigned i = 0; i < C.nva; i++) {
      struct va_seg *s = &C.va[i];
      if (s->used || s->hi - s->lo < size)
         continue;
      if (best == C.nva || s->hi - s->lo < C.va[best].hi - C.va[best].lo)
         best = i;
   }
   if (best == C.nva)
      return -ENOMEM;
   struct va_seg *s = &C.va[best];
   uint64_t start = s->lo;
   if (s->hi - s->lo == size) {
      s->used = true;
   } else {
      if (C.nva == C.va_cap && (e = va_grow_locked()) != 0)
         return e;
      s = &C.va[best];          /* realloc may have moved the table */
      s->lo += size;
      C.va[C.nva++] = (struct va_seg){ .lo = start, .hi = start + size, .used = true };
   }
   *out = start;
   return 0;
}

static void
va_free_locked(uint64_t start, uint64_t size)
{
   uint64_t end = start + size;
   for (unsigned i = 0; i < C.nva; i++) {
      if (C.va[i].used && C.va[i].lo == start && C.va[i].hi == end) {
         C.va[i].used = false;
         break;
      }
   }
   for (bool again = true; again;) {
      again = false;
      for (unsigned i = 0; i + 1 < C.nva; i++) {
         if (!C.va[i].used && !C.va[i + 1].used && C.va[i].hi == C.va[i + 1].lo) {
            C.va[i].hi = C.va[i + 1].hi;
            memmove(&C.va[i + 1], &C.va[i + 2],
                    (C.nva - i - 2) * sizeof(C.va[0]));
            C.nva--;
            again = true;
            break;
         }
      }
   }
}

/* --- helpers ------------------------------------------------------------ */

static struct s21_bo *
bo_find_locked(uint32_t handle)
{
   if (handle == 0 || handle > S21_MAX_BO || !C.bo[handle - 1].used)
      return NULL;
   return &C.bo[handle - 1];
}

static void
bo_destroy_locked(uint32_t handle)
{
   struct s21_bo *b = bo_find_locked(handle);
   if (b == NULL)
      return;
   if (C.verbose) {
      char line[192];
      snprintf(line, sizeof(line),
               "S21DRM: destroy h=%u va=%llx size=%llx nk=%u heap=%u k0=%u",
               handle, (unsigned long long)b->va, (unsigned long long)b->size,
               b->nk, b->heap, b->nk ? b->k[0] : 0);
      s21_iokit_log(line);
   }
   for (uint32_t i = 0; i < b->nk; i++) {
      uint64_t in[1] = { b->k[i] }, out[1] = { 0 };
      uint32_t n = 1;
      (void)s21_iokit_scalar(S21_DESTROY_BO, in, 1, out, &n);
   }
   if (b->cpu)
      (void)s21_iokit_unmap(b->k[0], (void *)(uintptr_t)b->cpu);
   if (b->size)
      C.total -= b->size;
   if (handle <= S21_MAX_BO)
      C.prime_fd[handle - 1] = 0;
   va_free_locked(b->va, b->size);
   memset(b, 0, sizeof(*b));
}

/* The SweetMali submit selector is synchronous: when it returns, the job has
 * finished. Publish completion on Mesa's out_sync so pipe fences do not need a
 * sync-file export (V18's fake export path destabilised backboardd). Called
 * with C.lock held. */
static void
sync_signal_locked(uint32_t handle)
{
   if (handle == 0 || handle > S21_MAX_BO || !C.sync[handle - 1].used)
      return;
   C.sync[handle - 1].signaled = true;
   C.sync[handle - 1].point++;
}

/* Send one job chain with the V22 selective-flush path, falling back to the
 * older full-flush selector if this kernel does not understand selector 11. */
static int
send_list_submit(uint32_t slot, uint64_t jc, unsigned nread, const uint64_t *list)
{
    uint64_t in[4] = { slot, jc, 5000, nread }, out[1] = { 0 };
    uint32_t n = 1;
    int e = s21_iokit_mixed(S21_SUBMIT_LIST, in, 4, list,
                            (size_t)nread * 2 * sizeof(uint64_t), out, &n);
    bool fallback = false;
    bool first = false, bad = false;
    if (e) {
       fallback = true;
       n = 1;
       out[0] = 0;
       e = s21_iokit_scalar(S21_SUBMIT, in, 3, out, &n);
    }
    {
       bool heartbeat = stats_note(fallback ? S21_NOTE_FULL : S21_NOTE_LIST,
                                   e, n, out, &first, &bad);
       if (C.verbose || first || bad) {
          char line[192];
          snprintf(line, sizeof(line),
                   "S21DRM: submit result slot=%u mode=%s ret=%d out=%llx",
                   slot, fallback ? "full" : "list", e,
                   (unsigned long long)(n ? out[0] : 0));
          s21_iokit_log(line);
       }
       if (heartbeat) {
          struct s21_drm_stats snap;
          stats_get(&snap);
          log_stats(&snap);
       }
    }
    if (e)
       return e;
    if (bad)
       return -EIO;
    return 0;
}

/* kSMSubmitRange already runs a slot-0 fragment chain immediately after a
 * slot-1 chain inside one sm_lock section. Build its {va, flags} range list
 * from the union of the two Mesa submits. If the union is too large for the
 * kernel's 256-entry cap, let the caller replay them as two selector-11 calls
 * instead of silently losing cache flushes. */
static int
send_pair_submit(const struct s21_pending_submit *p, uint64_t frag_jc,
                 unsigned nread, const uint64_t *list, bool *transport_failed)
{
    uint64_t range[2 * (S21_MAX_BO / 2)];
    unsigned nr = 0;
    uint64_t in[5] = { p->slot, p->jc, 5000, S21_RANGE_SMALL, frag_jc };
    uint64_t out[6] = { 0 };
    uint32_t n = 6;
    bool first = false, bad = false;

    *transport_failed = false;
    for (unsigned src = 0; src < 2; src++) {
       unsigned count = src == 0 ? p->nva : nread;
       const uint64_t *va = src == 0 ? p->va : list;
       for (unsigned i = 0; i < count; i++) {
          bool seen = false;
          for (unsigned k = 0; k < nr; k++)
             seen |= range[2 * k] == va[i];
          if (seen)
             continue;
          if (nr == S21_MAX_BO / 2) {
             pthread_mutex_lock(&C.lock);
             C.stats.errors++;
             pthread_mutex_unlock(&C.lock);
             s21_iokit_log("S21DRM: pair flush list overflow; replaying as two submits");
             *transport_failed = true;
             return -E2BIG;
          }
          range[2 * nr] = va[i];
          range[2 * nr + 1] = S21_RANGE_BOTH;
          nr++;
       }
    }

    int e = s21_iokit_mixed(S21_SUBMIT_RANGE, in, 5, range, nr * 2 * sizeof(uint64_t),
                            out, &n);
    {
       bool heartbeat = stats_note(S21_NOTE_PAIR, e, n, out, &first, &bad);
       if (C.verbose || first || bad) {
          char line[256];
          snprintf(line, sizeof(line),
                   "S21DRM: submit result slot=%u+0 mode=pair ret=%d "
                   "out=%llx/%llx ranges=%u pre_ns=%llu job_ns=%llu "
                   "post_ns=%llu lock_ns=%llu",
                   p->slot, e, (unsigned long long)(n ? out[0] : 0),
                   (unsigned long long)(n > 5 ? out[5] : 0), nr,
                   (unsigned long long)(n > 1 ? out[1] : 0),
                   (unsigned long long)(n > 2 ? out[2] : 0),
                   (unsigned long long)(n > 3 ? out[3] : 0),
                   (unsigned long long)(n > 4 ? out[4] : 0));
          s21_iokit_log(line);
       }
       if (heartbeat) {
          struct s21_drm_stats snap;
          stats_get(&snap);
          log_stats(&snap);
       }
    }
    if (e) {
       *transport_failed = true;
       return e;
    }
    /* A nonzero status means the jobs already ran or faulted; never replay. */
    if (bad)
       return -EIO;
    return 0;
}

/* Publish a deferred tiler job as its own submit when Mesa never sends the
 * matching fragment job, or before any ioctl that could observe syncobj state
 * or destroy/reuse the BOs it references. */
static int
drain_pending(void)
{
    for (;;) {
       struct s21_pending_submit p;
       pthread_mutex_lock(&C.lock);
       if (!C.pending.used) {
          pthread_mutex_unlock(&C.lock);
          return 0;
       }
       p = C.pending;
       memset(&C.pending, 0, sizeof(C.pending));
       pthread_mutex_unlock(&C.lock);
       stats_note_drain();
       if (C.verbose) {
          char line[160];
          snprintf(line, sizeof(line), "S21DRM: draining deferred slot=%u jc=%llx",
                   p.slot, (unsigned long long)p.jc);
          s21_iokit_log(line);
       }
       int e = send_list_submit(p.slot, p.jc, p.nva, p.va);
       if (e)
          return e;
    }
}

static int
get_param(const struct drm_panfrost_get_param *p, uint64_t *value)
{
   switch (p->param) {
   case DRM_PANFROST_PARAM_GPU_PROD_ID:   *value = P_GPU_ID >> 16; break;
   case DRM_PANFROST_PARAM_GPU_REVISION:  *value = P_GPU_ID & 0xffff; break;
   case DRM_PANFROST_PARAM_SHADER_PRESENT: *value = P_SHADER; break;
   case DRM_PANFROST_PARAM_TILER_PRESENT: *value = P_TILER; break;
   case DRM_PANFROST_PARAM_L2_PRESENT:    *value = P_L2; break;
   case DRM_PANFROST_PARAM_AS_PRESENT:    *value = P_AS; break;
   case DRM_PANFROST_PARAM_JS_PRESENT:    *value = P_JS; break;
   case DRM_PANFROST_PARAM_L2_FEATURES:   *value = P_L2_FEATURES; break;
   case DRM_PANFROST_PARAM_TILER_FEATURES: *value = P_TILER_FEAT; break;
   case DRM_PANFROST_PARAM_MEM_FEATURES:  *value = P_MEM_FEATURES; break;
   case DRM_PANFROST_PARAM_MMU_FEATURES:  *value = P_MMU_FEATURES; break;
   case DRM_PANFROST_PARAM_THREAD_FEATURES: *value = P_THREAD_FEAT; break;
   case DRM_PANFROST_PARAM_MAX_THREADS:   *value = P_MAX_THREADS; break;
   case DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ: *value = P_WORKGROUP; break;
   case DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ: *value = P_BARRIER; break;
   case DRM_PANFROST_PARAM_TEXTURE_FEATURES0: *value = P_TEX0; break;
   case DRM_PANFROST_PARAM_TEXTURE_FEATURES1: *value = P_TEX1; break;
   case DRM_PANFROST_PARAM_TEXTURE_FEATURES2: *value = P_TEX2; break;
   case DRM_PANFROST_PARAM_TEXTURE_FEATURES3: *value = P_TEX3; break;
   case DRM_PANFROST_PARAM_AFBC_FEATURES: *value = 0; break;
   case DRM_PANFROST_PARAM_ALLOWED_JM_CTX_PRIORITIES:
      *value = 1ULL << PANFROST_JM_CTX_PRIORITY_MEDIUM;
      break;
   case DRM_PANFROST_PARAM_SELECTED_COHERENCY:
      *value = DRM_PANFROST_GPU_COHERENCY_NONE;
      break;
   default:
      return -EINVAL;
   }
   return 0;
}

/* --- public libdrm entry points ---------------------------------------- */

int
s21_drm_open(void)
{
   pthread_mutex_lock(&C.lock);
   if (!C.up) {
      read_config_locked();
      int e = s21_iokit_open();
      if (e) {
         pthread_mutex_unlock(&C.lock);
         return e;
      }
      C.up = true;
      s21_iokit_log(C.no_pair ? "S21DRM: pair batching disabled by S21_NO_PAIR"
                              : "S21DRM: pair batching enabled");
      if (C.verbose)
         s21_iokit_log("S21DRM: drmdiag=1 hot logging enabled");
   }
   pthread_mutex_unlock(&C.lock);
   return S21_DRM_FD;
}

int
drmClose(int fd)
{
   struct s21_drm_stats snap;
   bool logged;

   (void)fd;
   /* Do not leave a tiler job stuck behind a present/teardown boundary. */
   int e = drain_pending();
   stats_get(&snap);
   pthread_mutex_lock(&C.lock);
   logged = C.stats_logged;
   C.stats_logged = true;
   pthread_mutex_unlock(&C.lock);
   if (!logged)
      log_stats(&snap);
   return e;
}

int
drmCommandWriteRead(int fd, unsigned int command, void *data, unsigned int size)
{
   return drmIoctl(fd, _IOWR('d', command, size), data);
}

/* Global -DHAVE_LIBDRM makes a few EGL/DRI helpers compile even though the S21
 * port has no real DRM node. They must link; keeping them as failure stubs is
 * safe because the surfaceless backend never calls them with a valid fd. */
int
drmAvailable(void)
{
   return 0;
}

int
drmOpen(const char *name, const char *busid)
{
   (void)name; (void)busid;
   return -1;
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
   (void)name; (void)busid; (void)type;
   return -1;
}

int
drmOpenControl(int minor)
{
   (void)minor;
   return -1;
}

int
drmOpenRender(int minor)
{
   (void)minor;
   return -1;
}

int
drmGetMagic(int fd, drm_magic_t *magic)
{
   (void)fd; (void)magic;
   return -1;
}

int
drmAuthMagic(int fd, drm_magic_t magic)
{
   (void)fd; (void)magic;
   return -1;
}

int
drmCreateContext(int fd, drm_context_t *handle)
{
   (void)fd; (void)handle;
   return -1;
}

int
drmDestroyContext(int fd, drm_context_t handle)
{
   (void)fd; (void)handle;
   return -1;
}

char *
drmGetBusid(int fd)
{
   (void)fd;
   return NULL;
}

char *
drmGetDeviceNameFromFd(int fd)
{
   (void)fd;
   return NULL;
}

int
drmGetNodeTypeFromFd(int fd)
{
   (void)fd;
   return -1;
}

char *
drmGetPrimaryDeviceNameFromFd(int fd)
{
   (void)fd;
   return NULL;
}

char *
drmGetRenderDeviceNameFromFd(int fd)
{
   (void)fd;
   return NULL;
}

/* Forcing -DHAVE_LIBDRM also compiles Mesa's DRI/pipe-loader DRM callers, but
 * Meson only adds pipe_loader_drm.c when it finds a real libdrm dependency.
 * There is no KMS device on iOS, so these must link but always report failure.
 * The opaque void * arguments keep Mesa's internal structs out of this shim;
 * only pointer values cross the ABI. */
int pipe_loader_drm_probe(void **devs, int ndev);
bool pipe_loader_drm_probe_fd(void **dev, int fd, bool zink);
const void *pipe_loader_drm_get_driconf_by_name(const char *driver_name,
                                               unsigned *count);
int pipe_loader_get_compatible_render_capable_device_fd(int kms_only_fd);
int *pipe_loader_get_compatible_render_capable_device_fds(int kms_only_fd,
                                                          unsigned int *n_devices);

int
pipe_loader_drm_probe(void **devs, int ndev)
{
   (void)devs; (void)ndev;
   return 0;
}

bool
pipe_loader_drm_probe_fd(void **dev, int fd, bool zink)
{
   (void)dev; (void)fd; (void)zink;
   return false;
}

const void *
pipe_loader_drm_get_driconf_by_name(const char *driver_name, unsigned *count)
{
   (void)driver_name;
   if (count != NULL)
      *count = 0;
   return NULL;
}

int
pipe_loader_get_compatible_render_capable_device_fd(int kms_only_fd)
{
   (void)kms_only_fd;
   return -1;
}

int *
pipe_loader_get_compatible_render_capable_device_fds(int kms_only_fd,
                                                     unsigned int *n_devices)
{
   (void)kms_only_fd;
   if (n_devices != NULL)
      *n_devices = 0;
   return NULL;
}

/* Panfrost's kmod backend uses eventfd/timerfd only for optional perf sessions.
 * Those are never enabled here; the declarations let it compile and these
 * stubs let the dylib link. */
#ifdef __APPLE__
int
eventfd(unsigned int initval, int flags)
{
   (void)initval; (void)flags;
   errno = ENOSYS;
   return -1;
}

int
eventfd_read(int fd, eventfd_t *value)
{
   (void)fd; (void)value;
   errno = ENOSYS;
   return -1;
}

int
eventfd_write(int fd, eventfd_t value)
{
   (void)fd; (void)value;
   errno = ENOSYS;
   return -1;
}

int
timerfd_create(int clockid, int flags)
{
   (void)clockid; (void)flags;
   errno = ENOSYS;
   return -1;
}

int
timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                struct itimerspec *old_value)
{
   (void)fd; (void)flags; (void)new_value; (void)old_value;
   errno = ENOSYS;
   return -1;
}

int
timerfd_gettime(int fd, struct itimerspec *curr_value)
{
   (void)fd; (void)curr_value;
   errno = ENOSYS;
   return -1;
}

int
sched_setscheduler(int pid, int policy, const struct sched_param *param)
{
   (void)pid; (void)policy; (void)param;
   errno = ENOSYS;
   return -1;
}
#endif /* __APPLE__ */

/* iOS has no DRM device tree. These stubs only need to satisfy libloader's
 * symbol references; s21_drm_open() is the only device path used. */
int
drmGetDevice(int fd, drmDevicePtr *device)
{
   (void)fd;
   if (device != NULL)
      *device = NULL;
   return -ENODEV;
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
   (void)fd; (void)flags;
   if (device != NULL)
      *device = NULL;
   return -ENODEV;
}

int
drmGetDevices(drmDevicePtr devices[], int max_devices)
{
   (void)devices; (void)max_devices;
   return 0;
}

int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
   (void)flags; (void)devices; (void)max_devices;
   return 0;
}

int
drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device)
{
   (void)dev_id; (void)flags;
   if (device != NULL)
      *device = NULL;
   return -ENODEV;
}

char *
drmGetDeviceNameFromFd2(int fd)
{
   (void)fd;
   return NULL;
}

int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
   /* Device enumeration always reports no devices on iOS. Pointer identity is
    * enough for the only EGL callers, which compare two cached pointers. */
   return a == b ? 1 : 0;
}

void
drmFreeDevice(drmDevicePtr *device)
{
   if (device != NULL)
      *device = NULL;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
   (void)devices; (void)count;
}

/* Copy the large BO VAs that kSMSubmitList must flush before and after one
 * job. The first nread entries are read-before; the duplicate tail is
 * write-after. Small BOs are always flushed by the kernel. */
static int
prepare_submit(const struct drm_panfrost_submit *s, uint32_t *slot,
               uint64_t *list, unsigned *nread)
{
   if (s->bo_handle_count > S21_MAX_BO ||
       (s->bo_handle_count != 0 && s->bo_handles == 0))
      return -EINVAL;
   const uint32_t *handles = (const uint32_t *)(uintptr_t)s->bo_handles;

   *slot = (s->requirements & PANFROST_JD_REQ_FS) ? 0 : 1;
   *nread = 0;
   pthread_mutex_lock(&C.lock);
   uint64_t seq = ++C.seq;
   for (uint32_t i = 0; i < s->bo_handle_count; i++) {
      struct s21_bo *b = bo_find_locked(handles[i]);
      if (b == NULL)
         continue;
      b->seq = seq;
      if (C.verbose) {
         char line[192];
         snprintf(line, sizeof(line), "S21DRM: submit bo h=%u va=%llx size=%llx",
                  handles[i], (unsigned long long)b->va,
                  (unsigned long long)b->size);
         s21_iokit_log(line);
      } else {
         C.stats.hot_skipped++;
      }
      if (b->size <= S21_SMALL_PAGES * 4096 || *nread == S21_MAX_BO / 2)
         continue;
      bool seen = false;
      for (unsigned k = 0; k < *nread; k++)
         seen |= list[k] == b->va;
      if (!seen)
         list[(*nread)++] = b->va;
   }
   pthread_mutex_unlock(&C.lock);
   for (unsigned i = 0; i < *nread; i++)
      list[*nread + i] = list[i];

   if (C.verbose) {
      char line[192];
      snprintf(line, sizeof(line),
               "S21DRM: submit slot=%u jc=%llx req=%x nbo=%u list=%u(nread=%u)",
               *slot, (unsigned long long)s->jc, s->requirements,
               s->bo_handle_count, *nread * 2, *nread);
      s21_iokit_log(line);
   }
   return 0;
}

static int
defer_submit(const struct drm_panfrost_submit *s, uint32_t slot,
             unsigned nread, const uint64_t *list)
{
   /* Only one pair is pending. If another tiler job arrives first, publish the
    * old one as a standalone submit instead of losing or crossing jobs. */
   for (;;) {
      pthread_mutex_lock(&C.lock);
      if (!C.pending.used)
         break;
      struct s21_pending_submit p = C.pending;
      memset(&C.pending, 0, sizeof(C.pending));
      pthread_mutex_unlock(&C.lock);
      int e = send_list_submit(p.slot, p.jc, p.nva, p.va);
      if (e)
         return e;
   }
   C.pending.used = true;
   C.pending.slot = slot;
   C.pending.ctx = s->jm_ctx_handle;
   C.pending.jc = s->jc;
   C.pending.nva = nread;
   memcpy(C.pending.va, list, (size_t)nread * 2 * sizeof(uint64_t));
   pthread_mutex_unlock(&C.lock);
   stats_note_deferred();
   if (C.verbose) {
      char line[160];
      snprintf(line, sizeof(line), "S21DRM: submit deferred slot=%u jc=%llx ctx=%u",
               slot, (unsigned long long)s->jc, s->jm_ctx_handle);
      s21_iokit_log(line);
   }
   return 0;
}

static int
panfrost_submit_ioctl(struct drm_panfrost_submit *s)
{
   uint64_t list[2 * (S21_MAX_BO / 2)];
   unsigned nread = 0;
   uint32_t slot = 1;
   int e = prepare_submit(s, &slot, list, &nread);
   if (e)
      return e;
   bool fs = (s->requirements & PANFROST_JD_REQ_FS) != 0;

   /* Mesa's jm_submit_batch() holds dev->submit_lock across these two ioctls.
    * Only defer the first half when it has no completion to publish and no
    * explicit dependencies; the fragment half owns the out_sync. */
   if (!C.no_pair && slot == 1 && !fs && s->out_sync == 0 && s->in_sync_count == 0)
      return defer_submit(s, slot, nread, list);

   if (!C.no_pair && slot == 0 && fs && s->in_sync_count == 0) {
      struct s21_pending_submit p;
      bool have = false;
      pthread_mutex_lock(&C.lock);
      if (C.pending.used) {
         p = C.pending;
         memset(&C.pending, 0, sizeof(C.pending));
         have = true;
      }
      pthread_mutex_unlock(&C.lock);
      if (have) {
         if (p.ctx == s->jm_ctx_handle) {
            bool transport_failed = false;
            e = send_pair_submit(&p, s->jc, nread, list, &transport_failed);
            if (e == 0) {
               pthread_mutex_lock(&C.lock);
               sync_signal_locked(s->out_sync);
               pthread_mutex_unlock(&C.lock);
               return 0;
            }
            /* Nonzero job status means the GPU already saw the work. Only a
             * rejected selector is safe to replay as two separate submits. */
            if (!transport_failed)
               return e;
         }
         if (p.slot != slot || p.jc != s->jc) {
            e = send_list_submit(p.slot, p.jc, p.nva, p.va);
            if (e)
               return e;
         }
      }
   }

   e = drain_pending();
   if (e)
      return e;
   e = send_list_submit(slot, s->jc, nread, list);
   if (e)
      return e;
   pthread_mutex_lock(&C.lock);
   sync_signal_locked(s->out_sync);
   pthread_mutex_unlock(&C.lock);
   return 0;
}

static int
drmIoctlImpl(int fd, unsigned long request, void *arg)
{
   (void)fd;
   if (arg == NULL)
      return -EINVAL;

   /* A deferred tiler job must be visible before any query, reset, transfer,
    * wait, BO destroy or other state change. SUBMIT itself handles pairing. */
   if (request != DRM_IOCTL_PANFROST_SUBMIT) {
      int e = drain_pending();
      if (e)
         return e;
   }

   if (request == DRM_IOCTL_PANFROST_GET_PARAM) {
      struct drm_panfrost_get_param *p = arg;
      if (p->pad)
         return -EINVAL;
      uint64_t v = 0;
      int e = get_param(p, &v);
      if (e)
         return e;
      p->value = v;
      return 0;
   }

   if (request == DRM_IOCTL_PANFROST_CREATE_BO) {
      struct drm_panfrost_create_bo *c = arg;
      uint64_t size;
      /* kSMCreateBO rounds up to XNU's 16 KiB page and kSMDestroyBO unmaps every
       * page in its VA range. Reserve that whole range here or a neighbouring
       * small BO can inherit PTEs that the kernel later clears. */
      int e = align_bo_size(c->size, &size);
      if (e)
         return e;
      uint32_t nk = (uint32_t)((size + S21_CHUNK_MAX - 1) / S21_CHUNK_MAX);
      if (nk > S21_MAX_CHUNKS)
         return -ENOMEM;

      pthread_mutex_lock(&C.lock);
      /* V21: C.total is shared by all backboardd GL threads. Reading it
       * before taking the lock made concurrent creates reject valid BOs. */
      if (C.total > S21_TOTAL_MAX || size > S21_TOTAL_MAX - C.total) {
         pthread_mutex_unlock(&C.lock);
         return -ENOMEM;
      }
      uint64_t va;
      e = va_alloc_locked(size, &va);
      if (e) {
         pthread_mutex_unlock(&C.lock);
         return e;
      }
      uint32_t slot = S21_MAX_BO;
      for (uint32_t i = 0; i < S21_MAX_BO; i++) {
         if (!C.bo[i].used) {
            slot = i;
            break;
         }
      }
      if (slot == S21_MAX_BO) {
         va_free_locked(va, size);
         pthread_mutex_unlock(&C.lock);
         return -ENOMEM;
      }
      struct s21_bo *b = &C.bo[slot];
      memset(b, 0, sizeof(*b));
      b->used = true;
      b->va = va;
      b->size = size;
      b->heap = (c->flags & PANFROST_BO_HEAP) != 0;

      for (uint32_t i = 0; i < nk; i++) {
         uint64_t chunk = size - (uint64_t)i * S21_CHUNK_MAX;
         if (chunk > S21_CHUNK_MAX)
            chunk = S21_CHUNK_MAX;
         uint64_t in[1] = { chunk }, out[2] = { 0, 0 };
         uint32_t n = 2;
         e = s21_iokit_scalar(S21_CREATE_BO, in, 1, out, &n);
         if (e || out[0] == 0 || n < 1) {
            for (uint32_t j = 0; j < i; j++) {
               uint64_t d[1] = { b->k[j] }, o[1] = { 0 };
               uint32_t dn = 1;
               (void)s21_iokit_scalar(S21_DESTROY_BO, d, 1, o, &dn);
            }
            memset(b, 0, sizeof(*b));
            va_free_locked(va, size);
            pthread_mutex_unlock(&C.lock);
            return e ? e : -EIO;
         }
         b->k[b->nk++] = (uint32_t)out[0];
         uint64_t m[2] = { out[0], va + (uint64_t)i * S21_CHUNK_MAX };
         e = s21_iokit_scalar(S21_MAP_BO, m, 2, NULL, NULL);
         if (e) {
            /* This BO already exists in the kernel. Leaving it in our slot
             * table leaks a handle (V19: 588 CREATE_BO failures and a
             * backboardd respring after the GPU page-table pool filled). */
            {
               char line[160];
               snprintf(line, sizeof(line),
                        "S21DRM: map failed h=%u size=%llx chunk=%u of %u err=%d; rolling back",
                        slot + 1, (unsigned long long)size, i + 1, nk, -e);
               s21_iokit_log(line);
            }
            for (uint32_t j = 0; j < b->nk; j++) {
               uint64_t d[1] = { b->k[j] }, o[1] = { 0 };
               uint32_t dn = 1;
               (void)s21_iokit_scalar(S21_DESTROY_BO, d, 1, o, &dn);
            }
            memset(b, 0, sizeof(*b));
            va_free_locked(va, size);
            pthread_mutex_unlock(&C.lock);
            return e;
         }
      }
      C.total += size;
      c->handle = slot + 1;
      c->size = size;
      c->offset = va;   /* GPU VA, also returned by GET_BO_OFFSET */
      if (C.verbose) {
         char line[160];
         snprintf(line, sizeof(line),
                  "S21DRM: create h=%u va=%llx size=%llx flags=%x heap=%u k0=%u",
                  c->handle, (unsigned long long)va, (unsigned long long)size,
                  (unsigned)c->flags, b->heap ? 1U : 0U, b->nk ? b->k[0] : 0);
         s21_iokit_log(line);
      }
      pthread_mutex_unlock(&C.lock);
      return 0;
   }

   if (request == DRM_IOCTL_PANFROST_GET_BO_OFFSET) {
      struct drm_panfrost_get_bo_offset *g = arg;
      pthread_mutex_lock(&C.lock);
      struct s21_bo *b = bo_find_locked(g->handle);
      int e = b ? 0 : -EINVAL;
      if (b)
         g->offset = b->va;
      pthread_mutex_unlock(&C.lock);
      return e;
   }

   if (request == DRM_IOCTL_PANFROST_MMAP_BO) {
      struct drm_panfrost_mmap_bo *m = arg;
      pthread_mutex_lock(&C.lock);
      struct s21_bo *b = bo_find_locked(m->handle);
      int e = b ? 0 : -EINVAL;
      if (b)
         m->offset = ((uint64_t)m->handle << 32);  /* opaque for s21_drm_bo_map */
      pthread_mutex_unlock(&C.lock);
      return e;
   }

   if (request == DRM_IOCTL_PANFROST_SUBMIT)
      return panfrost_submit_ioctl(arg);

   if (request == DRM_IOCTL_PANFROST_SET_LABEL_BO) {
      struct drm_panfrost_set_label_bo *l = arg;
      log_bo_label(l->handle, (const char *)(uintptr_t)l->label);
      return 0;
   }

   if (request == DRM_IOCTL_PANFROST_WAIT_BO ||
       request == DRM_IOCTL_PANFROST_SYNC_BO ||
       request == DRM_IOCTL_PANFROST_MADVISE ||
       request == DRM_IOCTL_PANFROST_JM_CTX_CREATE ||
       request == DRM_IOCTL_PANFROST_JM_CTX_DESTROY)
      return 0;   /* SUBMIT is synchronous, all other selectors are optional */

   if (request == DRM_IOCTL_SYNCOBJ_CREATE) {
      struct drm_syncobj_create *c = arg;
      pthread_mutex_lock(&C.lock);
      uint32_t h = 0;
      for (uint32_t i = 0; i < S21_MAX_BO; i++) {
         if (!C.sync[i].used) {
            memset(&C.sync[i], 0, sizeof(C.sync[i]));
            C.sync[i].used = true;
            C.sync[i].signaled = (c->flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0;
            h = i + 1;
            break;
         }
      }
      pthread_mutex_unlock(&C.lock);
      if (h == 0)
         return -ENOMEM;
      c->handle = h;
      return 0;
   }

   if (request == DRM_IOCTL_SYNCOBJ_DESTROY) {
      struct drm_syncobj_destroy *d = arg;
      pthread_mutex_lock(&C.lock);
      if (d->handle == 0 || d->handle > S21_MAX_BO || !C.sync[d->handle - 1].used) {
         pthread_mutex_unlock(&C.lock);
         return -EINVAL;
      }
      memset(&C.sync[d->handle - 1], 0, sizeof(C.sync[0]));
      pthread_mutex_unlock(&C.lock);
      return 0;
   }

   if (request == DRM_IOCTL_SYNCOBJ_WAIT ||
       request == DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT) {
      const uint32_t *h = NULL;
      const uint64_t *points = NULL;
      unsigned n = 0;
      uint32_t *first_out = NULL;
      if (request == DRM_IOCTL_SYNCOBJ_WAIT) {
         struct drm_syncobj_wait *w = arg;
         h = (const uint32_t *)(uintptr_t)w->handles;
         n = w->count_handles;
         first_out = &w->first_signaled;
      } else {
         struct drm_syncobj_timeline_wait *w = arg;
         h = (const uint32_t *)(uintptr_t)w->handles;
         points = (const uint64_t *)(uintptr_t)w->points;
         n = w->count_handles;
         first_out = &w->first_signaled;
      }
      if (n == 0 || n > S21_MAX_BO || h == NULL || (points == NULL && n > 0 && request == DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT))
         return -EINVAL;

      pthread_mutex_lock(&C.lock);
      unsigned first = n;
      for (unsigned i = 0; i < n; i++) {
         bool signaled = h[i] != 0 && h[i] <= S21_MAX_BO &&
                         C.sync[h[i] - 1].used && C.sync[h[i] - 1].signaled &&
                         (points == NULL || C.sync[h[i] - 1].point >= points[i]);
         if (signaled && first == n)
            first = i;
      }
      pthread_mutex_unlock(&C.lock);

      if (first == n)
         return -ETIMEDOUT;
      if (first_out != NULL)
         *first_out = first;
      return 0;
   }

   if (request == DRM_IOCTL_SYNCOBJ_RESET || request == DRM_IOCTL_SYNCOBJ_SIGNAL) {
      const struct drm_syncobj_array *a = arg;
      const uint32_t *handles = (const uint32_t *)(uintptr_t)a->handles;
      if (a->count_handles > S21_MAX_BO || (a->count_handles > 0 && handles == NULL))
         return -EINVAL;
      pthread_mutex_lock(&C.lock);
      for (uint32_t i = 0; i < a->count_handles; i++) {
         uint32_t h = handles[i];
         if (h && h <= S21_MAX_BO && C.sync[h - 1].used) {
            C.sync[h - 1].signaled = request == DRM_IOCTL_SYNCOBJ_SIGNAL;
            if (request == DRM_IOCTL_SYNCOBJ_SIGNAL)
               C.sync[h - 1].point++;
         }
      }
      pthread_mutex_unlock(&C.lock);
      return 0;
   }

   if (request == DRM_IOCTL_SYNCOBJ_TRANSFER) {
      struct drm_syncobj_transfer *t = arg;
      pthread_mutex_lock(&C.lock);
      int e = -EINVAL;
      if (t->src_handle && t->src_handle <= S21_MAX_BO &&
          t->dst_handle && t->dst_handle <= S21_MAX_BO &&
          C.sync[t->src_handle - 1].used && C.sync[t->dst_handle - 1].used) {
         C.sync[t->dst_handle - 1].signaled = C.sync[t->src_handle - 1].signaled;
         C.sync[t->dst_handle - 1].point = C.sync[t->src_handle - 1].point;
         e = 0;
      }
      pthread_mutex_unlock(&C.lock);
      return e;
   }

   return -ENOTSUP;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
   int e = drmIoctlImpl(fd, request, arg);

   /* Mesa logs errno, not ours, on ioctl failure. Without this the V18 log
    * showed stale 45/34 values and hid the real CREATE_BO error. */
   if (e < 0)
      errno = -e;
   return e;
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
   (void)fd;
   int e = drain_pending();
   if (e)
      return e;
   pthread_mutex_lock(&C.lock);
   struct s21_bo *b = bo_find_locked(handle);
   if (b == NULL) {
      pthread_mutex_unlock(&C.lock);
      return -EINVAL;
   }
   bo_destroy_locked(handle);
   pthread_mutex_unlock(&C.lock);
   return 0;
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
   (void)fd;
   (void)capability;
   if (value)
      *value = 0;
   return -EINVAL;
}

drmVersionPtr
drmGetVersion(int fd)
{
   (void)fd;
   drmVersionPtr v = calloc(1, sizeof(*v));
   if (v == NULL)
      return NULL;
   v->version_major = 1;
   v->version_minor = 2;
   const char name[] = "panfrost";
   v->name = malloc(sizeof(name));
   if (v->name != NULL) {
      memcpy(v->name, name, sizeof(name));
      v->name_len = (int)(sizeof(name) - 1);
   }
   return v;
}

void
drmFreeVersion(drmVersionPtr v)
{
   if (v == NULL)
      return;
   free(v->name);
   free(v);
}

/* --- BO CPU map hook (Mesa 26.2.4 Darwin patch will call these) --------- */

void *
s21_drm_bo_map(int fd, size_t size, int prot, int flags, uint64_t offset)
{
   (void)fd;
   (void)prot;
   (void)flags;
   uint32_t handle = (uint32_t)(offset >> 32);
   if ((offset & 0xffffffffULL) != 0)
      return MAP_FAILED;
   pthread_mutex_lock(&C.lock);
   struct s21_bo *b = bo_find_locked(handle);
   void *ret = MAP_FAILED;
   if (b && b->nk == 1 && size <= b->size) {
      if (b->cpu == 0) {
         void *mapped = s21_iokit_map(b->k[0], size);
         if (mapped != NULL)
            b->cpu = (uint64_t)(uintptr_t)mapped;
      }
      if (b->cpu != 0)
         ret = (void *)(uintptr_t)b->cpu;
   }
   pthread_mutex_unlock(&C.lock);
   return ret;
}

int
s21_drm_bo_unmap(void *addr, size_t size)
{
   (void)size;
   if (addr == NULL)
      return -EINVAL;
   pthread_mutex_lock(&C.lock);
   int ret = -EINVAL;
   for (uint32_t i = 0; i < S21_MAX_BO; i++) {
      struct s21_bo *b = &C.bo[i];
      if (b->used && b->cpu == (uint64_t)(uintptr_t)addr) {
         ret = s21_iokit_unmap(b->k[0], addr) == 0 ? 0 : -EIO;
         if (ret == 0)
            b->cpu = 0;
         break;
      }
   }
   pthread_mutex_unlock(&C.lock);
   return ret;
}

/* --- user-memory import (IOSurfaces) ------------------------------------ */

int
s21_drm_import_user(uint64_t uva, uint64_t len, uint32_t *handle, uint64_t *gpu_va)
{
   /* kSMImportUser maps len>>12 GPU pages and does not round len up to XNU's
    * 16 KiB page, unlike kSMCreateBO; 4 KiB alignment is correct here. */
   if (uva == 0 || len == 0 || (uva & 0xfff) || (len & 0xfff) ||
       len > S21_CHUNK_MAX)
      return -EINVAL;
   int drain = drain_pending();
   if (drain)
      return drain;
   pthread_mutex_lock(&C.lock);
   /* V21: imports consume the same global kernel budget as CREATE_BO. The
    * old shim counted only created BOs, so backboardd could overcommit. */
   if (C.total > S21_TOTAL_MAX || len > S21_TOTAL_MAX - C.total) {
      pthread_mutex_unlock(&C.lock);
      return -ENOMEM;
   }
   uint64_t va;
   int e = va_alloc_locked(len, &va);
   if (e) {
      pthread_mutex_unlock(&C.lock);
      return e;
   }
   uint64_t in[3] = { uva, len, va }, out[1] = { 0 };
   uint32_t n = 1;
   e = s21_iokit_scalar(S21_IMPORT_USER, in, 3, out, &n);
   if (e || n < 1 || out[0] == 0 || out[0] > S21_MAX_BO) {
      va_free_locked(va, len);
      pthread_mutex_unlock(&C.lock);
      return e ? e : -EIO;
   }
   uint32_t slot = UINT32_MAX;
   for (uint32_t i = 0; i < S21_MAX_BO; i++) {
      if (!C.bo[i].used) {
         slot = i;
         break;
      }
   }
   if (slot == UINT32_MAX) {
      uint64_t d[1] = { out[0] }, o[1] = { 0 };
      uint32_t dn = 1;
      (void)s21_iokit_scalar(S21_DESTROY_BO, d, 1, o, &dn);
      va_free_locked(va, len);
      pthread_mutex_unlock(&C.lock);
      return -ENOMEM;
   }
   struct s21_bo *b = &C.bo[slot];
   memset(b, 0, sizeof(*b));
   b->used = true;
   b->nk = 1;
   b->k[0] = (uint32_t)out[0];
   b->va = va;
   b->size = len;
   b->cpu = uva;   /* CPU memory belongs to the caller */
   C.total += len;
   if (handle)
      *handle = slot + 1;
   if (gpu_va)
      *gpu_va = va;
   pthread_mutex_unlock(&C.lock);
   return 0;
}

/* --- libdrm helpers ----------------------------------------------------- */

int64_t
s21_drm_prime_fd_size(int fd)
{
   if (fd < S21_PRIME_BASE || fd >= S21_PRIME_BASE + S21_MAX_BO)
      return -EINVAL;
   uint32_t handle = (uint32_t)(fd - S21_PRIME_BASE);
   pthread_mutex_lock(&C.lock);
   struct s21_bo *b = bo_find_locked(handle);
   int64_t size = b != NULL ? (int64_t)b->size : -EINVAL;
   pthread_mutex_unlock(&C.lock);
   return size;
}

int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
   (void)fd;
   if (handle == NULL || prime_fd < S21_PRIME_BASE ||
       prime_fd >= S21_PRIME_BASE + S21_MAX_BO)
      return -EINVAL;
   uint32_t h = (uint32_t)(prime_fd - S21_PRIME_BASE);
   pthread_mutex_lock(&C.lock);
   struct s21_bo *b = bo_find_locked(h);
   int e = b != NULL && C.prime_fd[h - 1] == prime_fd ? 0 : -EINVAL;
   if (e == 0)
      *handle = h;
   pthread_mutex_unlock(&C.lock);
   return e;
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
   (void)fd;
   (void)flags;
   if (prime_fd == NULL)
      return -EINVAL;
   pthread_mutex_lock(&C.lock);
   struct s21_bo *b = bo_find_locked(handle);
   int e = b == NULL ? -EINVAL : 0;
   if (e == 0) {
      if (C.prime_fd[handle - 1] == 0)
         C.prime_fd[handle - 1] = S21_PRIME_BASE + (int)handle;
      *prime_fd = C.prime_fd[handle - 1];
   }
   pthread_mutex_unlock(&C.lock);
   return e;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
   struct drm_syncobj_create c = { .flags = flags };
   int e = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &c);
   if (e == 0 && handle)
      *handle = c.handle;
   return e;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
   struct drm_syncobj_destroy d = { .handle = handle };
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &d);
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
               int64_t timeout_ns, unsigned flags, uint32_t *first_signaled)
{
   (void)timeout_ns; /* submits are synchronous in this shim */
   struct drm_syncobj_wait w = {
      .handles = (uint64_t)(uintptr_t)handles,
      .timeout_nsec = timeout_ns,
      .count_handles = num_handles,
      .flags = flags,
   };
   int e = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &w);
   if (e == 0 && first_signaled != NULL)
      *first_signaled = w.first_signaled;
   return e;
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points,
                       unsigned num_handles, int64_t timeout_ns,
                       unsigned flags, uint64_t *first_signaled)
{
   (void)timeout_ns;
   struct drm_syncobj_timeline_wait w = {
      .handles = (uint64_t)(uintptr_t)handles,
      .points = (uint64_t)(uintptr_t)points,
      .timeout_nsec = timeout_ns,
      .count_handles = num_handles,
      .flags = flags,
   };
   int e = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &w);
   if (e == 0 && first_signaled != NULL)
      *first_signaled = w.first_signaled;
   return e;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
   struct drm_syncobj_array a = {
      .handles = (uint64_t)(uintptr_t)handles,
      .count_handles = count,
   };
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &a);
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
   struct drm_syncobj_array a = {
      .handles = (uint64_t)(uintptr_t)handles,
      .count_handles = count,
   };
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &a);
}

int
drmSyncobjTransfer(int fd, uint32_t dst, uint64_t dst_point,
                   uint32_t src, uint64_t src_point, uint32_t flags)
{
   (void)dst_point;
   (void)src_point;
   (void)flags;
   struct drm_syncobj_transfer t = {
      .src_handle = src,
      .dst_handle = dst,
   };
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &t);
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
   (void)fd;
   (void)handle;
   int f = dup(S21_DRM_FD);
   if (f < 0)
      return -errno;
   *sync_file_fd = f;
   return 0;
}

int
drmSyncobjImportSyncFile(int fd, int sync_file_fd, uint32_t handle)
{
   (void)fd;
   (void)sync_file_fd;
   (void)handle;
   return 0;
}

int
drmSyncobjFDToHandle(int fd, int sync_file_fd, uint32_t *handle)
{
   int f = dup(sync_file_fd);
   if (f >= 0)
      close(f);
   return drmSyncobjCreate(fd, 0, handle);
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint32_t count,
                 uint64_t *points, uint32_t flags)
{
   (void)fd;
   (void)handles;
   (void)count;
   (void)points;
   (void)flags;
   return -ENOTSUP;
}

/* Panfrost resource code references these helpers even though the iOS target
 * creates its screen directly and never uses a renderonly/KMS screen. */
void *
renderonly_create_gpu_import_for_resource(void *rsc, void *ro, void *handle)
{
   (void)rsc;
   (void)ro;
   (void)handle;
   return NULL;
}

void
renderonly_scanout_destroy(void *scanout, void *ro)
{
   (void)scanout;
   (void)ro;
}
