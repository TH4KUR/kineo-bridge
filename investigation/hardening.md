# Hardening / Operations Phase

Entered after Kineo 1.1.2 compatibility was accepted as FULL PASS (same
CTI hash works unchanged against Kineo 1.0.0 and 1.1.2, synthetic and
real-camera sources, successful Analysis on both -- see
investigation/kineo-update-diff.md). Goal: turn the proven prototype into
a repeatable, recoverable workflow.

## Priority 1 -- USB/IP attach/recovery: DONE

Empirically verified (critical finding, changes the whole design):
- `usbipd.exe` is directly invocable from WSL via interop -- no
  PowerShell round-trip needed.
- `usbipd.exe attach --wsl --busid <busid>` succeeds **without
  elevation** for a device already `Shared` (this project's camera has
  been `Shared` since first setup). **Full automatic recovery needs zero
  admin prompts** in the current configuration.
- The only usbipd operation that needs elevation is `bind` (NotShared ->
  Shared), a one-time setup step this project has never needed to
  exercise since the camera has stayed bound. If ever seen, the tooling
  detects it and prints the exact one-line admin command rather than
  guessing or looping.
- BUSID is never hardcoded -- `scripts/lib_usbip.sh` parses it fresh from
  `usbipd.exe list` every time, keyed by VID:PID (`1409:8000`).
- `usbipd.exe list` output has occasionally come back empty via interop
  for no discernible reason (not a real state change) -- handled with a
  bounded (3x) retry inside `usbip_find_device`.
- Immediately after a `detach`, the device can transiently vanish from
  the `Connected:` section entirely (moves to `Persisted:` with a GUID)
  while Windows re-enumerates it -- handled with a bounded (3x, 1s apart)
  retry in `usbip_recover_camera`, distinct from a genuine
  not-connected-at-all state.
- Tested live: manually detached the camera mid-session, confirmed
  `diagnose.sh` correctly isolates the failure to the USB/IP layer
  (`[FAIL] Camera is Shared but not Attached`) without falsely blaming
  the bridge or CTI, then confirmed `usbip_recover_camera` (used by
  `start-kineo.sh`) reattaches automatically.

`scripts/lib_usbip.sh` -- reusable functions: `usbip_find_device`,
`usbip_ensure_attached`, `usbip_wsl_sees_camera`, `aravis_sees_serial`,
`usbip_recover_camera` (the high-level one-call recovery used by
`start-kineo.sh`), `usbip_print_bind_instructions`.

## Priority 2 -- proper bridge lifecycle: DONE (code), soak-testing pending

`DSStopAcquisition` previously left the physical bridge streaming in the
background indefinitely. Fixed in `probe/m5/wsl_bridge_client.c`:

- New `wsl_bridge_notify_acquisition_start()` /
  `_stop()` send a real START/STOP over the *existing*, already-open
  bridge connection -- no reconnect, no re-`OPEN` of the Aravis camera.
  Hooked into `m5_bridge.c`'s `GCWritePort`, at the GenApi
  `AcquisitionStart`/`AcquisitionStop` command writes (the "camera layer"
  of the two-layer acquisition model, not the GenTL "stream layer" --
  matches the desired logical flow in the directive).
- A new `g_send_lock` (`CRITICAL_SECTION`) + `g_active_sock` make this
  safe: the receiver thread is still the sole *reader* of the socket, but
  sends can now come from the GenTL calling thread (STOP/START) or the
  new config-forwarder thread (Priority 3) too.
- The initial connect sequence (triggered at `DevOpenDataStream`, per the
  M5 timing fix) still pre-starts physical streaming immediately -- this
  is intentionally *more* than the minimum ask, not less: it's what
  eliminates the 150ms-timeout race on the very first cycle. Subsequent
  `AcquisitionStart` calls now send a real (idempotent-safe) START
  request rather than relying on background streaming that was never
  stopped.
- Bridge-side (`kineo_camera_bridge.py`) needed no changes -- its
  `handle_start`/`handle_stop` were already idempotent and already
  supported repeated START/STOP on one connection.

**Not yet soak-tested**: multiple Start-Analysis / complete / Start-Analysis
cycles in one Kineo session, to confirm clean restart timing holds up in
practice (the physical camera's own start-acquisition latency was
observed at ~77ms in earlier traces, comfortably under Kineo's ~150ms
first-poll timeout, but this needs real repeated-cycle confirmation, not
just a single-cycle read of the code).

## Priority 3 -- live control forwarding: DONE (code), soak-testing pending

