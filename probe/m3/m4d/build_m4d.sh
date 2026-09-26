#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
CC=x86_64-w64-mingw32-gcc
echo "== Building m4d_standalone.cti =="
"$CC" -shared -O2 -Wall -Wextra -static -static-libgcc \
    -o m4d_standalone.cti m4d_standalone.c m4d_standalone.def \
    -Wl,--out-implib,libm4d.a
file m4d_standalone.cti
llvm-objdump -p m4d_standalone.cti | sed -n '/Export Table/,/^$/p' | grep -cE '^\s*[0-9]+'
