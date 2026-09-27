#!/usr/bin/env bash
# start-kineo.sh -- one-command entry point: bring the camera, the WSL
# bridge, and Kineo itself up together, from a normal WSL shell.
#
# Never touches the Kineo installation, never sets a persistent
# User/Machine environment variable -- GENICAM_GENTL64_PATH and
# KINEO_BRIDGE_SOURCE are passed only to the one Kineo process this
# script launches (via WSLENV, so WSL interop actually propagates them --
# see investigation/hardening.md for why that's required).
#
# Every stage below is VERIFIED to actually stay alive, not just assumed
# to have succeeded because a command returned -- a real launch-time
# crash (Kineo's own native module failing to load, unrelated to this
# project) was observed to slip past a naive "launched, sleep 2, done"
# check. Any failure that needs the user's attention shows a native
# Windows message box (confirmed working via WSL/PowerShell interop) in
# addition to the terminal message, since the user may not be watching
# this terminal.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib_common.sh"
source "$SCRIPT_DIR/lib_usbip.sh"

SOURCE_MODE="wsl"     # wsl (real camera) or synthetic
BRIDGE_PORT="$BRIDGE_PORT_DEFAULT"
MAX_FPS="10"          # physical camera cap -- see camera_source.py PHYSICAL_FPS_CAP
for arg in "$@"; do
    case "$arg" in
        --synthetic) SOURCE_MODE="synthetic" ;;
        --wsl) SOURCE_MODE="wsl" ;;
        --port=*) BRIDGE_PORT="${arg#*=}" ;;
        --max-fps=*) MAX_FPS="${arg#*=}" ;;
        -h|--help)
            echo "Usage: start-kineo.sh [--wsl|--synthetic] [--port=9494]"
            echo "  --wsl        real camera via the WSL/Aravis bridge (default)"
            echo "  --synthetic  synthetic test pattern, no camera/USB-IP needed"
            exit 0
            ;;
    esac
done

BRIDGE_LOG="$BRIDGE_DIR/bridge.$BRIDGE_PORT.log"

fail() { c_fail "$1"; exit 1; }
fail_modal() {
    local short="$1" detail="${2:-$1}"
    c_fail "$short"
    show_modal "Kineo bridge: startup failed" "$detail" "Error"
    exit 1
}
# Bounded retry (3x) on the check itself, same reasoning as
# usbip_find_device in lib_usbip.sh: WSL interop invoking a Windows .exe
# (tasklist.exe here, usbipd.exe there) has occasionally produced empty/
# stale output for no real state-change reason. A single missed reading
# must never be mistaken for a real process crash -- confirmed the hard
# way: this exact false positive fired mid-development, reporting Kineo
# "crashed" while it was actually running fine, which then triggered a
# redundant second launch that Kineo's own single-instance lock correctly
# rejected (logged as "App already running, quitting second instance"),
# compounding into a fully spurious failure report.
kineo_is_running() {
    local i
    for i in 1 2 3; do
        if tasklist.exe /FI "IMAGENAME eq $KINEO_EXE_NAME" 2>/dev/null | grep -qi "Kineo"; then
            return 0
        fi
        sleep 0.3
    done
    return 1
}

# ---- 1. validate Windows/Kineo paths ----
c_info "checking Kineo installation..."
if [ ! -d "$KINEO_INSTALL_DIR" ]; then
    fail_modal "Kineo install not found" "Kineo install not found at: $KINEO_INSTALL_DIR

Check that Kineo Software is installed at the expected location, or
update KINEO_INSTALL_DIR in scripts/lib_common.sh if it's been moved."
fi
if [ ! -f "$KINEO_INSTALL_DIR/$KINEO_EXE_NAME" ]; then
    fail_modal "Kineo executable missing" "$KINEO_EXE_NAME not found in $KINEO_INSTALL_DIR

The install directory exists but the executable is missing -- the
installation may be corrupted. Try reinstalling Kineo Software."
fi
c_ok "Kineo installation found ($(get_kineo_version))"

# ---- stage the CTI (always the current build, never touching the
# installation itself -- staged into the separate test directory that's
# been used throughout this project) ----
# A running Kineo process holds the CTI DLL open on Windows; overwriting
# it then silently does nothing (NTFS sharing violation, no error surfaced
# through a plain `cp`) -- close any stale instance FIRST, then verify the
# copy actually landed by comparing hashes rather than trusting a `cp`
# exit code alone.
if kineo_is_running; then
    c_warn "a Kineo process is already running -- closing it so the CTI file isn't locked"
    taskkill.exe /IM "$KINEO_EXE_NAME" /F >/dev/null 2>&1
    sleep 1