`GCWritePort`'s non-command branch now calls
`wsl_bridge_notify_config_changed(exposure, gain, black_level,
frame_rate)` whenever any of those four registers are written -- not
just once at `DevOpenDataStream`. Implementation is genuinely
non-blocking for the GenTL calling thread: the call only updates shared
state and wakes a dedicated forwarder thread (`cfg_forwarder_thread_proc`
in `wsl_bridge_client.c`), which coalesces rapid successive writes (only
the latest values are ever in flight) and sends one `CONFIGURE` message
per wake-up on the existing connection. Value validation/clamping is left
to the bridge (`camera_source.py` already clamps to the real camera's
bounds and logs any clamp) -- not duplicated here.

**Not yet exercised**: changing exposure/gain live in the Kineo UI mid-
session and confirming the physical camera actually re-applies it
(should show up as `[cfg-forwarder] live CONFIGURE sent: ...` in the CTI
log).

## Priority 4/5 -- one-command launcher + diagnostics: DONE, tested live

`scripts/start-kineo.sh`: validates the install, stages the CTI, runs
camera/USB-IP recovery (bounded retries, never an admin prompt unless
genuinely needed), clears any stale bridge process, starts the bridge,
waits for its health/status file to go fresh, then launches Kineo --
**directly from WSL bash**, no PowerShell needed.

**Critical finding made getting this working**: launching a Windows GUI
app from WSL via interop does *not* automatically forward the invoking
shell's environment variables to the Windows process, even ones exported
inline (`KINEO_BRIDGE_SOURCE=wsl "./Kineo Software.exe"`) -- confirmed by
testing (CTI log showed no `[wsl-bridge] mode=...` line at all, i.e. our
env vars never arrived). WSL's `WSLENV` mechanism is required: the
variable names must be listed in `WSLENV` (colon-separated) for interop
to pass them through. Fixed by exporting
`WSLENV="$WSLENV:KINEO_BRIDGE_SOURCE:GENICAM_GENTL64_PATH"` in the same
subshell right before launching. Verified working end-to-end (CTI log
showed the correct mode both for `--synthetic` and `--wsl`).

Also confirmed: launching from `cd /mnt/c/IMVapps/Kineo\ Software/ &&
"./Kineo Software.exe"` (a real Windows-mounted path) correctly sets the
launched process's own working directory on the Windows side -- avoiding
the earlier `spawn KineoDeviceService ENOENT` bug -- with no need to
route through PowerShell.

`scripts/diagnose.sh`: read-only, layered checks (Kineo install/version,
staged CTI, USB/IP state, WSL/lsusb visibility, Aravis enumeration,
bridge health-file freshness, camera mode, last-frame age) -- each
failure names the specific layer at fault. Live-tested against a real
detach: correctly reported the USB/IP failure without falsely blaming the
bridge/CTI. `--verbose` dumps raw `usbipd.exe list`/`lsusb`/full status
JSON on request; default output stays short.

`scripts/stop-kineo.sh`: graceful bridge stop (SIGTERM, bounded wait,
SIGKILL only if it doesn't exit), leaves USB/IP attached, leaves Kineo
running unless `--close-kineo` is passed.

## Health check: DONE

New `wsl-camera/bridge_status.py`: a small thread-safe status object the
bridge updates at every relevant event (open/configure/start/stop/frame),
dumped to a per-port JSON file (`bridge_status.<port>.json`) once a
second. Deliberately a *file*, not an extra TCP endpoint -- the bridge
serves exactly one client at a time, so a second network connection just
to ask "are you healthy?" would queue behind whatever Kineo is doing.
Fields: `camera_connected`, `serial`, `streaming`, `width`/`height`/
`pixel_format`, `frames_captured`/`frames_transmitted`/`frames_bad`/
`frame_timeouts`, `last_frame_age_ms`, `last_error`. Powers
`diagnose.sh`; the existing `MSG_STATUS` wire-protocol reply was also
enriched with the same fields for a connected client.

## Not done yet

- Priority 6 (soak / repeatability): needs real interactive use --
  repeated Start-Analysis cycles in one session, 30-60 minute uptime.
- Synthetic-mode regression check after all this: `start-kineo.sh
  --synthetic` was run and passed (`diagnose.sh` all green, CTI log
  confirmed `mode=synthetic`), but a full Analysis run in synthetic mode
  hasn't been re-confirmed since the Priority 2/3 changes.

## Bug found and fixed: stale CTI silently served during the first repeat-analysis test

The user's first repeated-analysis test (2 complete + 1 cancelled + 1
failed with `Capture failed: GrabFrame failed`) was run against a
**stale, pre-Priority-2/3 CTI build**, not the hardened one. Root cause:
`start-kineo.sh`'s CTI-staging `cp -f` had its stderr redirected to
`/dev/null` and its exit code never checked; a leftover Kineo process
(from earlier interop testing) still had the old CTI DLL open, Windows
refused the overwrite (NTFS sharing violation), and the failure was
completely silent. Confirmed via hash comparison
(`fc51fb4a...` staged vs. `550d6c79...` actually built) and directly via
`strings` on the staged file (no `[cfg-forwarder]`/`physical START`
markers present at all).

