#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

CC="${CC:-gcc}"
CFLAGS="-std=c11 -O2 -Wall -Wextra"

if pkg-config --exists raylib 2>/dev/null; then
    RAYLIB_LIBS="$(pkg-config --libs raylib)"
    RAYLIB_CFLAGS="$(pkg-config --cflags raylib)"
else
    RAYLIB_LIBS="-lraylib"
    RAYLIB_CFLAGS=""
fi

echo "Building woomer..."
"$CC" $CFLAGS $RAYLIB_CFLAGS -o woomer src/main.c $RAYLIB_LIBS -lm -lpthread -ldl

echo "Done. Run with: ./woomer"
echo "(requires grim and swaymsg to be installed and available on PATH)"
