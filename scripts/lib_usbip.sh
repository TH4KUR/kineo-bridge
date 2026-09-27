#!/usr/bin/env bash
# lib_usbip.sh -- USB/IP detection and recovery for the Kineo camera
# bridge. Sourced by diagnose.sh and start-kineo.sh, never run directly.
#
# Empirically verified in this environment (2026-09-27):
#   - usbipd.exe is directly invocable from WSL via interop (no PowerShell
#     wrapper, no elevation needed for `list`).
#   - `usbipd.exe attach --wsl --busid <busid>` succeeds WITHOUT elevation
#     for a device already in the "Shared" state.
#   - `usbipd.exe bind` (NotShared -> Shared, a one-time setup step) is
#     documented by usbipd-win as requiring an elevated prompt; this
#     project has never needed to test that empirically because the
#     camera has stayed bound/shared since first setup -- if it's ever
#     seen NotShared, this library reports it and prints the exact
#     one-time admin command rather than guessing.
#
# BUSID is NEVER hardcoded -- always parsed fresh from `usbipd.exe list`,
# keyed by VID:PID, because it can change (different port, different
# hub, etc).

usbip_bin() {
    if command -v usbipd.exe >/dev/null 2>&1; then
        echo "usbipd.exe"
        return 0
    fi
    return 1
}

# Prints "<busid>\t<state>" for the given VID:PID (state is one of
# NotShared/Shared/Attached/Unknown), or nothing + exit 1 if the device
# isn't listed as Connected at all (i.e. not plugged into Windows, or a
# genuinely different device).
usbip_find_device() {
    local vidpid="$1" bin out attempt
    bin=$(usbip_bin) || return 2
    # WSL interop invoking usbipd.exe has occasionally produced empty/
    # truncated output for no discernible reason (observed empirically,
    # not a real device-state change) -- a short bounded retry (3x)
    # distinguishes that from a genuine "not connected".
    for attempt in 1 2 3; do
        out=$("$bin" list 2>/dev/null | awk -v vidpid="$vidpid" '
            /^Connected:/ { in_connected=1; next }
            /^Persisted:/ { in_connected=0; next }
            in_connected && NF >= 3 && $2 == vidpid {
                busid=$1
                if ($0 ~ /Not shared/)   { state="NotShared" }
                else if ($0 ~ /Attached/) { state="Attached" }
                else if ($0 ~ /Shared/)   { state="Shared" }
                else                      { state="Unknown" }
                print busid "\t" state
                found=1
                exit 0
            }
            END { if (!found) exit 1 }
        ')
        if [ -n "$out" ]; then
            printf '%s\n' "$out"
            return 0
        fi
        [ "$attempt" -lt 3 ] && sleep 0.3
    done
    return 1
}

# Ensures the device is Attached. Prints one machine-parseable status
# token to stdout and returns an exit code; never loops/retries itself
# (callers own their own retry/backoff policy) and never attempts an
# operation this library can't do unprivileged.
#
# Return codes: 0 = attached (already, or just now). 1 = attach attempt
# failed. 2 = usbipd.exe not found. 3 = device not shared (needs a one-
# time admin `bind`). 4 = device not connected to Windows at all.
usbip_ensure_attached() {
    local vidpid="$1" result busid state
    if ! usbip_bin >/dev/null; then
        echo "USBIPD_NOT_FOUND"
        return 2
    fi
    result=$(usbip_find_device "$vidpid") || { echo "NOT_CONNECTED"; return 4; }
    busid=$(printf '%s' "$result" | cut -f1)
    state=$(printf '%s' "$result" | cut -f2)
    case "$state" in
        Attached)
            echo "ALREADY_ATTACHED:$busid"
            return 0
            ;;
        Shared)
            if usbipd.exe attach --wsl --busid "$busid" >/dev/null 2>&1; then
                echo "ATTACHED:$busid"
                return 0
            else
                echo "ATTACH_FAILED:$busid"
                return 1
            fi
            ;;
        NotShared)
            echo "NOT_SHARED:$busid"
            return 3
            ;;
        *)
            echo "UNKNOWN_STATE:$busid"
            return 1
            ;;
    esac
}