fi
mkdir -p "$CTI_STAGING_DIR"
if [ ! -f "$CTI_SOURCE_DIR/$CTI_FILE_NAME" ]; then
    fail_modal "CTI not built" "CTI not built: $CTI_SOURCE_DIR/$CTI_FILE_NAME

Run probe/m5/build_m5.sh first, then re-run this script."
fi
cp -f "$CTI_SOURCE_DIR/$CTI_FILE_NAME" "$CTI_SOURCE_DIR/$CTI_XML_NAME" "$CTI_STAGING_DIR/"
src_hash=$(sha256sum "$CTI_SOURCE_DIR/$CTI_FILE_NAME" | awk '{print $1}')
staged_hash=$(sha256sum "$CTI_STAGING_DIR/$CTI_FILE_NAME" 2>/dev/null | awk '{print $1}')
if [ "$src_hash" != "$staged_hash" ]; then
    fail_modal "CTI staging failed" "CTI staging failed -- the staged file does not match the freshly
built one (a running process may still have it locked).

Steps to try:
  1. Close any 'Kineo Software.exe' processes in Task Manager.
  2. Re-run this script."
fi
c_ok "CTI staged at $CTI_STAGING_DIR (hash-verified)"

# ---- 2-5. camera detection / USB-IP verify+repair (skipped in synthetic mode) ----
if [ "$SOURCE_MODE" = "wsl" ]; then
    c_info "checking camera / USB-IP state..."
    attempt=0
    max_attempts=5
    backoff=2
    until usbip_recover_camera "$CAMERA_VIDPID" "$CAMERA_SERIAL"; do
        attempt=$((attempt+1))
        if [ "$attempt" -ge "$max_attempts" ]; then
            fail_modal "Camera not usable" "The camera could not be made ready after $max_attempts attempts.

See the terminal output above for the specific reason (USB/IP not
attached, a one-time admin 'usbipd bind' needed, or the camera not
connected at all) and the exact command to fix it.

You can also run scripts/diagnose.sh for a full breakdown."
        fi
        c_info "retrying in ${backoff}s (attempt $attempt/$max_attempts)..."
        sleep "$backoff"
        backoff=$((backoff*2))
    done
    c_ok "camera present on Windows, attached to WSL, visible to Aravis (serial $CAMERA_SERIAL)"
else
    c_info "synthetic mode -- skipping camera/USB-IP checks"
fi

# ---- 6. ensure no stale bridge process on this port ----
existing_pids=$(pgrep -f "python3.*kineo_camera_bridge\.py.*--port $BRIDGE_PORT" 2>/dev/null || true)
if [ -n "$existing_pids" ]; then
    c_warn "stale bridge process found on port $BRIDGE_PORT (pid(s): $existing_pids) -- stopping it"
    kill $existing_pids 2>/dev/null
    sleep 1
    still=$(pgrep -f "python3.*kineo_camera_bridge\.py.*--port $BRIDGE_PORT" 2>/dev/null || true)
    if [ -n "$still" ]; then
        c_warn "did not exit gracefully, force-killing"
        kill -9 $still 2>/dev/null
        sleep 1
    fi
fi

# ---- 7. start the bridge, capturing its real PID so we can verify it
# actually stays alive (not just that it started) ----
c_info "starting bridge (source=$SOURCE_MODE port=$BRIDGE_PORT max_fps=$MAX_FPS)..."
bridge_source_arg="camera"
[ "$SOURCE_MODE" = "synthetic" ] && bridge_source_arg="synthetic"
BRIDGE_PID_FILE=$(mktemp)
(
    cd "$BRIDGE_DIR" || exit 1
    GI_TYPELIB_PATH="$ARAVIS_LIB_DIR" LD_LIBRARY_PATH="$ARAVIS_LIB_DIR" KINEO_BRIDGE_MAX_FPS="$MAX_FPS" \
        nohup python3 -u kineo_camera_bridge.py \
            --source "$bridge_source_arg" --host "$BRIDGE_HOST_DEFAULT" --port "$BRIDGE_PORT" \
            > "$BRIDGE_LOG" 2>&1 &
    echo $! > "$BRIDGE_PID_FILE"
    disown
)
BRIDGE_PID=$(cat "$BRIDGE_PID_FILE" 2>/dev/null); rm -f "$BRIDGE_PID_FILE"

# ---- 8. wait for bridge READY (status file appears and is fresh),
# while also checking it hasn't already died -- a crashed process never
# produces a status file, so without this check the loop would just spin
# for the full timeout on an already-dead process before failing, with no
# indication that's what happened. ----
ready=0
died=0
for _ in $(seq 1 20); do
    if [ -n "$BRIDGE_PID" ] && ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
        died=1
        break
    fi
    if python3 -c "
import sys; sys.path.insert(0, '$BRIDGE_DIR')
from bridge_status import read_status
sys.exit(0 if read_status(port=$BRIDGE_PORT) else 1)
" 2>/dev/null; then
        ready=1
        break
    fi
    sleep 0.5
