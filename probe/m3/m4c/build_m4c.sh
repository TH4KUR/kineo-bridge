#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
CC=x86_64-w64-mingw32-gcc
echo "== Building m4c_standalone.cti =="
"$CC" -shared -O2 -Wall -Wextra -static -static-libgcc \
    -o m4c_standalone.cti m4c_standalone.c m4c_standalone.def \
    -Wl,--out-implib,libm4c.a
file m4c_standalone.cti
llvm-objdump -p m4c_standalone.cti | sed -n '/Export Table/,/^$/p' | grep -cE '^\s*[0-9]+'
