#!/usr/bin/env bash
# stop-kineo.sh -- safely stop the WSL bridge (graceful first, force only
# if it doesn't exit). Leaves USB/IP attached (no reason to detach the
# camera just because the bridge is stopping) and leaves Kineo itself
# running unless --close-kineo is explicitly passed.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib_common.sh"

BRIDGE_PORT="$BRIDGE_PORT_DEFAULT"
CLOSE_KINEO=0
for arg in "$@"; do
    case "$arg" in
        --port=*) BRIDGE_PORT="${arg#*=}" ;;
        --close-kineo) CLOSE_KINEO=1 ;;
        -h|--help)
            echo "Usage: stop-kineo.sh [--port=9494] [--close-kineo]"
            echo "  --close-kineo  also force-close the Kineo application (off by default)"
            exit 0
            ;;
    esac
done

pids=$(pgrep -f "python3.*kineo_camera_bridge\.py.*--port $BRIDGE_PORT" 2>/dev/null || true)
if [ -z "$pids" ]; then
    c_info "no bridge process found on port $BRIDGE_PORT"
else
    c_info "stopping bridge (pid(s): $pids)..."
    kill $pids 2>/dev/null   # SIGTERM -- the bridge's own signal handling
                             # is just default Python behavior (KeyboardInterrupt
                             # doesn't apply to SIGTERM, but the accept()/recv()
                             # loops unblock on the socket closing when the
                             # process exits, and there's no persistent state to
                             # corrupt -- a plain SIGTERM is a clean stop here)
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        still=$(pgrep -f "python3.*kineo_camera_bridge\.py.*--port $BRIDGE_PORT" 2>/dev/null || true)
        [ -z "$still" ] && break
        sleep 0.5
    done
    still=$(pgrep -f "python3.*kineo_camera_bridge\.py.*--port $BRIDGE_PORT" 2>/dev/null || true)
    if [ -n "$still" ]; then
        c_warn "bridge did not exit gracefully within 5s, force-killing (pid(s): $still)"
        kill -9 $still 2>/dev/null
    fi
    c_ok "bridge stopped"
fi

c_info "USB/IP attachment left as-is (camera stays attached to WSL)."

if [ "$CLOSE_KINEO" -eq 1 ]; then
    c_info "closing Kineo (--close-kineo was given)..."
    if taskkill.exe /IM "Kineo Software.exe" /F >/dev/null 2>&1; then
        c_ok "Kineo closed"
    else
        c_info "Kineo was not running"
    fi
else
    c_info "Kineo left running (pass --close-kineo to also close it)."
fi
