#!/bin/sh
# Build and run the S21 shim host test on Linux with fake IOKit.
# Usage: host-test.sh PATCHED_MESA_DIR [OUT_DIR]
# PATCHED_MESA_DIR must have Mesa 26.2.5 patches 0002-0004 applied (only the
# headers xf86drm.h, darwin_drm.h and drm-uapi/* are needed).
set -eu
M=${1:?usage: host-test.sh PATCHED_MESA_DIR [OUT_DIR]}
H=$(cd "$(dirname "$0")" && pwd)
O=${2:-${TMPDIR:-/tmp}/s21-sweetdrm-host}
mkdir -p "$O"
${CC:-cc} -O1 -Wall -Wextra -Werror -pthread -DS21_DRM_HOST_TEST -I"$H" \
	-isystem "$M/include" -isystem "$M/src/panfrost/darwin_drm" -isystem "$M/src" \
	"$H/s21_sweet_drm.c" "$H/fake_iokit.c" "$H/host_test.c" -o "$O/s21_sweetdrm_host_test"
# macOS runners have no coreutils timeout(1); the test is bounded by design.
if command -v timeout >/dev/null 2>&1; then
	timeout 60 "$O/s21_sweetdrm_host_test"
else
	"$O/s21_sweetdrm_host_test"
fi
