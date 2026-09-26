# Kineo Bridge — Final Plan (Phase 8)

Revision 2 — corrected per user review before any M0 implementation work
began. Revision 1's Step 0/M0-M2 sequencing, kill-point criteria, M4 scope,
IPC assumptions, and effort estimates were all found insufficiently precise
and are corrected below. No implementation has started; this is still a
planning-only document.

Based on the full investigation in `~/kineo-bridge/investigation/` (Phases
1-7). All evidence is CONFIRMED unless marked INFERENCE or UNKNOWN in the
underlying phase files — this document doesn't re-derive it, only builds on
it.

## 1. Recommended architecture

```
Kineo UI -> real KineoDeviceService.exe -> real ids_peak.dll
    -> custom x64 GenTL producer (.cti, new file, added alongside — not
       replacing — the existing IDS/TIS CTIs)
    -> TCP over 127.0.0.1 (or WSL2 VM IP — see M5a) (control + data channel)
    -> WSL2 ARM64 bridge process -> Aravis 0.8.36 -> real IDS U3-3560XCP-M
```

This is Route A from `investigation/routes.md`, unchanged from the user's
original preferred architecture.

## 2. Why this is preferred over the alternatives

See `investigation/routes.md` for the full comparison. In one line each:

- **B (GigE bridge):** camera is natively USB3 Vision, not GigE — this route
  requires emulating a different vision standard as a detour, and the user's
  own prior testing already got stuck after GVCP discovery with no clear next
  step, whereas Route A stays inside the abstraction the camera already
  speaks (via Aravis).
- **C (WinUSB/custom U3V stack):** the real camera already streams
  successfully today via WSL/Aravis/USB-IP — this route would reimplement
  that from scratch in user-mode Windows code for a fraction of the benefit.
- **D (proxy ids_peak.dll):** Kineo links `ids_peak.dll`'s C++ class API
  directly (confirmed via `peak::core::ProducerLibrary`/`CTILoadingException`
  symbols) — proxying that is far more fragile than the flat-C GenTL export
  boundary Route A targets.
- **E (replace the IDS CTI in place):** conflicts with the user's explicit
  standing rule not to modify installed `.cti` files, and isn't necessary —
  Route A's own M1 findings (below) show the actual requirement (correct
  `TL_INFO_VENDOR` string + functional completeness) is satisfiable in a new,
  additively-placed CTI without touching the existing one.
- **F (WebSocket emulation of KineoDeviceService):** already deprioritized by
  the user; would require reimplementing Kineo's proprietary OpenVINO/
  tracking pipeline, which every other route (including this one) is
  specifically designed to avoid.
- **G (native ARM64 driver):** gated on IDS's roadmap and Windows ARM64
  kernel driver signing — outside engineering control on any useful
  timeline.
- **H (spare x86-64 PC):** zero engineering risk but doesn't solve the actual
  goal (single portable ARM64 device); kept as a fallback safety net during
  development, not a target architecture.

