#!/usr/bin/env bash
# package_release.sh -- assembles the customer-facing release bundle.
# Output (gitignored -- this is a BUILD OUTPUT, not source):
#
#   release/
#     KineoBridge.exe          <- the only thing the customer runs
#     payload/
#       m5_bridge.release.cti  <- release (stripped) CTI
#       kineo_bridge_m5.xml
#       bridge/
#         *.pyc                <- compiled bridge, NO .py source
#     VERSION.txt
#     README_INSTALL.txt
#
# The customer never receives: CTI source, bridge .py source, build
# scripts, investigation notes, protocol docs, debug symbols, or dev logs.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RELEASE_DIR="$REPO_ROOT/release"
VERSION="1.0.0"

echo "== 1/4: building release CTI =="
"$REPO_ROOT/probe/m5/build_m5_release.sh"

echo "== 2/4: building launcher =="
"$REPO_ROOT/launcher/build_launcher.sh"

echo "== 3/4: compiling bridge to .pyc (no .py source shipped) =="
rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_DIR/payload/bridge"
# Compile each module with py_compile directly, forcing co_filename
# (via dfile=) to the bare module name -- never the absolute dev path
# it's actually read from -- and unchecked_hash invalidation (a
# content hash, not a mtime). Both together make the shipped .pyc
# bytes fully reproducible across rebuilds of unchanged source, and
# ensure no dev machine path is ever embedded in a shipped artifact.
for mod in protocol camera_source kineo_camera_bridge bridge_status; do
    python3 -c "
import py_compile
py_compile.compile(
    '$REPO_ROOT/wsl-camera/$mod.py',
    cfile='$RELEASE_DIR/payload/bridge/$mod.pyc',
    dfile='$mod.py',
    doraise=True,
    invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH,
)
"
done

echo "== 4/4: assembling release bundle =="
cp "$REPO_ROOT/launcher/KineoBridge.exe" "$RELEASE_DIR/"
cp "$REPO_ROOT/probe/m5/m5_bridge.release.cti" "$RELEASE_DIR/payload/"
cp "$REPO_ROOT/probe/m5/kineo_bridge_m5.xml" "$RELEASE_DIR/payload/"

echo "$VERSION" > "$RELEASE_DIR/VERSION.txt"
cat > "$RELEASE_DIR/README_INSTALL.txt" << 'EOF'
Kineo Bridge -- Installation (v1, manual)

1. Copy this entire folder to:
     C:\Program Files\IMV Technologies\KineoBridge\

2. Double-click KineoBridge.exe.

That's it -- no WSL, PowerShell, or environment variables to touch by
hand. KineoBridge.exe deploys its own private runtime files the first
time it runs, checks/repairs the camera's USB connection automatically,
starts the camera bridge, and launches Kineo for you.

Prerequisites (already true on a machine that's run Kineo's WSL bridge
before; a fresh machine needs these once):
  - WSL2 with a default Linux distribution installed
  - usbipd-win installed
  - The camera shared with WSL at least once (a one-time admin step;
    KineoBridge will tell you the exact command if this is still needed)
  - Aravis 0.8.36 (with GObject-Introspection Python bindings) built and
    available inside WSL at ~/aravis-0.8.36/build/src
    (this dependency is not yet bundled by the installer in v1 -- see
    PROJECT_MEMORY.md, "Known limitations")

Support: if KineoBridge.exe shows an error, note the exact message and
contact support.
EOF

echo
echo "=== Release bundle ready: $RELEASE_DIR ==="
find "$RELEASE_DIR" -type f | sort
echo
echo "=== sha256 of shipped files ==="
find "$RELEASE_DIR" -type f -exec sha256sum {} \;
