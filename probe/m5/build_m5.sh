#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
CC=x86_64-w64-mingw32-gcc
echo "== Building m5_bridge.cti =="
"$CC" -shared -O2 -Wall -Wextra -static -static-libgcc \
    -o m5_bridge.cti m5_bridge.c wsl_bridge_client.c m5_bridge.def \
    -lws2_32 \
    -Wl,--out-implib,libm5.a
file m5_bridge.cti
llvm-objdump -p m5_bridge.cti | sed -n '/Export Table/,/^$/p' | grep -cE '^\s*[0-9]+'