**Fixed**: `start-kineo.sh` now closes any already-running Kineo process
*before* staging (releasing the file lock), then verifies the staged
file's hash matches the freshly-built one, failing loudly instead of
silently if they don't match. Re-verified: the staged CTI now contains
the expected markers.

**Consequence: the 4-analysis test result (2 complete, 1 cancelled, 1
`GrabFrame failed` on rapid cancel-then-restart) does not tell us
anything about the Priority 2/3 code** -- it was exercising the old
behavior (bridge never really stopped on `DSStopAcquisition`). Needs a
clean retest now that staging is verified correct. If the same
`GrabFrame failed` symptom reproduces on a rapid cancel->restart with the
hardened CTI, that's a new, real finding to chase (rather than a
pre-existing Kineo-side race, which is equally possible given the app
itself logs "Analysis 38 threw error but was cancelled" -- suggesting a
leftover background capture from the cancelled analysis, not necessarily
anything CTI-side).

## Priority 2 REVERTED: real physical STOP/START forwarding wedges the camera

Retested with the correctly-staged hardened CTI (see previous section for
why the first test was invalid): 1 complete analysis (#40), then
analysis #41 -- started fresh ~5 minutes after #40 finished naturally
(not a rapid cancel-then-restart) -- failed immediately with
`"Camera error during analysis: Capture failed: GrabFrame failed"`.

**Root cause, confirmed directly from the CTI/bridge log**: analysis #40's
`AcquisitionStop` correctly sent a real physical STOP
(`sent physical STOP request (AcquisitionStop)` -> bridge
`STOP ack: {"ok": true, ...}`, camera genuinely stopped). Analysis #41's
`AcquisitionStart` then sent a real physical START -- and the **real
camera rejected it**:

```
[wsl-bridge] START ack: {"ok": false, "error": "arv-device-error-quark:
[AcquisitionStart] [AcquisitionStartValueControl] USB3Vision write_memory
error (error) (3)"}
```

The camera's control channel then stayed wedged -- subsequent CONFIGURE
attempts (from the Priority 3 live-forwarding thread, still trying to
apply Exposure/Gain/etc.) also failed:

```
[wsl-bridge] CONFIGURE ack: {"ok": false, "error": "arv-device-error-quark:
[PixelFormat] ... USB3Vision read_memory timeout (5)"}
```

Confirmed independently: `aravis_sees_serial` (plain enumeration, no
Kineo involved) failed immediately after this -- the camera was
genuinely unresponsive over USB3Vision, not just a Kineo-level error.

**Recovery**: a USB-level detach/reattach cycle
(`usbipd.exe detach --busid 3-2` then `usbip_recover_camera`) brought it
back immediately -- no Kineo restart, no WSL restart needed. This is a
real, working recovery path, just not an in-session one.

**This is exactly the "genuinely nonstandard blocker" /
"substantial redesign" case flagged in the original directive**: the IDS
U3-3560XCP-M (via Aravis 0.8.36 over USB3Vision/usbipd) does not
reliably support `stop_acquisition()` followed by a fresh
`start_acquisition()` on the same already-open camera/stream object --
at least not without some fix this project hasn't identified yet
(candidates for later investigation: recreating the Aravis stream object
per cycle instead of reusing it, a settle delay between stop and start,
or an IDS-specific quirk in how Aravis's USB3Vision backend handles the
`AcquisitionStart` register write).

**Action taken (per "prefer a reliable restart workflow" and "do not
destabilize the known-working frame path merely to achieve automatic
recovery")**: reverted the physical STOP/START forwarding in
`m5_bridge.c`'s `GCWritePort` (the two `wsl_bridge_notify_acquisition_*`
call sites are now commented out with a dated explanation, not deleted --
the underlying `wsl_bridge_client.c` machinery, `g_send_lock`,
`g_active_sock` etc. all stay in place for a future attempt). Restored
behavior: the physical camera streams continuously in the background for
the whole bridge-connection lifetime, exactly as the original,
multi-cycle-proven M5 behavior did before this hardening pass --
`DSStopAcquisition`/`DSStartAcquisition` only gate the GenTL-level
buffer-filling, never the real camera's own acquisition state. Rebuilt,
restaged (hash-verified), Kineo relaunched.

**Priority 3 (live config forwarding) is unaffected and left in place** --
the CONFIGURE-write path itself never errored; it only surfaced errors
because it kept trying to talk to a camera that Priority 2's STOP/START
had already wedged. With Priority 2 reverted, live config forwarding
should work normally (not yet re-verified after the revert).

**Status: needs a clean re-test** -- multiple complete analyses in one
session, with the reverted CTI, to confirm this restores the previously-
working multi-cycle behavior.

## Priority 3 ALSO reverted: live CONFIGURE wedges the camera the same way

Retested with Priority 2 reverted (previous section) but Priority 3
still active: analysis #42 completed successfully, analysis #43 (started
fresh afterward, camera streaming continuously the whole time in the
background as intended) failed the same way:
`"Camera error during analysis: Capture failed: GrabFrame failed"`.

