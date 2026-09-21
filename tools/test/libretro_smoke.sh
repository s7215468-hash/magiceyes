#!/bin/bash
# Build and run the tiny libretro frontend. Arguments after the core are forwarded to it:
#   tools/test/libretro_smoke.sh GAME SYSTEM_DIR SAVE_DIR [SECOND_GAME]
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cc -O2 -Wall -Wextra -I"$ROOT/host/libretro" -o "$ROOT/bin/libretro_smoke" \
  "$ROOT/tools/test/libretro_smoke.c" -ldl
exec "$ROOT/bin/libretro_smoke" "$ROOT/magiceyes_libretro.so" "$@"
