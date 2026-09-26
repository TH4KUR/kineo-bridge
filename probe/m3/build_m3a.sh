#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
CC=x86_64-w64-mingw32-gcc
echo "== Building m3a_standalone.cti =="
"$CC" -shared -O2 -Wall -Wextra -static -static-libgcc \
    -o m3a_standalone.cti m3a_standalone.c m3a_standalone.def \
    -Wl,--out-implib,libm3a.a
file m3a_standalone.cti
llvm-objdump -p m3a_standalone.cti | sed -n '/Export Table/,/^$/p' | grep -cE '^\s*[0-9]+'