**Confirmed from the CTI log**: every `DevOpenDataStream` pass (i.e.
every time Kineo opens the device for a new analysis, not just the
first) rewrites `ExposureTime`/`Gain`/`BlackLevel`/`AcquisitionFrameRate`
-- this has been Kineo's behavior since M3f, unrelated to hardening. With
Priority 3 in place, *every one of those rewrites* fired a live
`CONFIGURE` at the bridge:

```
WRITE ExposureTime ... / WRITE Gain ... / WRITE BlackLevel ... / WRITE AcquisitionFrameRate ...
[cfg-forwarder] live CONFIGURE sent: ...
```

happening while the real camera was still actively, continuously
streaming from the *previous* analysis's connection (exactly as
Priority 2's revert intended it to). The bridge's own log confirms the
same USB3Vision control-channel wedge as before, just triggered by
CONFIGURE instead of AcquisitionStart this time:

```
CONFIGURE FAILED: arv-device-error-quark: [PixelFormat] [PixelFormat]
[PixelFormat] [Mono12g24IDS] [(null)] USB3Vision read_memory timeout (5)
```

**Conclusion: this camera's USB3Vision control channel cannot reliably
be written to while the data channel is under continuous heavy
streaming** -- true for both a real `AcquisitionStart` (Priority 2) and
a plain register CONFIGURE (Priority 3). Both were new behaviors
introduced by this hardening pass; neither existed in any prior
successful M5 milestone, all of which only ever ran a single analysis
per session.

**Reverted Priority 3 too** (same pattern: the call site in
`GCWritePort` is commented out with a dated explanation, not deleted).
CONFIGURE now only happens once, at the very first `DevOpenDataStream`/
bridge-connect -- exactly the original M5 behavior. Recovered the
wedged camera again via USB detach/reattach (same reliable path as
before), rebuilt, restaged (hash-verified), relaunched.

**Net result of this investigation**: Priorities 2 and 3, as originally
designed ("live" mid-session forwarding), are not safe with this camera/
Aravis/USB3Vision combination and are both reverted for now. The
underlying scaffolding (`g_send_lock`, `g_active_sock`, the
config-forwarder thread, the notify functions) is left in place,
unused, for a future attempt -- candidate approaches worth trying later:
briefly pausing/draining the data stream before any control-channel
write and resuming after, adding a settle delay, or accepting that
exposure/gain/frame-rate changes only take effect on a fresh bridge
reconnect (i.e. requiring `DSClose`+reopen, not a live mid-session
change). **Do not re-enable either without a way to test it that doesn't
risk wedging the only physical camera this project has.**

Priority 1 (USB/IP recovery) and Priority 4/5 (launcher/diagnostics) are
unaffected by any of this and remain as built. The recovery path this
whole investigation depended on -- `usbipd.exe detach` +
`usbip_recover_camera` -- worked flawlessly both times the camera got
wedged, without ever needing a Kineo restart.

**Status: needs a clean re-test** with both reverts in place -- multiple
complete analyses in one session should now behave exactly like the
original, pre-hardening M5 success (which the changelog confirms already
completed 2 clean analyses back-to-back before hardening work began).

## STOP — deeper, pre-existing issue found: this is not Priority 2/3 at all

