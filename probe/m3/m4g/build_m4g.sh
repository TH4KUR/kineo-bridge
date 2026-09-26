#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
CC=x86_64-w64-mingw32-gcc
echo "== Building m4g_standalone.cti =="
"$CC" -shared -O2 -Wall -Wextra -static -static-libgcc \
    -o m4g_standalone.cti m4g_standalone.c m4g_standalone.def \
    -Wl,--out-implib,libm4g.a
file m4g_standalone.cti
llvm-objdump -p m4g_standalone.cti | sed -n '/Export Table/,/^$/p' | grep -cE '^\s*[0-9]+'
