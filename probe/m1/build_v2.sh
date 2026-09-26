#!/usr/bin/env bash
# build_v2.sh — M1 iteration build. Produces kineo_probe_v2.cti (superset of
# the M0-validated kineo_probe.cti, with Event*/DS*/stacked-port stub exports
# added). Does not touch or rebuild the M0 baseline.
set -euo pipefail
cd "$(dirname "$0")"

CC=x86_64-w64-mingw32-gcc
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "ERROR: $CC not found." >&2
    exit 1
fi

echo "== Compiling version resource =="
x86_64-w64-mingw32-windres kineo_probe_v2.rc -O coff -o kineo_probe_v2_res.o

echo "== Building kineo_probe_v2.cti =="
"$CC" -shared -O2 -Wall -Wextra \
    -static -static-libgcc \
    -o kineo_probe_v2.cti kineo_probe_v2.c kineo_probe_v2_res.o kineo_probe_v2.def \
    -Wl,--out-implib,libkineo_probe_v2.a

echo "== Verifying output =="
file kineo_probe_v2.cti
echo "--- export count ---"
llvm-objdump -p kineo_probe_v2.cti | sed -n '/Export Table/,/^$/p' | grep -c '  0x' || true