**Revised (M1 corrects Phase 3's original assessment):** `ids_peak.dll`
discovers producers via a generic env-var + directory-scan + `LoadLibrary`
mechanism (confirmed, Phase 3) — but `ProducerLibrary::Open()`, the actual
acceptance API both `ids_peak` and (per Phase 3's symbol evidence) Kineo's
own code use, **does enforce a vendor gate**, empirically confirmed via a
control-matrix test: both real, currently-installed, otherwise-legitimate
TIS GenTL producers are rejected with the identical
`PEAK_RETURN_CODE_NOT_AVAILABLE` error real third-party CTIs get. This is
not a route-killer — M1's bisection (`investigation/progress.md` M1 section)
pinned down exactly two gates a producer must satisfy: (1) `GCGetInfo
(TL_INFO_VENDOR)` must return the exact string `"IDS Imaging Development
Systems GmbH"`, and (2) some functional-completeness check beyond the
GC-lifecycle family (not yet pinned to one exact function) must not see
`NOT_IMPLEMENTED` responses. Both are straightforwardly satisfiable in the
real bridge CTI: report IDS's vendor string, and implement genuine (not
stub) port/XML/streaming behavior in M3/M4 rather than placeholders. Prior
runtime diagnostics also confirmed `KineoDeviceService.exe` loads TIS x64
CTIs at the raw `LoadLibrary` level — consistent with this two-layer model
(file loads fine; the *application-level* `ProducerLibrary::Open()` gate is
where vendor/completeness is actually enforced).

## 3. Unknowns that must be resolved first

In order of how early they can surface:

1. **RESOLVED by M1 (see `investigation/progress.md` M1 section):** the
   producer-library-level gate is now known precisely — `GCGetInfo
   (TL_INFO_VENDOR)` must return `"IDS Imaging Development Systems GmbH"`
   exactly, AND some functional-completeness check beyond the GC-lifecycle
   functions must pass (not yet pinned to one exact function among the other
   78 exports). What's still genuinely open: (a) the exact function(s)
   responsible for gate 2, (b) whether Kineo's own code path enforces
   anything *further* beyond what `ProducerLibrary::Open()` itself checks
   (untested against real Kineo — this is what M2 must now confirm, with the
   probe's vendor string fixed first), and (c) whether device-level
   `DEVICE_INFO` metadata (vendor/model/serial) needs to similarly match an
   IDS-like pattern once a device (not just the producer library) is
   presented — not yet tested since M1 only got as far as `ProducerLibrary`/
   `System` level, not device enumeration.
2. **Does IDS peak's C++ GenApi node-map wrapper (confirmed linkage,
   `kineo-camera-contract.md`) require a fairly complete/well-formed GenICam
   XML, or will a minimal synthetic one work?** — affects how much M3 effort
   is needed.
3. **Which GenICam nodes does Kineo actually read/write and in what order**
   relative to `AcquisitionStart` (PixelFormat and device-identity nodes in
   particular weren't confirmed as literal strings — `kineo-camera-contract.md`
   §4 UNKNOWN #1-3) — resolved by a live GenApi node-map dump once a real
   device is being opened by Kineo (naturally falls out of M2/M3 testing).
4. **Exactly which DataStream/Event calls `ids_peak` actually issues during
   acquisition** — do not assume the minimal `DSStartAcquisition`/
   `DSGetBufferInfo` pair is sufficient; see the expanded M4 scope below.
5. **Whether Windows x64 process -> WSL2 bridge TCP connectivity actually
   works over `127.0.0.1` in the current WSL NAT configuration**, or whether
   the bridge needs to bind to the WSL2 VM's own IP instead — untested,
   addressed by the new M5a below, before any real IPC code is written.
6. **Actual sustained FPS achievable through the existing USB/IP -> Aravis
   link** — not a blocker for the bridge design (confirmed non-bottleneck at
   the IPC layer, `wsl-bridge-design.md`), but determines what FPS target is
   realistic to promise by M9.

## 4. Milestone sequence

Ordering (unchanged, preserved as the strong existing structure): **CTI load
→ device enumeration → device open/GenICam → synthetic frames → independent
WSL bridge → real frames → controls → full analysis → hardening.**

### Step 0 — Optional observation only (NOT a kill signal, no approval needed to skip)

**Status: downgraded.** Revision 1 treated this as an early kill-point test.
It is not one: prior runtime diagnostics already confirm
`KineoDeviceService.exe` loads TIS x64 CTIs, so the "does Kineo load a
third-party CTI at all" question this step targeted is already answered.
Additionally, a TIS USB3Vision transport with **zero physically attached TIS
devices** may legitimately show nothing in Kineo's UI even when the CTI is
correctly loaded and enumerated — an absence of UI signal here would prove
nothing either way.

**Retained only as an optional, low-cost observation**, not a gating test:
if convenient during Step-0-adjacent work, note whether Kineo's UI shows any
trace of a second transport layer/interface — but do not block, delay, or
draw conclusions from this alone. Skip it entirely if it adds friction.

### M0 — CTI loads and enumerates a complete fake device (standalone harness, no env var touched)

**Deliverable:** `gcc-mingw-w64-x86-64` toolchain installed (pending separate
approval — see `investigation/toolchain.md`); a probe `.cti` exporting the
minimal GenTL set identified in `investigation/gentl-exports.md` §"Minimal
loadable export set" (`GCGetInfo`/`GCInitLib`/`GCCloseLib`/`GCGetLastError`,
`TLOpen`/`TLClose`/`TLGetInfo`/`TLGetNumInterfaces`/`TLGetInterfaceID`/
`TLGetInterfaceInfo`/`TLOpenInterface`/`TLUpdateInterfaceList`,
`IFGetNumDevices`/`IFGetDeviceID`/`IFGetDeviceInfo`/`IFOpenDevice`/
`IFUpdateDeviceList`/`IFClose`/`IFGetInfo`/`IFGetParentTL`,
`DevGetInfo`/`DevClose`/`DevGetParentIF`/`DevGetPort`), each call appending a
timestamped line to a log file under the test harness's own scratch
directory (never inside Kineo's tree).

**Corrected scope (was inconsistent in Rev 1):** the probe reports, from
this milestone onward, **1 transport layer, 1 interface, and 1 minimal fake
device** — not just a bare interface with zero devices. The device does
**not** need streaming support yet (no working `DS*`/`Event*` calls
required at M0) — it only needs to exist and answer `IFGetDeviceID`/
`IFGetDeviceInfo`/`IFOpenDevice`/`DevGetInfo` with plausible values.

A tiny standalone test harness `.exe` (also mingw-built) `LoadLibrary`s the
probe by absolute path and calls each function directly, including opening
the fake device — no Kineo, no `ids_peak.dll`, no env var involved at all.

**Success criterion:** Harness enumerates exactly 1 interface and 1 device,
opens the device successfully, and the probe's log shows each function
entered in the expected order.

**Exact test:** Run `harness.exe <path-to-probe.cti>` from a scratch
directory; inspect the log; confirm the harness reports the device as open.

**Expected logs:** `[GCInitLib] called`, `[TLUpdateInterfaceList] called`,
`[IFUpdateDeviceList] called`, `[IFGetNumDevices] returning 1`,
`[IFGetDeviceInfo] ...`, `[IFOpenDevice] called`, `[DevGetInfo] called`.

**Rollback:** Delete the scratch directory. Nothing else was touched.

**Likely failure modes:** DLL fails to load because the mingw runtime wasn't
statically linked (`toolchain.md` gotcha — fix with `-static
-static-libgcc -static-libstdc++`); exports come out `__stdcall`-decorated
(`_Name@N`) instead of the plain names GenTL expects (fix with a `.def` file
or `-Wl,--kill-at`).

### M1 — `ids_peak.dll` itself accepts the probe (DONE — findings below; supersedes the original plan)

**What actually happened (better than planned):** Rather than a hand-rolled
harness calling undocumented `PEAK_*` signatures, used IDS's own official
`ids_peak` PyPI package (`ids-peak==1.9.0.0.2`), whose bundled `ids_peak.dll`
is sha256-identical to Kineo's copy — results are directly authoritative.
Called `ProducerLibrary.Open(cti_path)` on the probe by explicit path (no
`GENICAM_GENTL64_PATH` touched at all — even simpler than planned, and still
fully process-local/reversible).

**Result:** Initial probe **rejected** (`PEAK_RETURN_CODE_NOT_AVAILABLE`,
"Provided producerLibrary is not supported."). Extensive bisection (control
matrix across 7 candidates including both real installed TIS producers, a
forwarding proxy, and a hybrid proxy — see `investigation/progress.md` M1
section for full method) found **two distinct required gates**, both now
**CONCLUSIVELY RESOLVED**:

1. `GCGetInfo(TL_INFO_VENDOR)` must return exactly `"IDS Imaging Development
   Systems GmbH"` — both TIS producers and our probe's original
   `"IMV/KineoBridge"` value were rejected on this basis alone.
2. **Exact export-name parity** with the real CTI's 81-symbol table — a
   fully standalone producer (`m1c_standalone.c`, zero forwarding of any
   ordinary GenTL function, all 57 real GenTL functions + vendor string
   are 100% our own code, only the 24 non-GenTL noise symbols
   PE-forwarded purely for symbol-surface completeness) **PASSED**
   `ProducerLibrary.Open()` → `OpenSystem()` → interface/device enumeration
   → `IsOpenable(Control)=True`, stopping only at `OpenDevice()` with a
   precise, expected M3-territory cause (`GCGetPortInfo(PORT_INFO_PORTNAME)
   = NOT_IMPLEMENTED`) — confirming gate 2 requires no deeper functional
   behavior than export completeness. Full official-EMVA-header ABI diff
   (`investigation/gentl-header-diff.md`) and exact export-set diff
   (`investigation/m1-export-diff.txt`) back this up. No angr/disassembly
   was needed — stop condition met via runtime evidence alone.

**Confirmed working recipe for the real bridge CTI going forward:** exact
81-symbol export table (55 common GenTL + 2 IDS-only multipart DS + 23
`OS_*` + 1 CRT noise symbol — the latter 24 can remain PE-forwarded to any
throwaway target, or eventually be dropped once we understand whether
`ids_peak` truly needs them present vs. just doesn't crash when they exist)
+ `TL_INFO_VENDOR = "IDS Imaging Development Systems GmbH"`. No other
functional requirement gates `ProducerLibrary.Open()` or device enumeration.

**Rollback:** None needed (no persistent state was touched — all testing
used explicit-path `ProducerLibrary.Open()`, never `GENICAM_GENTL64_PATH`).

**Next step:** M3 (real port/GenICam-XML behavior) is now unblocked and
correctly scoped — implement `GCGetPortInfo`/`GCGetPortURL` iteratively
against this same fast official-bindings test loop. M2 (real Kineo) is also
now unblocked, with the exact vendor-string + export-parity recipe in hand.

### M2 — Real Kineo process loads the CTI and attempts to enumerate/open the device (env var scoped to Kineo's own process tree only)

**Prerequisite (from M1's finding):** the probe must report
`TL_INFO_VENDOR = "IDS Imaging Development Systems GmbH"` and satisfy
whatever functional-completeness gate 2 requires — do not attempt M2 with a
probe still using a placeholder vendor string or all-stub port/streaming
behavior; it will be rejected before Kineo's own code ever gets a chance to
apply any *further* filtering, confounding the M2 test.

**Corrected environment handling (Rev 1 was wrong here):** Do **not** make
any persistent User- or Machine-scope `GENICAM_GENTL64_PATH` change. Instead,
launch Kineo from a parent process (a small launcher script/exe) that sets
`GENICAM_GENTL64_PATH` (original 4 segments + the probe's scratch directory)
**only in that process's own environment block**, then starts
`KineoDeviceService.exe`/Kineo's Electron app as a child process, which
inherits the modified variable only within that process tree. **Closing that
Kineo instance and its launcher restores the original environment
automatically** — no registry/User/Machine env var is ever touched, and no
manual revert step is needed.

**Deliverable:** none new — same probe CTI from M0/M1, launched under the
scoped-environment launcher above.

**Test:** Start the launcher (which starts Kineo with the modified
process-local env), check the probe's log file to confirm the real
`KineoDeviceService.exe`/`ids_peak.dll` process actually touched the probe
CTI, and separately check whether the fake device appears in Kineo's own
device list (UI or the `ws://localhost:9002` API).

**Success criterion (two independent things to check, not one):**
- (a) The probe's log confirms the real Kineo process called into the CTI at
  all (proves the CTI *loads* under real Kineo — this is close to already
  confirmed by the prior TIS-CTI runtime diagnostic, but confirm it for
  *this* probe specifically).
- (b) The fake device appears in Kineo's own enumeration/UI (proves Kineo's
  application layer surfaces third-party-CTI devices, not just loads the
  CTI file).

**These are logged and evaluated separately** — per §6, failing (b) while
(a) succeeds is not itself a kill signal; it narrows down *where* in the
pipeline the gap is (device metadata vs. GenICam bootstrap vs. genuine
filtering).

**Rollback:** Close the launcher/Kineo process. No persistent state was
changed.

**Likely failure modes:** See the multi-condition kill-point criteria in §6
— several distinct causes could explain the device not appearing, most of
which are fixable (not route-ending) on their own.

### M3 — Device open + valid GenICam XML (M3a/b/c DONE against official bindings; M3d-full-node-set + real-Kineo retest remain)

**What actually happened:** Built `probe/m3/m3a_standalone.c` +
`kineo_bridge.xml` on the M1c baseline (M0/M1 untouched), tested
incrementally against the official Python bindings after each addition.

- **M3a (`GCGetPortInfo`):** implemented for both `H_DEV` (local "Device"
  port — the actual handle `ids_peak` uses per the M2 trace, `DevGetPort`
  is never called) and `H_PORT` (`RemoteDevice` port). All `PORT_INFO_CMD`
  values per official `GenTL_v1_5.h`.
- **M3b (port URL):** `URL_SCHEME_FILE` was tried first (simpler, official)
  but empirically **not acted on** by this `ids_peak` version — it queried
  URL info but never opened the file, silently yielding an empty node map.
  Switched to `URL_SCHEME_LOCAL` (register-mapped via `GCReadPort` against
  a fixed virtual base address) — worked immediately.
- **M3c (minimal XML):** `DeviceVendorName`/`DeviceModelName`/
  `DeviceSerialNumber`/`Width`/`Height`/`PixelFormat`/`PayloadSize` as
  simple non-register-backed nodes. First attempt got a node map but
  `Nodes()` failed with `"Could not connect node map with port
  (Port-Name: Remote)!"` — fixed by adding an explicit `<Port
  Name="Remote">` element (GenApi's mandatory connection anchor,
  independent of whether any feature node is actually register-backed).

**Result: `NodeMaps(): OK, count=1`, `Nodes(): OK, count=10`.** Values
verified readable: `DeviceVendorName`/`DeviceModelName`/`DeviceSerialNumber`/
`Width`/`Height`/`PayloadSize` all read back exactly as authored.
`PixelFormat` (an `EnumerationNode`) needs a different test-script accessor
— cosmetic, not a producer defect. Full detail:
`investigation/progress.md` M3a/M3b/M3c section.

**Revised success criterion (met, for the official-bindings path):**
node map constructs and connects with no error; core identity/dimension
values readable. **Not yet done:** re-running the M2-style real-Kineo smoke
test against this M3-complete CTI (only tested against the pre-M3 stub
version so far, and only against the official Python bindings for the M3
work itself); adding the remaining SFNC nodes
(`ExposureTime`/`Gain`/`BlackLevel`/`AcquisitionStart-Stop`/
`TLParamsLocked`/`TriggerMode`/`OffsetX-Y`) — deferred until a real trace
(official bindings or real Kineo) proves they're actually requested, per
the established "implement only what's proven necessary" discipline;
write-path support (`GCWritePort`/settable nodes) — not implemented, all
current nodes are effectively read-only.

**Exact test:** `probe/m3/nodemap_test.py` / `read_values_test.py` against
the official `ids_peak` bindings (fast iteration); real-Kineo retest still
pending, same scoped-launcher approach as M2.

**Rollback:** Close launcher/Kineo — same as M2. No persistent state
touched by any M3 testing (official-bindings tests don't touch
`GENICAM_GENTL64_PATH` at all, same as M1).

**Likely failure modes going forward:** real Kineo may request additional
node names or a different read/write order than the official bindings did;
the `URL_SCHEME_LOCAL` register-address scheme may need to handle
multi-chunk reads correctly at larger XML sizes (not yet stress-tested with
a much bigger XML once the full SFNC node set is added).

### M4 — Synthetic 1920x1200 Mono8 frames delivered end-to-end (expanded scope)

**Corrected scope (Rev 1 under-specified this):** Do **not** assume
`DSStartAcquisition` + `DSGetBufferInfo` alone are sufficient. Implement
runtime logging in the probe CTI's DataStream/Event layer from the start,
and build out **only what `ids_peak` actually calls**, observed via that
log, rather than guessing the full set upfront. The following are likely
needed and should each be logged/verified individually rather than assumed
present or absent:

- `DSGetInfo`
- `DSAnnounceBuffer` (and `DSAllocAndAnnounceBuffer` if `ids_peak` requests
  that variant instead/as well)
- `DSQueueBuffer`
- `DSRevokeBuffer`
- `DSFlushQueue`
- `DSStartAcquisition` / `DSStopAcquisition`
- `DSGetBufferInfo` (including correct buffer status, payload size,
  block/frame ID, and timestamp fields per buffer — not just a bare
  "success" flag)
- `EventRegister` for `EVENT_NEW_BUFFER`, `EventGetData`, `EventKill` (or
  whatever cancellation/shutdown behavior `ids_peak` expects — confirm via
  log rather than assuming polling-only vs. event-driven delivery)
- Correct buffer queue recycling (a delivered buffer must be re-announced/
  re-queued in the exact sequence `ids_peak` expects, confirmed via log, not
  assumed from the GenTL spec alone)

**Deliverable:** Probe CTI's DataStream delivers synthetic (e.g. solid-gray
or test-pattern) frames of the correct payload size (2,304,000 bytes,
confirmed) on `AcquisitionStart`, implementing whichever of the above calls
the runtime log shows `ids_peak` actually issuing — no WSL bridge involved
yet.

**Success criterion:** Kineo's own viewer shows a live synthetic feed at
correct resolution; `Analysis-Start`/`PerfTest-Start` protocol messages can
be exercised without crashing.

**Exact test:** Trigger the same UI flow used for a real camera; cross-check
the DataStream/Event call log against the list above and note anything
called that wasn't anticipated.

**Rollback:** Same as M2 (close launcher/Kineo).

**Likely failure modes:** Missing an event/buffer-field the spec allows as
optional but `ids_peak` treats as required in practice; buffer-recycling
timing issues if Kineo/`ids_peak` expects a specific acquisition-thread
model.

### M5 — WSL bridge built and verified independently

**Deliverable:** Standalone WSL bridge implementing the control+data
protocol from `investigation/wsl-bridge-design.md` (OPEN/CONFIGURE/START/
STOP/GET_BUFFER_INFO + one-way frame data channel), using the confirmed
Aravis 0.8.36 API (`arv_update_device_list`/serial-match/`arv_camera_new`/
`arv_camera_set_pixel_format_from_string`/`arv_camera_set_region`/exposure-
gain-black-level setters/`arv_camera_create_stream`+callback streaming).

#### M5a — Verify Windows-x64 <-> WSL2 TCP connectivity FIRST (new — before writing the real IPC layer)

**Do not hard-code networking assumptions.** `wsl-bridge-design.md`
recommended plain TCP over `127.0.0.1` based on WSL2's typical default
localhost-forwarding behavior, but this was never actually tested in this
WSL NAT configuration. Before building the real bridge protocol:

1. Stand up a trivial TCP echo/ping listener in WSL2, bound to
   `127.0.0.1:<port>`.
2. From a small Windows-side test client (can be a throwaway script/exe),
   attempt to connect to `127.0.0.1:<port>`.
3. **If that connects reliably:** proceed with `127.0.0.1` as designed.
4. **If it does not connect reliably:** find the WSL2 VM's actual IP (e.g.
   `ip addr show eth0` inside WSL, or `wsl hostname -I`), bind the bridge's
   listener to that address (or `0.0.0.0`), and have the Windows-side client
   connect to that IP instead. Note that the WSL2 VM IP can change across
   reboots in NAT mode — if this path is needed, the launcher from M2 should
   also resolve/pass the current WSL IP to the CTI at startup rather than
   hard-coding it.

**Success criterion:** A basic byte-echo round-trip succeeds reliably
(repeat 10+ times) over whichever address is chosen.

**Only after M5a passes** does the real control+data protocol
implementation begin.

**Success criterion (M5 overall):** Bridge opens serial `4110010861`,
configures 1920x1200 Mono8 + exposure/gain/black level, streams real frames
to a throwaway TCP test client (using the address confirmed in M5a) at a
chosen FPS.

**Exact test:** Run the bridge; connect a simple test client; verify
received frame count/size/rate.

**Rollback:** Kill the bridge process — no Windows/Kineo state touched.

**Likely failure modes:** USB/IP throughput ceiling caps achievable FPS
(pre-existing, known constraint); `control-lost` signal handling needs
tuning; WSL2 VM IP instability across reboots if `127.0.0.1` forwarding
turns out not to work (see M5a).

### M6 — Real camera frames flow through the full stack

**Deliverable:** Probe CTI's DataStream now pulls frames from the bridge's
data channel (TCP client role, using the M5a-confirmed address) instead of
generating synthetic ones, reusing the exact call sequence proven in M4.

**Success criterion:** Kineo's live view shows the real camera image
end-to-end.

**Exact test:** Point the real camera at a recognizable scene; confirm it
appears live in Kineo.

**Rollback:** Stop bridge/CTI; close launcher/Kineo as in M2.

**Likely failure modes:** Frame-rate mismatch between Kineo's expected
cadence and sustainable USB/IP throughput; buffer-queue starvation.

### M7 — Camera controls plumbed through

**Deliverable:** `CONFIGURE` messages triggered by Kineo writing
`ExposureTime`/`Gain`/`BlackLevel`/`TriggerMode` nodes are forwarded through
CTI -> bridge -> Aravis -> real camera, with read-back matching.

**Success criterion:** Changing exposure/gain in Kineo's UI visibly changes
the real camera's image.

**Exact test:** Adjust exposure in Kineo UI, observe the change live.

**Rollback:** Same as above.

**Likely failure modes:** Node write-ordering expectations (e.g.
`TLParamsLocked` must be 0 during configuration, 1 during acquisition) not
correctly modeled.

### M8 — Complete Kineo analysis pipeline verified

**Deliverable:** none new — run Kineo's actual `Analysis-Start`/
`PerfTest-Start` OpenVINO/tracking pipeline against the bridged real camera.

**Success criterion:** All protocol messages (`Analysis-Start-Response`,
`Video-Ready`, completion) succeed with no new errors vs. known-good
behavior; tracking output looks sane.

**Exact test:** Run one full analysis session via Kineo's own UI.

**Rollback:** N/A (observe-only); close launcher/Kineo only if abandoning.

**Likely failure modes:** Residual GenApi node-map gaps not caught earlier
surface under sustained/realistic load for the first time.

### M9 — Reliability / performance hardening

**Deliverable:** `control-lost` -> bridge -> CTI `DEVICE_LOST` -> Kineo
surfaces a sane error instead of hanging; sustained multi-minute acquisition
without drops; documented achievable FPS ceiling; install/packaging (bridge
auto-start, the M2 scoped-environment launcher packaged as the standard way
to start Kineo with the bridge active, WSL IP resolution per M5a if needed).

**Success criterion:** 30-60 minute soak test with zero unhandled
crashes/hangs; deliberate camera unplug/replug (or usbip detach/reattach —
non-destructive, explained-first per the standing safety rules) recovers
cleanly.

**Exact test:** Extended soak run + deliberate disconnect test.

**Rollback:** N/A — finishing milestone.

## 5. Earliest kill point (corrected — no longer a single milestone)

**Rev 1 incorrectly named M2 as an automatic kill point. It is not.** Device
non-appearance at M2 has multiple possible causes, most of which are fixable
within the route rather than route-ending:

- missing or incorrect `DEVICE_INFO` fields
- incomplete GenTL exports for whatever `ids_peak` actually calls
  (discoverable and fixable via M1's logging)
- invalid/incomplete metadata in the fake device's identity
- incomplete device-port behavior (`GCReadPort`/`GCGetPortURL` edge cases)
- missing or malformed GenICam bootstrap/XML behavior (M3's concern, may
  need to be pulled earlier if M2 fails for this reason)
- genuine Kineo-side device filtering (the only truly route-ending cause)

**The route may only be considered fundamentally blocked once all four of
these hold simultaneously:**

**A.** The standalone harness + `ids_peak.dll` combination (M1) successfully
enumerates **and opens** the fake device — proving the CTI itself is
correctly built and `ids_peak`'s generic path accepts it.

**B.** The real Kineo process **definitely loads** the custom CTI (confirmed
via the probe's own log during an M2 test run — independent of whether the
device appears in Kineo's UI).

**C.** Kineo/`ids_peak` **definitely invokes its enumeration path** against
the loaded CTI (again confirmed via log — i.e., it's not simply never
calling `TLUpdateInterfaceList`/`IFUpdateDeviceList` on this producer at
all).

**D.** The presented device has **required standard/IDS-like metadata**
(iterated based on what M1/M2 logging reveals `ids_peak`/Kineo actually
queries — vendor/model/serial/access-status fields, GenICam bootstrap
behavior from M3 pulled forward if needed).

**...and Kineo still deliberately declines to surface/open the device.**
Only at that point — with A-D all independently confirmed — is there real
evidence of intentional Kineo-side filtering rather than an incomplete CTI
implementation. Until then, treat any M2 non-appearance as a debugging
target (iterate the CTI), not a route-kill signal.

## 6. Effort estimate (engineering hours + compile/test iterations, not calendar time)

**Corrected: presented per-stage, not as a single total implying "time to
first real frame."**

| Stage | Est. hours | Est. iterations | Notes |
|---|---|---|---|
| Toolchain setup (part of M0) | 0.5-1 | 1-2 | apt install + trivial DLL sanity check |
| **CTI load** (M0 harness-level load) | 1-2 | 3-5 | |
| **Fake device enumeration** (M0 device fields, M1 ids_peak, M2 real Kineo) | 3-6 | 8-15 | Includes iterating on §6 A-D criteria if M2 doesn't show the device immediately |
| **Fake device open + GenICam** (M3) | 4-8 | 10-20 | Highest uncertainty stage — node-map strictness risk |
| **First synthetic frame** (M4, expanded DataStream/Event scope) | 5-8 | 10-18 | Larger than Rev 1's estimate given the expanded call list in §4 M4 |
| M5a (connectivity check) | 0.5-1 | 2-4 | Do first, before real IPC code |
| **WSL bridge, independent of CTI** (M5 core) | 6-10 | 10-15 | |
| **First real camera frame** (M6, wiring bridge into CTI) | 4-6 | 8-12 | |
| Controls (M7) | 3-5 | 6-10 | |
| **Complete Kineo analysis working** (M8) | 2-4 | 3-5 | Mostly testing/observation |
| **Production hardening** (M9) | 6-10+ | ongoing | Open-ended by nature |
| **Subtotal, CTI load -> first real camera frame (M0-M6)** | **~24-42 hours** | **~52-89** | This is the milestone most people mean by "does the bridge basically work" |
| **Subtotal, through complete analysis (M0-M8)** | **~30-51 hours** | **~61-99** | |

No single number should be quoted as "time to first real frame" — that
specific milestone (M6) only completes after M0-M5 (including M5a) are all
done, at roughly the 24-42 hour mark, not at the start.

## 7. First implementation task

The first implementation task with actual code is **M0**: a probe x64 `.cti`
that exports the minimal GenTL API identified in
`investigation/gentl-exports.md`, writes every call to a log file, and
reports **exactly one fake interface and one fake device** (not zero
devices — corrected from Rev 1) that the standalone test harness can
enumerate and open. It is loaded only by a throwaway standalone test
harness — never by Kineo, never via any system-scope `GENICAM_GENTL64_PATH`
change — so it is safe to build and run without touching the physical
camera, Kineo, or any Windows-protected path at all.

Step 0 (optional TIS-CTI observation) may be done opportunistically but is
not a prerequisite and should not delay starting M0.

## Files created this investigation

- `investigation/inventory.md` (Phase 1)
- `investigation/gentl-exports.md` (Phase 2)
- `investigation/ids-peak-loading.md` (Phase 3)
- `investigation/kineo-camera-contract.md` (Phase 4)
- `investigation/toolchain.md` (Phase 5)
- `investigation/wsl-bridge-design.md` (Phase 6)
- `investigation/routes.md` (Phase 7)
- `investigation/progress.md` (running log, all phases)
- `PLAN.md` (this file, Phase 8 — Revision 2)
- `investigation/raw/` (raw strings/objdump dumps backing the above)