# Prints the one-time admin command needed when a device shows NotShared.
# This is the ONLY usbip operation this project has ever needed elevation
# for; everything else (list, attach for an already-shared device) works
# from a normal WSL shell via interop.
usbip_print_bind_instructions() {
    local busid="$1"
    cat <<EOF
This device has never been shared with WSL (or was un-shared). That is a
one-time setup step that needs an elevated prompt. Run this in
Administrator PowerShell, once:

    usbipd bind --busid $busid

After that, this device stays "Shared" permanently (survives reboots);
attach/detach never need elevation again.
EOF
}

# 0 = WSL's own USB stack sees the device (lsusb), 1 = it doesn't (even if
# usbipd itself thinks it's Attached -- a genuine mismatch worth flagging
# distinctly rather than conflating with the usbipd-level state).
usbip_wsl_sees_camera() {
    local vidpid="$1"
    lsusb 2>/dev/null | grep -qi "ID ${vidpid}"
}

# 0 = Aravis can enumerate a device with this serial right now, 1 = it
# can't (camera not opened by anything else, driver/permission issue, or
# genuinely not visible yet -- caller decides how to react).
aravis_sees_serial() {
    local serial="$1"
    GI_TYPELIB_PATH="$ARAVIS_LIB_DIR" LD_LIBRARY_PATH="$ARAVIS_LIB_DIR" python3 - "$serial" <<'PYEOF' 2>/dev/null
import sys
try:
    import gi
    gi.require_version("Aravis", "0.8")
    from gi.repository import Aravis
except Exception:
    sys.exit(1)
Aravis.update_device_list()
target = sys.argv[1]
for i in range(Aravis.get_n_devices()):
    if Aravis.get_device_serial_nbr(i) == target:
        sys.exit(0)
sys.exit(1)
PYEOF
}

# High-level: try hard (but only through non-elevated means) to get the
# camera visible to Aravis. Returns 0 on success. Prints short, actionable
# lines as it goes -- never a wall of raw command output.
usbip_recover_camera() {
    local vidpid="$1" serial="$2"
    local token rc attempt

    if usbip_wsl_sees_camera "$vidpid" && aravis_sees_serial "$serial"; then
        return 0
    fi

    # Bounded retry (3x, 1s apart): immediately after a detach, the device
    # can transiently vanish from `usbipd.exe list`'s Connected section
    # while Windows re-enumerates it (observed empirically) -- not a real
    # NOT_CONNECTED state, just a race. Never loops unboundedly.
    for attempt in 1 2 3; do
        token=$(usbip_ensure_attached "$vidpid")
        rc=$?
        if [ $rc -ne 4 ]; then break; fi
        [ "$attempt" -lt 3 ] && sleep 1
    done
    case $rc in
        0)
            # usbipd thinks it's attached (or just attached it) -- give the
            # kernel a brief moment to enumerate before checking lsusb.
            for _ in 1 2 3 4 5; do
                usbip_wsl_sees_camera "$vidpid" && break
                sleep 0.5
            done
            if ! usbip_wsl_sees_camera "$vidpid"; then
                c_fail "usbipd reports Attached ($token) but WSL's own USB stack doesn't see it yet"
                c_info "This can be a slow enumeration -- try again in a few seconds, or replug the camera."
                return 1
            fi
            if ! aravis_sees_serial "$serial"; then
                c_fail "Camera visible in WSL but Aravis can't see serial $serial"
                c_info "Another process may already have it open (check for a stale bridge -- see stop-kineo.sh)."
                return 1
            fi
            return 0
            ;;
        2)
            c_fail "usbipd.exe not found -- is usbipd-win installed on Windows?"
            return 1
            ;;
        3)
            local busid="${token#NOT_SHARED:}"
            c_fail "Camera is present but never shared with WSL"
            usbip_print_bind_instructions "$busid"
            return 1
            ;;
        4)
            c_fail "Camera not connected to Windows at all (VID:PID $vidpid not in 'usbipd.exe list')"
            c_info "Check the physical USB connection."
            return 1
            ;;
        *)
            c_fail "usbipd attach failed ($token)"
            return 1
            ;;
    esac
}
