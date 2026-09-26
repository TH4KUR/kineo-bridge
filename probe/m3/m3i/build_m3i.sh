#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
CC=x86_64-w64-mingw32-gcc
echo "== Building m3i_standalone.cti =="
"$CC" -shared -O2 -Wall -Wextra -static -static-libgcc \
    -o m3i_standalone.cti m3i_standalone.c m3i_standalone.def \
    -Wl,--out-implib,libm3i.a
file m3i_standalone.cti
llvm-objdump -p m3i_standalone.cti | sed -n '/Export Table/,/^$/p' | grep -cE '^\s*[0-9]+'