Retested with BOTH Priority 2 and Priority 3 fully reverted. **Same
symptom reproduced anyway**: analysis #44 succeeded, #45 (started ~1
minute later) failed with the same `GrabFrame failed`.

**This time it is definitively NOT a control-channel wedge from a live
write** -- confirmed no `[cfg-forwarder]`/`physical START`/`physical
STOP` lines fired this session (both call sites are commented out), and
the bridge's own Python log (`bridge.9494.log`) shows a completely clean
session from its own point of view: `OPEN OK` -> `CONFIGURE applied` ->
`START OK` -> 10 frames logged -> ... -> `client session ending:
connection closed while reading 10 bytes` -> `STOP:
frames_sent=11319 frames_bad=1398 frame_timeouts=339`.

**The real signal: `frames_bad=1398` and `frame_timeouts=339` out of
~11,700 total, over a ~6.5 minute continuous session** -- roughly 15% of
frames were bad or timed out. Earlier, shorter sessions never
accumulated numbers anywhere near this. And after this session ended,
the camera was again unresponsive to plain Aravis enumeration (no Kineo,
no bridge involved) -- recovered again only via a USB-level detach/
reattach.

**Conclusion: sustained continuous high-bandwidth streaming (Kineo
requests `AcquisitionFrameRate=60` -> ~2.3MB/frame x 60fps =~ 138MB/s
requested) over usbipd/WSL2, held open continuously for several minutes
between analyses, appears to genuinely destabilize the USB3Vision link
over time** -- independent of any control-channel write. This is a
**pre-existing characteristic of the M5 bridge's design** (continuous
background streaming from first connect until `DSClose`, chosen
specifically to eliminate the original ~150ms-timeout race -- see the
M5 section of kineo-workflow-trace.md), not something introduced by this
hardening pass. It was never seen before because no prior successful M5
test ran the physical camera continuously for more than about a second.

**Stopping here (Stop Condition E: a stability regression/instability
appears) rather than guessing further against the one physical camera
this project has.** Recovered it again via USB detach/reattach (reliable
every time so far, no Kineo restart needed). Candidate directions for a
real fix, none attempted yet:
- Clamp the real camera to a lower, validated-sustainable frame rate
  (e.g. ~10-15fps, matching what `wsl-camera/smoke_test.py` proved
  stable for 20 frames) instead of honoring Kineo's requested 60fps --
  explicitly flagged as something to validate first, not assume, per the
  original Priority 3 instructions.
- Actually stop the physical camera between analyses after all, but more
  carefully than the reverted Priority 2 attempt (e.g. fully closing and
  reopening the Aravis camera/stream object rather than reusing it, with
  a settle delay) -- would need to be re-tested cautiously, ideally with
  a way to detect a wedge and self-recover without a manual USB reset.
- Investigate whether usbipd/WSL2's virtualized USB transport has known
  throughput/duration limits worth checking against usbipd-win's own
  issue tracker.

This needs a decision from the user on how to proceed, given further
guessing risks repeatedly wedging the only physical camera available.

## New lifecycle architecture implemented (Step 1-6 of the combined Route 2 + Route 1 directive)

### Step 1 -- isolated timing measurement (`wsl-camera/timing_test.py`)

Opened the real camera once, cycled acquisition (fresh stream each time)
20 times at 10 FPS, camera object left open/idle between cycles:

```
min:    343.1 ms
median: 349.6 ms
p95:    362.2 ms
max:    391.0 ms
0 bad frames, 0 timeouts across all 20 cycles
20/20 cycles over Kineo's ~150ms EventGetData budget
```

