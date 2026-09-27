#!/usr/bin/env bash
# build_launcher.sh -- builds KineoBridge.exe, the single customer-facing
# entry point. Native Win32 GUI app (no console window), asInvoker
# manifest embedded (no elevation by default -- see kineobridge.manifest
# for why), version info embedded.
set -euo pipefail
cd "$(dirname "$0")"
CC=x86_64-w64-mingw32-gcc
WINDRES=x86_64-w64-mingw32-windres

echo "== Compiling resources (manifest + version info) =="
"$WINDRES" kineobridge.rc -O coff -o kineobridge_res.o

echo "== Building KineoBridge.exe (release, stripped) =="
"$CC" -O2 -s -mwindows -static -static-libgcc \
    -o KineoBridge.exe kineobridge_launcher.c kineobridge_res.o \
    -lcomctl32 -Wl,--no-insert-timestamp

file KineoBridge.exe
echo "--- sha256 ---"
sha256sum KineoBridge.exe
