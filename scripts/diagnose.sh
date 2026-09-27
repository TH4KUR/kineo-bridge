#!/usr/bin/env bash
# diagnose.sh -- short, actionable health check for the whole Kineo/WSL/
# camera/bridge stack. Read-only: never attaches, starts, or kills
# anything -- see start-kineo.sh/stop-kineo.sh for that. Distinguishes
# USB/IP, Aravis, bridge, and CTI/Kineo problems from each other, per
# instructions, so a failure points at the right layer immediately.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib_common.sh"
source "$SCRIPT_DIR/lib_usbip.sh"

VERBOSE=0
BRIDGE_PORT="$BRIDGE_PORT_DEFAULT"
for arg in "$@"; do
    case "$arg" in
        --verbose) VERBOSE=1 ;;
        --port=*) BRIDGE_PORT="${arg#*=}" ;;
    esac
done

FAILURES=0

# ---- 1. Kineo installation ----
if [ -f "$KINEO_INSTALL_DIR/$KINEO_EXE_NAME" ]; then
    ver=$(get_kineo_version)
    c_ok "Kineo version: $ver"
else
    c_fail "Kineo install not found at: $KINEO_INSTALL_DIR"
    FAILURES=$((FAILURES+1))
fi

# ---- 2. Custom CTI staged ----
if [ -f "$CTI_STAGING_DIR/$CTI_FILE_NAME" ] && [ -f "$CTI_STAGING_DIR/$CTI_XML_NAME" ]; then
    c_ok "custom CTI found: $CTI_STAGING_DIR/$CTI_FILE_NAME"
else
    c_fail "custom CTI not staged at: $CTI_STAGING_DIR"
    c_info "Fix: re-run start-kineo.sh (it stages the CTI automatically), or copy"
    c_info "     $CTI_SOURCE_DIR/{$CTI_FILE_NAME,$CTI_XML_NAME} there yourself."
    FAILURES=$((FAILURES+1))
fi

# ---- 3/4. USB/IP: camera present on Windows + usbipd status ----
usbip_result=$(usbip_find_device "$CAMERA_VIDPID")
usbip_rc=$?
if [ $usbip_rc -eq 2 ]; then
    c_fail "usbipd.exe not found -- is usbipd-win installed on Windows?"
    FAILURES=$((FAILURES+1))
elif [ $usbip_rc -ne 0 ]; then
    c_fail "camera not present on Windows (VID:PID $CAMERA_VIDPID not seen by usbipd)"
    c_info "Check the physical USB connection on the Windows side."
    FAILURES=$((FAILURES+1))
else
    busid=$(printf '%s' "$usbip_result" | cut -f1)
    state=$(printf '%s' "$usbip_result" | cut -f2)
    c_ok "camera present on Windows: $CAMERA_VIDPID (busid $busid)"
    case "$state" in
        Attached)
            c_ok "usbipd status: Attached"
            ;;
        Shared)
            c_fail "Camera is Shared but not Attached"
            c_info "Fix (no admin needed): run start-kineo.sh, or directly:"
            c_info "    usbipd.exe attach --wsl --busid $busid"
            FAILURES=$((FAILURES+1))
            ;;
        NotShared)
            c_fail "Camera is NotShared (one-time setup never done, or was undone)"
            usbip_print_bind_instructions "$busid" | sed 's/^/    /'
            FAILURES=$((FAILURES+1))
            ;;
        *)
            c_warn "usbipd status: unrecognized ($state)"
            ;;
    esac
fi

# ---- 5. WSL sees the camera (lsusb) ----
if usbip_wsl_sees_camera "$CAMERA_VIDPID"; then
    c_ok "WSL sees camera"
else
    c_fail "WSL sees camera: NO (usbipd may say Attached, but lsusb disagrees)"
    c_info "Try again in a few seconds (enumeration lag), or re-run start-kineo.sh."
    FAILURES=$((FAILURES+1))
fi

# ---- 6. Aravis enumeration ----
if aravis_sees_serial "$CAMERA_SERIAL"; then
    c_ok "Aravis device: serial $CAMERA_SERIAL"
else
    c_fail "Aravis cannot see serial $CAMERA_SERIAL"
    c_info "If WSL sees the camera but Aravis doesn't, another process may already"
    c_info "hold it open (a stale bridge?) -- see stop-kineo.sh."
    FAILURES=$((FAILURES+1))
fi

# ---- 7/8/9. Bridge health (from the status file, not a TCP round trip --
# a live Kineo session may already be using the bridge's one connection
# slot) ----
status_json=$(python3 -c "
import sys, os
sys.path.insert(0, '$BRIDGE_DIR')
from bridge_status import read_status
d = read_status(port=$BRIDGE_PORT)
import json
print(json.dumps(d) if d else '')
" 2>/dev/null)

if [ -z "$status_json" ]; then
    c_fail "bridge reachable: NO (no fresh status file -- is kineo_camera_bridge.py running on port $BRIDGE_PORT?)"
    c_info "Fix: run start-kineo.sh, or start it manually (see wsl-camera/README)."
    FAILURES=$((FAILURES+1))
else
    c_ok "bridge reachable"
    width=$(python3 -c "import json,sys; d=json.loads(sys.argv[1]); print(d.get('width'))" "$status_json")
    height=$(python3 -c "import json,sys; d=json.loads(sys.argv[1]); print(d.get('height'))" "$status_json")
    pixfmt=$(python3 -c "import json,sys; d=json.loads(sys.argv[1]); print(d.get('pixel_format'))" "$status_json")
    if [ "$width" = "1920" ] && [ "$height" = "1200" ] && [ "$pixfmt" = "Mono8" ]; then
        c_ok "expected mode: 1920x1200 Mono8"
    elif [ "$width" = "None" ] || [ -z "$width" ]; then
        c_warn "camera mode: not yet configured (no CONFIGURE seen this session)"
    else
        c_warn "camera mode: ${width}x${height} ${pixfmt} (expected 1920x1200 Mono8)"
    fi
    age=$(python3 -c "import json,sys; d=json.loads(sys.argv[1]); print(d.get('last_frame_age_ms'))" "$status_json")
    streaming=$(python3 -c "import json,sys; d=json.loads(sys.argv[1]); print(d.get('streaming'))" "$status_json")
    if [ "$streaming" = "True" ]; then
        if [ "$age" != "None" ] && [ -n "$age" ] && [ "$age" -lt 2000 ] 2>/dev/null; then
            c_ok "last frame age: ${age} ms"
        else
            c_warn "streaming but last frame age is high or unknown: ${age} ms"
        fi
    else
        c_info "not currently streaming (idle -- expected when no Kineo session is active)"
    fi
    last_error=$(python3 -c "import json,sys; d=json.loads(sys.argv[1]); print(d.get('last_error'))" "$status_json")
    if [ "$last_error" != "None" ] && [ -n "$last_error" ]; then
        c_warn "last bridge error: $last_error"
    fi
    if [ "$VERBOSE" = "1" ]; then
        echo "--- verbose: full bridge status ---"
        echo "$status_json" | python3 -m json.tool
    fi
fi

if [ "$VERBOSE" = "1" ]; then
    echo "--- verbose: usbipd.exe list ---"
    usbipd.exe list 2>&1
    echo "--- verbose: lsusb ---"
    lsusb
fi

echo
if [ "$FAILURES" -eq 0 ]; then
    c_ok "all checks passed"
    exit 0
else
    c_fail "$FAILURES check(s) failed -- see above"
    exit 1
fi
