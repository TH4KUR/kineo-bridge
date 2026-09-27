# Kineo Real-Workflow GenTL Trace

Purpose: use real Kineo itself (not assumptions) to determine which GenTL
operations it actually requires during a normal user workflow, before
implementing any streaming stack. Method: process-local
`GENICAM_GENTL64_PATH` scoping (no persistent env var touched), the CTI's
own bounded logfile as the authoritative trace, and a separate
user-driven marker tool for action correlation (both use the same
`HH:mm:ss.fff` local-clock format).

## 1. Exact CTI build/version per pass

**Pass 1:** `probe/m3/m3a_standalone.c` (M3a/b/c-complete, unchanged from
the prior M2b real-Kineo verification run) + `kineo_bridge.xml`.
- Binary: `m3a_standalone.cti`, sha256
  `4a440f7ba63797bfda12da60b02ee14a840ec5c6c8509df49ecc1237919841de`
- `PROBE_VERSION` string: `"0.2-M1"`
- `DevGetNumDataStreams()` returns `0` (unchanged, deliberately, per
  instructions) — this is the exact CTI whose behavior Pass 1 is testing.

**Pass 2:** not yet run — gated on Pass 1's result (see §7-9 below).

## 2. User action timeline (from `markers.log`, Pass 1)

| Time | Action |
|---|---|
| 15:05:58.620 | marker session started |
| 15:06:26.214 | Press Not-Now button on update modal |
| 15:06:47.716 | press production nav tab |
| 15:07:12.080 | click name field and select a record |
| 15:07:35.013 | enter 5 into the volume field |
| **15:08:10.780** | **click start analysis** |
| **15:09:03.912** | **click cancel analysis** |
| 15:09:35.718 | marker session ended |

(Kineo itself was launched at 15:04:54.811 per the CTI log, ~64 seconds
before the marker session started — normal app-launch/settle time before
the user began interacting.)

## 3. Concise ordered GenTL timeline

**Phase A — app launch / initial device discovery (15:04:54.811 -
15:04:54.850, <40ms total):**

1. Three back-to-back `DllMain` attach/detach cycles in ~20ms — the first
   two are quick version/vendor probes (`GCInitLib`→`GCGetInfo(9,10)`→
   `GCCloseLib`→`GCInitLib`→`GCGetInfo(1=VENDOR)`→`GCCloseLib`→detach),
   consistent with `ids_peak`'s own internal "trial then real" construct
   pattern already seen in the official-bindings testing.
2. Third cycle is the real one: `GCGetInfo(ID)` → `TLOpen` →
   `TLUpdateInterfaceList` → `IFOpenDevice` → `GCGetPortInfo`(local
   "Device" port) → `GCGetNumPortURLs`(local port)→0 → `DevGetPort` →
   `GCGetPortInfo`(remote "Remote" port) → `GCGetNumPortURLs`(remote
   port)→**1** → `GCGetPortURLInfo` (URL/SCHEME/FILE_REGISTER_ADDRESS/
   FILE_SIZE/FILENAME, all queried) → **`GCReadPort` reads the full
   2366-byte XML in one call** → `DevGetInfo(DISPLAYNAME)`×4 →
   **`DevGetNumDataStreams` → 0** → `GCGetPortInfo`(remote port,
   PORTNAME)×4 more (node-map connect verification) → clean settle.

**Phase B — steady-state background polling (15:04:59 → 15:08:23, ~3.5
minutes, spans the entire user-interaction window through "start
analysis"):** `TLUpdateInterfaceList(timeout=100)` +
`IFUpdateDeviceList(timeout=300)` every ~5 seconds, nothing else. **No
GenTL activity at all** during: dismissing the update modal, navigating to
the production tab, selecting a record, or entering the volume value —
these are purely Kineo-internal UI/state operations, not GenTL calls.

**Phase C — triggered by "click start analysis" (15:08:23.738, ~13s after
the click at 15:08:10.780):** A **complete fresh re-open cycle**, byte-for-
byte the same shape as Phase A (three attach/detach cycles, then the real
one: enumerate → `IFOpenDevice` → port info → `GCGetNumPortURLs`→1 →
`GCGetPortURLInfo` → `GCReadPort` (full XML again) → `DevGetInfo`×4 →
**`DevGetNumDataStreams` → 0** again → 4× `GCGetPortInfo` → clean settle).
Port handle addresses differ from Phase A's (`...bd42d1`/`d0` vs
`...c142d1`/`d0`), confirming this is a genuinely new open, not reuse of
the earlier handle.

**Phase D — steady-state resumes (15:08:28 onward):** Identical ~5-second
`TLUpdateInterfaceList`/`IFUpdateDeviceList` polling resumes immediately
after Phase C, continuing through and past "click cancel analysis"
(15:09:03.912) with **no additional GenTL activity at or after the cancel
click** — cancel produced no observable CTI-level effect at all.

## 4. Call counts (whole Pass 1 log, 320 lines total — small enough that no
counter-based summarization was needed this pass)

| Call | Count |
|---|---|
| `TLUpdateInterfaceList` | 39 (2 per bring-up cycle ×2, + ~35 steady-state polls) |
| `IFUpdateDeviceList` | 37 |
| `GCGetPortInfo` | 20 (5 per full-open cycle × 2 cycles, +... see raw log) |
| `GCGetInfo` | 20 |
| `GCGetPortURLInfo` | 10 |
| `DllMain PROCESS_ATTACH`/`DETACH` pairs | 6 (3 per full bring-up × 2) |
| `GCReadPort` | 2 (once per full-open cycle, each reading the whole 2366-byte XML) |
| `DevGetInfo` | 8 (4 per cycle × 2) |
| `DevGetNumDataStreams` | 2 (once per cycle, **both return 0**) |
| `TLOpen` / `TLOpenInterface` / `IFOpenDevice` / `DevGetPort` | 2 each (once per full-open cycle) |
| `DevOpenDataStream` / `DevGetDataStreamID` / any `DS*` / any `Event*` (beyond the already-stubbed `GCRegisterEvent`) | **0** |
| `GCWritePort` | 0 |

## 5. Calls that occurred at startup regardless of UI action

All of Phase A (§3) — the entire initial producer/system/interface/device
bring-up and XML read happens automatically on app launch, before any user
interaction, and is identical in shape to the earlier M2b verification run.

## 6. Calls triggered specifically after the analysis/start action

**The entire Phase C re-open cycle** — this is the single, clear, only
GenTL-level consequence of clicking "start analysis" observed in this
trace. Everything else the user did (modal dismissal, navigation, record
selection, volume entry, and the later cancel click) produced **zero**
GenTL activity.

## 7. First unsupported operation

**None reached — Kineo never got far enough to hit an unsupported
operation.** It stops cleanly at `DevGetNumDataStreams() → 0` both times
(initial launch and again after "start analysis"), and — per the known
GenTL contract that a device reporting 0 data streams has nothing to open
— simply does not proceed to `DevGetDataStreamID`/`DevOpenDataStream`/any
`DS*`/`Event*` function. This is not a failure or crash; it is Kineo
correctly respecting what our CTI currently reports.

## 8. Inferred Kineo acquisition state machine (INFERENCE — not confirmed beyond what's directly observed)

```
[App launch]
     |
     v
[Enumerate producers/interfaces/devices, open device, read XML,
 build/connect GenApi node map]  <-- Phase A, automatic, no user action
     |
     v
[Idle / watching for interface & device list changes]  <-- Phase B/D,
     |                                                      ~5s poll loop
     | (user navigates UI freely -- no GenTL activity;
     |  this appears to be pure Kineo-internal state)
     v
[User clicks "Start Analysis"]
     |
     v
[Kineo RE-OPENS the device from scratch: fresh IFOpenDevice/DevGetPort/
 XML re-read/node-map reconnect -- NOT a reuse of the already-open handle
 from launch]  <-- Phase C
     |
     v
[DevGetNumDataStreams() == 0 -> Kineo concludes no stream is available
 -> silently gives up on starting acquisition, returns to idle polling]
     |
     v
[Idle / watching]  <-- Phase D, indistinguishable from Phase B
     |
     | (user clicks "Cancel Analysis" -- no observable GenTL effect;
     |  likely Kineo tears down purely-internal "waiting" UI state that
     |  was never backed by any actual open stream)
     v
[Idle / watching]
```

**Confidence notes:** The re-open-on-start-analysis behavior and the
stop-at-zero-streams point are both directly observed, high-confidence
facts. The *reason* Kineo re-opens rather than reusing its already-open
device handle is inferred (plausible explanations: Kineo may deliberately
re-validate device availability immediately before starting an acquisition
session, or "Start Analysis" may be architected to always establish a
fresh acquisition-specific device handle separate from a lighter
"watch/enumerate" handle held since launch — not distinguishable from this
trace alone). Whether Kineo silently gives up vs. surfaces a UI-visible
error to the user on the 0-streams condition was not directly observed
(the marker "click cancel analysis" suggests the user chose to cancel
rather than the app auto-failing, but this wasn't explicitly noted as an
error state in the markers — worth clarifying in a future pass if useful).

## 9. Exact minimum implementation required for the next iteration (Pass 2)

Per the original instructions' Pass-1-stops-at-zero-streams branch, exactly
this minimum, on a new versioned file (M3 source untouched):

```
DevGetNumDataStreams() -> 1
DevGetDataStreamID(index 0) -> "KineoStream0"
DevOpenDataStream("KineoStream0") -> valid, stable DS_HANDLE
```

No buffers, events, acquisition, frame data, or IPC yet. Every other
`DS*`/`Event*` export must remain present (required by the accepted
81-symbol surface) and continue returning the correct
`GC_ERR_NOT_IMPLEMENTED` rather than crashing if called. Then repeat the
exact same manual real-Kineo workflow (with fresh markers) to observe the
real, ordered sequence of the first operations Kineo performs once a
stream exists — do not guess this ordering.

## Pass 2 — Stream-Stub Producer (unexpected, precise, new finding)

**Exact CTI build:** `probe/m3/pass2/pass2_standalone.c`, sha256
`bf3bcf6d4496473f7e5852fb64f84ef359ebeb3bea23cf8c5b7ff811ab71609d`. Only
three functions changed from Pass 1's build, per the exact-minimum spec:
`DevGetNumDataStreams()` → `1`; `DevGetDataStreamID(index 0)` →
`"KineoStream0"`; `DevOpenDataStream("KineoStream0")` → a valid, stable
`H_DS` handle. No buffers/events/acquisition/IPC. Sanity-checked against
the official Python bindings first (no regression: node map still builds,
10 nodes, same as Pass 1) before the real-Kineo run.

**User markers (corrected):** the marker tool *was* run, but saved into
the old `workflow-trace-pass1\markers.log` (appended to Pass 1's file,
since the user re-ran the same script path) rather than the Pass 2
directory — confirmed by timestamp, not guesswork: this session ran
15:14:03.782 → 15:21:10.954, which only overlaps the Pass 2 trace window
(15:12:44 → 15:21:16), not Pass 1's (15:04:54 → 15:09:35).

| Time | Action |
|---|---|
| 15:14:09.558 | press not now |
| 15:14:30.215 | login |
| 15:14:37.941 | prodcution tab |
| 15:14:46.851 | select animal |
| 15:14:56.977 | enter volume |
| **15:15:04.851** | **start analysis** |
| **15:21:03.107** | **cancel analysis** |

**This confirms, with real timestamps rather than inferred shape-matching:**
- **"start analysis" (15:15:04.851) causally triggers the device re-open
  cycle** — the CTI's `DllMain PROCESS_ATTACH` for the second cycle fires
  at 15:15:14.087, **9.236 seconds later**. This is a direct, confirmed
  causal link, not a shape-based inference.
- Within that cycle, Kineo queries `DevGetDataStreamID` successfully but
  **genuinely never proceeds to `DevOpenDataStream`**.
- **"cancel analysis" came 5 minutes 58 seconds after "start analysis"**,
  and the CTI trace shows **zero GenTL activity of any kind for that
  entire ~6-minute window** (pure ~5s steady-state polling, unchanged) —
  Kineo was genuinely idle at the GenTL level for nearly six minutes, not
  doing something else invisible to us elsewhere in the same subsystem. It
  did not time out, retry, or produce any further port/device/stream call
  on its own; the user manually gave up after nearly 6 minutes of no
  visible progress.

