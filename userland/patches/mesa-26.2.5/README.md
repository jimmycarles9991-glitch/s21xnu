# Mesa 26.2.5 patches for Panfrost on iOS / S21

These four patches are the minimum Mesa-side changes needed to let a userland
libdrm shim drive `panfrost` on iOS instead of a Linux DRM device. They came
from `PixOS/tools/mesa-patches` (MIT; authored by TheSmuks with Claude Sonnet
5.5) and are kept verbatim here with attribution. Apply in numeric order to a
pristine `mesa-26.2.5` checkout:

```
for p in 000*.patch; do git apply "$p"; done
```

| Patch | What it does |
|---|---|
| `0001` | Adds the Mali-G78 model row for `PAN_PROD_ID(9,2,2)` (`GPU_ID 0x92020010`). **The tilebuffer sizes and rates are copied from G57 and are explicitly unverified guesses**; this is enough for Mesa to accept the GPU, not a claim of correct perf tuning. |
| `0002` | Adds `-Ddarwin-drm=true`, a declarations-only `xf86drm.h`, and weak `ENOSYS` definitions so Mesa links without libdrm. The embedding dylib supplies the strong `drm*` symbols. |
| `0003` | Compiles `panfrost_kmod.c` on Darwin (no `eventfd`/`timerfd`/`SCHED_FIFO`) and converts Mesa's absolute `WAIT_BO` deadline to a relative timeout before it reaches the user client. |
| `0004` | Routes BO `mmap`/`munmap` through `darwin_drm_bo_map()` / `darwin_drm_bo_unmap()` on `__APPLE__`. |

Nothing in this directory is a kernel change. The matching userland shim is in
`userland/sweetdrm/`; it targets this fork's `SweetMali` IOKit selectors, not
PixOS's selectors.

Validated locally: all four patches apply cleanly in numeric order to a
pristine `mesa-26.2.5` checkout (tag `f9ce5e81f30cd694726bff6d8197e509a8374ea7`),
and the S21 shim host test passes against those patched headers with clang and
with clang ASan/UBSan. No Mesa build, iOS link, or device boot has been done
from this directory yet.
