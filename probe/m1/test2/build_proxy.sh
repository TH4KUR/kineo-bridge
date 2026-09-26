#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

CC=x86_64-w64-mingw32-gcc
echo "== Building kineo_proxy.cti =="
"$CC" -shared -O2 -Wall -Wextra \
    -static -static-libgcc \
    -o kineo_proxy.cti kineo_proxy.c kineo_proxy.def \
    -Wl,--out-implib,libkineo_proxy.a

echo "== Verifying export count (expect 81) =="
file kineo_proxy.cti
llvm-objdump -p kineo_proxy.cti | sed -n '/Export Table/,/^$/p' | grep -cE '^\s*[0-9]+'
