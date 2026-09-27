#!/usr/bin/env bash
# lib_common.sh -- shared constants and paths for the Kineo hardening
# scripts (start-kineo.sh, stop-kineo.sh, diagnose.sh). Sourced, not run.
#
# Every path here matches what has been empirically verified working
# throughout the M1-M6 project (see investigation/golden-baseline.md and
# investigation/kineo-update-diff.md) -- nothing here is a guess.

# ---- camera identity ----
CAMERA_VIDPID="1409:8000"          # IDS U3-3560XCP-M
CAMERA_SERIAL="4110010861"

# ---- WSL-side paths ----
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_DIR="$REPO_DIR/wsl-camera"
BRIDGE_SCRIPT="$BRIDGE_DIR/kineo_camera_bridge.py"
BRIDGE_HOST_DEFAULT="0.0.0.0"     # what the bridge server itself binds to
BRIDGE_CLIENT_HOST_DEFAULT="127.0.0.1"  # what the CTI (Windows side) connects to
BRIDGE_PORT_DEFAULT="9494"
ARAVIS_LIB_DIR="$HOME/aravis-0.8.36/build/src"

# ---- Windows-side paths (via /mnt/c) ----
KINEO_INSTALL_DIR="/mnt/c/IMVapps/Kineo Software"
KINEO_EXE_NAME="Kineo Software.exe"
CTI_STAGING_DIR="/mnt/c/Users/IMV/kineo-bridge-test/m1/m5"   # what GENICAM_GENTL64_PATH points at
CTI_SOURCE_DIR="$REPO_DIR/probe/m5"
CTI_FILE_NAME="m5_bridge.cti"
CTI_XML_NAME="kineo_bridge_m5.xml"
# Windows-visible path to this repo, for GENICAM_GENTL64_PATH-style values
# that must be a real Windows path Kineo's process can resolve (WSL-native
# paths under /home are NOT directly usable by a Windows process without
# the \\wsl.localhost\ UNC prefix, which some Windows tools reject as a
# working directory -- the staging copy above sidesteps that entirely).
WIN_CTI_STAGING_DIR='C:\Users\IMV\kineo-bridge-test\m1\m5'
WIN_KINEO_INSTALL_DIR='C:\IMVapps\Kineo Software'

# ---- logging helpers (kept deliberately terse -- see diagnose.sh's
# "keep output SHORT" requirement) ----
c_ok()   { printf '\033[32m[OK]\033[0m %s\n' "$1"; }
c_fail() { printf '\033[31m[FAIL]\033[0m %s\n' "$1"; }
c_warn() { printf '\033[33m[WARN]\033[0m %s\n' "$1"; }
c_info() { printf '[INFO] %s\n' "$1"; }

# Shows a native Windows message box via PowerShell/WSL interop -- for
# failures that need the user's attention even if they're not watching
# this terminal. Confirmed working in this environment (2026-09-27): a
# real, visible, clickable dialog, not a no-op. Blocks until dismissed
# (bounded to 5 minutes so a script never hangs forever if nobody's at
# the keyboard) -- this is always a final step right before a script
# exits on failure, never something to wait on mid-flow. Falls back to
# silently doing nothing if PowerShell/interop isn't reachable (the
# terminal message a caller prints alongside this is always the
# authoritative, guaranteed-visible copy).
#
# icon: Information | Warning | Error
show_modal() {
    local title="$1" message="$2" icon="${3:-Warning}"
    local escaped_msg="${message//\'/\'\'}"
    local escaped_title="${title//\'/\'\'}"
    timeout 300 powershell.exe -NoProfile -Command "
        Add-Type -AssemblyName System.Windows.Forms | Out-Null
        [System.Windows.Forms.MessageBox]::Show('$escaped_msg', '$escaped_title', 'OK', '$icon') | Out-Null
    " >/dev/null 2>&1 || true
}

# Reads the Kineo app version (app.asar's own package.json "version") with
# a hash-keyed cache -- extracting from asar takes a few seconds (spawns
# node/npx), so repeat diagnose.sh runs should be fast. Cache invalidates
# automatically whenever app.asar itself changes (e.g. after an update).
# Prints "unknown" (never errors out) if anything about this fails --
# purely informational, must never block diagnostics.
get_kineo_version() {
    local asar="$KINEO_INSTALL_DIR/resources/app.asar"
    local cache="$REPO_DIR/scripts/.kineo_version_cache"
    [ -f "$asar" ] || { echo "unknown"; return 1; }
    local current_hash
    current_hash=$(sha256sum "$asar" 2>/dev/null | awk '{print $1}')
    if [ -f "$cache" ]; then
        local cached_hash cached_version
        cached_hash=$(sed -n '1p' "$cache")
        cached_version=$(sed -n '2p' "$cache")
        if [ "$cached_hash" = "$current_hash" ] && [ -n "$cached_version" ]; then
            echo "$cached_version"
            return 0
        fi
    fi
    local workdir version
    workdir=$(mktemp -d)
    version="unknown"
    if (cd "$workdir" && timeout 30 npx --yes asar extract-file "$asar" package.json) >/dev/null 2>&1; then
        version=$(python3 -c "import json; print(json.load(open('$workdir/package.json')).get('version','unknown'))" 2>/dev/null)
        [ -n "$version" ] || version="unknown"
    fi
    rm -rf "$workdir"
    printf '%s\n%s\n' "$current_hash" "$version" > "$cache" 2>/dev/null
    echo "$version"
}
