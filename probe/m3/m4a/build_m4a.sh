#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
CC=x86_64-w64-mingw32-gcc
echo "== Building m4a_standalone.cti =="
"$CC" -shared -O2 -Wall -Wextra -static -static-libgcc \
    -o m4a_standalone.cti m4a_standalone.c m4a_standalone.def \
    -Wl,--out-implib,libm4a.a
file m4a_standalone.cti
llvm-objdump -p m4a_standalone.cti | sed -n '/Export Table/,/^$/p' | grep -cE '^\s*[0-9]+'
