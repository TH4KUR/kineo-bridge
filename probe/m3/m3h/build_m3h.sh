#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
CC=x86_64-w64-mingw32-gcc
echo "== Building m3h_standalone.cti =="
"$CC" -shared -O2 -Wall -Wextra -static -static-libgcc \
    -o m3h_standalone.cti m3h_standalone.c m3h_standalone.def \
    -Wl,--out-implib,libm3h.a
file m3h_standalone.cti
llvm-objdump -p m3h_standalone.cti | sed -n '/Export Table/,/^$/p' | grep -cE '^\s*[0-9]+'