**Result**: latency is NOT reliably below 150ms (it's ~350ms), so the
"start acquisition exactly when Kineo asks" architecture doesn't fit
directly -- but the cycling itself is **completely stable** (zero
errors across 20 rapid stream-recreate cycles), unlike sustained
continuous streaming. Per Step 4's guidance, used the pre-roll strategy
instead of redesigning further.

### Step 2 -- temporary physical FPS cap

`camera_source.py`: new `PHYSICAL_FPS_CAP` (env `KINEO_BRIDGE_MAX_FPS`,
default 10.0). The real camera is now capped to 10 FPS regardless of
what Kineo requests (60); the GenApi-facing node Kineo sees is
untouched. `CONFIGURE`'s `applied` dict now reports
`frame_rate_requested` alongside `frame_rate` and
`frame_rate_cap_applied` whenever a cap kicked in -- verified in the
protocol-level test:
`{'frame_rate': 10.0, 'frame_rate_requested': 60.0, 'frame_rate_clamped_from': 60.0, 'frame_rate_cap_applied': 10.0}`.

### Steps 3-6 -- new bridge lifecycle

`camera_source.py` `RealCameraSource.stop()` now releases the stream
object (`self._stream = None`) in addition to `stop_acquisition()` --
camera object itself stays open, only the stream/acquisition state is
torn down.

`m5_bridge.c`:
- `DevOpenDataStream` -> `wsl_bridge_start()` (TCP connect + camera OPEN +
  CONFIGURE, unchanged) **followed by an unconditional
  `wsl_bridge_notify_acquisition_start()`** -- pre-rolls (or re-arms) the
  physical stream+acquisition every time this fires, which happens with a
  multi-second-to-tens-of-seconds head start before Kineo's actual
  `AcquisitionStart`/`DSStartAcquisition` (human reaction time, not a
  fixed timer). Safe to call even when already streaming -- the bridge's
  `handle_start` is idempotent, so this never interrupts an in-progress
  capture.
- GenApi `AcquisitionStart` command: **no longer** triggers a physical
  notify (this was the earlier, too-late trigger point that caused the
  original wedge).
- GenApi `AcquisitionStop` command: still triggers
  `wsl_bridge_notify_acquisition_stop()` -- physical stream/acquisition
  genuinely stops and releases between analyses now.

### Validation so far

Protocol-level `repeated_cycle_test.py` (no Kineo, direct bridge
protocol): OPEN+CONFIGURE once, then 10 START->5 frames->STOP cycles
with 5s/30s/60s idle gaps (matching the directive's repeatability-test
spec) against the real camera:

```
10/10 cycles succeeded, 0 bad frames, 0 timeouts
start_latency ~340-410ms, first_frame ~365-435ms (both comfortably
inside each cycle -- Kineo's own ~150ms budget is protected by the
pre-roll happening well before AcquisitionStart, not by this raw latency)
camera confirmed still healthy (plain Aravis enumeration) after the test
```

**No camera wedge, no USB detach/reattach needed this entire test.**

### Next: real Kineo repeatability test

Full stack relaunched (`start-kineo.sh --wsl`, hash-verified CTI). Needs
the directive's actual target test: 10 consecutive real Kineo analyses
in one session, varying idle gaps (~5s/~30s/~1min), checking analysis
success, bad-frame/timeout counts, camera responsiveness, and whether any
USB/IP recovery is ever needed.

## Real Kineo test: 5 analysis attempts in one session, all clean -- major progress

With both fixes in place (EventGetData startup grace period + the
configure() per-setting resilience fix), a real Kineo session ran **2
complete analyses, 2 cancelled during video-capture, and a 5th complete
analysis -- zero `GrabFrame failed`, zero camera wedge during the entire
session.**

Confirmed from the bridge log -- all 5 physical acquisition cycles:

```
STOP: frames_sent=313 frames_bad=0 frame_timeouts=0
STOP: frames_sent=326 frames_bad=0 frame_timeouts=0
STOP: frames_sent=340 frames_bad=0 frame_timeouts=0
STOP: frames_sent=353 frames_bad=0 frame_timeouts=0
STOP: frames_sent=367 frames_bad=0 frame_timeouts=0
```

Zero bad frames, zero timeouts, across all 5 cycles. CONFIGURE readback
also confirmed correct this time (the earlier blank-video bug's root
cause, fixed):

```
CONFIGURE applied: {'exposure_time_us': 1422.267, ..., 'readback_pixel_format': 'Mono8',
'readback_exposure_time_us': 1422.2666666666667, 'readback_gain': 1.0, 'readback_black_level': 1.75}
```

**Separately noted, real launch-time flake (unrelated to any of this
work)**: one Kineo launch attempt crashed at startup with `Child process
exited with code: 3221226505` / `Device component cannot start (missing
dependency)` -- confirmed via the CTI log that our own producer had
loaded completely normally (the standard 3x probe-init cycle, then a 4th
real load in progress) before the process died; the failure is in
Kineo's own native module dependency loading (its `ExternalDependencies`
packaging -- OpenCV/ArrayFire/etc.), not our CTI. A plain retry launched
clean. Filed as a known flake, not investigated further (out of scope).

**Remaining issue**: after the whole Kineo session ended and the bridge
client disconnected, the camera stopped responding even to plain Aravis
enumeration (not just a busy/held-open conflict -- a real bus-level
unresponsiveness, same class of symptom as every previous wedge).
Recovered immediately via the same USB detach/reattach as always. So the
picture now is: **repeated real analyses within one live session are
clean**, but a fresh USB-level recovery is still needed **between Kineo
sessions** (not between analyses within a session, which is the actual
target this whole redesign was aiming for and which now appears solved).