**Result — new, precise, unanticipated finding:** In **both** the
launch-time cycle (15:12:44) and the second cycle (15:15:14, ~2m30s later,
matching Pass 1's "start analysis" timing shape), the sequence is
identical and stops one step earlier than expected:

```
... DevGetInfo(DISPLAYNAME)x4 -> DevGetNumDataStreams -> 1
                               -> DevGetDataStreamID(index=0)  [x2, succeeds]
                               -> (nothing else stream-related)
                               -> GCGetPortInfo(Remote,PORTNAME) x4
                               -> settle into steady-state polling
```

**`DevOpenDataStream` is never called, in either cycle** — confirmed by
exact grep count: 4 total hits for `DevGetDataStreamID|DevOpenDataStream`
across the whole 418-line log, all 4 being `DevGetDataStreamID` (2 per
cycle × 2 cycles), zero `DevOpenDataStream`. Kineo successfully retrieves
the stream ID (`"KineoStream0"`, no error) but does not attempt to open
it, in either the automatic launch-time enumeration or the
presumed-"start analysis" cycle. No error, crash, or unsupported-operation
signal of any kind — it simply doesn't proceed to that call, indistinguishable in shape from a producer that reported 0 streams as far as
what happens *next*.

**This does not cleanly match any of the four anticipated interpretation
cases** (not stuck at `GCGetPortInfo` [case B]; not failing on a missing
node [case C]; not reaching acquisition functions [case D]; and unlike
case A's literal wording, it does query the stream but does not "begin
querying feature nodes" in any way observable to us). Two plausible,
**unconfirmed** explanations, offered as inference only:
1. Kineo's "start analysis" trigger may gate on something read from the
   GenApi node map first (e.g. `PayloadSize`/`Width`/`Height`/access
   status) — which, per the already-known architectural limitation, is
   invisible to us with the current literal-`<Value>` node types — and
   silently declines to proceed to `DevOpenDataStream` if that check
   fails for a reason we can't currently observe.
2. The `DevGetDataStreamID` call observed here may be purely informational
   (e.g. populating a device-capabilities display) and structurally
   unrelated to whatever actually triggers `DevOpenDataStream`, which may
   require a distinct precondition or a different user action than what
   was exercised this run (no markers this pass to confirm exactly what
   was clicked and when).

**Not implementing further or converting nodes to register-backed types
to chase this** — per the standing instruction not to convert nodes for
tracing purposes and to stop once the first new operation/gap after
`DevOpenDataStream` is found. Since we're not yet even *at*
`DevOpenDataStream`, this is reported as a checkpoint requiring your
interpretation rather than something to keep pushing on unilaterally.

## Raw logs

Preserved at `logs/workflow-trace/pass1/` and `logs/workflow-trace/pass2/`:
- `pass1_trace.log` (320 lines) — Pass 1 CTI trace
- `pass2_trace.log` (418 lines) — Pass 2 CTI trace
- `markers.log` (17 lines total — both marker sessions appended to the
  same file since the user re-ran the script from the same path each
  time; lines 1-8 are the Pass 1 session, lines 9-17 are the Pass 2
  session) — copied into both `pass1/` and `pass2/` for convenience,
  content identical in both locations.

## M3f — Register-Backed Feature Trace (real Kineo, GenApi writes now observable)

**Exact CTI build:** `probe/m3/m3f/m3f_standalone.c` + `kineo_bridge_m3f.xml`,
sha256 `cc40754fb7ed9d37688164062061fcd0984b138509892978cb0a13f53cb95143`.
New versioned file derived from Pass 2 (M0/M1/M3/pass2 sources untouched).
`DevGetNumDataStreams()`→1, `DevGetDataStreamID(0)`→`"KineoStream0"`
unchanged from Pass 2. Converted `Width`/`Height`/`PixelFormat`/
`PayloadSize`/`OffsetX`/`OffsetY`/`ExposureTime`/`Gain`/`BlackLevel`/
`BrightnessAutoTarget`/`BrightnessAutoPercentile`/
`BrightnessAutoTargetTolerance`/`TriggerMode`/`TLParamsLocked` to real
register-backed GenApi nodes (`IntReg`/`FloatReg`/`Enumeration`+`pValue`),
each at a unique virtual address, backed by `GCReadPort`/`GCWritePort`
against an in-memory register table with symbolic per-name logging
(first 5 accesses logged in full, then counted silently, final tally on
unload). Added `AcquisitionStart`/`AcquisitionStop`/`ExposureStart` as
real `Command` nodes (write → log → auto-clear, so GenApi's `IsDone` check
sees immediate completion). Initial values set from the known
`camera_settings.json` schema and native sensor mode
(`Width`=1920, `Height`=1200, `PixelFormat`=Mono8, `PayloadSize`=2304000,
`ExposureTime`=1422.267, `Gain`=1.0, `BlackLevel`=1.75,
`BrightnessAutoTarget`=170, `BrightnessAutoPercentile`=30,
`BrightnessAutoTargetTolerance`=20).

**XML authoring bugs found and fixed (both real, both fixed before the
real-Kineo run, both confirmed via the fast official-bindings test loop):**
1. XML comments containing a literal `--` inside the body (not just at the
   closing `-->`) are not well-formed XML — two instances found and fixed.
2. Register-backed nodes (`IntReg`/`FloatReg`) do **not** support `Min`/`Max`
   as direct children (unlike the plain `Integer`/`Float` wrapper types) —
   caused a generic, imprecisely-located "unexpected element encountered"
   schema error. Root-caused by comparing against a known-working real
   GenApi XML already on this machine (Aravis's `arv-fake-camera.xml`),
   which uses the correct pattern: wrapper `Integer`/`Float` node carries
   `Min`/`Max`/`pValue`; the paired `IntReg`/`FloatReg` carries only
   `Address`/`Length`/`AccessMode`/`pPort`/`Sign`/`Endianess`. Fixed by
   converting every register-backed feature to this two-node pattern.

**Official-bindings test (before touching real Kineo): full PASS.** 42
nodes total (14 feature wrappers + their registers + Root/Remote/enum
entries). All initial values read back exactly as authored. All 7 tested
writable nodes (`ExposureTime`, `Gain`, `BlackLevel`,
`BrightnessAutoTarget`, `Width`, `Height`, `TLParamsLocked`) wrote and
read back correctly. All 3 command nodes (`AcquisitionStart`,
`AcquisitionStop`, `ExposureStart`) executed with `IsDone=True`.

**Real-Kineo workflow (markers: "start analysis" 15:38:36.204, "cancel
analysis" 15:39:30.642):**

- **At launch, before any user action** (15:37:31, immediately after the
  XML read and `DevGetDataStreamID` — i.e. in exactly the same position
  Pass 1/2 stopped cleanly): Kineo starts **writing real GenApi features**:
  `WRITE ExposureTime <- 1422.267000`, `WRITE Gain <- 1.000000`,
  `WRITE BlackLevel <- 1.750000`, in that exact order.
- **These three writes repeat on every ~5-second steady-state poll cycle**
  (`TLUpdateInterfaceList`/`IFUpdateDeviceList` immediately followed by
  the same 3 writes, same values, every time) — this is baseline
  steady-state behavior, **not** something "start analysis" specifically
  triggers. Our own logging correctly capped detailed output at 5
  occurrences per register then switched to silent counting, exactly as
  specified.
- **"start analysis" → fresh re-open 11.94 seconds later** (15:38:48.144)
  — same causal-trigger pattern confirmed a third time across three
  separate test passes now. The re-open cycle: enumerate → `IFOpenDevice`
  → port info → XML re-read (10172 bytes, matching the larger M3f XML) →
  `DevGetInfo`×4 → `DevGetNumDataStreams`→1 → `DevGetDataStreamID`×2
  (succeeds) → port-access-mode queries (`PORT_INFO_ACCESS_READ`/`WRITE`,
  both true) → **the same 3 writes again** → settles into steady-state,
  writes suppressed after 5 more occurrences.
- **"cancel analysis" (15:39:30.642): no observable CTI effect**, as in
  both prior passes — plain polling continues unbroken through and past
  it.
- **`DevOpenDataStream` was never called.** Zero occurrences in the
  316-line log, across both the launch cycle and the "start analysis"
  re-open cycle.
- **No other feature was read or written at all** — `Width`, `Height`,
  `PixelFormat`, `PayloadSize`, `OffsetX`, `OffsetY`,
  `BrightnessAutoTarget`, `BrightnessAutoPercentile`,
  `BrightnessAutoTargetTolerance`, `TriggerMode`, `TLParamsLocked` were
  never touched. **`AcquisitionStart`/`AcquisitionStop`/`ExposureStart`
  were never touched.**
- **No error, access-mode problem, or missing-node failure of any kind**
  was logged anywhere in this run.

**This is a new, third outcome — not a clean match for either of the two
anticipated branches.** It is not "zero register-backed activity" (three
real, successful writes did occur), but it is also not "Kineo progresses
to `DevOpenDataStream`" (it still never does, even with those writes
succeeding). The three fields written (`ExposureTime`/`Gain`/`BlackLevel`)
look like a **continuous background settings-sync** independent of the
acquisition-start workflow (writes begin before any user interaction, and
repeat on a fixed timer unrelated to the "start analysis" click) — not
part of whatever check gates `DevOpenDataStream`. Per the stop condition,
**not implementing further or converting more nodes/guessing at this
point** — this needs interpretation. Per instructions, this specific
outcome (some but insufficient register activity, no progression to
`DevOpenDataStream`, zero errors) most closely resembles — but does not
exactly match — the "if Kineo still goes silent with zero port activity,
inspect the KineoDeviceService code path" branch, since activity is not
literally zero, just apparently unrelated to the stream-open decision.

## Raw logs (M3f)

Preserved at `logs/workflow-trace/m3f/`:
- `m3f_workflow.log` (316 lines)
- `markers.log` (4 lines: start analysis 15:38:36.204, cancel analysis
  15:39:30.642)

## Step 1/2/3 — Application-Level Gate Identified (root cause found via Kineo's own logs, no debugger needed)

**Step 1 (WebSocket capture attempts) — both pre-authorized methods hit
genuine, confirmed technical dead ends, not guesswork:**
- **CDP/DevTools (preferred method):** enabled via the standard Electron
  `--remote-debugging-port=9222` flag (process-local, no file changes).
  Connected successfully, found exactly one renderer target, enabled
  `Network` domain — **zero WebSocket events captured** across a 100-second
  window spanning a full workflow. Root cause confirmed via static
  inspection of the app bundle (read-only `asar` extraction, using Node.js
  already present on this machine): `ws://localhost:9002` is opened from
  **`dist/Main/main.js`** — the Electron **main process** (Node.js `ws`
  library) — not the renderer. Chromium's `Network.webSocketFrame*` CDP
  events only instrument browser-side (renderer) WebSocket/fetch/XHR APIs;
  they architecturally cannot see Node-side socket usage in the main
  process, regardless of which target is attached to. This is a hard
  limitation, not a configuration miss.
- **Read-only loopback packet capture (fallback method):** used Windows'
  built-in `pktmon`, already present, filtered to port 9002, full packet
  size. **Zero packets captured** despite Kineo definitely connecting
  (`pktmon list` shows no loopback-capable capture component at all) —
  Windows' TCP/IP stack handles `127.0.0.1` traffic entirely internally,
  never touching the NDIS layer `pktmon` (and most capture tools) hook
  into. The standard fixes (WinDivert, Npcap's loopback adapter) both
  require installing a new capture driver, which is explicitly prohibited
  by this project's standing safety rules — not attempted.
- **Both pre-authorized methods for Step 1 are conclusively ruled out.**
  Answered instead via a better, already-available source (see below).

**A much better source was found instead: Kineo's own real runtime log
files**, at `Data/Logs/Application/ChironLog_*.log` (Electron/Node
application layer) and `Data/Logs/DeviceManager/KineoDeviceManagerLogs_*.log`
(native `KineoDeviceService.exe` layer) — read-only inspection, copies
preserved at `logs/kineo-own-logs/`.

**`ChironLog` (application layer) directly answers Step 1's original
question, more precisely than a packet capture would have:**
- `Analysis-Start` (or any WebSocket message text) is **not logged
  verbatim** in this file, but the application-level *consequence* is: an
  **`Analysis` database record is genuinely created and tracked** every
  time the user clicks Start — confirmed by 4 independent instances across
  today's sessions: `Analysis 9`, `10`, `11`, `12`, each eventually
  `cancelled by user` or (`Analysis 10`) automatically `marked as failed:
  Analysis timeout - periodic cleanup`. **This proves Analysis-Start is
  genuinely sent and accepted** at the application layer — it is not
  silently rejected or dropped.

**`KineoDeviceManagerLogs` (native layer) directly explains why it never
progresses further — a precise, repeatable, 100%-reproduced root cause:**
- **`grep -c` results across the whole day's log:** `"Camera opened
  successfully"` = 10, `"Camera initialized successfully"` = **0**,
  `"AcquisitionMode"` = **0**, `"Device has no datastream"` = **0**,
  `"Failed to initialize camera"` = **246**, `"Failed to load camera
  settings"` = **492**.
- **The exact, always-identical failure, on every single retry (every
  ~5 seconds, matching the steady-state poll cycle exactly):**
  ```
  Initializing the camera
  Loading camera settings from C:\IMVapps\Kineo Software\Data\camera_settings.json
  Settings file not found. Using default camera settings.
  Failed to load camera settings: Error-Code: 11 (PEAK_RETURN_CODE_NOT_FOUND)
      | Error-Description: There is no node with the given name (AcquisitionFrameRate)!
  Failed to initialize camera: Failed to load camera settings: [same message]
  ```
- **Root cause, stated precisely per the stop condition: Kineo does not
  call `DevOpenDataStream` because camera initialization itself fails on
  every attempt** — Kineo's settings-loading step looks up a GenICam node
  named `AcquisitionFrameRate`, which our XML does not define at all,
  gets `PEAK_RETURN_CODE_NOT_FOUND`, and treats this as a **fatal**
  "Failed to initialize camera" error. This happens **before** Kineo ever
  reaches the `AcquisitionMode`/datastream-check code path identified
  earlier via `KineoDeviceService.exe`'s string layout (that code is
  never reached at all — confirmed by zero occurrences of
  `"AcquisitionMode"` and `"Device has no datastream"` in the log,
  despite 246 initialization attempts). This is **not** stream-selection
  related, **not** a stream-ID naming issue (Step 2's hypothesis is now
  moot — the failure happens one step earlier, before any stream code
  runs at all) — it is squarely **camera-feature/GenICam-node-related**.
- This also explains why `ExposureTime`/`Gain`/`BlackLevel` writes
  succeeded in our M3f trace: Kineo applies known settings one field at a
  time in some order, and `AcquisitionFrameRate` is evidently processed
  after those three (or the load fails on the first missing field it
  hits, and `AcquisitionFrameRate` — not in our XML at all — is that
  field), aborting the rest of the settings-load and the whole
  camera-init call before touching `Width`/`Height`/`PixelFormat`/etc.

**Significant additional finding, not originally anticipated — flagging
prominently rather than burying it:** `ChironLog` shows
**`KineoDeviceService.exe` (the native child process) crashes with an
access violation (exit code `3221226505` = `0xC0000005`) in every single
test session today**, always within roughly 30-90 seconds of an Analysis
being cancelled/marked-failed (`Child process exited with code:
3221226505` appears 4 times, paired with `Analysis 9/11/12
cancelled`/`Analysis 10 marked as failed`). Electron detects this and
auto-restarts the child process (new PID each time), so the app keeps
appearing to run, masking the crash from the user. **Whether this crash is
directly caused by our probe CTI's behavior under repeated (246+) failed
retry cycles, or is a pre-existing Kineo/ids_peak bug unrelated to us, is
not yet determined** — the `KineoDeviceManagerLogs` file has no per-line
process/PID marker and restarts write to the same daily log file
seamlessly, so the exact crash moment can't be pinpointed from log
inspection alone (would need the runtime debugger approach, or Windows
Error Reporting / crash dump inspection, neither attempted this pass).
This is independent of the `AcquisitionFrameRate` finding but likely
related (both stem from the same repeated-failed-retry situation) and
should be resolved or at least understood before further M4 work, since a
native-process crash is a more serious category of problem than a missing
GenICam node.

**Smallest next experiment:** add a single `AcquisitionFrameRate` node
(`Float`, SFNC-standard, same register-backed pattern as `ExposureTime`)
to the XML and retest — per the stop condition, **not implemented in this
pass** (this isn't the trivial stream-ID A/B test the instructions
pre-authorized implementing directly; it's a new finding requiring your
sign-off first, and the crash finding should be weighed before continuing
regardless).

## M3g — AcquisitionFrameRate Fix (real Kineo, DevOpenDataStream reached for the first time)

**Step 0 (crash baseline, before this fix):** `Get-WinEvent` query of
Application Error (Event ID 1000), `KineoDeviceService.exe` faults only, 22
events over the prior two days. Two fingerprints: **A (dominant, 21/22)**
`ucrtbase.dll`, `0xc0000409`, fault offset `0x161b7c`, identical every
time; **B (rare, 1/22)** `KineoDeviceService.exe` itself, `0xc0000005`,
offset `0x7ad5`. Full detail in `logs/kineo-own-logs/crash_baseline_summary.md`.

**Step 1 (query real camera via Aravis/WSL):** blocked. `usbipd attach
--wsl --busid 4-2` succeeded (kernel-level enumeration confirmed via
`dmesg`, correct serial 4110010861), but `arv-tool-0.8` reported "No
device found" — the dynamically-attached USB device node
(`/dev/bus/usb/002/002`) is owned `root:root` with no group/world write
access and no udev rule grants it to a non-root group; `sudo
arv-tool-0.8` failed non-interactively ("a terminal is required to
authenticate"). Camera detached again (`usbipd detach --busid 4-2`) to
restore original state. Per the pre-agreed fallback, proceeded with a
conservative **synthetic** default/range instead of a real-camera-derived
one.

**Step 2 (new versioned XML/producer):** `probe/m3/m3g/m3g_standalone.c` +
`kineo_bridge_m3g.xml`, derived from the M3f baseline (M3f files
untouched). Added exactly one feature: `AcquisitionFrameRate`, `Float` +
`FloatReg` wrapper pattern (same as `ExposureTime`), address `0x20110`,
default **30.0**, range **1.0–60.0** fps — clearly marked SYNTHETIC in
both the C source and XML comments/tooltip, since the real node
characteristics couldn't be queried this pass. `PROBE_VERSION` bumped to
`"0.3-M3g"`.

**Step 3 (official IDS bindings sanity test):** full PASS after fixing one
new XML authoring bug (a literal `--` inside a multi-line comment body —
same class of bug as M3f, caught immediately by the same test loop). 44
nodes total (was 40 in M3f). `AcquisitionFrameRate` exists as a
`FloatNode`, reads its synthetic default (30.0), writes/reads-back
correctly (tested with 15.0). All 7 previously-tested M3f writable nodes
and all 3 command nodes still pass — no regressions.

**Step 4 (real Kineo retest), launched 16:33:19 local
(2026-09-26T11:03:19.900Z per ChironLog, PID 5352), same process-local
Machine-scope `GENICAM_GENTL64_PATH` setup (M3g dir prepended to the real
4-segment base value; confirmed User/Machine scopes untouched):**

- **(A) Old error disappears: YES, completely.** `KineoDeviceManagerLogs`
  at 16:33:20.026-20.040: `Camera opened successfully` →
  `Initializing the camera` → `Loading camera settings...` → `Settings
  file not found. Using default camera settings.` → **`Camera settings
  loaded and applied successfully`** → **`Camera initialized
  successfully`**. Zero occurrences of `Failed to load camera settings`
  or `Failed to initialize camera` anywhere in this run (compare: 246
  occurrences total before the fix, always on this exact
  `AcquisitionFrameRate` NOT_FOUND error).
- **(B) Kineo reads/writes it: YES.** Our CTI log:
  `WRITE ExposureTime <- 1422.267000`, `WRITE Gain <- 1.000000`,
  `WRITE BlackLevel <- 1.750000`, **`WRITE AcquisitionFrameRate <-
  60.000000`** (Kineo's own default, distinct from our synthetic 30.0
  default — confirms Kineo is actively driving this value, not just
  reading ours), then `READ Width -> 1920`, `READ Height -> 1200`.
- **(C) Next missing node/error: NONE.** No further `NOT_FOUND` or any
  other GenApi error appeared anywhere in the CTI log or
  `KineoDeviceManagerLogs` for the full ~2-minute observation window.
- **(D) AcquisitionMode/DevOpenDataStream/DS*/Event* reached: YES —
  `DevOpenDataStream id=KineoStream0 [Pass 2]` was called at 16:33:20.037,
  immediately after the `Width`/`Height` reads.** This is the **first time
  in the entire project** this call has ever been reached. (Literal string
  `AcquisitionMode` still does not appear in `KineoDeviceManagerLogs` for
  this run — Kineo's code apparently doesn't log that node name even on
  the success path; it was only ever an inferred marker from binary string
  layout, not a confirmed log line.) After `DevOpenDataStream`, the CTI
  settled into clean steady-state polling (`TLUpdateInterfaceList`/
  `IFUpdateDeviceList` every ~5s) for the full observation window, no
  DS*/Event* calls yet.
- **(E) KineoDeviceService crashes again: NO.** Same PID (5352) alive
  throughout the full ~2-minute test, no `Child process exited` line in
  `ChironLog` after the `Child process created with PID: 5352` entry, and
  a fresh `Get-WinEvent` query for Event ID 1000 shows no new crash event
  (latest remains 15:53:07, predating this test). **This matches CASE 1
  from the pre-agreed interpretation framework: the crash was downstream
  of the repeated failed-camera-init retry loop and has disappeared now
  that init succeeds on the first attempt.** Not fully conclusive from one
  ~2-minute run (no long-soak test performed), but a clean, non-ambiguous
  result for the observation window used.

**Note on workflow scope:** this was a background/headless session with no
GUI automation available, so the manual "start analysis"/"cancel analysis"
marker-tool workflow from M3f was not repeated. The M3f trace already
established that the critical camera-init sequence runs automatically at
launch and on every ~5s poll cycle, independent of user interaction — this
pass's launch-time observation is sufficient to answer all of Step 4's
questions A-E. Manual UI interaction (start/cancel analysis) has not yet
been retested against this fix and remains open for a future pass if
deeper application-level behavior (e.g. actual frame delivery once DS*/
buffer calls begin) needs verifying.

**STOP CONDITION MET: `DevOpenDataStream` was called for the first time.**
Per instructions, halting here — no DS*/buffer/streaming implementation
attempted this pass.

**Smallest next implementation step:** implement the minimal `DSGetInfo`/
`DSAnnounceBuffer`/`DSStartAcquisition`/buffer-queue path (M4 scope) and
observe exactly which DS*/Event* calls real Kineo makes next, using the
same iterate-against-the-real-app method that worked for M1-M3g — plus,
independently, resolve the Step 1 USB-permission blocker (e.g. a one-time
interactive `sudo` command or a udev rule) so `AcquisitionFrameRate`'s
default/range can be replaced with the real camera's actual values before
this ships.

## M3g retest with actual "start analysis" workflow — new node found (`AcquisitionMode`), CASE 4 confirmed

**Context:** the prior M3g Step 4 run (above) only observed the automatic
launch-time/poll-cycle camera-init path; the user had not yet navigated to
the start-analysis screen. Re-ran the identical M3g build (same CTI, same
XML, same process-local `GENICAM_GENTL64_PATH` scoping) with a fresh log
file (`kineo_probe_cti_m3g_step4b.log`) so the user could reach and use
that screen this time.

**Timeline (CTI log times local `HH:mm:ss.fff`; `ChironLog` times UTC,
offset local = UTC+5:30):**

- **16:39:58** — launch. Identical to the first M3g run: `ExposureTime`/
  `Gain`/`BlackLevel`/`AcquisitionFrameRate` all written successfully,
  `DevOpenDataStream` succeeds immediately, settles into clean 5s-interval
  background polling. No errors.
- **16:40:54 (= `ChironLog` 2026-09-26T11:10:54.255Z)** — user clicks
  start analysis. Our CTI log shows a full, clean re-open cycle
  (`DevClose` → enumerate → `IFOpenDevice` → port/XML re-read →
  `DevGetNumDataStreams`→1 → all 4 feature writes succeed →
  `DevOpenDataStream` succeeds again) — **no error at the GenTL/CTI layer
  at all.**
- **Same instant, at the application layer, `ChironLog` shows the actual
  failure:**
  ```
  Analysis 13 result: {"message":"Camera error during analysis: Capture
  failed: Error-Code: 11 (PEAK_RETURN_CODE_NOT_FOUND) | Error-Description:
  There is no node with the given name (AcquisitionMode)!",
  "succeed":false,"type":"Analysis-Start-Response"}
  ERROR: Camera error during analysis: Capture failed: ... (AcquisitionMode)!
  ERROR: Background analysis failed: Camera error during analysis: ... (AcquisitionMode)!
  ```
  **This is the new, previously-unseen error the user observed.** Kineo's
  "Capture" logic (invoked by Analysis-Start, one layer beyond simple
  camera init/settings-load) looks up a GenApi node named
  **`AcquisitionMode`**, which our XML does not define at all, gets
  `PEAK_RETURN_CODE_NOT_FOUND`, and marks that one analysis
  (`Analysis 13`) as failed. **This is a graceful, bounded, single failure
  — not a crash, not a retry storm** (compare: the old `AcquisitionFrameRate`
  bug retried every ~5s, 246+ times; this one surfaced once, as a normal
  failed-analysis result, and Kineo kept running normally afterward).
- **16:40:59** — a second, identical re-open cycle (5s later) — most
  likely Kineo's own retry/re-arm behavior after a failed analysis, or
  coincidental overlap with the steady-state poll; not independently
  significant.
- **16:41:04 / :09 / :14** — clean steady-state background polling
  resumes, no further writes or errors, matching baseline behavior.
- **16:41:16 (`ChironLog` 11:11:16.113Z: "Kineo process exiting. Killing
  child process...")** — user exits Kineo.
- **16:41:19.096** — `COMMAND AcquisitionStop`, then `DSGetInfo cmd=8`
  **NOT_IMPLEMENTED ×3** (each followed by 2× `GCGetLastError`), then
  `DevClose`. **This is Kineo's own graceful-shutdown cleanup sequence**
  (stop acquisition → check stream status → close), triggered by process
  exit ~3s earlier, not a live in-use error. `cmd=8` is not a value our
  `DS_INFO_CMD_LIST` subset defines (only 0,1,2,3,4,5,6,9,10,11 are
  implemented) — first time this code path has ever been reached in the
  whole project, so this specific stub gap was previously unknown/moot.
- **No crash.** Same `KineoDeviceService.exe` PID (8296) alive the entire
  session; fresh `Get-WinEvent` query for Event ID 1000 shows no new
  crash event (latest remains 15:53:07, from well before this run).

**Interpretation — matches CASE 4 from the pre-agreed framework exactly:**
"a new missing GenApi node appears before stream open → add only that
concrete node next." `AcquisitionMode` is a real SFNC-standard
`Enumeration` node (`Continuous`/`SingleFrame`/`MultiFrame`/etc.),
analogous to `TriggerMode`'s existing register-backed
`Enumeration`+`pValue` pattern already proven to work.

**STOP CONDITION MET (again): Kineo reported the next missing GenApi
node (`AcquisitionMode`).** Per instructions, halting here — `AcquisitionMode`
has not been implemented yet, pending sign-off.

**Smallest next implementation step (superseded by the M3h section below):**
add a single `AcquisitionMode` node (`Enumeration`, SFNC-standard, same
register-backed `Enumeration`+`pValue`+`IntReg` pattern as `TriggerMode`)
to a new versioned M3h build derived from M3g, sanity-test via the
official bindings, then retest against real Kineo the same way — watching
for whether `Analysis-Start` now succeeds end-to-end or a further
node/error appears. The `DSGetInfo(cmd=8)` stub gap (hit only during
Kineo's own shutdown cleanup, not blocking) can be deferred to the real M4
streaming-implementation pass rather than fixed reactively now.

## M3h — AcquisitionMode (real Kineo, next node found: `TriggerSelector`)

**XML definition:** `AcquisitionMode` as an SFNC-style writable
`Enumeration` backed by a virtual `IntReg`, exact register-backed pattern
as `TriggerMode`. Three entries with stable, self-assigned integer values
(consumers select by symbolic name, not raw integer, so no external
convention needed): `Continuous=0`, `SingleFrame=1`, `MultiFrame=2`.
Address `0x20120`, default `Continuous` (0). New versioned build:
`probe/m3/m3h/` (`m3h_standalone.c`/`kineo_bridge_m3h.xml`), derived from
M3g, M3g files untouched. `PROBE_VERSION` bumped to `"0.4-M3h"`. One new
XML authoring bug (same `--` inside a comment body class of bug as
before) caught and fixed before the official-bindings test.

**Official-bindings sanity result: full PASS**, no further fixes needed.
49 nodes total (was 44 in M3g). `AcquisitionMode` exists as an
`EnumerationNode`, default reads back as `'Continuous'`, all 3 entries
(`Continuous`/`SingleFrame`/`MultiFrame`) write and read back correctly
by symbolic name, and every prior M3g node/write/command still passes —
no regressions.

**Real Kineo retest** (same process-local `GENICAM_GENTL64_PATH`
scoping, fresh CTI log `kineo_probe_cti_m3h_step4.log`, launched
16:52:43 local / `2026-09-26T11:22:43.273Z` per `ChironLog`, PID 3796):

1. **Does the `AcquisitionMode` NOT_FOUND error disappear? YES,
   completely.** Zero occurrences anywhere in the CTI log or
   `KineoDeviceManagerLogs` this run.
2. **Value Kineo writes/reads for `AcquisitionMode`:** `WRITE
   AcquisitionMode <- 0` (i.e. `Continuous`) at 16:53:43.736, immediately
   before the start-analysis re-open cycle — Kineo accepts and uses our
   default without needing to change it.
3. **Next concrete missing node/error:** **`TriggerSelector`.** From
   `ChironLog` (2026-09-26T11:23:43.745Z = 16:53:43.745 local, the exact
   moment of the re-open cycle):
   ```
   Analysis 14 result: {"message":"Camera error during analysis: Capture
   failed: Error-Code: 11 (PEAK_RETURN_CODE_NOT_FOUND) | Error-Description:
   There is no node with the given name (TriggerSelector)!",
   "succeed":false,"type":"Analysis-Start-Response"}
   ```
   Same graceful, single, bounded failure pattern as `AcquisitionMode` and
   `AcquisitionFrameRate` before it — one failed analysis (`Analysis 14`),
   no retry storm, no crash.
4. **DS*/Event* calls reached:** same as M3g — `DevOpenDataStream`
   succeeds on every open/re-open cycle; `DSGetInfo(cmd=8)` is reached
   only during Kineo's own graceful-shutdown cleanup (×3, still
   `NOT_IMPLEMENTED`), not during live analysis. No `DSAnnounceBuffer`,
   `DSAllocAndAnnounceBuffer`, `DSQueueBuffer`, `EventRegister`, or
   `DSStartAcquisition` calls observed — the `TriggerSelector` lookup
   fails before Kineo's Capture logic reaches that stage.
5. **Does `AcquisitionStart` occur:** No (same as M3g) — only
   `AcquisitionStop` appears, as part of the app-exit cleanup sequence,
   not `AcquisitionStart`. Kineo's Capture path evidently fails the
   `TriggerSelector` lookup before ever writing the `AcquisitionStart`
   command.
6. **Does `KineoDeviceService` remain alive:** **Yes.** Same PID (3796)
   throughout; fresh `Get-WinEvent` query for Event ID 1000 shows no new
   crash event (latest remains 15:53:07, predating this test).

**`DS_INFO_CMD` cmd=8, symbolic meaning (recorded per instructions, NOT
implemented):** looked up in the official EMVA `GenTL_v1_5.h`
`STREAM_INFO_CMD_LIST` — value 8 is **`STREAM_INFO_IS_GRABBING`**
("Flag indicating whether the acquisition engine is started or not").
Consistent with Kineo's own shutdown-cleanup behavior (checking grab
state before/while issuing `AcquisitionStop`). Note: our own
`gentl_v2.h`'s `DS_INFO_CMD_LIST` numbering does **not** match the
official header's `STREAM_INFO_CMD_LIST` values (e.g. we have
`STREAM_INFO_PAYLOAD_SIZE=9`/`STREAM_INFO_IS_GRABBING=10`, spec has
`=7`/`=8`) — a latent ABI bug, currently harmless since `DSGetInfo` isn't
implemented at all regardless of which `cmd` value arrives, deferred to
the real M4 pass per instructions.

**Crash status:** the previous `KineoDeviceService.exe` access violation
(Fingerprint B, `0xC0000005`/offset `0x7ad5`) and the dominant UCRT
fast-fail (Fingerprint A, `0xC0000409`/offset `0x161b7c`) have **not
recurred** across either of the two most recent real-Kineo passes (M3g's
two runs and this M3h run) — four consecutive clean sessions since the
`AcquisitionFrameRate` fix. This remains an inference, not a proven root
cause: consistent with the crash being tied to the repeated
failed-camera-init retry loop that existed before that fix, but not
independently verified via a debugger or crash dump. Not investigated
further per instructions, unless it returns.

**STOP CONDITION MET: Kineo reported the next missing GenApi node
(`TriggerSelector`).** Per instructions, halting here — not implemented,
no speculative additional nodes added.

## M3i — TriggerSelector (real Kineo, GenApi node lookups exhausted — first live DS* blocker reached)

**XML definition:** `TriggerSelector` as an SFNC-style writable
`Enumeration` backed by a virtual `IntReg`, same register-backed pattern
as `TriggerMode`/`AcquisitionMode`. Values cross-checked against a real
working reference already on this machine (Aravis's
`arv-fake-camera.xml`): `FrameStart=0`, `AcquisitionStart=1` match that
reference exactly; `ExposureStart=2` added as a third, straightforward
entry (we already expose an `ExposureStart` command node). Address
`0x20130`, default `FrameStart` (0). New versioned build: `probe/m3/m3i/`
(`m3i_standalone.c`/`kineo_bridge_m3i.xml`), derived from M3h, M3h files
untouched. `PROBE_VERSION` bumped to `"0.5-M3i"`.

**Official-bindings sanity result: full PASS**, no XML fixes needed this
time. 54 nodes total (was 49 in M3h). `TriggerSelector` exists as an
`EnumerationNode`, default reads back as `'FrameStart'`, all 3 entries
(`FrameStart`/`AcquisitionStart`/`ExposureStart`) write and read back
correctly by symbolic name — including no naming collision between the
pre-existing `AcquisitionStart` `Command` node and the new
`TriggerSelector` enum entry of the same name (bindings resolve each
correctly in its own context). All prior M3h nodes/writes/commands still
pass — no regressions.

**Real Kineo retest** (same process-local `GENICAM_GENTL64_PATH`
scoping, fresh CTI log `kineo_probe_cti_m3i_step4.log`, launched
17:00:29 local / `2026-09-26T11:30:29.478Z` per `ChironLog`, PID 14780):

1. **Exact `TriggerSelector` value Kineo writes:** `WRITE TriggerSelector
   <- 2` (i.e. `ExposureStart`) at 17:02:47.746 local.
2. **Does the `TriggerSelector` NOT_FOUND error disappear? YES,
   completely.** Zero occurrences anywhere this run.
3. **Next missing node/error: NONE — GenApi node lookups are now fully
   exhausted for this analysis path.** No further `PEAK_RETURN_CODE_NOT_FOUND`
   of any kind appears. The blocker has moved one full layer deeper.
4. **Does Kineo access `TriggerMode` afterward? YES — exactly as
   anticipated in the "IMPORTANT OBSERVATION."** Immediately following
   `TriggerSelector`, the CTI log shows:
   ```
   WRITE AcquisitionMode              addr=0x20120 <- 0
   WRITE TriggerSelector              addr=0x20130 <- 2
   WRITE TriggerMode                  addr=0x200c0 <- 0
   READ  PayloadSize                  addr=0x20030 -> 2304000
   ```
   `TriggerMode` writes `0` (`Off`) successfully — no new error from this
   pre-existing node being touched in this new context.
5. **Does `AcquisitionStart` occur? No** — same as M3g/M3h, Capture fails
   before ever writing the `AcquisitionStart` command register.
6. **First live DS*/Event* call reached: YES — `DSGetInfo(cmd=12)`,
   called immediately after the `PayloadSize` read, in the live
   acquisition path (not shutdown cleanup this time).** Looked up in the
   official `STREAM_INFO_CMD_LIST`: cmd=12 is **`STREAM_INFO_BUF_ANNOUNCE_MIN`**
   ("minimum number of buffers to announce before starting acquisition").
   Per instructions, this **promotes `DSGetInfo` to a real M4
   requirement** — it is no longer only a graceful-shutdown-only stub
   gap. Kineo's own `ChironLog` confirms this precisely, quoting our own
   error string back verbatim:
   ```
   Analysis 15 result: {"message":"Camera error during analysis: Capture
   failed: Error-Code: 1 (PEAK_RETURN_CODE_ERROR) | Error-Description:
   [Function: DSGetInfo | Info-Command: 12 (STREAM_INFO_BUF_ANNOUNCE_MIN)
   | Error-Code: -1003 (GC_ERR_NOT_IMPLEMENTED) | Error-Text: DSGetInfo
   not implemented until M4","succeed":false,"type":"Analysis-Start-Response"}
   ```
7. **Analysis result:** `Analysis 15` failed, same graceful/bounded
   single-failure pattern as every previous node-gap (no retry storm, no
   crash). A second, identical device re-open cycle occurred ~3s later
   (17:02:50.751, `DevOpenDataStream` succeeds again) with no further
   activity after — consistent with Kineo's own internal re-arm behavior
   after a failed analysis, not a second user-initiated attempt (only one
   `Analysis` entry, no. 15, appears in `ChironLog` this run).
8. **`KineoDeviceService` stability: remains alive throughout.** Same PID
   (14780); fresh `Get-WinEvent` query for Event ID 1000 shows no new
   crash event (latest remains 15:53:07, predating this test) — fifth
   consecutive clean real-Kineo session since the `AcquisitionFrameRate`
   fix.

**Significance:** this is the first time in the whole project that
Kineo's Capture path has exhausted all GenApi node lookups and reached
genuine DS*/streaming territory. The remaining blocker is no longer
"which node is missing" but "implement the real M4 buffer/streaming
path" — `DSGetInfo`, `DSAnnounceBuffer`/`DSAllocAndAnnounceBuffer`,
`DSQueueBuffer`, `EventRegister`/`GCRegisterEvent`, `DSStartAcquisition`,
in that rough order, matching PLAN.md's original M4 scope.

**STOP CONDITION MET: a DS*/Event* function (`DSGetInfo`) became the
first live acquisition blocker.** Per instructions, halting here — no M4
implementation attempted this pass.

## M4a — Stream Discovery Shim (real Kineo, exact DS*/Event* call sequence learned)

**Purpose:** implement `DSGetInfo` properly (starting with
`STREAM_INFO_BUF_ANNOUNCE_MIN`, the exact command M3i found), leave every
other DS*/Event* function as a safe logged stub, and run one real-Kineo
trace to learn the exact next call sequence — without implementing the
stream engine itself yet. New versioned build: `probe/m3/m4a/`
(`m4a_standalone.c`/`kineo_bridge_m4a.xml`, XML content unchanged from
M3i — this is a pure GenTL-layer change, no new GenApi node), derived
from M3i, M3i files untouched. `PROBE_VERSION` bumped to `"0.6-M4a"`.

**Implementation:**
- Added `info_sizet()` helper (mirrors the existing
  `info_string`/`info_i32`/`info_bool8`/`info_u64` query-size-negotiation
  pattern: NULL buffer → report required size; buffer too small →
  `GC_ERR_BUFFER_TOO_SMALL` + required size; else copy + `GC_ERR_SUCCESS`).
- `DSGetInfo` now handles `STREAM_INFO_BUF_ANNOUNCE_MIN` for real,
  returning `INFO_DATATYPE_SIZET` = **1** (a standards-valid conservative
  minimum — nothing in GenTL v1.5 requires more than one announced buffer
  before acquisition can start). Every other `DS_INFO_CMD` still returns
  `NOT_IMPLEMENTED`, but now logs its symbolic name via a new
  `ds_info_cmd_name()` lookup table.
- **Fixed a latent, previously-harmless ABI bug** in `gentl_v2.h`'s
  `DS_INFO_CMD_LIST`: the enum's numeric values didn't match the official
  GenTL v1.5 header (e.g. old `STREAM_INFO_PAYLOAD_SIZE=9` vs spec's `=7`).
  This never mattered before (our code always switched on the raw numeric
  `iInfoCmd`, never the symbolic name), but now that `DSGetInfo` logs
  symbolic names for real, correctness matters — corrected to match the
  official header exactly (verified via the EMVA `GenTL_v1_5.h` reference
  fetched again this pass: full `STREAM_INFO_CMD_LIST` 0-13 plus
  `STREAM_INFO_CUSTOM_ID=1000`, with datatypes noted in comments).

**Regression check (official bindings):** re-ran the same M3i sanity
test — 54 nodes, identical output to M3i in every respect (this change
only touches GenTL-layer code the Python bindings test doesn't directly
exercise via GenApi nodes, but confirms no accidental breakage). No
dedicated new bindings test was needed for `DSGetInfo` itself since the
official `ids_peak` Python SWIG bindings don't expose a direct
`DSGetInfo` call path — real Kineo is the only practical caller.

**Real Kineo discovery trace** (same process-local
`GENICAM_GENTL64_PATH` scoping, fresh CTI log
`kineo_probe_cti_m4a_step4.log`, launched 17:12:15 local /
`2026-09-26T11:42:15.375Z` per `ChironLog`, PID 11332):

**Exact call sequence observed (live acquisition path, 17:13:03 local):**
```
WRITE AcquisitionMode              <- 0   (Continuous)
WRITE TriggerSelector              <- 2   (ExposureStart)
WRITE TriggerMode                  <- 0   (Off)
READ  PayloadSize                  -> 2304000
DSGetInfo cmd=12 (STREAM_INFO_BUF_ANNOUNCE_MIN)  -> rc=0 piType=12(SIZET) *piSize=8   [called twice: size query, then value]
DSAllocAndAnnounceBuffer size=2304000  [STUB -> NOT_IMPLEMENTED]
GCGetLastError ×2
```

1. **`DSAnnounceBuffer` or `DSAllocAndAnnounceBuffer`, or both?**
   **`DSAllocAndAnnounceBuffer` only**, called with `size=2304000` (exact
   match to `PayloadSize`). Per the pre-agreed ownership rule, this means
   **the producer (our CTI) must allocate and own the buffer memory**
   until revoke/teardown — Kineo does not supply its own buffer pointers
   via `DSAnnounceBuffer` in this flow.
2. **What `DSGetInfo` commands are requested?** Only
   **`STREAM_INFO_BUF_ANNOUNCE_MIN`** (cmd=12) in the live path, called
   twice (standard query-size-then-fetch-value pattern) — both succeeded
   cleanly this time. `STREAM_INFO_IS_GRABBING` (cmd=8) still only
   appears during the app-exit graceful-shutdown cleanup (×3, still
   `NOT_IMPLEMENTED`), same as every prior pass.
3. **Exact order:** `AcquisitionMode` → `TriggerSelector` → `TriggerMode`
   → `PayloadSize` (read) → `DSGetInfo(BUF_ANNOUNCE_MIN)` ×2 →
   `DSAllocAndAnnounceBuffer` **[fails here]**. No `DSQueueBuffer`,
   `EventRegister`/`GCRegisterEvent`, `EventGetData`, `DSStartAcquisition`,
   `DSGetBufferInfo`, `DSFlushQueue`, or `DSStopAcquisition` reached yet —
   `DSAllocAndAnnounceBuffer` is the very next call after `DSGetInfo`
   succeeds, and it fails immediately since it's still a stub.
4. **First unsupported function after `DSGetInfo` is fixed:
   `DSAllocAndAnnounceBuffer`.**

**Application-level confirmation**, `ChironLog`
(`2026-09-26T11:43:03.580Z` = 17:13:03.580 local, same instant as the CTI
log's `DSAllocAndAnnounceBuffer` call), quoting our own error text
verbatim again:
```
Analysis 16 result: {"message":"Camera error during analysis: Capture
failed: Error-Code: 15 (PEAK_RETURN_CODE_NOT_IMPLEMENTED) |
Error-Description: [Function: DSAllocAndAnnounceBuffer | Error-Code: -1003
(GC_ERR_NOT_IMPLEMENTED) | Error-Text: DSAllocAndAnnounceBuffer not
implemented until M4","succeed":false,"type":"Analysis-Start-Response"}
```
Same graceful, single, bounded failure pattern as every previous gap
(`Analysis 16`, no retry storm). A second identical re-open cycle occurred
~3s later (17:13:05.926, `DevOpenDataStream` succeeds again, no further
DS* activity) — Kineo's own internal re-arm behavior, consistent with
every prior pass.

**`KineoDeviceService` stability: remains alive throughout.** Same PID
(11332); fresh `Get-WinEvent` query for Event ID 1000 shows no new crash
event (latest remains 15:53:07, predating this test) — sixth consecutive
clean real-Kineo session since the `AcquisitionFrameRate` fix.

**STOP CONDITION MET (M4a-specific): the exact post-`DSGetInfo` call
sequence has been learned.** Per the M4a instructions ("stop after
obtaining this call sequence; do not implement the stream engine in the
same trace run"), halting here. M4b (minimal buffer state machine for
`DSAllocAndAnnounceBuffer`/`DSQueueBuffer`/`EventRegister`/`EventGetData`)
has **not** been implemented yet, pending sign-off.

## M4b — Producer-Owned Buffer Objects (real Kineo allocates exactly 1 buffer, next blocker: `DSQueueBuffer`)

**Scope:** implement `DSAllocAndAnnounceBuffer` + `DSRevokeBuffer` +
`DSGetInfo(STREAM_INFO_IS_GRABBING)` only. Explicitly not implemented this
pass: frame generation, `EventGetData`, `DSStartAcquisition`, any worker
thread, WSL IPC. New versioned build: `probe/m3/m4b/`
(`m4b_standalone.c`/`kineo_bridge_m4b.xml`, XML unchanged from M3i),
derived from M4a, M4a files untouched. `PROBE_VERSION` bumped to
`"0.7-M4b"`.

**Design (per instructions — small internal object, not a raw pointer as
the handle):**
- `buffer_t { magic, generation, base, capacity, private_ptr, queued, completed }`,
  fixed static table `g_buffers[MAX_BUFFERS=16]`, guarded by its own
  `CRITICAL_SECTION`.
- `BUFFER_HANDLE` = address of the slot (stable, array never moves/reallocates).
  Validated on every operation via `find_buffer_locked()`: pointer must
  fall within the table's bounds, at a correct stride offset, **and**
  `magic == BUFFER_MAGIC` — a malformed, out-of-range, or already-revoked
  handle is rejected cleanly (`GC_ERR_INVALID_HANDLE`) rather than
  dereferenced. A per-slot `generation` counter is tracked and logged for
  diagnostic value; full ABA-safe handle encoding (embedding generation
  into the handle itself) was judged unnecessary at this scope, per the
  instructions' own "narrowly focused" framing — noted as a possible
  future hardening if real reuse patterns ever demand it.
- **Ownership**: per M4a's finding, Kineo uses `DSAllocAndAnnounceBuffer`
  only (never `DSAnnounceBuffer`), so **our producer allocates and owns
  every buffer's memory**. `DSRevokeBuffer` frees that memory itself
  (`free(b->base)`) and returns the (now-stale, informational-only)
  address via `*pBuffer` plus the stored `*pPrivate` — the official
  GenTL v1.5 header has no inline documentation on this exact point (
  confirmed via the EMVA header text itself), so this follows the
  convention used by working GenTL producers for producer-allocated
  buffers, stated explicitly as an engineering judgment call rather than
  a guess from memory.
- **Defensive teardown**: `free_all_buffers_defensive()` sweeps and frees
  any still-announced buffer, called from both `DSClose` and `DllMain`'s
  `PROCESS_DETACH`, idempotent. Final tally on unload: buffers allocated
  (count + total bytes), revoked, and leaked-and-freed-defensively.
- `DSGetInfo(STREAM_INFO_IS_GRABBING)` now answered honestly: always
  `FALSE` this build (no `DSStartAcquisition` yet, so grabbing can never
  be true).

**Official-bindings test — extended, not a hand-rolled consumer:** used
only the official `ids_peak` Python bindings' high-level `DataStream` API
(`OpenDataStream()`, `NumBuffersAnnouncedMinRequired()`, `IsGrabbing()`,
`AllocAndAnnounceBuffer()`, `RevokeBuffer()`) — deliberately did **not**
call `ds.PayloadSize()` or inspect `buf.Size()`/`buf.BasePtr()`, since
those map to `DSGetInfo(STREAM_INFO_PAYLOAD_SIZE)` and
`DSGetBufferInfo(BUFFER_INFO_*)` respectively, neither of which real
Kineo has ever actually called (confirmed via the M3i/M4a traces) and
neither in scope for M4b. **Full pass:** `NumBuffersAnnouncedMinRequired()`
→ 1 (matches M4a), `IsGrabbing()` → `False`, 3 buffers allocated
successfully, all 3 revoked without error, a 4th allocation correctly
**reused slot 0** with a fresh generation after revoke, that one also
revoked cleanly. CTI log confirms: `allocated=4 (9216000 bytes) revoked=4
leaked-and-freed-defensively=0` — zero leaks, correct slot-reuse behavior,
no double-free, no crash.

**Real Kineo trace** (same process-local `GENICAM_GENTL64_PATH` scoping,
fresh CTI log `kineo_probe_cti_m4b_step4.log`, launched 17:21:48 local /
`2026-09-26T11:51:48.514Z` per `ChironLog`, PID 5788):

1. **How many times does Kineo call `DSAllocAndAnnounceBuffer`? Exactly
   once.** `DSAllocAndAnnounceBuffer #1 size=2304000 ... (slot=0 gen=1)`
   — matches `STREAM_INFO_BUF_ANNOUNCE_MIN`'s advertised minimum of 1
   exactly. Kineo allocates only the minimum it was told is required, not
   an arbitrary fixed count.
2. **Requested size: `2304000`** (exact `PayloadSize` match), single call.
3. **Private pointer behavior:** `pPrivate=0000000000000000` (NULL) —
   Kineo does not supply a private/user pointer at announce time.
4. **Returned handle scheme:** our slot-address handle worked
   transparently — Kineo passed it straight through to the next call with
   no issue.
5. **Exact next GenTL call after successful allocation: `DSQueueBuffer`**
   — called immediately (same millisecond) after the single allocation,
   **fails immediately** since it's still a stub. **No second allocation,
   no `DSGetBufferInfo`, no `EventRegister` — allocation happens exactly
   once, then straight to queuing.**
6. **Any `BUFFER_INFO` commands reached?** No — `DSGetBufferInfo` was
   never called this run.
7. **Any `Event*` operation?** No — `EventRegister`/`GCRegisterEvent`
   never called with a real event type this run (only the harmless,
   already-known `GCRegisterEvent id=1000` at TL-open time, unchanged
   since M1).
8. **Does `DSQueueBuffer` appear?** Yes — exactly once, immediately after
   the single allocation, and it is the actual failure point.
9. **Teardown/revoke sequence:** **`DSRevokeBuffer` is never called by
   Kineo.** The allocated-but-never-queued buffer is simply abandoned
   when the analysis fails. On app exit, the shutdown sequence
   (`AcquisitionStop` → 3×(`DSGetInfo(IS_GRABBING)`×2,
   `DSFlushQueue`-stub) → `DevClose`) runs as before, but **`DSClose` is
   never called and no final `DllMain PROCESS_DETACH`/buffer tally
   appears in this run's log at all** — confirmed by grepping the full
   log for both markers, present only in the three earlier
   trial-init/close cycles at process start, absent after the real
   session. This means our defensive-cleanup path (which triggers on
   `DSClose` or `DLL_PROCESS_DETACH`) **never actually executes on real
   Kineo shutdown**, because the parent Electron process hard-terminates
   the child process (`ChironLog`: *"Kineo process exiting. Killing child
   process..."*) rather than letting it unwind through GenTL
   close/DllMain notifications — `TerminateProcess`-style termination
   skips `DllMain` entirely on Windows.
10. **Memory cleanup result:** the single 2,304,000-byte buffer is never
    explicitly freed by our own code in this shutdown path (defensive
    cleanup never runs) — but since the whole process is terminated, the
    OS reclaims all of that process's memory regardless. Not a real leak
    in practice, but worth flagging: our C-level "defensive cleanup"
    design assumption (that `DSClose`/`DLL_PROCESS_DETACH` will run before
    the process disappears) does **not** hold for this specific
    hard-kill shutdown path, confirmed empirically rather than assumed.

**Application-level confirmation**, `ChironLog`
(`2026-09-26T11:52:54.587Z` = 17:22:54.587 local, same instant as the CTI
log's `DSQueueBuffer` stub hit):
```
Analysis 17 result: {"message":"Camera error during analysis: Capture
failed: Error-Code: 15 (PEAK_RETURN_CODE_NOT_IMPLEMENTED) |
Error-Description: [Function: DSQueueBuffer | Error-Code: -1003
(GC_ERR_NOT_IMPLEMENTED) | Error-Text: DSQueueBuffer not implemented until
M4","succeed":false,"type":"Analysis-Start-Response"}
```
Same graceful, single, bounded failure pattern as every previous gap. A
second identical re-open cycle occurred ~5s later (17:22:59.194,
`DevOpenDataStream` succeeds again, no further buffer/DS activity) —
Kineo's own internal re-arm behavior, consistent with every prior pass.

**`KineoDeviceService` stability: remains alive throughout.** Same PID
(5788); fresh `Get-WinEvent` query for Event ID 1000 shows no new
`KineoDeviceService` crash event (latest remains 15:53:07, predating this
test — the two most recent Event ID 1000 entries at query time were an
unrelated `AnyDesk.exe` crash, a different application on this machine,
not part of this investigation) — seventh consecutive clean real-Kineo
session since the `AcquisitionFrameRate` fix.

**STOP CONDITION MET: `DSQueueBuffer` is the first currently-unsupported
function reached after a successful buffer allocation.** Per instructions,
halting here — not implemented this pass. Allocation count (1, matching
the advertised minimum), allocation size (2304000), and the exact next
call (`DSQueueBuffer`) are now known, which was M4b's stated goal.

## M4c-1 — Queue the Producer-Owned Buffer (implemented; real-Kineo retest was anomalous, did not exercise it)

**Implementation:** new versioned build `probe/m3/m4c/`
(`m4c_standalone.c`/`kineo_bridge_m4c.xml`), derived from M4b, M4b files
untouched. `PROBE_VERSION` bumped to `"0.8-M4c"`.

- Replaced the `buffer_t` struct's ad-hoc `queued`/`completed` int flags
  with an explicit `buf_state_t` enum (`ANNOUNCED`/`QUEUED`/`COMPLETED`),
  per instructions — states are now explicit rather than inferred from
  pointer presence or boolean flags.
- Added an explicit input-pool FIFO (`g_input_queue[MAX_BUFFERS]` +
  count), even though only one buffer exists so far, so the design stays
  correct if the buffer count is ever increased later.
- `DSQueueBuffer`: validates the stream handle and the buffer handle
  (rejects stale/garbage handles cleanly, same `find_buffer_locked()`
  validation as M4b), **rejects illegal duplicate queueing**
  (`GC_ERR_RESOURCE_IN_USE` if already `QUEUED`), transitions
  `ANNOUNCED → QUEUED`, pushes onto the FIFO, and logs handle, base
  pointer, capacity, old/new state, and resulting queue depth.
- `DSRevokeBuffer` updated with a new safety rule (per the "buffer
  lifetime safety" instruction): **refuses to free a currently-queued
  buffer** (`GC_ERR_RESOURCE_IN_USE`, "must be flushed/dequeued first") —
  a queued buffer belongs to the stream's input pool and must not be
  freed out from under it. `DSFlushQueue` itself remains a logged stub
  (not implemented this pass — it has only ever appeared in the
  app-exit shutdown-cleanup sequence, never on a live path, so
  implementing it now would be speculative).
- Final unload tally now also reports total successful `DSQueueBuffer`
  calls alongside allocated/revoked/leaked counts.

**Official-bindings test — extended from M4b, still only the high-level
API:** added a check that queues buffer #0, confirms a duplicate
`QueueBuffer` call on the same buffer is rejected
(`BadAccessException` ↔ our `GC_ERR_RESOURCE_IN_USE`), confirms
`RevokeBuffer` on that same still-queued buffer is also rejected, then
revokes the other two (still-`ANNOUNCED`) buffers cleanly, and re-verifies
slot reuse after revoke. **Full pass.** CTI log confirms exact expected
sequence: `queue_depth=1` on the first queue, both rejections logged with
clear reasons, the two `ANNOUNCED` buffers freed normally, and — since
this test exits normally (not a hard-kill) — the defensive-teardown path
correctly swept up the one still-queued buffer at `DllMain
PROCESS_DETACH` (`allocated=4 queued=1 revoked=3
leaked-and-freed-defensively=1`), confirming the design handles a
mid-lifecycle buffer at normal shutdown correctly too.

**Real Kineo retest — ANOMALOUS, did not reach `DSQueueBuffer` at all**
(process-local `GENICAM_GENTL64_PATH` scoping unchanged, fresh CTI log
`kineo_probe_cti_m4c_step4.log`, launched 17:31:15 local /
`2026-09-26T12:01:15.428Z` per `ChironLog`, PID 2032):

- The CTI log shows only the normal launch-time sequence (feature writes,
  `DevOpenDataStream` succeeds) followed by **~4 minutes of pure clean
  steady-state polling with zero re-open cycles, zero buffer allocation,
  and zero `DSQueueBuffer` calls at all** — a start-analysis attempt
  never reached our producer this time.
- **`ChironLog` explains why — an unrelated application-level problem
  occurred first:** at `12:01:46.816Z`, `ERROR: Error in User-Login |
  Error: Error invoking remote method 'User-Login': Error: Login
  failed`. Then, ~3.5 minutes later at `12:05:29.067Z`:
  ```
  Analysis 18 result: {"message":"Device status error:Device not
  ready.","succeed":false,"type":"Analysis-Start-Response"}
  ```
  This is a **new, different kind of error never seen before in this
  project** — not a GenApi node lookup failure, not a GenTL DS*/stub hit;
  it never touched our CTI at all (confirmed: zero corresponding activity
  in the CTI log at that timestamp). It looks like an application-level
  device-readiness/session-state gate inside Kineo itself, most likely
  connected to the login failure a few minutes earlier, unrelated to the
  camera-bridge/GenTL work this whole project has been exercising.
- **Per the pre-agreed CASE E guidance** ("if Kineo becomes idle,
  inspect its own log for the next application-level failure before
  adding stream functionality blindly") — since this doesn't match any
  of CASE A-D and doesn't represent the intended `DSQueueBuffer` boundary
  at all, **nothing was implemented based on this run.** This needs a
  clean retest (fresh Kineo launch, successful login, normal start
  -analysis workflow) to actually observe what happens after
  `DSQueueBuffer` — that observation still has not been made yet.
- **`KineoDeviceService` stability: remains alive throughout, no crash.**
  Same PID (2032); fresh `Get-WinEvent` query for Event ID 1000 shows no
  new crash event (latest remains 15:53:07 for `KineoDeviceService`
  specifically; the two most recent Event ID 1000 entries overall were
  unrelated `AnyDesk.exe` crashes on this machine).

**Not a stop condition in the M4c-1 sense** (no new GenApi/GenTL boundary
was actually reached) — this is a data-collection miss, not a finding.
`DSQueueBuffer`'s implementation is believed correct per the
official-bindings test, but **has not yet been exercised by real Kineo**.
A clean retest of the same start-analysis workflow is needed before M4c-2
(event model) can be scoped with real evidence.

## M4c-1 clean retest — Kineo allocates 16 buffers in a tight alloc→queue loop, hits our own artificial cap

**Retest** (fresh Kineo launch, login confirmed successful this time,
same M4c CTI, fresh log `kineo_probe_cti_m4c_step4b.log`, launched
17:38:30 local / `2026-09-26T12:08:30.609Z` per `ChironLog`, PID 7468):

**Exact call sequence observed (live acquisition path, 17:39:31.088-096
local, ~8ms total):**
```
WRITE AcquisitionMode <- 0 / TriggerSelector <- 2 / TriggerMode <- 0
READ  PayloadSize -> 2304000
DSGetInfo(STREAM_INFO_BUF_ANNOUNCE_MIN) x2 -> 1
DSAllocAndAnnounceBuffer #1 (slot=0) -> DSQueueBuffer (queue_depth=1)
DSAllocAndAnnounceBuffer #2 (slot=1) -> DSQueueBuffer (queue_depth=2)
... (alternating alloc/queue, one pair per buffer) ...
DSAllocAndAnnounceBuffer #16 (slot=15) -> DSQueueBuffer (queue_depth=16)
DSAllocAndAnnounceBuffer #17 -> RESOURCE_EXHAUSTED (buffer table full, MAX_BUFFERS=16)
```

**Significant finding, flagged prominently rather than buried: Kineo does
NOT stop at the advertised `STREAM_INFO_BUF_ANNOUNCE_MIN=1`.** It
allocates far more — a tight `DSAllocAndAnnounceBuffer` →
`DSQueueBuffer` pair, immediately repeated, all the way up to **exactly
`MAX_BUFFERS` (16) — our own compile-time table size limit** — at which
point it hits `GC_ERR_RESOURCE_EXHAUSTED` and stops, **not because it
reached its own desired count, but because our own implementation ran
out of table slots.** The 16-buffer stopping point in this run is an
artifact of our code, not a discovery about Kineo's real target buffer
count, which remains **unknown and likely higher than 16.** This
directly supersedes the earlier interpretation from M4b/M4c-1's first
(anomalous) retest, where only 1 buffer was allocated before
`DSQueueBuffer` failed — that halt was because queuing failed
immediately, not because Kineo only wanted 1 buffer; now that queuing
succeeds, its real appetite is shown to be much larger.

**Application-level confirmation**, `ChironLog`
(`2026-09-26T12:09:31.101Z` = 17:39:31.101 local, same moment as the 17th
alloc attempt), quoting our own error text verbatim:
```
Analysis 19 result: {"message":"Camera error during analysis: Capture
failed: Error-Code: 1 (PEAK_RETURN_CODE_ERROR) | Error-Description:
[Function: DSAllocAndAnnounceBuffer | Error-Code: -1020
(GC_ERR_RESOURCE_EXHAUSTED) | Error-Text: DSAllocAndAnnounceBuffer:
buffer table full","succeed":false,"type":"Analysis-Start-Response"}
```
Same graceful, single, bounded failure pattern as every previous gap —
`Analysis 19` failed cleanly, no crash, no retry storm within the burst
itself (all 16 successful allocations queued correctly, only the 17th
was rejected). Two further re-open cycles followed (17:39:31.176 and
17:39:36.216, ~5s apart) with only the routine feature writes and
`DevOpenDataStream` — **the 16x alloc/queue burst does not repeat on
these re-opens**, confirming it's tied specifically to the Analysis-Start
attempt, not the background poll cycle. Shutdown sequence (`AcquisitionStop`
→ 3×(`DSGetInfo(IS_GRABBING)`×2, `DSFlushQueue`-stub) → `DevClose`)
followed at 17:39:48, same pattern as every prior pass, `DSClose` never
called (consistent with the M4b hard-kill finding).

**`KineoDeviceService` stability: remains alive throughout.** Same PID
(7468); fresh `Get-WinEvent` query for Event ID 1000 confirms no new
`KineoDeviceService` crash event (latest remains 15:53:07) — eighth
consecutive clean real-Kineo session since the `AcquisitionFrameRate`
fix.

**Per the stop condition, halting here without raising `MAX_BUFFERS` or
implementing anything further this pass** — this is exactly the
"another `DSAllocAndAnnounceBuffer`" branch the instructions explicitly
anticipated as a possible outcome. The true minimum viable buffer count
Kineo needs is still unknown (definitely >16); the smallest next step is
to raise `MAX_BUFFERS` to a more generous value (e.g. 64 or higher, a
number GenTL producers commonly use for ring buffers) and re-run the
identical Start Analysis workflow to observe where Kineo's real
allocation loop actually stops on its own.

## M4c-1b — Controlled Buffer-Capacity Discovery (Kineo's natural pool size is >64, still unknown)

**Implementation:** new versioned build `probe/m3/m4d/`
(`m4d_standalone.c`/`kineo_bridge_m4d.xml`), derived from M4c, M4c files
untouched. `PROBE_VERSION` bumped to `"0.9-M4d"`.

- `MAX_BUFFERS` raised from 16 to **64** — a deliberate, bounded safety
  ceiling for this controlled experiment (each buffer is 2,304,000 bytes,
  so 64 is ~147MB, acceptable), not unbounded allocation. The table
  itself stays a small static array of handle/metadata slots; payload
  memory is still only `malloc`'d lazily, one buffer at a time, exactly
  when `DSAllocAndAnnounceBuffer` is actually called — nothing is
  preallocated.
- `STREAM_INFO_BUF_ANNOUNCE_MIN` unchanged, still returns `1`.
- **Logging overhauled per instructions:** first 5 allocations and first
  5 queues logged in full (matching the existing `REG_LOG_LIMIT`
  convention), then switched to silent counting. Added a generic
  `note_buffer_burst_boundary()` hook, called at the top of every
  plausible "next function" (`DSGetInfo`, `DSGetBufferID`,
  `DSGetBufferInfo`, `DSGetBufferChunkData`, `DSFlushQueue`,
  `DSStartAcquisition`, `DSStopAcquisition`, `GCRegisterEvent`,
  `EventGetData`/`EventGetInfo`/`EventGetDataInfo`/`EventFlush`/`EventKill`,
  `DSClose`, `DevClose`, and the `COMMAND` write path for
  `AcquisitionStart`/`AcquisitionStop`) — logs a single clear summary
  line the exact moment Kineo moves on to a different function after a
  burst: buffer count this burst, alloc-number range, total bytes, max
  queue depth, and the exact next call. A separate, distinctly-worded log
  fires if the ceiling itself is hit mid-burst (not a "next function"
  transition — a deliberate limit).

**Official-bindings regression test:** identical to M4c-1's — full pass,
no behavior change detected (this build doesn't touch anything the
Python-bindings test exercises differently). Directly verified the new
burst-boundary logging mechanism works correctly: allocated 4 buffers,
queued 1, then closing the library correctly triggered a boundary log
naming `DSGetInfo(STREAM_INFO_IS_GRABBING)` as the "next call", with
accurate burst statistics.

**Real Kineo retest** (same process-local `GENICAM_GENTL64_PATH`
scoping, fresh CTI log `kineo_probe_cti_m4d_step4.log`, launched
20:56:01 local, PID 7500):

**Result: Kineo again allocated straight through to the ceiling —
64 buffers, then requested a 65th and was rejected.** Exact sequence
(after the same `AcquisitionMode`/`TriggerSelector`/`TriggerMode`/
`PayloadSize`/`BUF_ANNOUNCE_MIN` preamble as every prior pass):
```
DSAllocAndAnnounceBuffer #1..#5 -> DSQueueBuffer #1..#5 (full detail logged)
DSAllocAndAnnounceBuffer #6 ... further allocations suppressed (counting silently)
DSQueueBuffer #6 ... further queues suppressed (counting silently, queue_depth continues climbing)
[buffers #6 through #64 allocated and queued silently]
DSAllocAndAnnounceBuffer size=2304000 -> RESOURCE_EXHAUSTED (buffer table full, MAX_BUFFERS=64)
=== SAFETY CEILING REACHED: Kineo requested buffer #65, MAX_BUFFERS=64 hit deliberately --
    natural buffer count remains UNKNOWN (still climbing when the ceiling stopped it) ===
```
Every one of the 64 successful allocations was `size=2304000`, every one
was immediately queued (queue depth climbed 1→64 in lockstep with
allocation, exactly as at 16 buffers before) — **the natural buffer
count is still unknown, now confirmed definitively >64.**

**Application-level confirmation**, `ChironLog`
(`2026-09-26T15:28:31.545Z` = 20:58:31.545 local, same instant as the
65th alloc attempt), quoting our own error text verbatim again:
```
Analysis 20 result: {"message":"Camera error during analysis: Capture
failed: Error-Code: 1 (PEAK_RETURN_CODE_ERROR) | Error-Description:
[Function: DSAllocAndAnnounceBuffer | Error-Code: -1020
(GC_ERR_RESOURCE_EXHAUSTED) | Error-Text: DSAllocAndAnnounceBuffer:
buffer table full","succeed":false,"type":"Analysis-Start-Response"}
```
Same graceful, single, bounded failure pattern as every previous gap —
one failed analysis (`Analysis 20`), no crash, no retry storm. Two
further routine re-open cycles followed (only the standard feature
writes, no repeat of the alloc/queue burst), then the same shutdown
sequence as always (`AcquisitionStop` → 3×(`DSGetInfo(IS_GRABBING)`×2,
`DSFlushQueue`-stub) → `DevClose`, no `DSClose`, hard-kill on exit).

**`KineoDeviceService` stability: remains alive throughout.** Same PID
(7500); fresh `Get-WinEvent` query confirms no new `KineoDeviceService`
crash event (latest remains 15:53:07) — **ninth** consecutive clean
real-Kineo session since the `AcquisitionFrameRate` fix.

**Per the stop condition ("allocation #65 is attempted"), halting here —
not raising `MAX_BUFFERS` beyond 64 in this iteration.** Kineo's real
target buffer count is still unknown; it could be a larger fixed number
(100, 128, 256...), a formula based on expected frame rate/buffering
duration, or effectively unbounded until some other internal limit. The
smallest next step is either (a) another controlled ceiling raise (e.g.
to 256) to keep narrowing the search, or (b) considering whether the
actual buffer count even matters for correctness — GenTL producers
commonly just accept whatever count a consumer requests up to a sane
memory-based limit, so the real fix may be to size the table generously
(or make it genuinely dynamic/unbounded with a sensible memory-based cap)
rather than continuing to binary-search for Kineo's exact preferred
count.

## M4c-1d — Uncapped Dynamic Buffer Pool (N=81 found; next boundary is real GCRegisterEvent)

**Direction change:** stopped optimizing for *discovering* Kineo's exact
buffer count and switched to optimizing for interoperability — remove
the artificial ceiling entirely and let Kineo have as many real,
genuinely usable buffers as it wants, with physical memory exhaustion as
the only natural limit (the same limit any real GenTL producer has).

**Implementation:** new versioned build `probe/m3/m4e/`
(`m4e_standalone.c`/`kineo_bridge_m4e.xml`), derived from M4d, M4d files
untouched. `PROBE_VERSION` bumped to `"0.10-M4e"`.

- **Removed `MAX_BUFFERS` entirely.** The buffer registry is now a
  growable array of *pointers* to individually-`malloc`'d `buffer_t`
  objects (`g_buffers`, doubling via `realloc` as needed). Because each
  `buffer_t` object is allocated once and never moved, and only the
  pointer array (not the objects) is ever `realloc`'d, every
  `BUFFER_HANDLE` (= the object's own address) stays permanently valid
  regardless of how much the registry grows or shrinks — growing the
  array can never invalidate a handle Kineo is currently holding.
  `find_buffer_locked()` now validates a handle by identity (linear scan
  against the live registry) rather than address-range arithmetic, since
  objects can be anywhere in the heap; a malformed/garbage handle simply
  matches nothing and is never dereferenced.
- **Real, fully-committed buffers** (`malloc`, not `VirtualAlloc`
  reserve-only — the earlier M4c-1c approach was abandoned per updated
  direction before any real-Kineo test) — "these should be real buffers
  suitable for eventual synthetic-frame delivery."
- **Genuine safety valve, not a synthetic ceiling:** before every
  allocation, `GlobalMemoryStatusEx` checks available physical memory;
  below 512MB free, allocation is refused
  (`GC_ERR_RESOURCE_EXHAUSTED`) as a deliberate stop *before* the host
  risks instability — distinct from an ordinary `malloc` failure (which
  is now the real, natural limit, logged distinctly as "NATURAL MEMORY
  LIMIT REACHED" if it ever occurs mid-burst).
- **Logging:** first 5 allocations/queues in full, then checkpoints at
  every power of two ≥16 (16, 32, 64, 128, 256, ... indefinitely, no
  upper bound), each reporting cumulative bytes and available physical
  memory. The existing `note_buffer_burst_boundary()` mechanism
  (unchanged from M4d) still fires the moment Kineo calls something
  other than alloc/queue.
- `STREAM_INFO_BUF_ANNOUNCE_MIN` unchanged, still returns `1`.

**Official-bindings test:** full pass, identical to M4c-1's core checks,
plus a new dynamic-growth check — allocated and queued 200 buffers (well
past the old 16/64 fixed ceilings) with no artificial limit hit,
confirming the registry grows correctly. Library teardown's defensive
cleanup correctly freed all 201 never-revoked buffers (buffer #0 from
the earlier check + the 200 growth buffers), final tally exactly
consistent (`allocated=204 queued=201 revoked=3
leaked-and-freed-defensively=201`) — no leaks, no crash, checkpoint
logging confirmed firing correctly at 16/32/64/128.

**Real Kineo retest** (same process-local `GENICAM_GENTL64_PATH`
scoping, fresh CTI log `kineo_probe_cti_m4e_step4.log`, launched
21:13:54 local, PID 7556):

**Kineo stopped allocating on its own, for the first time ever in this
project — at exactly 81 buffers, with no ceiling of any kind
involved:**
```
DSAllocAndAnnounceBuffer #1..#5 -> DSQueueBuffer #1..#5 (full detail)
[checkpoints at #16, #32, #64 -- all succeed, ~7.3GB physical memory still free throughout]
=== buffer alloc/queue burst ended: 81 buffer(s) this burst (alloc #1-#81),
    186624000 bytes, max queue depth 81 -- next call: GCRegisterEvent(id=1) ===
GCRegisterEvent id=1  [STUB -> NOT_IMPLEMENTED]
```

1. **N = 81** — Kineo's genuine, natural announced-buffer count. Not a
   round number (not 64, not 100) — this really is what Kineo's own
   internal logic decided to request, uninfluenced by any artificial
   limit this time.
2. **N × 2,304,000 = 186,624,000 bytes (~178 MiB)** total logical/real
   memory for the full buffer pool — trivial for any modern machine, far
   below the 512MB safety threshold and nowhere near real memory
   exhaustion (7.3GB physical memory was still free at the last
   checkpoint).
3. **All 81 buffers were queued** — allocation and queueing stayed in
   lockstep exactly as at every smaller scale (16, 32, 64) before it;
   queue depth reached exactly 81, matching the allocation count.
4. **Next function: `GCRegisterEvent`**, called with `iEventID=1`. This
   matches **CASE A** from the original M4c directive exactly
   ("`DSQueueBuffer` → `GCRegisterEvent(EVENT_NEW_BUFFER)`").
   **Flagging a genuine oddity rather than glossing over it:** `1` does
   not match any standard GenTL `EVENT_TYPE_LIST` value we have on
   record (`EVENT_ERROR=0`, `EVENT_NEW_BUFFER=1000`,
   `EVENT_FEATURE_INVALIDATE=2000`, `EVENT_FEATURE_CHANGE=2001`,
   `EVENT_REMOTE_DEVICE=3000` — all independently confirmed against the
   official EMVA `GenTL_v1_5.h` earlier in this project). The earlier,
   harmless `GCRegisterEvent(id=1000)` call at `TLOpenInterface` time
   *does* match `EVENT_NEW_BUFFER` exactly — so this new, real
   registration attempt using `id=1` instead is genuinely unexplained
   from the header alone. Possibilities not yet distinguished: an
   IDS-internal/vendor-specific event-type numbering inside
   `ids_peak.dll`'s own GenTL usage that doesn't follow the public
   `EVENT_TYPE_LIST` codes; a different `hEventSrc` context changing how
   the ID is interpreted (not logged this pass); or something else. Not
   guessed at further — reported as observed, to be resolved before
   implementing real event support.
5. Application-level confirmation, `ChironLog`
   (`2026-09-26T15:47:53.846Z` = 21:17:53.846 local, same instant),
   quoting our own error text verbatim: `Analysis 21` failed with
   `[Function: GCRegisterEvent | Error-Code: -1003 (GC_ERR_NOT_IMPLEMENTED)
   | Error-Text: GCRegisterEvent not implemented]`. Same graceful,
   bounded single-failure pattern as every previous gap — no crash, no
   retry storm.
6. A routine re-open cycle followed (only the standard feature writes,
   no repeat of the 81-buffer burst), then the usual shutdown sequence.

**`KineoDeviceService` stability: remains alive throughout.** Same PID
(7556); fresh `Get-WinEvent` query confirms no new crash event (latest
remains the same pre-fix baseline entry, well before this test) — tenth
consecutive clean real-Kineo session since the `AcquisitionFrameRate` fix.

**Backing-strategy decision (per the "after N is known" framework):**
with N=81 and ~178 MiB total, this is squarely case **(A) ordinary
committed buffers** — modest enough that no lazy-commit or file-backed
strategy is warranted. The real, fully-`malloc`'d approach already
implemented this pass is the right final direction; no architecture
change needed once real event/streaming support is added.

**STOP CONDITION MET: Kineo naturally stopped allocating and called a
different function (`GCRegisterEvent`).** Per instructions, halting here
— not implementing event support this pass. The `id=1` oddity should be
understood (or at minimum handled defensively) before implementing
`GCRegisterEvent` for real.

## Investigation — `GCRegisterEvent(id=1)` mystery solved empirically (not guessed)

**Method:** rather than speculate from the GenTL spec alone, tested
directly against the official `ids_peak` Python bindings (no Kineo, no
guessing) using the already-passing M4e CTI.

**Step 1 — inspected the official bindings' own event-type surface:**
```python
EventType_Error            = 0
EventType_FeatureInvalidate = 2
EventType_FeatureChange     = 3
EventType_RemoteDevice      = 4
EventType_Module            = 5
EventType_Custom            = 1000
```
Value **`1` is conspicuously absent** from this public enum — a real gap,
not an oversight in our own reading of it (values present: 0, 2, 3, 4, 5,
1000 — "1" is the one skipped). This enum is IDS's own high-level
abstraction, separate from the raw GenTL `EVENT_TYPE_LIST` (`EVENT_ERROR=0`,
`EVENT_NEW_BUFFER=1000`, `EVENT_FEATURE_INVALIDATE=2000`,
`EVENT_FEATURE_CHANGE=2001`, `EVENT_REMOTE_DEVICE=3000` — all previously
confirmed against the official EMVA header). Neither enum contains "1"
as a public, documented value.

**Step 2 — direct empirical test:** wrote a minimal script using only the
official bindings (`DataStream.OpenDataStream()` →
`AllocAndAnnounceBuffer()` → `QueueBuffer()` → `StartAcquisition()`) — no
Kineo involved at all, no guessing about Kineo-specific behavior. Result:
```
StartAcquisition: FAILED as expected: Exception: Error-Code: 15
(PEAK_RETURN_CODE_NOT_IMPLEMENTED) | Error-Description:
[Function: GCRegisterEvent | Error-Code: -1003 (GC_ERR_NOT_IMPLEMENTED)
| Error-Text: GCRegisterEvent not implemented]
```
CTI log confirms the exact same signature real Kineo produced:
`GCRegisterEvent id=1  [STUB -> NOT_IMPLEMENTED]`, immediately after
queueing, immediately inside the single official `StartAcquisition()`
call — never reaching the real GenTL `DSStartAcquisition` function at
all.

**Conclusion (verified, not inferred):** `GCRegisterEvent(iEventID=1)` is
called **automatically, internally, by the official `ids_peak.dll` SDK
itself** as a mandatory prerequisite step inside `DataStream::StartAcquisition()`
— **this has nothing to do with Kineo's own application code.** `1` is
IDS's own private/internal GenTL event-type constant for the
DataStream's new-buffer-ready notification (distinct from the public
GenTL `EVENT_NEW_BUFFER=1000`, and not exposed in the bindings' own
public `EventType_*` enum, which explains the gap at exactly the value
we needed explained). Any GenTL producer used with `ids_peak.dll` — not
just our own, and not something Kineo chose or configured — will see
this exact same call with this exact same value. The earlier, harmless
`GCRegisterEvent(id=1000)` at `TLOpenInterface` time is unrelated: it's
registered against the **Interface** handle (a different `hEventSrc`
context entirely), not the DataStream, and coincidentally matches the
*public* `EVENT_NEW_BUFFER`/`EventType_Custom` numeric value without
being the same logical event.

**Practical implication for implementation:** `GCRegisterEvent` must
return a valid `EVENT_HANDLE` for `iEventID=1` (registered against the
DataStream's event source) before the real `DSStartAcquisition` GenTL
function will ever be reached at all — the SDK's `StartAcquisition()`
wrapper gates on this internally and won't proceed past it. This means
`EventGetData`/actual event delivery is **not** required just to unblock
`DSStartAcquisition` — only a successful `GCRegisterEvent` call is,
confirmed by this test never reaching `EventGetInfo`/`EventGetDataInfo`/
`EventGetData` before failing. Real event *delivery* (`EventGetData`
returning `EVENT_NEW_BUFFER_DATA` when a buffer completes) will still be
needed once acquisition genuinely starts producing frames, but that is a
separate, later boundary from this one.

**CORRECTION (flagging my own error rather than silently rewriting the
above): the claim that `1` is "IDS's own private/internal event-type
constant, distinct from the public `EVENT_NEW_BUFFER=1000`" is WRONG.**
The earlier WebFetch that reported `EVENT_NEW_BUFFER=1000`,
`EVENT_FEATURE_INVALIDATE=2000`, `EVENT_FEATURE_CHANGE=2001`,
`EVENT_REMOTE_DEVICE=3000` (quoted above, and previously written into
`gentl_v2.h`'s comments as "independently confirmed") was **simply
incorrect — a bad fetch summary that was never independently
re-verified before being trusted and propagated into code comments and
these notes.** A second, careful re-fetch of the actual EMVA
`GenTL_v1_5.h` text, quoted verbatim this time, gives the real official
values:
```c
EVENT_ERROR               = 0
EVENT_NEW_BUFFER          = 1
EVENT_FEATURE_INVALIDATE  = 2
EVENT_FEATURE_CHANGE      = 3
EVENT_REMOTE_DEVICE       = 4
EVENT_MODULE              = 5   /* GenTL v1.4 */
EVENT_CUSTOM_ID           = 1000
```
**`GCRegisterEvent(iEventID=1)` is the standard, public GenTL
`EVENT_NEW_BUFFER` registration — exactly what every correctly-behaving
GenTL producer is supposed to support.** It is not IDS-specific, not
private, not a coincidence, and the "gap at 1" in the Python bindings'
`EventType_*` enum is unrelated (that enum simply doesn't expose
`EVENT_NEW_BUFFER` as a chooseable value since `StartAcquisition()`
registers it automatically/internally — nothing to do with numbering
mystery-solving). The earlier `GCRegisterEvent(id=1000)` seen on the
**Interface** handle is genuinely unrelated to this, but for a different
reason than previously stated: `1000` is `EVENT_CUSTOM_ID`, the
*starting* value for a producer's own custom/vendor event ID range, not
a second occurrence of the (wrongly-remembered) public
`EVENT_NEW_BUFFER`. `gentl_v2.h` has been corrected to the verified
values as part of implementing M4c-2 (see below); the empirical
finding itself (that `StartAcquisition()` internally requires
`GCRegisterEvent` before reaching `DSStartAcquisition`) remains valid
and unaffected by this correction.

## M4c-2 — Standard EVENT_NEW_BUFFER Registration (implemented; real DSStartAcquisition now reached)

**Implementation:** new versioned build `probe/m3/m4f/`
(`m4f_standalone.c`/`kineo_bridge_m4f.xml`), derived from M4e, M4e files
untouched. `PROBE_VERSION` bumped to `"0.11-M4f"`. `gentl_v2.h`'s
`EVENT_TYPE_LIST` corrected to the verified official values
(`EVENT_ERROR=0`, `EVENT_NEW_BUFFER=1`, `EVENT_FEATURE_INVALIDATE=2`,
`EVENT_FEATURE_CHANGE=3`, `EVENT_REMOTE_DEVICE=4`, `EVENT_MODULE=5`,
`EVENT_CUSTOM_ID=1000`).

- New `event_t` object: `magic`, `type` (always `EVENT_NEW_BUFFER` this
  pass), `owner_ds`, `killed` flag, `pending_count` (unused until real
  delivery), and a per-event `CRITICAL_SECTION` as the synchronization
  primitive. Individually `malloc`'d, address = stable `EVENT_HANDLE`
  (same handle-stability approach as `buffer_t`). Only one event
  (`EVENT_NEW_BUFFER` on the DataStream) is supported so far — a single
  global slot, since only one DataStream exists.
- `GCRegisterEvent`: for `hEventSrc==H_DS && iEventID==EVENT_NEW_BUFFER`,
  creates the event object and returns a valid handle
  (`GC_ERR_SUCCESS`). Duplicate registration is rejected with
  `GC_ERR_RESOURCE_IN_USE` (GenTL has no dedicated "already registered"
  code; this is the best-fit standard code, same one already used for
  the analogous "buffer already queued" case). Any other
  module/event-ID combination still falls through to the existing
  `NOT_IMPLEMENTED` stub behavior unchanged (e.g. the harmless
  `id=1000`/`EVENT_CUSTOM_ID` registration on the Interface handle,
  observed since M1, untouched).
- `GCUnregisterEvent`: validates the currently-registered event matches,
  removes it from the registry *before* freeing (preventing any
  concurrent lookup from touching it mid-free), then frees it. A
  mismatched/already-unregistered call gets `GC_ERR_INVALID_HANDLE`
  cleanly rather than a double-free.
- `EventKill`: validates the handle against the live registry; sets
  `killed=1` (would interrupt a pending `EventGetData` wait once
  implemented) but does **not** free the object — that's
  `GCUnregisterEvent`'s job, matching the standard "kill interrupts,
  unregister destroys" GenTL convention.
- Defensive cleanup (`free_event_defensive()`, mirroring the buffer
  registry's): frees any still-registered event at `DSClose` or
  `DllMain PROCESS_DETACH`, in case normal unregistration never happens
  (consistent with the established "Kineo may hard-kill the process"
  finding).
- `DSStartAcquisition`'s stub logging enriched (per instructions — still
  **not implemented**, just observing): now logs `iStartFlags` and
  `iNumToAcquire`, with a heuristic noting when the count looks like
  `INFINITE_NUMBER` (continuous acquisition).

**Official-bindings test — repeated the exact isolated `StartAcquisition()`
probe from before implementing anything, no Kineo involved:**
```
StartAcquisition: FAILED as expected: [Function: DSStartAcquisition |
Error-Code: -1003 (GC_ERR_NOT_IMPLEMENTED) | Error-Text: DSStartAcquisition
not implemented until M4]
```
**`GCRegisterEvent(EVENT_NEW_BUFFER)` now succeeds and the SDK proceeds
straight to the real `DSStartAcquisition`** — confirmed via the CTI log:
```
GCRegisterEvent(hEventSrc=DS, EVENT_NEW_BUFFER) -> SUCCESS, handle=...
DSStartAcquisition iStartFlags=0 iNumToAcquire=18446744073709551615
    (looks like INFINITE_NUMBER/continuous)  [STUB -> NOT_IMPLEMENTED]
...
GCUnregisterEvent(DS, EVENT_NEW_BUFFER) -> SUCCESS, freed
```
Full regression suite (M4c-1's core + M4e's 200-buffer dynamic-growth
check) still passes unchanged.

**Real Kineo retest** (same process-local `GENICAM_GENTL64_PATH`
scoping, fresh CTI log `kineo_probe_cti_m4f_step4.log`, launched
21:40:30 local, PID 16128):

1. **`GCRegisterEvent(EVENT_NEW_BUFFER=1)`: PASS.** Same 81-buffer
   natural allocation count as the previous pass (reproducible, stable
   across runs) →
   `GCRegisterEvent(hEventSrc=DS, EVENT_NEW_BUFFER) -> SUCCESS`.
2. **Exact next function: `DSStartAcquisition`** — reached immediately,
   no intermediate `EventGetInfo`/`EventGetDataInfo` query.
3. **`DSStartAcquisition` reached: YES.** Flags/count are **identical to
   the isolated SDK-only test**: `iStartFlags=0`,
   `iNumToAcquire=18446744073709551615` (`INFINITE_NUMBER`, continuous
   acquisition) — confirms this is standard `ids_peak` SDK default
   behavior, not something Kineo configures specially.
4. **`EventGetData` requested? No** — neither before nor after
   `DSStartAcquisition`. Consistent with the earlier finding: event
   *delivery* is a separate, later concern from registration/start.
5. **Cleanup/unregistration observed:** `GCUnregisterEvent(DS,
   EVENT_NEW_BUFFER) -> SUCCESS, freed`, called cleanly as part of the
   very next `DevClose` in the routine re-open cycle — Kineo's regular
   device-close path always unregisters the event, independent of
   whether acquisition itself succeeded. Buffers themselves are still
   never explicitly revoked (same as every prior pass) — only the event
   gets clean lifecycle treatment here.
6. **Application-level confirmation**, `ChironLog`
   (`2026-09-26T16:12:27.516Z` = 21:42:27.516 local, same instant),
   quoting our own error text verbatim: `Analysis 22` failed with
   `[Function: DSStartAcquisition | Error-Code: -1003
   (GC_ERR_NOT_IMPLEMENTED) | Error-Text: DSStartAcquisition not
   implemented until M4]`. Same graceful, bounded single-failure
   pattern as every previous gap.
7. **`KineoDeviceService` stability: remains alive throughout.** Same PID
   (16128); fresh `Get-WinEvent` query confirms no new crash event
   (latest remains the same pre-fix baseline entry) — **eleventh**
   consecutive clean real-Kineo session since the `AcquisitionFrameRate`
   fix.

**STOP CONDITION MET: the real `DSStartAcquisition` GenTL function has
been reached for the first time in this entire project**, with its exact
flags/count now on record. Per instructions, halting here — not
implementing acquisition/frame delivery in the same pass. This is the
next controlled milestone.

## M4c-3 — Minimal Acquisition State (DSStartAcquisition implemented; real next gate is EventGetInfo(EVENT_SIZE_MAX), not EventGetData)

**Implementation:** new versioned build `probe/m3/m4g/`
(`m4g_standalone.c`/`kineo_bridge_m4g.xml`), derived from M4f, M4f files
untouched. `PROBE_VERSION` bumped to `"0.12-M4g"`.

- **Thread-ID logging added globally**, from this build onward: every
  `probe_log()` line now includes `[tid=...]` (`GetCurrentThreadId()`),
  per instructions ("include thread IDs in logging from this point
  onward... IDS Peak may create a waiting thread").
- `DSStartAcquisition`: validates the stream handle, verifies at least
  one buffer is actually queued (`GC_ERR_RESOURCE_EXHAUSTED` if not),
  then sets minimal state only — `g_ds_grabbing=1`, records
  `iStartFlags`/`iNumToAcquire` — and returns `GC_ERR_SUCCESS`.
  Deliberately does **not** touch the queue/buffers/event: no dequeue, no
  completion, no `EVENT_NEW_BUFFER` signal, no worker thread (exactly as
  instructed).
- `DSStopAcquisition`: validates the stream, sets `g_ds_grabbing=0`,
  returns success. No thread to join yet (none exists).
- `DSGetInfo(STREAM_INFO_IS_GRABBING)` now reports the real state
  (`FALSE` before start, `TRUE` while grabbing, `FALSE` after stop)
  instead of the previous hardcoded `FALSE`.

**Official-bindings test — two stages, both essential:**
1. Bare `ds.StartAcquisition()` (no explicit frame-wait call): succeeded
   cleanly. The very next call was just `DSGetInfo(STREAM_INFO_IS_GRABBING)`
   ×2 (a sanity read-back) — **not** `EventGetData`. Everything ran on a
   single thread; no separate worker/waiting thread is spawned merely by
   calling `StartAcquisition()` itself.
2. **Extended test using `ds.WaitForFinishedBuffer(2000)`** (the actual
   official API for consuming a frame) — this is what revealed the real
   next gate:
   ```
   WaitForFinishedBuffer: FAILED as expected: InternalErrorException:
   [Function: EventGetInfo | Info-Command: 3 (EVENT_SIZE_MAX) |
   Error-Code: -1003 (GC_ERR_NOT_IMPLEMENTED) | Error-Text: EventGetInfo
   not implemented until M4]
   ```
   **The SDK calls `EventGetInfo(EVENT_SIZE_MAX)` before ever calling
   `EventGetData`** — presumably to size its own receive buffer first.
   This was verified with the official bindings *before* touching real
   Kineo, per the established methodology.

**Real Kineo retest** (same process-local `GENICAM_GENTL64_PATH`
scoping, fresh CTI log `kineo_probe_cti_m4g_step4.log`, launched
22:00:02 local, PID 6000) — **confirms the isolated-test finding exactly,
plus reveals `AcquisitionStart`'s real position in the sequence:**
```
=== buffer alloc/queue burst ended: 81 buffer(s) ... -- next call: GCRegisterEvent(id=1) ===
[tid=6620] GCRegisterEvent(hEventSrc=DS, EVENT_NEW_BUFFER) -> SUCCESS
[tid=6620] DSStartAcquisition iStartFlags=0 iNumToAcquire=18446744073709551615
    (INFINITE_NUMBER/continuous) queued_buffers=81 -> SUCCESS, grabbing=TRUE
[tid=6620] COMMAND AcquisitionStart             addr=0x200e0
[tid=6620] COMMAND AcquisitionStart             addr=0x200e0
[tid=6620] EventGetInfo cmd=3  [STUB -> NOT_IMPLEMENTED]
...
[tid=15924] GCUnregisterEvent(DS, EVENT_NEW_BUFFER) -> SUCCESS, freed
```

1. **`DSStartAcquisition`: PASS** — same 81-buffer natural pool
   (`81 × 2,304,000 = 186,624,000 bytes` logical image memory, per the
   interoperability-first decision — not investigating *why* 81 further),
   succeeds cleanly with the exact same flags/count as the isolated test.
2. **Exact next call: the GenApi `AcquisitionStart` command** (written
   via `GCWritePort`, address `0x200e0`) — called **twice**, 3ms apart,
   immediately after `DSStartAcquisition` succeeds. **This is the first
   time `AcquisitionStart` has ever been invoked in this entire
   project.**
3. **Ordering relative to `EventGetInfo`: sequential, not concurrent.**
   Both `AcquisitionStart` writes and the `EventGetInfo` call happen on
   the *same* thread (`tid=6620`), with a genuine ~522ms gap between the
   second `AcquisitionStart` write (`22:05:57.355`) and `EventGetInfo`
   (`22:05:57.877`) — consistent with real device/hardware-interaction
   time on Kineo's own side before it moves on to waiting for a frame,
   not a race between two threads.
4. **`EventGetData` reached? No.** The real next call is
   **`EventGetInfo(cmd=3)`** — matches `EVENT_SIZE_MAX` exactly (per the
   isolated bindings test's own error text) — still a stub
   (`NOT_IMPLEMENTED`). `EventGetData` is never reached because this
   prerequisite gate fails first, exactly mirroring the isolated SDK-only
   test's behavior.
5. **Cleanup:** `GCUnregisterEvent(DS, EVENT_NEW_BUFFER) -> SUCCESS,
   freed` still happens (this time observed on a *different* thread,
   `tid=15924` — the original app-launch thread, distinct from `tid=6620`
   which handled the acquisition-start/event sequence — confirming Kineo
   genuinely uses multiple threads against our producer). Buffers
   themselves still never explicitly revoked. Shutdown sequence otherwise
   matches every prior pass (`AcquisitionStop` → 3×(`IS_GRABBING`×2,
   `DSFlushQueue`-stub) → `DevClose`, no `DSClose`).
6. **Application-level result — notably different this time:**
   `ChironLog` (`2026-09-26T16:35:57.895Z` = 22:05:57.895 local, right
   after the `EventGetInfo` failure): `Analysis 23` failed with
   **`"Camera error during analysis: Capture failed: GrabFrame
   failed"`** — a generic, higher-level message, **not** a verbatim
   quote of our raw GenTL error text this time (unlike every previous
   gap). This suggests Kineo's own "Capture"/grab-frame logic wraps
   whatever low-level exception occurs at this specific depth in its own
   simplified message, rather than surfacing the raw `ids_peak`
   exception string as it did for every earlier (shallower) failure.
7. **`KineoDeviceService` stability: remains alive throughout.** Same PID
   (6000); fresh `Get-WinEvent` query confirms no new crash event (latest
   remains the same pre-fix baseline entry) — **twelfth** consecutive
   clean real-Kineo session since the `AcquisitionFrameRate` fix.

**STOP CONDITION MET: a genuinely new operation (`EventGetInfo`) was
reached after successful `DSStartAcquisition`**, immediately following
the first-ever `AcquisitionStart` invocation. Per instructions, halting
here — not implementing `EventGetInfo`/frame delivery/synthetic
acquisition in this pass. `EVENT_SIZE_MAX` (and whatever other
`EventGetInfo` commands follow it) is the next controlled milestone
before `EventGetData` can ever be reached.

## M4d — Complete Minimal New-Buffer Pipeline: SYNTHETIC FRAMES SUCCESSFULLY DELIVERED, REAL ANALYSIS COMPLETES (`succeed: true`)

**Scope:** wide implementation window authorized (multiple edit/build/test
cycles without stopping for every ordinary GenTL function), target =
first synthetic frame(s) delivered to real Kineo, no WSL/Aravis
integration. New versioned build `probe/m3/m4h/`
(`m4h_standalone.c`/`kineo_bridge_m4h.xml`), derived from M4g, M4g files
untouched. `PROBE_VERSION` bumped to `"0.13-M4h"`.

**Header corrections (source of truth: fresh, verbatim EMVA GenTL v1.5
fetches, per instructions — "do not guess ABI details"):**
- `BUFFER_INFO_CMD_LIST` had the **same class of bug** as the earlier
  `EVENT_TYPE_LIST` mistake — several values were simply wrong
  (`PIXELFORMAT` was 14 not 20, `PIXEL_ENDIANNESS` was 20 not 26,
  `DATA_SIZE` was 21 not 27, `FRAMEID` was 24 not 16, `IMAGEPRESENT` was
  25 not 17, `PAYLOADTYPE` was 32/UINT32 not 19/SIZET) — fully corrected
  and cross-checked against a second fetch quoting the raw enum
  verbatim.
- Added, freshly and correctly: `EVENT_INFO_CMD_LIST`,
  `EVENT_DATA_INFO_CMD_LIST`, the official `EVENT_NEW_BUFFER_DATA`
  struct (packed, `BufferHandle`+`pUserPointer`, `sizeof()` used directly
  rather than a hard-coded size anywhere), `PAYLOADTYPE_INFO_IDS`,
  `PIXELFORMAT_NAMESPACE_IDS`.

**Implementation summary:**
- **Buffer state machine extended**: `ANNOUNCED → QUEUED → FILLING →
  COMPLETED → (requeue) → QUEUED`, all four states explicit (not
  inferred), each buffer carrying real `filled_size`/`is_incomplete`/
  `new_data`/`frame_id`/`timestamp_ns`, reset correctly on requeue per
  `BUFFER_INFO_SIZE_FILLED`'s official "reset to 0 when placed into the
  Input Buffer Pool" semantics. `DSQueueBuffer` now distinguishes and
  logs initial-queue vs. requeue explicitly.
- **Thread safety**: everything (buffer registry, input/completed FIFOs,
  grabbing/camera-running state, event registration/lifetime) unified
  under one `CRITICAL_SECTION` (`g_buf_lock`) + one `CONDITION_VARIABLE`
  (`g_buf_cv`), broadcast on every state change a waiter might care
  about. Native Win32 primitives only, no external runtime dependency.
  Event object is refcounted (`refcount`/`pending_free`) so
  `GCUnregisterEvent` from a different thread than a blocked
  `EventGetData` (already proven to happen — M4g) can never cause a
  use-after-free: it marks `killed`+broadcasts immediately, but only
  frees once the last referencing `EventGetData` call has left.
- **Two-layer acquisition state model**, exactly matching Kineo's
  demonstrated ordering: `DSStartAcquisition` arms `stream_armed`
  (`g_ds_grabbing`); the GenApi `AcquisitionStart`/`AcquisitionStop`
  commands (detected inside `GCWritePort`'s existing `COMMAND` branch by
  comparing `r->storage` against the known register globals) arm/disarm
  `g_camera_running`. Synthetic production requires both. The duplicate
  `AcquisitionStart` writes Kineo sends are harmless/idempotent by
  construction.
- **Worker thread**: created once, lazily, on the first
  `DSStartAcquisition`; idles (blocked on the condition variable)
  whenever not both armed; pops one buffer from the input FIFO, marks it
  `FILLING`, fills it **without holding the lock** (moving vertical
  bright bar, deterministic and visibly different frame-to-frame, per
  instructions — not constant gray), marks it `COMPLETED`, pushes onto
  the completed-event FIFO, wakes waiters. Paced at 10 FPS
  (`Sleep(100)`).
- **`EventGetInfo`** implemented for the full confirmed+adjacent set:
  `EVENT_EVENT_TYPE`, `EVENT_NUM_IN_QUEUE`, `EVENT_NUM_FIRED`,
  `EVENT_SIZE_MAX`, `EVENT_INFO_DATA_SIZE_MAX` (the latter two both
  return `sizeof(EVENT_NEW_BUFFER_DATA)`, computed, not hard-coded).
- **`EventGetData`** implemented with real blocking (`SleepConditionVariableCS`
  in a deadline-tracked loop), correct finite/`INFINITE` timeout
  handling, correct size-query/buffer-too-small semantics, delivers the
  official `EVENT_NEW_BUFFER_DATA` struct, returns `GC_ERR_ABORT` on
  kill/unregister (matches the official bindings' own documented
  `AbortedException` mapping) and `GC_ERR_TIMEOUT` on timeout (matches
  `TimeoutException`) — both confirmed against the real docstrings
  captured earlier in this project, not guessed.
- **`DSGetBufferInfo`** implemented for a broad, spec-correct set
  (`BASE`, `SIZE`, `USER_PTR`, `TIMESTAMP`/`TIMESTAMP_NS`, `NEW_DATA`,
  `IS_QUEUED`, `IS_ACQUIRING`, `IS_INCOMPLETE`, `TLTYPE`,
  `SIZE_FILLED`, `WIDTH`, `HEIGHT`, `X/YOFFSET`, `X/YPADDING`,
  `FRAMEID`, `IMAGEPRESENT`, `PAYLOADTYPE`, `PIXELFORMAT`,
  `PIXELFORMAT_NAMESPACE`, `DATA_SIZE`, `DATA_LARGER_THAN_BUFFER`,
  `CONTAINS_CHUNKDATA`). **`DSGetBufferID`** implemented (returns the
  handle at a given registry index).
- **`EventFlush`** discards pending completed-event notifications
  (buffers themselves untouched). **`EventKill`** now actually
  interrupts a blocked `EventGetData` (broadcasts the CV) rather than
  being a no-op. **`GCUnregisterEvent`** is now refcount-safe as above.
- **Teardown**: `stop_worker_defensive()` (stop-flag + broadcast + join
  with a 2s timeout) called from `DSClose`/`DllMain PROCESS_DETACH`,
  before the event and buffer defensive-cleanup paths (correct order:
  worker joined first, so nothing is still touching a buffer/event when
  they're freed).
- `EventGetDataInfo` deliberately left as a stub — no evidence yet
  (official bindings or real Kineo) that it's ever called for
  `EVENT_NEW_BUFFER`'s fixed-struct data.

**Official-bindings validation (before touching Kineo, per the
established methodology):**
1. Full M4c-1/M4e regression suite: unchanged, still passes (200-buffer
   dynamic growth, duplicate-queue/revoke-while-queued rejection, slot
   reuse).
2. New end-to-end test (`AllocAndAnnounceBuffer`×5 → `QueueBuffer`×5 →
   `StartAcquisition()` → `AcquisitionStart.Execute()` →
   `WaitForFinishedBuffer(3000)`×5 with `QueueBuffer` requeue after
   each): **5/5 frames delivered**, all successfully requeued, clean
   `AcquisitionStop`/`StopAcquisition`. CTI log confirms exact expected
   mechanics: worker on its own thread, `EventGetData` blocking for
   ~100-140ms between deliveries (matching the 10 FPS pace), correct
   `(REQUEUE)` logging, accurate final tally
   (`frames_produced=5 events_delivered=5 eventgetdata_calls=5
   queue_starvation_events=0`), zero leaks after defensive cleanup swept
   the 5 never-explicitly-revoked buffers. One minor, non-blocking
   observation: a ~2s delay joining the worker thread specifically
   during the *test script's own* clean Python-process exit (not
   expected to matter for real Kineo's hard-kill shutdown, which never
   reaches this path anyway).

**Real Kineo test — round 1 — found and fixed the actual remaining
blocker:** first run delivered exactly **one** real frame
(`EventGetData → rc=0 (NEW_BUFFER delivered)`), then immediately hit
`DSGetBufferInfo(cmd=18) → NOT_IMPLEMENTED` (checked right after
`IMAGEPRESENT`/`HEIGHT`/`WIDTH`/`SIZE`, all of which succeeded) — `18` is
`BUFFER_INFO_IMAGEOFFSET` per the corrected enum. `ChironLog` confirmed
this was fatal to the whole analysis: `Analysis 24/25/26` all failed
with the generic `"Capture failed: GrabFrame failed"` message (Kineo's
own wrapper, not our raw error text, at this depth — consistent with the
M4c-3 finding). Fixed by implementing `BUFFER_INFO_IMAGEOFFSET` (returns
`0` — single-part image, no offset) — the *only* missing command found
across the entire run (`grep` for "unrecognized command" = exactly one
hit). Re-verified via the official-bindings tests (no regression, and
neither test exercises `IMAGEOFFSET` — only real Kineo does) before
retesting.

**Real Kineo test — round 2 — SUCCESS (target result achieved):**
```
81 buffers allocated/queued -> GCRegisterEvent(EVENT_NEW_BUFFER) -> SUCCESS
-> DSStartAcquisition -> SUCCESS -> AcquisitionStart (x2)
-> worker produces frames -> EventGetData -> rc=0 (NEW_BUFFER delivered) [x10 logged, more thereafter uncounted]
-> DSGetBufferInfo(IMAGEPRESENT/HEIGHT/WIDTH/SIZE/IMAGEOFFSET) all succeed
-> DSQueueBuffer (REQUEUE) x26+ -> steady frame flow
-> [second Start Analysis, fresh 81-buffer re-open cycle at 22:46:42]
-> more frames delivered, zero "unrecognized command" hits this entire run
```
**Application-level confirmation, `ChironLog` — the actual target
result:**
```
Analysis 27 result: {"succeed":true, "concentration":-119174.05, "count":-2147483648,
    "motility":0, "vap":0, "vcl":0, "vsl":0, "str":0, "lin":0, "wob":-17700225024 (coerced to 0), ...}
Analysis 28 result: {"succeed":true, ...same shape...}
```
**Two independent Start-Analysis attempts, each ran the full
capture-and-CASA-motility-analysis pipeline through to completion with
`"succeed":true`** — the first time in this entire project that Kineo's
own application logic has completed successfully, not just reached a
deeper GenTL call. The nonsensical numeric values (`count` at
`INT32_MIN`, wildly out-of-range `concentration`/`wob`, `"WOB out of
range or below epsilon, coerced to 0"` warning, `"Tracking image not
found"` warning) are the **expected, correct consequence of feeding a
real CASA sperm-motility algorithm a synthetic test pattern with zero
actual cells to track** — not a bug in our implementation; the pipeline
itself completed cleanly, it simply had nothing meaningful to analyze.
**This is genuinely fake/synthetic frame content, not real camera
output** — no WSL/Aravis/real-camera code was touched this pass, exactly
as instructed; the worker thread fills every buffer in-memory with a
deterministic moving-bar pattern.

**`KineoDeviceService` stability: remains alive and stable throughout
both full analysis runs (several minutes of continuous operation).**
Same PID; fresh `Get-WinEvent` query confirms no new crash event (latest
remains the pre-fix baseline entry) — no hangs, no deadlocks, no memory
corruption observed. The process was hard-killed at final app exit as
always (no `DSClose`/`DllMain PROCESS_DETACH` reached in the real-Kineo
run, so the true total frame/event counts beyond the first 10 logged in
detail are unknown — only the official-bindings test, which exits
cleanly, produced a full final tally).

**STOP CONDITION MET: (A) SUCCESS and (B) STRONG SUCCESS both achieved —
real Kineo received multiple synthetic frames, and Analysis progressed
all the way past Capture to full completion (`succeed:true`) twice.**
Per instructions, halting here — no WSL/Aravis/real-camera integration
attempted this pass.

**Direct visual confirmation (user-observed, not just log/API evidence):
the synthetic video actually appeared in Kineo's own UI** — the live
viewer rendered our moving-bar test pattern, not just a numeric
`succeed:true` result. This is the strongest possible form of evidence
for stop condition (B) ("Kineo viewer/video behavior visibly changes") —
confirms the full pipeline (buffer fill → `EVENT_NEW_BUFFER` → IDS peak
SDK → Kineo's own rendering path) is genuinely intact end-to-end, not
merely passing internal API checks.

---

## M5 — WSL/Aravis Real-Camera Bridge: Built and Independently Validated, Awaiting Windows-Side Test

Per the M5/M6 directive: replace the synthetic frame source with the real
IDS U3-3560XCP-M via a WSL2/Aravis bridge, while leaving the M4d golden
baseline (`probe/m3/m4h/`) completely untouched.

**WSL bridge server (`wsl-camera/`):** `protocol.py` (KCB1 wire framing,
10-byte header + JSON control / 33-byte frame header + raw pixels),
`camera_source.py` (`RealCameraSource` via Aravis 0.8.36 async streaming --
`create_stream`+`push_buffer`+`start_acquisition`+`timeout_pop_buffer` loop,
never the synchronous convenience call; `SyntheticSource` for regression
testing without the camera), `kineo_camera_bridge.py` (one-client-at-a-time
TCP server), `test_client.py`.

**Independent WSL-side validation (both sources), before any CTI change:**
- Synthetic source: 20/20 good frames, exactly 2,304,000 bytes each,
  monotonic frame IDs, ~9.7 FPS, clean START/STOP/CLOSE.
- Real camera source: opened serial `4110010861` (vendor/model match),
  exposure/gain/black-level applied and echoed back, 20/20 good frames,
  exactly 2,304,000 bytes each, monotonic IDs, ~10.4 FPS, clean shutdown.
- Found and fixed one benign race: the streaming thread can emit one more
  FRAME message between a client's STOP request and the server noticing it;
  `test_client.py` now skips stray FRAME messages while waiting for a
  control reply instead of misinterpreting one as JSON (a real GenTL
  consumer needs the same tolerance).

**Networking check (read-only, no changes made):** `/etc/wsl.conf` has no
`networkingMode` override; `/mnt/c/Users/IMV/.wslconfig` explicitly sets
`networkingMode=nat` with no `localhostForwarding=false` override, so
Windows' default WSL2 localhost-forwarding should let `127.0.0.1:9494` from
Windows reach this bridge. Not verified from the Windows side yet (requires
running something on the actual machine).

**CTI integration (`probe/m5/`, `probe/m3/m4h/` untouched):** `m5_bridge.c`
is a copy of the protected M4d baseline (`PROBE_VERSION "0.14-M5"`) with a
new `wsl_bridge_client.c/h` module (Winsock2 TCP client speaking the same
KCB1 protocol) and a minimal-diff hook: `GCInitLib` reads
`KINEO_BRIDGE_SOURCE` (`synthetic` by default -- regression-safe; `wsl` or
`camera` selects the bridge) via `wsl_bridge_configure_mode()`; the first
`DSStartAcquisition` (same lazy point the worker thread itself already
used) also calls `wsl_bridge_start()`, forwarding Kineo's already-written
`ExposureTime`/`Gain`/`BlackLevel`/`AcquisitionFrameRate` register values as
the bridge's CONFIGURE payload; the worker thread's per-buffer fill call
branches on `g_frame_source_mode` -- WSL mode calls
`wsl_bridge_get_frame_blocking()` (blocks up to 2s for a not-yet-consumed
frame, paced by the bridge's real frame rate rather than the synthetic
path's fixed `Sleep(100ms)`) instead of `fill_synthetic_frame()`; a timeout
completes the buffer as incomplete, reusing the exact same
`is_incomplete`/`filled_size=0` path the too-small-capacity case already
used in M4d. Teardown (`DSClose`, `DllMain PROCESS_DETACH`) calls
`wsl_bridge_stop()` alongside the existing `stop_worker_defensive()`.
`KINEO_BRIDGE_HOST`/`KINEO_BRIDGE_PORT` (default `127.0.0.1`/`9494`) are
read the same way -- never hard-coded, per instructions.

Built cleanly with `mingw-w64` (`x86_64-w64-mingw32-gcc ... -lws2_32`),
81 exports (parity with the M4d export count), only pre-existing cosmetic
warnings (a `%z` format warning already present in the M4d file, and a
benign winsock2.h-include-order warning).

**Not yet run: the official-bindings sanity test and the real-Kineo test.**
Both require executing on the actual Windows Surface Pro machine (this
session only has WSL/Linux execution; WSL interop's `cmd.exe` cannot use a
UNC working directory, and no Windows-side Python/ids_peak install is
reachable from here) -- consistent with every prior milestone, where the
user ran these steps themselves and reported results back. The WSL bridge
(real camera source) is running now, listening on `0.0.0.0:9494`, ready for
that test.

### M5 — Official-Bindings Test PASSED With Real Camera Frames

Root cause of the first attempt's failure found and fixed: `python` on the
Windows machine resolves to the Microsoft Store app-execution-alias stub,
not a real interpreter -- the project already has a working portable
Python from earlier milestones at
`C:\Users\IMV\kineo-bridge-test\m1\pyembed\python.exe` with `ids_peak`
installed under `m1\pip_ids_peak_190`. Staged `m5_bridge.cti`,
`kineo_bridge_m5.xml`, and copies of the M4h-style test scripts into a new
`C:\Users\IMV\kineo-bridge-test\m1\m5\`, mirroring the existing per-
milestone convention there.

**Ran `test_frame_delivery.py` against `m5_bridge.cti` with
`KINEO_BRIDGE_SOURCE=wsl`:** OpenDevice/OpenDataStream/StartAcquisition/
AcquisitionStart all OK, **5/5 real frames delivered** via
`WaitForFinishedBuffer`, clean AcquisitionStop.

**Confirmed via the CTI log this was genuinely the real camera, not a
silent synthetic fallback:** `mode=WSL`, bridge OPEN reply
`"vendor": "IDS Imaging Development Systems GmbH", "model":
"U3-356xXCP-M", "serial": "4110010861"`, CONFIGURE echoed back real
exposure/gain/black-level/frame-rate, then 67 real
1920x1200/2304000-byte frames streamed from the actual USB3 camera over
WSL/usbipd/Aravis during the test's ~2.7s window (only 5 buffers were
queued so most were naturally superseded in the single-slot latest-frame
hand-off -- expected, not a bug). `worker` thread completed 6 GenTL
buffers before `DSStopAcquisition`; final tally
`frames_produced=6 events_delivered=5 eventgetdata_calls=5`.

**Success Levels reached so far: 1 (Windows receives real frames), 2 (CTI
produces completed GenTL buffers from them), 4 (repeated GrabFrame-
equivalent succeeds)** -- via the official bindings. **Levels 3 (real
video visible in Kineo's UI) and 5 (complete Analysis using real camera
frames) not yet attempted** -- require running real Kineo itself against
this CTI, not just the official-bindings script.

**Known simplification, not yet a problem:** `DSStopAcquisition` does not
forward STOP to the WSL bridge -- the bridge keeps streaming from the
real camera in the background until `DSClose`/process exit. Harmless for
a single short-lived test; worth revisiting (forward STOP/START to the
bridge to match GenTL-level start/stop) if a long real-Kineo session
shows it matters for camera contention or bandwidth.

### M5 — Real Kineo, Real Camera: STRONG SUCCESS (Success Levels 3 and 5 reached)

**First real-Kineo attempt failed** with the exact symptom the earlier
official-bindings test had not exposed: Kineo's very first
`EventGetData` after `AcquisitionStart` uses a short (~150ms) timeout,
but the WSL bridge's one-time handshake (TCP connect + open the real
camera via Aravis/usbipd + configure + start) took ~1-1.5s -- because
`wsl_bridge_start()` was triggered lazily at `DSStartAcquisition`, right
before that tight-timeout poll. Kineo timed out, retried opening the
device twice more, never recovered, and tore the session down
(`AcquisitionStop`/`DevClose`) without ever running Analysis.

**Fix:** moved the trigger to `DevOpenDataStream`, which this trace shows
happens **~31 seconds before `DSStartAcquisition`** -- by the time
acquisition actually starts, the bridge has been streaming into the
single-slot latest-frame hand-off for tens of seconds already, so the
worker thread's first fill call returns instantly instead of blocking.

**Second attempt failed differently:** bridge OPEN returned
`{"ok": false, "error": "no device with serial '4110010861' found (saw:
[])"}"` -- not a code bug. The `usbipd` USB/IP attachment to WSL had
silently dropped (`usbipd list` showed the device `Shared` but not
`Attached`) between test runs. Re-ran `usbipd attach --wsl --busid 3-2`;
the already-running WSL bridge's automatic reconnect-with-backoff picked
the camera back up with no code changes needed.

**Third attempt: full success.** Real video appeared in Kineo's live
viewer. Start-Analysis completed and wrote a real report:
`C:\Users\IMV\Documents\Kineo\Reports\Analysis_33_Abcd_2026-09-26.pdf`.
CTI log confirms the whole capture window
(`AcquisitionStart` 00:24:15.761 -> `AcquisitionStop` 00:24:16.811,
Kineo-initiated, no error): **35 real frames** delivered and cleanly
requeued in that 1.05s window (~33 fps effective over WSL/usbipd/Aravis).
The short (~1s) capture duration is Kineo's own application-level
behavior (consistent with typical CASA short high-fps capture windows),
not an artifact of the bridge.

**STOP CONDITION MET: Success Levels 3 (real video visible in Kineo's
UI) and 5 (complete Analysis using real camera frames, real report
produced) both reached**, via the real IDS U3-3560XCP-M streamed through
WSL2/Aravis/usbipd into the custom `probe/m5/m5_bridge.cti` GenTL
producer, unmodified `probe/m3/m4h/` baseline preserved throughout.
