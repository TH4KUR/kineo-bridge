#!/usr/bin/env bash
# build.sh — M0 build script for the probe CTI + standalone harness.
#
# Requires x86_64-w64-mingw32-gcc (see ../investigation/toolchain.md).
# Install with:
#   sudo apt-get install -y gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 \
#       binutils-mingw-w64-x86-64 mingw-w64-x86-64-dev
set -euo pipefail
cd "$(dirname "$0")"

CC=x86_64-w64-mingw32-gcc
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "ERROR: $CC not found. Install the mingw-w64 toolchain first (see investigation/toolchain.md)." >&2
    exit 1
fi

echo "== Building kineo_probe.cti =="
"$CC" -shared -O2 -Wall -Wextra \
    -static -static-libgcc \
    -o kineo_probe.cti kineo_probe.c kineo_probe.def \
    -Wl,--out-implib,libkineo_probe.a

echo "== Building harness.exe =="
"$CC" -O2 -Wall -Wextra \
    -static -static-libgcc \
    -o harness.exe harness.c

echo "== Verifying output =="
file kineo_probe.cti
file harness.exe
echo "--- export table (llvm-objdump — GNU objdump can't parse these PE files, per investigation/gentl-exports.md) ---"
llvm-objdump -p kineo_probe.cti | sed -n '/Export Table/,/^$/p'

echo
echo "Build complete. To run the M0 test:"
echo "  1. Copy kineo_probe.cti and harness.exe to a scratch dir on the Windows side,"
echo "     e.g. /mnt/c/Users/<you>/kineo-bridge-test/"
echo "  2. From PowerShell/cmd in that dir: harness.exe kineo_probe.cti"