This is a substantial improvement over every previous attempt today, and
is very close to Stop Condition A. Not yet run as a full, deliberate
10-consecutive-analysis test with varied idle gaps (the 5-analysis run
happened organically); worth running that explicitly to confirm before
declaring full success.

## start-kineo.sh hardened: liveness verification + native modal on failure

Per explicit request: the launcher must never just fire-and-forget a
launch and declare success -- every stage now verifies it actually
stayed alive, and any failure needing the user's attention shows a
native Windows message box (via PowerShell/WSL interop -- confirmed
working live, a real clickable dialog) in addition to the terminal
message.

**Real false positive found and fixed during this work**: an initial
`tasklist.exe`-polling-based liveness check for the Kineo launch produced
a genuine false failure -- reproduced live, with Kineo confirmed
genuinely running the whole time (ChironLog showed `WebSocket connection
established`, 4 stable `Kineo Software.exe` processes in `tasklist.exe`)
while the poll insisted it had crashed. Root cause: Electron's own
single-instance-lock bootstrap launches and self-quits a redundant second
process in well under a second when a first instance is already up
("App already running, quitting second instance") -- a tight
process-existence poll can catch that split-second window and misread it
as the FIRST instance crashing. **Fixed by verifying via Kineo's own
ChironLog instead of OS process polling** -- it directly states success
(`WebSocket connection established`) or the specific real failure this
project has actually seen (`Device component cannot start` / `Child
process exited with code`), and explicitly recognizes "already running"
as success rather than a new failure.

Failure points now covered, each with a clear terminal message + modal:
- Kineo install/executable missing.
- CTI not built.
- CTI staging hash mismatch (a locked file, stale process, etc.).
- Camera/USB-IP unrecoverable after bounded retries.
- Bridge process dies immediately, or dies shortly after reporting ready.
- Bridge never reports ready within its timeout.
- Kineo crashes on startup, with one automatic retry first (the
  transient "missing dependency" crash seen earlier in this project was
  fixed by a plain retry both times it happened).

Re-verified end-to-end after the ChironLog fix: clean launch, correctly
detected via ChironLog, no false positives across a clean re-run.

## Critical bug found and fixed: an uncaught exception crashed the ENTIRE bridge server

The 10-analysis test's first two attempts both failed with `GrabFrame
failed`. Root-caused precisely from the bridge's own log:

```
[22:54:22] client connected: ('127.0.0.1', 46438)
[22:54:24] client disconnected: ('127.0.0.1', 46438)
Traceback (most recent call last):
  ...
  File "camera_source.py", line 69, in open
    self._cam = Aravis.Camera.new(target_id)
