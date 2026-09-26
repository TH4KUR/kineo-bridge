#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
CC=x86_64-w64-mingw32-gcc
echo "== Building m4f_standalone.cti =="
"$CC" -shared -O2 -Wall -Wextra -static -static-libgcc \
    -o m4f_standalone.cti m4f_standalone.c m4f_standalone.def \
    -Wl,--out-implib,libm4f.a
file m4f_standalone.cti
llvm-objdump -p m4f_standalone.cti | sed -n '/Export Table/,/^$/p' | grep -cE '^\s*[0-9]+'
