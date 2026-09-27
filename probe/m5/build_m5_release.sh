#!/usr/bin/env bash
# build_m5_release.sh -- RELEASE build of the proven m5 CTI. Same source,
# same behavior, same GenTL export surface as build_m5.sh (the private
# dev build) -- this only changes what's embedded in the binary:
#   - debug symbols stripped (-s), no separate .pdb ever produced by this
#     mingw toolchain in the first place
#   - no source file paths should remain embedded (verified below)
#   - identical optimization level and export table (81 exports)
# Do NOT change any -D flags or source files here -- if the dev build
# needs a source change, make it once in m5_bridge.c and rebuild BOTH
# build_m5.sh and this script from the same source tree.
set -euo pipefail
cd "$(dirname "$0")"
CC=x86_64-w64-mingw32-gcc
OUT="m5_bridge.release.cti"
echo "== Building RELEASE $OUT =="
"$CC" -shared -O2 -s -static -static-libgcc \
    -o "$OUT" m5_bridge.c wsl_bridge_client.c m5_bridge.def \
    -lws2_32 -Wl,--no-insert-timestamp

echo "--- file type ---"
file "$OUT"
echo "--- export count (must be 81, matching the dev build) ---"
llvm-objdump -p "$OUT" | sed -n '/Export Table/,/^$/p' | grep -cE '^\s*[0-9]+'
echo "--- checking for embedded absolute source paths ---"
if strings "$OUT" | grep -qE "/home/[a-zA-Z0-9_-]+/kineo-bridge"; then
    echo "WARNING: absolute dev paths found embedded in the release binary:"
    strings "$OUT" | grep -E "/home/[a-zA-Z0-9_-]+/kineo-bridge"
else
    echo "OK: no absolute dev source paths found"
fi
echo "--- sha256 ---"
sha256sum "$OUT"