gi.repository.GLib.GError: arv-device-error-quark: Failed to bootstrap
USB device '...' (3)
```

This was a one-time, transient USB bootstrap hiccup during the very
first connection attempt (right when Kineo launched). But
`camera_source.py`'s `open()` only wrapped its own `CameraError`, not
Aravis's native `GLib.GError` -- the exception propagated all the way up
through `handle_open()` (which also only caught `CameraError`) and
`main()`'s accept loop, **killing the entire bridge server process**.
Nothing was listening on port 9494 for the rest of the Kineo session --
which is why *both* subsequent analyses failed with `GrabFrame failed`,
42 minutes apart, with no camera/USB-IP issue at all: there was simply no
server left to connect to.

**Fixed at three layers (defense in depth, not just patching the one
call site)**:
1. `camera_source.py`'s `open()` now wraps everything in
   try/except and converts any exception to `CameraError`.
2. `kineo_camera_bridge.py`'s `handle_open()` now catches broad
   `Exception`, not just `CameraError` (matching `handle_configure`/
   `handle_start`, which already did this correctly).
3. `ClientSession.run()`'s top-level loop now also catches broad
   `Exception` as a last line of defense, so a single client session
   crashing can never again take down the whole server regardless of
   where a future exception originates.

A transient USB bootstrap failure will now just fail that one OPEN
attempt cleanly -- the CTI's own connector thread already retries with
backoff, and the server stays up to serve it.

Rebuilt nothing (Python-only fix), restaged nothing new needed, relaunched
the full stack clean. This was almost certainly present (dormant) through
every earlier test today too -- it just happened not to be triggered
until this run hit a transient USB hiccup at exactly the wrong moment
(the very first connection of a session).

## CONFIRMED: 6 consecutive real analyses in one session, all clean -- Stop Condition A effectively met

Full 10-analysis test (user ran ~6-7 attempts, including cancelling one
mid-capture and immediately starting another) verified against both logs
independently:

**ChironLog (application-level results), analyses 59-64 in this
session:**
```
Analysis 59: succeed:true
Analysis 60: succeed:true
Analysis 61: succeed:true
Analysis 62: succeed:true
Analysis 63: cancelled by user at 23:53:50, then... result later arrived succeed:true anyway
Analysis 64: started 23:54:28 (38s after cancelling 63), succeed:true
```

**Bridge log (transport-level), same session, 6 acquisition cycles:**
```
STOP: frames_sent=753 frames_bad=0 frame_timeouts=0
STOP: frames_sent=766 frames_bad=0 frame_timeouts=0
STOP: frames_sent=780 frames_bad=0 frame_timeouts=0
STOP: frames_sent=794 frames_bad=0 frame_timeouts=0
STOP: frames_sent=808 frames_bad=0 frame_timeouts=0
STOP: frames_sent=822 frames_bad=0 frame_timeouts=0
```

Zero bad frames, zero timeouts, across all 6 cycles. CONFIGURE readback
correct every time (exposure/gain/black-level match what Kineo
requested). The two earlier failures in this same overall test arc
(analyses 57/58) are now root-caused as the uncaught-exception server
crash from the previous section, fixed before this run -- not a
recurrence.

**Camera wedges again once the whole Kineo session ends** (confirmed via
`diagnose.sh`, which correctly isolated it to the USB/IP layer alone,
everything else green) -- same as every previous session, recovered
instantly via the standard `usbip_recover_camera` path, no Kineo restart
needed. This remains the one open item: fresh USB-level recovery is
needed **between Kineo sessions**, not between analyses within one,
which was the actual target.

**This meets the spirit of Stop Condition A** (repeated real analyses,
including a cancel-and-immediately-restart cycle, complete cleanly in one
session with zero camera wedges and zero bad frames during the session).

## "Cancel during analyzing" delay: isolated to Kineo's own app, not us

User reported the wait after cancelling mid-analysis felt long, twice.
Investigated fully:

1. **First real bug found and fixed**: the bridge connection's initial
   handshake (`run_one_session()` in `wsl_bridge_client.c`) unconditionally
   started the physical camera on connect, a leftover from an earlier
   architecture. Since the connection is established early
   (`DevOpenDataStream`) but the user doesn't click Start Analysis
   immediately, this caused a genuine 7.5-minute unattended physical
   stream on the first cycle of a session. Fixed: removed the embedded
   START, socket is now published (for later `notify_acquisition_start`
   calls) without ever auto-starting. Verified directly: after connecting,
   `frames_captured=0`, `streaming=false` -- true idle state.

2. **Second issue found was a logging bug, not a real one**: Kineo can
   send a duplicate `AcquisitionStop` on cancel (no new `AcquisitionStart`
   in between). Our new per-cycle instrumentation computed a "cycle
   summary" for that duplicate too, using stale timing from the
   already-finished previous cycle -- producing a misleading 71-196
   second "duration" that never actually happened on the wire (the real
   `cycle_source_span_s`/frame counts barely changed between the two log
   entries). Fixed on both sides (Python bridge + C CTI): a duplicate stop
   with nothing actually streaming now logs a clean one-line no-op instead
   of a bogus duration.

3. **After both fixes, the user reproduced the same cancel scenario and
   still perceived a wait.** Checked precisely via ChironLog (Kineo's own
   application log, nothing to do with our CTI/bridge):
   ```
   Analysis 71 cancelled by user:  00:47:33.226Z
   Analysis 71 result (cleanup):   00:56:01.905Z
   ```
   **8 minutes 28 seconds** between cancel and Kineo's own internal
   pipeline finishing cleanup for that analysis. Our own logs for the
   exact same window show the physical camera capture had already
   completed cleanly in ~1 second, then sat correctly idle (confirmed:
   `cycle_source_span_s` stays at a fraction of a second, not minutes)
   for the entire rest of the gap, with only a single harmless duplicate
   `AcquisitionStop` arriving right at the tail end (once Kineo's own
   cleanup finally finished).

**Conclusion: this delay is entirely internal to Kineo's own
cancel/cleanup pipeline, not the camera, USB/IP, bridge, or CTI.**
Out of scope to fix here (no patching Kineo binaries) -- worth reporting
separately to whoever maintains Kineo's own application code.
