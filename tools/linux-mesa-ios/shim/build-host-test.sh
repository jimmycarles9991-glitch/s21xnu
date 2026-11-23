#!/usr/bin/env bash
# Compile and run the fake-IOKit host test for the fresh S21 drm shim.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MESA_DIR=${MESA_DIR:-/home/tom/s21/mesa-26.2.4}
BUILD=${BUILD:-/tmp/opencode/s21-mesa-ios-host}
CC=${CC:-cc}

if [[ ! -f "$MESA_DIR/include/drm-uapi/panfrost_drm.h" ]]; then
   echo "ERROR: MESA_DIR=$MESA_DIR has no include/drm-uapi/panfrost_drm.h" >&2
   exit 1
fi

mkdir -p "$BUILD"
"$CC" -std=c11 -O0 -g -Wall -Wextra -Wno-unused-parameter \
   -DS21_HOST_TEST \
   -I"$ROOT" -I"$ROOT/../include" -I"$MESA_DIR/include" \
   "$ROOT/s21_drm.c" "$ROOT/s21_iokit_host.c" "$ROOT/host_test.c" \
   -pthread -o "$BUILD/s21-drm-host-test"

"$BUILD/s21-drm-host-test"