done
if [ "$died" -eq 1 ]; then
    fail_modal "Bridge crashed on startup" "The WSL camera bridge process died immediately after starting.

See $BRIDGE_LOG for the exact error (often a camera/Aravis library
issue). Fix the underlying cause and re-run this script -- Kineo has
not been launched."
fi
if [ "$ready" -ne 1 ]; then
    fail_modal "Bridge did not become ready" "The bridge process is running but never reported itself ready within
10 seconds.

See $BRIDGE_LOG for details. Kineo has not been launched."
fi
# Re-check a moment later -- catches a bridge that reports ready and then
# crashes almost immediately after (e.g. failing on the first real client
# interaction), which the check above alone would miss.
sleep 1
if [ -n "$BRIDGE_PID" ] && ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
    fail_modal "Bridge crashed right after starting" "The bridge reported ready but then crashed a moment later.

See $BRIDGE_LOG for the exact error. Kineo has not been launched."
fi
c_ok "bridge ready and stable (log: $BRIDGE_LOG)"

# ---- 9/10/11. launch Kineo, process-local env only -- verified via
# Kineo's OWN ChironLog, not OS process polling. tasklist.exe-based
# polling was tried first and produced a real false positive: Electron's
# single-instance-lock bootstrap briefly launches/exits a second process
# in well under a second when a first instance is already up, which a
# tight process-existence poll can misread as "crashed" -- confirmed by
# reproducing it live (Kineo was genuinely running the whole time,
# confirmed via tasklist and ChironLog, while the poll insisted it had
# died). ChironLog directly states success ("WebSocket connection
# established") or the specific real failure this project has actually
# seen ("Device component cannot start" / "Child process exited with
# code"), so it's authoritative instead of inferred. ----
find_chiron_log() {
    ls -t "$KINEO_INSTALL_DIR/Data/Logs/Application/"ChironLog_*.log 2>/dev/null | head -1
}
launch_kineo_once() {
    (
        cd "$KINEO_INSTALL_DIR" || exit 1
        export WSLENV="${WSLENV:-}:KINEO_BRIDGE_SOURCE:GENICAM_GENTL64_PATH"
        export KINEO_BRIDGE_SOURCE="$SOURCE_MODE"
        export GENICAM_GENTL64_PATH="$WIN_CTI_STAGING_DIR"
        nohup "./$KINEO_EXE_NAME" > /dev/null 2>&1 &
        disown
    )
}

c_info "launching Kineo (source=$SOURCE_MODE)..."
kineo_ok=0
for attempt in 1 2; do
    [ "$attempt" -gt 1 ] && c_warn "retrying Kineo launch (attempt $attempt/2)..."
    log_file=$(find_chiron_log)
    mark_line=0
    [ -n "$log_file" ] && mark_line=$(wc -l < "$log_file" 2>/dev/null || echo 0)

    launch_kineo_once

    result="timeout"
    for _ in $(seq 1 20); do
        sleep 1
        log_file=$(find_chiron_log)
        [ -z "$log_file" ] && continue
        new_content=$(tail -n "+$((mark_line+1))" "$log_file" 2>/dev/null)
        if echo "$new_content" | grep -qE "Device component cannot start|Child process exited with code"; then
            result="crashed"
            break
        fi
        if echo "$new_content" | grep -q "App already running, quitting second instance"; then
            # Only possible if an earlier attempt actually succeeded and
            # is still running -- that IS success, not a new failure.
            result="already_running"
            break
        fi
        if echo "$new_content" | grep -q "WebSocket connection established"; then
            result="ok"
            break
        fi
    done

    case "$result" in
        ok|already_running) kineo_ok=1; break ;;
        crashed) c_warn "Kineo crashed on startup (attempt $attempt/2) -- see ChironLog" ;;
        timeout) c_warn "no clear success/failure signal from Kineo within 20s (attempt $attempt/2)" ;;
    esac
done

if [ "$kineo_ok" -ne 1 ]; then
    fail_modal "Kineo failed to start" "Kineo crashed shortly after starting, twice in a row (confirmed via
its own ChironLog, not just a process check).

This looks like a transient Windows-side issue (e.g. a native module
failing to load) rather than a problem with the camera bridge -- the
bridge itself is running fine and does not need to be restarted.

Steps to try:
  1. Close any leftover 'Kineo Software.exe' processes (Task Manager).
  2. Run scripts/start-kineo.sh again.
  3. If it keeps failing, check Windows Event Viewer > Application log
     for the exact crash reason, or try restarting Windows."
fi
c_ok "Kineo running and stable"
echo
c_info "Run scripts/diagnose.sh to check overall health, or scripts/stop-kineo.sh to shut down."
