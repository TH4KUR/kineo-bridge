# Progress Log

## Phase 1 — Inventory

**What was tested:** Direct filesystem/PowerShell inspection (no subagent) of all
key binaries (`KineoDeviceService.exe`, `ids_peak.dll`, `ids_peak_ipl.dll`, both
Kineo-bundled CTIs, all 4 TIS IC4 CTIs), `GENICAM_GENTL64_PATH` env var (Machine +
User scope), the standalone `C:\Program Files\IDS\ids_peak` tree, and host
toolchain presence (Visual Studio, Windows SDK, mingw, clang) on both the Windows
and WSL sides.

**What was learned:** All target binaries confirmed present, correct arch
(PE32+ x86-64, TIS ARM64 siblings also present), with exact FileVersion/Company
metadata recorded in `inventory.md`. `GENICAM_GENTL64_PATH` is set identically at
Machine and User scope, listing 4 producer directories. The first segment
(`C:\Program Files\IDS\ids_peak\ids_u3vgentl\64`) doesn't exist — its parent dirs
exist but are completely empty, one day old, consistent with a partially
uninstalled/failed standalone IDS Peak SDK install. No Visual Studio, Windows SDK,
or `cl.exe` found on the Windows side; no clang/mingw yet on the WSL side (but
`objdump`/`llvm-objdump`/`nm`/`strings`/`python3` are present). No Kineo process
was running at investigation time, so runtime CTI-load confirmation was deferred
to Phase 3.

**What remains unknown:** Which CTIs Kineo actually loads at runtime (needs a live
process check); whether the TIS "USB3Vision Devices 1.4" 4th path segment exists
(resolved later in Phase 3: it does, and is populated).

**Next highest-value experiment:** Parallel dispatch of Phases 2-6 against the
confirmed file set (done — see below).

## Phase 2 — GenTL Export Comparison

- Extracted full PE export tables from all 4 target x64 CTIs plus both arm64
  siblings using `llvm-objdump -p` (GNU `objdump -p` failed to recognize the
  PE format on these files — noted as a tooling quirk, llvm-objdump is the
  reliable tool here going forward).
- IDS's two CTIs (`ids_u3vgentlk.cti`, `ids_ueyegentl.cti`) export an
  identical 81-symbol surface; TIS's two CTIs (`u3v`, `gev`, x64 and arm64)
  export an identical 59-symbol surface. So there are really only 2 distinct
  export surfaces across all 4+2 files, not 4.
- 55 GenTL-named exports are common to both vendors (full GC/Event/TL/IF/Dev
  families, 13/16 of DS family). IDS adds multi-part buffer support
  (`DSGetNumBufferParts`, `DSGetBufferPartInfo`); TIS adds
  `GCInitLibShared`/`DSGetBufferInfoStacked` (uncertain spec status) plus
  internal debug exports (`HandleMapCount`, `ObjectTrackerCount`). IDS also
  incidentally exports ~24 internal `OS_*`/CRT helper symbols that are noise,
  not GenTL API.
- String evidence (`EVENT_FEATURE_DEVEVENT in GenTL up to version 1.4`
  embedded in TIS XML text, current `EVENT_REMOTE_DEVICE` naming used by both
  vendors) directly confirms both vendors target **GenTL ≥1.5**; exact minor
  version (1.5 vs 1.6) remains UNKNOWN — no literal "GenTL 1.x" version string
  was found in any binary via static `strings` search.
- Remaining unknown: whether TIS's omission of the multi-part DS exports
  reflects a lower feature level or just unused-for-their-device-class
  optional exports; whether `DSGetBufferInfoStacked`/`GCInitLibShared` are
  official spec functions or vendor extensions (would need the actual GenTL
  spec PDF/header to confirm — not available in this environment).
- Next highest-value experiment: pull the actual `GenTL.h`/GenTL spec version
  this environment can access (or the GenApiSchemaVersion inside the embedded
  XML, e.g. `SchemaVersion` attribute at top of the XML blob) to pin the exact
  GenTL version, and/or use `GCGetInfo`/`TLGetInfo` info command IDs from a
  real GenTL.h header (if one can be located anywhere on disk, e.g. inside a
  GenICam SDK install) to map which numeric `INFO_CMD` values these producers
  actually answer — that would nail down the version question this phase left
  as ≥1.5-but-unconfirmed-exact.

## Phase 3 — ids_peak Loading Behavior

**What was tested:** Static/binary inspection only (no process launches). Ran `strings`
on `ids_peak.dll` (v1.9.0.0) and `KineoDeviceService.exe` (v1.2.0.0), targeting
`GENICAM_GENTL64_PATH`, `.cti`, registry-key-looking strings, and vendor-name/allowlist
strings. Ran `llvm-objdump -p` on `ids_peak.dll` to inspect its import table (plain GNU
`objdump -p` failed with "file format not recognized" on this binary). Searched the
entire `/mnt/c/IMVapps/Kineo Software` tree for config files (json/ini/xml/yaml/cfg/txt)
mentioning `.cti`/`GenTL`/`producer`. Performed read-only PowerShell registry reads
(`Get-ChildItem`) against `HKLM:\SOFTWARE\GenICam`, its `WOW6432Node` twin, and a broader
filtered scan for GenTL/GenICam/EMVA/IDS/Kineo/IMV-named keys. Also verified on disk
whether the 4th `GENICAM_GENTL64_PATH` segment (TIS USB3Vision 1.4 driver) exists.

**What was learned:**
- Discovery mechanism is env-var + directory-scan + LoadLibrary, **not** registry-based:
  `ids_peak.dll` imports `_dupenv_s` (reads `GENICAM_GENTL64_PATH`), `FindFirstFileA`/
  `FindNextFileA` (scans directories for `*.cti`), and `LoadLibraryA` (loads matches).
  It imports **no `ADVAPI32.dll`** (no registry APIs) and no Shell32/special-folder APIs.
- Read-only registry scan confirms zero GenICam/GenTL-related keys exist under
  `HKLM:\SOFTWARE` (or Wow6432Node) — no registry involvement anywhere.
- No config file anywhere in the Kineo tree specifies an explicit CTI path list; the
  env var is the sole source of producer search paths.
- No hardcoded vendor-name allowlist strings exist in either `ids_peak.dll` or
  `KineoDeviceService.exe` (checked against ~15 competitor vendor names). CTI validation
  in `ids_peak.dll` looks like a generic GenTL conformance check (`GCInitLib` →
  `GCGetInfo`, "Provided cti is not supported!" on failure), not vendor filtering.
  `KineoDeviceService.exe` is built against IDS's generic `peak::core::ProducerLibrary`/
  `CTILoadingException` C++ wrapper — a multi-producer-capable API, not an IDS-only path.
- Corrected/refined earlier background: the 4th `GENICAM_GENTL64_PATH` segment
  (`...\IC4 GenTL Driver for USB3Vision Devices 1.4`) actually **exists and is
  populated** with real `.cti` files (`ic4-gentl-u3v_x64.cti`, `_arm64.cti`), unlike the
  two empty IDS Peak SDK folders identified in the prior phase.

**What remains unknown:**
- Whether Kineo's running process actually enumerates *all* CTIs on the
  `GENICAM_GENTL64_PATH` (including third-party ones like the TIS driver) at runtime, or
  silently restricts itself to only the two IDS-shipped CTIs via a filter not visible in
  strings (e.g., numeric vendor-ID comparison or hardcoded filename check).
- Whether a custom/bridge `.cti` placed on the path would actually surface a device in
  Kineo's UI/API end-to-end.
- Whether the two empty IDS Peak SDK folders on the path cause any visible startup
  error, or are silently skipped.

**Next highest-value experiment (live test, user must run — this agent does not launch
Kineo or any Windows binary):**
1. Preferred: Process Monitor (Sysinternals ProcMon) trace filtered to
   `KineoDeviceService.exe`, watching `LoadLibrary`/`CreateFile` events against each of
   the 4 `GENICAM_GENTL64_PATH` directories/`.cti` files while Kineo starts normally —
   proves which CTIs are actually touched at runtime.
2. Simpler direct test: temporarily add the TIS `ic4-gentl-u3v_x64.cti` (already
   confirmed present) to a path directory, restart Kineo's service, and check whether a
   connected TIS device appears in Kineo's device list — direct proof of whether Kineo
   accepts a non-IDS CTI's devices, ahead of building any custom bridge CTI.

Full detail: see `~/kineo-bridge/investigation/ids-peak-loading.md`.

## Phase 4 — Kineo Camera Contract

**What was tested:**
- `strings -n 4` (ASCII, 37,501 lines) and `strings -n 4 -e l` (UTF-16LE, 38
  lines) dumped from `KineoDeviceService.exe` to
  `~/kineo-bridge/investigation/raw/strings_ascii.txt` and
  `raw/strings_wide.txt`.
- Exact-line, case-insensitive grep of the ASCII dump against the requested
  GenICam/SFNC target pattern list (`Width Height PixelFormat Mono8
  ExposureTime Gain BlackLevel AcquisitionStart AcquisitionStop
  ExposureStart PayloadSize TriggerMode TriggerSource DeviceVendorName
  DeviceModelName DeviceSerialNumber DeviceUserID TLParamsLocked
  StreamBufferHandlingMode BrightnessAuto GevSCPS StreamID PixelSize
  OffsetX OffsetY`).
- Re-read of the legacy hint file
  `~/kineo-bridge/legacy/websocket-service-probe/protocol-hints.txt`
  (read-only, hint-list use only) to re-confirm IDS peak C++ GenApi
  linkage evidence.
- Listed the exe's install directory contents (read-only) for adjacent
  config/template files.

**What was learned:**
- 13 target node names confirmed present as literal standalone strings:
  `AcquisitionStart`, `AcquisitionStop`, `BlackLevel`, `ExposureStart`,
  `ExposureTime`, `Gain`, `Height`, `OffSetX` (non-standard capitalization),
  `OffsetY`, `PayloadSize`, `TLParamsLocked`, `TriggerMode`, `Width`.
- `PixelFormat`, `Mono8`, `TriggerSource`, `DeviceVendorName`,
  `DeviceModelName`, `DeviceSerialNumber`, `DeviceUserID`,
  `StreamBufferHandlingMode`, `BrightnessAuto*` (as literal node name),
  `GevSCPS*`, `StreamID`, `PixelSize` did NOT appear as exact standalone
  lines in this first pass (does not rule out substring presence).
- Legacy hint file re-confirms Kineo statically links IDS peak's C++
  GenApi node-map wrapper (`ClassCreator<peak::core::nodes::CommandNode>`
  RTTI symbol) — meaning Kineo likely walks a real GenApi node-map object
  model at some point, not just raw GenTL byte streams.
- No `camera_settings.json` template/default file found in the exe's
  immediate install directory (only DLLs, license, encrypted_message.bin,
  private_key.pem, temperature_pids.json — the latter two suggest a
  separate serial/peripheral-hardware subsystem, not the camera contract).
- Full findings, inferred required/optional node classification, and a
  minimal-producer-XML recommendation written to
  `~/kineo-bridge/investigation/kineo-camera-contract.md`.

**What remains unknown:**
- Whether `PixelFormat`/device-identity nodes are queried via IDS peak's
  typed C++ API (never surfacing as literal ASCII strings) rather than
  string-keyed GenApi lookups.
- Full extent of IDS-peak-proprietary (non-SFNC) node names Kineo may
  depend on — planned broader substring/symbol sweep (`peak::core::nodes`,
  `NodeMap`, `FloatNode`/`IntegerNode`/`BooleanNode`/`EnumerationNode`/
  `StringNode`) was not executed this pass.
- Context (error-string pairing) around each hit to firm up
  required-vs-optional classification was not executed this pass.
- Read vs. write semantics and call ordering for every node — unresolved
  by static analysis; explicitly flagged as INFERENCE-only in the
  deliverable.
- Whether a bundled camera_settings template or GenICam XML exists
  elsewhere in the wider Kineo Software tree — not searched this pass.

**Next highest-value experiment:**
Live GenApi node-map dump via ids_peak while Kineo is actually running and
driving the IDS U3-3560XCP-M camera — this would definitively resolve
which nodes are enumerated/read/written and in what order relative to
`AcquisitionStart`, replacing the static-string inference in this phase
with ground truth. Secondary/cheaper follow-up: run the broader
substring/IDS-peak-symbol sweep and context-error-string greps that were
planned but not executed in this pass, before committing to the live-dump
experiment.

## Phase 5 — Build Toolchain

**What was tested:** Read-only `apt-cache`/`apt list` queries (no installs) for
cross-compilation packages in WSL Ubuntu; checked for Visual Studio/Windows SDK/
`cl.exe` on the Windows side (none found, per Phase 1); checked for `clang`/`lld`
and `llvm-mingw` availability in WSL.

**What was learned:** `gcc-mingw-w64-x86-64`/`g++-mingw-w64-x86-64` (GNU mingw-w64,
v13.2.0) is available directly via apt, not yet installed — recommended primary
toolchain: it emits genuine PE32+ x86-64 DLLs with a flat C export table directly
from ARM64 Linux, no emulation, sufficient since only `extern "C"` exports cross
the GenTL ABI boundary. `clang`/`lld` (v21.1.6, plus pinned clang-17/18) are also
available as a fallback, but ride the same mingw-w64 sysroot/headers — not an
independent path. No standalone `llvm-mingw` bundle is packaged. `wine`/`wine64`
available for later load-testing only. Windows-side ARM64-hosted MSVC Build Tools
(x64-targeting `cl.exe`) plausibly exist in recent VS2022 17.x releases but need a
multi-GB download — kept as a deferred, heavier fallback. Key gotchas: must
statically link the mingw runtime (`-static -static-libgcc -static-libstdc++`) to
avoid shipping extra runtime DLLs to Kineo's machine; use `__declspec(dllexport)`
(optionally + `.def`/`--kill-at`) to control export-name decoration for GenTL's
flat C ABI. Full toolchain.md includes the exact trivial-DLL build/verify command
lines for both primary and fallback approaches.

**What remains unknown:** Whether the mingw-built DLL will be accepted by
`ids_peak.dll`'s CTI loader without issue (no MSVC-ABI-specific requirement is
expected for a flat C export surface, but unverified until actually tried).

**Next highest-value experiment:** With user approval, run
`sudo apt-get install -y gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 binutils-mingw-w64-x86-64 mingw-w64-x86-64-dev`,
then build and verify a trivial `extern "C" __declspec(dllexport) void Foo() {}`
DLL, confirming with `file`/`llvm-objdump -p` that it's a valid PE32+ x86-64 DLL
with the expected export — this is the direct precursor to the probe CTI milestone
(M0) in the final plan.

## Phase 6 — WSL Bridge Design

**What was tested:** Read-only inspection of the Aravis 0.8.36 source tree
(`~/aravis-0.8.36/src`) for the concrete API surface needed to discover, open,
configure, and stream from the IDS U3-3560XCP-M by serial number; designed (on
paper, no code written) a minimal Windows-CTI <-> WSL-bridge IPC protocol;
computed bandwidth requirements at 10/20/30/60/100 FPS for 2,304,000-byte frames.

**What was learned:** Confirmed real Aravis 0.8.36 function names (with source-line
citations) for the full pipeline: `arv_update_device_list()` +
`arv_get_n_devices()`/`arv_get_device_serial_nbr(i)` to find the target serial,
then `arv_camera_new(arv_get_device_id(i))` to open it — notably, passing the bare
serial number directly to `arv_camera_new()` does **not** work; USB3 Vision's
device hash table only registers `manufacturer-guid-serial`, `vendor_alias-serial`,
`manufacturer-serial`, and `guid` keys, not the bare serial, so the bridge must go
through the device-list lookup rather than a shortcut. Configuration uses
`arv_camera_set_pixel_format_from_string(cam, "Mono8", ...)` and
`arv_camera_set_region(cam, 0, 0, 1920, 1200, ...)`; exposure/gain/black level have
dedicated convenience setters (`arv_camera_set_exposure_time`, `_set_gain`,
`_set_black_level`), each with an `_is_*_available()` guard. Async streaming uses
`arv_camera_create_stream()` + an `ArvStreamCallback` (INIT/EXIT/START_BUFFER/
BUFFER_DONE) with `arv_stream_push_buffer`/`arv_stream_timeout_pop_buffer` and
`arv_camera_start_acquisition`/`stop_acquisition`. Reconnect handling is exposed via
`ArvDevice`'s `"control-lost"` GObject signal (emitted from the USB3 Vision device
backend) — there's no built-in auto-reconnect; that's bridge-level policy.
Recommended IPC transport: plain TCP over 127.0.0.1 (WSL2's default localhost
forwarding works transparently both directions) with two connections — a small
length-prefixed control channel (OPEN/CONFIGURE/START/STOP/GET_BUFFER_INFO,
async DEVICE_LOST/ERROR) and a separate one-way data channel for frame payloads
(`[u64 frame_id][u64 ts_ns][u32 length][u8 status][raw bytes]`), so bulk frame
data never blocks control acks. Named pipes were rejected (not reachable from
WSL2 without an extra relay); Unix socket + socat rejected (extra moving part, no
bandwidth benefit). Bandwidth table: 23/46/69/138/230 MB/s (184/369/553/1106/1843
Mbps) at 10/20/30/60/100 FPS respectively — all well under typical multi-Gbps
WSL2 loopback TCP throughput, so **the new localhost IPC hop is not the
bottleneck at any tested rate.**

**What remains unknown:** Actual sustained FPS the existing async Aravis pipeline
achieves through USB/IP today — the real, already-known constraint is one hop
upstream (USB/IP throughput/scheduling from the physical camera into WSL2, the
same link already causing unreliable synchronous USB acquisition), not the new
IPC hop.

**Next highest-value experiment:** Measure actual sustained FPS through the
existing async Aravis/USB-IP pipeline (not further IPC optimization) to establish
the real achievable frame rate ceiling before designing the bridge's data channel
around a specific target FPS.

## Phases 7-8

See `~/kineo-bridge/investigation/routes.md` (route comparison) and
`~/kineo-bridge/PLAN.md` (final recommended architecture and milestone plan).

## M0 — CTI Loads and Enumerates a Complete Fake Device (implementation, not investigation)

**What was tested:** Wrote `~/kineo-bridge/probe/gentl.h` (hand-written,
standard-accurate GenTL 1.5 C declarations — no real GenTL.h exists anywhere
on this machine, confirmed by search), `kineo_probe.c` (probe producer
reporting 1 TL/1 interface/1 fake device presenting as the real IDS
U3-3560XCP-M identity, with per-call logging to a file, streaming stubbed as
`NOT_IMPLEMENTED` per M0 scope), `kineo_probe.def` (forces undecorated export
names), and `harness.c` (standalone consumer that `LoadLibrary`s the probe by
absolute path and exercises the full enumerate+open sequence — no Kineo, no
`ids_peak.dll`, no environment variable touched). Installed
`gcc-mingw-w64-x86-64` (user ran the apt install). Built both binaries with
`build.sh`, copied to a fresh scratch dir (`C:\Users\IMV\kineo-bridge-test\`,
created only for this test, no existing Kineo/Windows path touched), and ran
`harness.exe kineo_probe.cti` via `powershell.exe`.

**What was learned:**
- Both binaries build cleanly as genuine PE32+ x86-64 (`file` confirmed);
  GNU `objdump -p` again fails to parse the CTI's PE structure (same quirk
  Phase 2 found), `llvm-objdump -p` works and confirms all 33 exports present
  with plain, undecorated names (the `.def` file successfully prevented
  `__stdcall`-style `_Name@N` decoration).
- Harness run result: **PASS** — `LoadLibraryA` succeeded, all `GetProcAddress`
  lookups resolved, `TLGetNumInterfaces`→1, `IFGetNumDevices`→1,
  `IFGetDeviceInfo` returned the intended IDS-identity fields (vendor/model/
  serial/displayname), `IFOpenDevice` succeeded (device genuinely opened, not
  just listed), `DevGetInfo` confirmed post-open, clean teardown.
- The probe's own internal log (independent of the harness's stdout)
  confirms every function was actually entered inside the DLL in the correct
  order — this log mechanism is what will distinguish "harness called it"
  from "`ids_peak.dll` called it" in M1.
- Mingw warnings about `FARPROC`→typed-function-pointer casts in the harness
  are cosmetic (standard `GetProcAddress` pattern), not a real issue.

**What remains unknown:** Whether `ids_peak.dll` itself accepts and
completes its `GCInitLib`→`GCGetInfo` validation sequence against this same
probe (M1) — untested until next step. Whether `ids_peak` will call any
export beyond the 33 implemented here (would surface as an unresolved
`GetProcAddress` in `ids_peak.dll`'s own loading code, not visible from this
M0 pass).

**Next highest-value experiment:** M1 — extend the harness (or write a new
one) to call `ids_peak.dll`'s own producer-discovery API against this probe,
using a **process-local** `SetEnvironmentVariable("GENICAM_GENTL64_PATH", ...)`
(never the system/registry-backed one) pointed at a scratch directory
containing only the probe CTI.

## M1 — ids_peak.dll Support-Gate Finding (CONCLUSIVE, corrects Phase 3's Route-A risk assessment)

**What was tested:** Used IDS's own official `ids_peak` PyPI package
(`ids-peak==1.9.0.0.2`, Windows wheel) rather than hand-guessed C ABI calls —
its bundled `ids_peak.dll` is **sha256-identical** to Kineo's own copy, so
results are directly authoritative for Kineo's exact version. Ran a
7-candidate control matrix through `ProducerLibrary.Open()`: the original
Kineo IDS CTI (read-only), a byte-identical scratch copy (same filename), a
byte-identical scratch copy (renamed), a signature-stripped IDS copy, both
real installed TIS CTIs (`ic4-gentl-gev_x64.cti`, `ic4-gentl-u3v_x64.cti`),
and our own probe. Then built a **forwarding proxy** (`test2/kineo_proxy.c`)
re-exporting the real IDS CTI's exact 81-symbol table — 55 real GenTL
functions get logging wrapper code that calls through via `GetProcAddress`
on a scratch-copied real target (env-var-supplied path, never the installed
file); the 22 `OS_*`/1 CRT noise exports (Phase 2: not GenTL API) use native
PE export-forwarding (`Name = real_target.cti.Name`), a zero-code,
argument-perfect redirect — confirmed working via an isolated sanity test
first. Then built `hybrid1.c`: identical to the proxy except `GCInitLib`/
`GCGetInfo`/`GCCloseLib` are our own from-scratch implementation (not
forwarded), bisecting one function group at a time per instructions. Used
detailed per-call logging (cmd, pBuffer null-ness, input/output `*piSize`,
datatype, return code) on both our own probe and the proxy to compare
byte-for-byte, not just final values.

**What was learned — TWO DISTINCT, INDEPENDENT GATES, both required for
acceptance:**

1. **Control matrix result:** original/copied/renamed/unsigned real IDS CTI
   all **PASS**. Both real, currently-installed, otherwise-legitimate **TIS**
   producers **FAIL** with the identical `PEAK_RETURN_CODE_NOT_AVAILABLE` /
   "Provided producerLibrary is not supported." error. This directly
   contradicts Phase 3's inference that `ids_peak.dll` has no vendor
   filtering — **it does**, just not via a human-readable allowlist string
   table (which is why Phase 3's static `strings` search found none).
2. **Forwarding proxy (100% behaviorally real, our own PE binary) PASSES.**
   This proved the gate is about *observable behavior*, not file origin,
   signing, PE version resource, or binary-construction toolchain (mingw vs
   MSVC) — all already ruled out in earlier single-variable tests (version
   resource, Authenticode signature, `CompanyName` metadata field: none of
   these mattered).
3. **Gate 1 — vendor-string check.** Isolated via `hybrid1`: with our own
   `GCGetInfo`/`GCInitLib`/`GCCloseLib` but 78 other functions genuinely
   forwarded to the real target, changing **only** the
   `GCGetInfo(TL_INFO_VENDOR)` return value from `"IMV/KineoBridge"` to the
   exact real string `"IDS Imaging Development Systems GmbH"` flips
   `ProducerLibrary.Open()` from FAIL to **PASS** (it then fails at a later,
   unrelated step — `OpenSystem()` → "Invalid library object" — a separate,
   uninvestigated bug in this proxy's re-attach handling, not the original
   gate). This directly explains the TIS rejections too (`"The Imaging
   Source Europe GmbH"` ≠ `"IDS Imaging Development Systems GmbH"`).
4. **Gate 2 — an earlier functional-completeness pre-check, independent of
   vendor.** Confirmed by patching **only** the fully-standalone (zero
   forwarding) probe's vendor string to the exact real value and re-testing:
   it **still fails**, at the **identical early point** as before (rejected
   immediately after the `GCGetInfo(cmd=9)`/`(cmd=10)` major/minor exchange,
   never even reaching a `GCGetInfo(TL_INFO_VENDOR)` call). Since `hybrid1`
   (same GC-family code, but 78 *other* functions forwarding to a fully
   working real implementation) gets **past** this point and into a second
   `GCInitLib`/`GCGetInfo(VENDOR)`/`GCCloseLib` cycle, the gate must be
   triggered by calling into one or more of those other 78 functions (most
   likely `GCReadPort`/`GCGetPortURL`/the port-XML family, or possibly a
   `DS*` family probe) and finding them `NOT_IMPLEMENTED` — exactly the
   state of every M0/M1 probe variant's port/streaming stubs so far. Not yet
   pinned to one exact function (would need further bisection of the 78, not
   done this pass — deprioritized since the qualitative finding is
   sufficient to inform the plan).

**Byte-for-byte comparison (Test 3's specific ask):** `GCGetInfo(cmd=9)` and
`(cmd=10)` calls are **provably identical** between our probe and the real
target — same non-null `pBuffer`, same input `*piSize=4`, same `rc=0`, same
`piType=6` (UINT32), same output `*piSize=4`. This rules out any subtlety in
those two calls' semantics as a factor; the real cause lies entirely in (a)
the vendor string value and (b) behavior of calls beyond the GC-lifecycle
family.

**Revises Phase 3's risk assessment:** Phase 3 concluded "a well-formed
third-party `.cti` would very likely be accepted" based on no vendor
allowlist string being found in `strings`. That inference is now
**superseded** — there is a real, working vendor gate, just implemented as a
single hardcoded comparison value rather than a string table. **This is not
necessarily a route-killing finding**: since the exact requirement is now
known (report `TL_INFO_VENDOR = "IDS Imaging Development Systems GmbH"`,
plus whatever functional completeness the M3/M4 real port/XML/streaming
implementation will naturally provide instead of `NOT_IMPLEMENTED` stubs),
it converts an open unknown into a concrete, satisfiable requirement for the
real bridge CTI.

**What remains unknown:**
- The exact function(s) among the 78 non-GC-lifecycle exports whose
  `NOT_IMPLEMENTED` response triggers Gate 2 — not pinned down to one
  function this pass.
- Whether `TL_INFO_VENDOR` must match *exactly* "IDS Imaging Development
  Systems GmbH" or whether a short allowlist of vendor strings would work
  (only IDS's own string was tested as the "accepted" value; no second
  accepted vendor string was tried).
- Whether this same two-gate logic applies uniformly to Kineo's own
  `KineoDeviceService.exe` code path (which uses `peak::core::ProducerLibrary`
  per Phase 3's symbol evidence) or whether Kineo adds further filtering on
  top — not yet tested against real Kineo (still pending PLAN.md's M2, which
  itself now needs revision to fold in the vendor-string requirement).
- The unrelated `hybrid1` "Invalid library object" bug at `OpenSystem()`
  once vendor is fixed — likely a re-attach/module-reload issue in the test
  scaffolding itself, not investigated further as it's outside the core
  question this test round targeted.

**Next highest-value experiment:** Build the real M3/M4 CTI with
`TL_INFO_VENDOR` set to the exact IDS string and full (non-stub) port/XML/
streaming behavior, then retest the full M0→M2 sequence against both the
official `ids_peak` Python bindings (cheap, fast iteration) and real Kineo
(authoritative, per PLAN.md M2) to confirm both gates are satisfied together
and that no further gate exists deeper in the construction sequence.

## M1c — Exact Export-Parity Standalone Test (CONCLUSIVE — Gate 2 fully resolved, STOP condition met)

**Method correction first:** re-derived exact sorted export-name lists for
all three binaries with a robust extractor (handles both regular and
PE-forwarded export-table row formats) — see
`investigation/m1-export-diff.txt`. This corrected an earlier arithmetic
error (had said "22 OS_* + 1 CRT = 78", actual is 23 `OS_*` + 2 IDS-only
multipart DS + 1 CRT = 26 symbols beyond the 55 common set, for the correct
81 total). Confirmed via `diff`: the Test-2 forwarding proxy has **exact,
zero-difference** export-name parity with the real IDS CTI (81/81 both
directions).

Also fetched the official EMVA `GenTL_v1_5.h` reference header and diffed it
against our hand-written `probe/gentl.h` (full results:
`investigation/gentl-header-diff.md`). Every ABI-relevant value we've
exercised (`GC_ERROR` codes, `INFO_DATATYPE`, `TL_INFO_CMD`, `DEVICE_INFO_CMD`,
and the exact `GCGetInfo`/`GCInitLib`/`GCCloseLib` signatures) matches the
spec exactly. Two minor, irrelevant-to-Gate-2 gaps found (calling-convention
keyword inert on x64; `DEVICE_ACCESS_STATUS` missing 3 GenTL-1.5 values) —
ruled out our header as an explanation for anything seen so far.

**What was built:** `probe/m1/m1c/m1c_standalone.c` — a **fully standalone**
producer (M0 baseline untouched; this is a new file). All 57 real GenTL
functions (55 common + 2 IDS-only multipart DS) are **our own
implementation**, not forwarded — copied from the vendor-fixed
`kineo_probe_v2.c` plus 2 new multipart-DS stubs. Only the 24 non-GenTL noise
symbols (23 `OS_*` + 1 mangled CRT symbol, unknown signatures, irrelevant per
Phase 2) use PE export-forwarding to a scratch `real_target.cti` copy, purely
for exact symbol-surface parity. Confirmed via `diff`: **0 differences**
against the real CTI's 81-symbol export table in either direction.

**Result: PASS, decisively.** `ProducerLibrary.Open()` → `producer.System()`
→ `sys_desc.OpenSystem()` → `system.UpdateInterfaces()` → `system.Interfaces()`
(1, as expected) → `iface.OpenInterface()` → `iface.UpdateDevices()` →
`iface.Devices()` (1, as expected, full correct `IDS Imaging Development
Systems GmbH` / `U3-3560XCP-M` / `4110010861` metadata read back) →
`device_descriptor.IsOpenable(Control)` = `True` — **all succeed**, using a
100% from-scratch implementation with zero forwarding of any ordinary GenTL
function. The pipeline only stops at `OpenDevice(Control)`, and with a
precise, expected, well-understood cause: `GCGetPortInfo | Info-Command: 12
(PORT_INFO_PORTNAME) | GC_ERR_NOT_IMPLEMENTED` — this is real M3 (GenICam
XML/port) territory, not a Gate 2 problem; our probe's port functions are
still deliberate `NOT_IMPLEMENTED` stubs (M0/M1 scope explicitly excluded
real port/XML behavior).

**Per the stop condition: Gate 2 investigation ends here.** We do not need
to know why `ids_peak.dll` performs this check internally. **Conclusion:
Gate 2 is satisfied by exact export-name parity (all 81 symbols present)
combined with correct `TL_INFO_VENDOR`** — no deeper functional behavior
(streaming, real port data, etc.) was required to pass `ProducerLibrary.Open()`
through full device-open-attempt. angr static analysis was not needed and
was not performed, per the stop condition.

**What remains unknown (now correctly scoped to M3+, not Gate 2):**
- The exact set of `GCGetPortInfo`/`GCGetPortURL`/port-family behavior
  `ids_peak` requires for `OpenDevice` to fully succeed — this is exactly
  M3's job (implement real port/XML behavior, iterate against this same
  fast Python-bindings test loop rather than guessing the full GenICam XML
  schema upfront).
- Whether device-level metadata (vendor/model/serial) needs to match
  anything IDS-specific beyond what we already report (untested — our
  probe already reports plausible IDS-style values and got this far without
  issue, so likely not a concern, but not exhaustively verified).
- Whether Kineo's own code path (vs. the official Python bindings) enforces
  anything further — still pending PLAN.md's M2 (real Kineo test), now
  unblocked since the vendor-string + export-parity requirements are known
  and satisfiable.

**Next highest-value experiment:** Implement real (non-stub) `GCGetPortInfo`/
`GCGetPortURL`/port-XML behavior (M3), iterating against this same
official-Python-bindings test loop after each addition — exactly the "don't
assume DSStartAcquisition alone is enough, log what's actually called"
philosophy already established for M4, now confirmed as the right approach
for M3 too.

## M2 Smoke Test — Real Kineo Confirmed to Enter Our CTI (condition A met)

**What was tested:** Launched the real Kineo Electron app (`Kineo Software.exe`,
from its own install directory as working directory) from a scoped PowerShell
process with `GENICAM_GENTL64_PATH` prepended with the M1c scratch CTI
directory **only in that process's own environment block** — no User/Machine
env var was touched (confirmed: `[Environment]::GetEnvironmentVariable(...,
'User')` was empty both before and after; the real 4-segment value lives at
Machine scope, `'Machine'` scope untouched throughout). Caveat: this specific
run's scoped value only had the scratch dir + the (empty-at-User-scope) base,
not the full original 4 real segments — a query-scope mistake, not a
mutation of anything; Kineo's own real IDS/TIS CTIs were simply not on the
path for this one run. Waited ~10s, confirmed `KineoDeviceService.exe` (PID
8220) was running alongside 4 `Kineo Software` Electron processes, then
stopped all Kineo-related processes afterward (confirmed via `Get-Process`
returning empty).

**Per explicit instruction: the CTI's own logfile is the authoritative
trace, not Kineo's UI/WebSocket output** — no Kineo-side diagnostics were
consulted; this finding rests entirely on `probe_log()` output written by
our own DLL code while genuinely loaded and called by the real
`KineoDeviceService.exe`/`ids_peak.dll` process tree.

**Result: the real Kineo process tree entered our CTI extensively.** The
logfile reached 43,461 lines / 1.7MB in roughly 10 seconds of Kineo running
before it was stopped. Concise call-type tally:

```
   6202 TLUpdateInterfaceList
   6201 IFUpdateDeviceList
   6201 IFOpenDevice
   6201 GCGetPortInfo
   6201 DevClose
      6 GCInitLib / GCGetInfo(cmd=1) / GCCloseLib / DllMain attach-detach
      3 GCGetInfo(cmd=9) / GCGetInfo(cmd=10)
      2 TLOpen / GCGetInfo(cmd=0)
      1 IFGetNumDevices
```

Real Kineo/`ids_peak` entered a continuous background polling loop (its own
device-scan retry, ~every 1-5ms): `TLUpdateInterfaceList` →
`IFUpdateDeviceList` → `IFOpenDevice("KineoBridgeDevice0")` → **directly**
`GCGetPortInfo(cmd=12=PORT_INFO_PORTNAME)` → `DevClose` → repeat. This is
the **exact same stopping point** the official `ids_peak` Python bindings
test found independently (`GCGetPortInfo(PORT_INFO_PORTNAME)` =
`NOT_IMPLEMENTED`) — strong convergent confirmation from two completely
different consumer code paths (IDS's own Python SWIG bindings vs. Kineo's
real C++ `peak::core` usage) that this is genuinely the next real blocker,
not an artifact of the Python test harness.

**Important correction to the planned M3a starting point:** `DevGetPort()`
is **never called** in this sequence — `GCGetPortInfo` is called **directly
on the `DEV_HANDLE` returned by `IFOpenDevice`**, not on a port handle
obtained via `DevGetPort()`. This is consistent with GenTL's "local port"
concept: every module handle (TL/IF/Dev) can itself be passed as a
`PORT_HANDLE` to access that module's own *local* port/description,
distinct from the *remote device* port `DevGetPort()` would return (the
actual camera's GenICam register space). **M3a must therefore implement
`GCGetPortInfo` to correctly answer queries against the raw `DEV_HANDLE`
itself** (at minimum `PORT_INFO_PORTNAME`), not assume `DevGetPort()` is the
first thing exercised.

**M2 status: PARTIAL/PASS**, per instructions — real Kineo enters our CTI;
integration confirmed; returning to M3 implementation rather than debugging
Kineo-specific behavior further (the failure is the already-known missing
device-port operation, not a Kineo-specific quirk).

**What remains unknown:** whether Kineo's polling loop would ever stop
retrying once `GCGetPortInfo`/port behavior is implemented enough to satisfy
it (untested — will fall out naturally once M3a is retested against real
Kineo); whether the missing real IDS/TIS segments in this run's scoped env
var affected anything observed (unlikely, given the identical stopping
point matches the Python-bindings test exactly, but not run again with the
fully-correct original value this pass).

**Next highest-value experiment:** M3a — implement `GCGetPortInfo` to
handle at least `PORT_INFO_PORTNAME` (and whatever else is requested next)
when called on the raw device handle, test against the official Python
bindings first (fast iteration), then re-run this same real-Kineo smoke
test to confirm the polling loop progresses past `IFOpenDevice`.

## M3a/M3b/M3c — Device Port, Port URL, and Minimal GenICam XML (all DONE this pass — both report conditions met)

**New versioned file** (M0/M1 baseline untouched): `probe/m3/m3a_standalone.c`
+ `probe/m3/kineo_bridge.xml`, built on the M1c passing baseline. All work
tested against the official `ids_peak` Python bindings first (fast
iteration), consistent with the established incremental philosophy.

**M3a — `GCGetPortInfo`:** Implemented properly (was a stub). Correctly
handles **both** `H_DEV` (the local "Device" port — confirmed via the M2
real-Kineo trace as the actual handle `ids_peak` passes, since `DevGetPort`
is never called) and `H_PORT` (the "RemoteDevice" port, from `DevGetPort`,
used later for `Remote` port connection). All `PORT_INFO_CMD` values
implemented per the official `GenTL_v1_5.h` semantics, including correcting
an initial mistake: `LITTLE_ENDIAN`/`BIG_ENDIAN`/`ACCESS_*` are `BOOL8`, not
`INT32` — caught before testing by re-checking the spec.

**M3b — Port URL, with one real empirical correction:** First tried
`URL_SCHEME_FILE` (GenTL 1.5's own "read from local hard drive" scheme,
official and simpler than register-mapped access). `ids_peak` queried
`URL_INFO_URL`/`URL_INFO_SCHEME` successfully but **never actually opened
the file** — silently abandoned node-map construction with no error
(`NodeMaps()` returned count=0). **Empirical finding: this consumer does
not act on `URL_SCHEME_FILE` in practice**, despite it being in the spec
header — a real, useful negative result, not a bug in our code. Switched to
`URL_SCHEME_LOCAL` (the original, universally-implemented register-mapped
scheme: XML loaded into memory at `DllMain`, served via `GCReadPort` against
a fixed virtual base address `0x10000`, using the standard
`local:<filename>;<hex address>;<hex length>` URL format) — **this worked
immediately** (`NodeMaps(): OK, count=1`).

**M3c — Minimal GenICam XML:** Wrote the smallest schema-valid XML with
`DeviceVendorName`/`DeviceModelName`/`DeviceSerialNumber`/`Width`/`Height`/
`PixelFormat`/`PayloadSize` as simple non-register-backed nodes (`<String>`/
`<Integer>`/`<Enumeration>` with literal `<Value>` — no `GCReadPort`
involvement needed for individual feature values, only for the XML content
itself). First attempt got `NodeMaps(): OK, count=1` but `Nodes()` raised
`"Could not connect node map with port (Port-Name: Remote)!"` — a precise,
actionable GenApi error. **Root cause:** GenApi's node-map/port connection
step unconditionally requires an explicit `<Port Name="Remote">` element
declared in the XML (the connection anchor), even when no individual
feature node is register-backed. Added it — **immediately fixed**:
`Nodes(): OK, count=10` (all 7 feature nodes + `Root` + `Remote` port +
the `Mono8` enum entry, all correctly parsed and connected).

**Value-read verification (partial M3e):** Extended testing beyond node
*existence* to actual value *reads* — `DeviceVendorName`, `DeviceModelName`,
`DeviceSerialNumber`, `Width`, `Height`, `PayloadSize` all read back exactly
the values authored in the XML (`PixelFormat`, an `EnumerationNode`, needs a
different Python accessor than the plain `.Value()` used for the others —
a test-script limitation, not a producer defect; the node itself parsed and
connected correctly).

**Both report conditions from the review are now satisfied and exceeded:**
- Condition A (real Kineo enters our CTI): confirmed via the M2 smoke test
  above (43K-line log, `IFOpenDevice` called 6201 times).
- Condition B (IDS Peak constructs the first GenApi node map): confirmed
  and gone further — a full 10-node map, connected, with real values
  readable, not just an empty/stub node map.

**What remains unknown / not yet done:**
- Whether real Kineo (not just the official Python bindings) also succeeds
  through this same M3a/b/c sequence — the M2 smoke test was run against
  the *pre-M3* CTI (stub port behavior); has not been re-run against this
  M3a/b/c-complete CTI yet.
- Writing feature values back (`GCWritePort`/settable nodes) — not
  implemented or tested this pass; current XML nodes are all effectively
  read-only (literal `<Value>`, no `pValue`/register backing for writes).
- `PixelFormat`'s correct Python-side read accessor (cosmetic, not a
  producer-side gap).
- Whether Kineo's own device-open path requires anything beyond what the
  official bindings exercised (e.g., specific node read/write ordering) —
  only testable via a fresh M2-style real-Kineo run against this new CTI.

**Next highest-value experiment:** Re-run the M2 real-Kineo smoke test
against this M3a/b/c-complete CTI to confirm real Kineo also progresses
past `IFOpenDevice` into node-map construction, not just the official
Python bindings.

## M2b — Real Kineo Against the M3a/b/c-Complete CTI (real Kineo confirms the official-bindings result)

**What was tested:** Same scoped-launcher approach as M2, corrected per
instruction: read the **Machine**-scope `GENICAM_GENTL64_PATH` explicitly
(`[Environment]::GetEnvironmentVariable(..., 'Machine')`) and prepended only
the scratch M3 CTI directory to that exact value, in the launching
PowerShell process's own environment only — User/Machine scope confirmed
untouched throughout. Polled the log's line count every second instead of
waiting a fixed duration, specifically to avoid another uncontrolled dump.

**Result: the log self-bounded at 134 lines total** (vs. 43,461 in the
pre-M3 M2 run) — because, unlike before, Kineo did **not** re-enter a fast
open/fail/retry cycle. It authenticated the device once and settled into a
slow, steady-state ~5-second interval `TLUpdateInterfaceList` /
`IFUpdateDeviceList` poll — the normal background behavior of a GenTL
consumer watching for *new* devices, not retrying a broken one.

**Concise ordered trace of the one real open/read cycle** (full log is
already only 119 lines; not excerpted further):

1. Standard producer/system/interface bring-up (`GCInitLib`, version
   check, vendor query, `TLOpen`, `TLUpdateInterfaceList`, `IFOpenDevice`) —
   identical to the pre-M3 run up to this point.
2. `GCGetPortInfo(H_DEV, PORTNAME)` → `"Device"` (matches M3a).
3. `GCGetNumPortURLs(H_DEV)` → 0 (correct — no XML on the local port).
4. `DevGetPort` → `H_PORT` (this **is** called this time, unlike the
   pre-M3/M1 traces — now that there's something worth fetching).
5. `GCGetPortInfo(H_PORT, PORTNAME)` → `"Remote"`.
6. `GCGetNumPortURLs(H_PORT)` → **1** (found our `local:` URL).
7. `GCGetPortURLInfo(H_PORT, idx=0)` queried for `URL_INFO_URL`(0),
   `URL_INFO_SCHEME`(9), `URL_INFO_FILE_REGISTER_ADDRESS`(7),
   `URL_INFO_FILE_SIZE`(8), `URL_INFO_FILENAME`(10) — all succeed.
8. **`GCReadPort(H_PORT, addr=0x10000, *piSize=2366) → copied=2366`** — the
   **entire XML file read in one call**, byte count exactly matching the
   file size. This is real Kineo/`ids_peak` successfully retrieving our
   GenICam XML via the `local:` scheme, independently confirming the
   official-bindings M3b result.
9. `DevGetInfo(DISPLAYNAME)` ×4, `DevGetNumDataStreams` → 0 (streaming
   stub, expected/correct for M3), a few more `GCGetPortInfo(H_PORT,
   PORTNAME)` calls (likely internal to GenApi's node-map/port `Connect()`
   step, consistent with the same "Could not connect... Port-Name: Remote"
   mechanism already understood from M3c).
10. **No error, no exception, no further retry.** Transitions directly to
    slow steady-state polling.

**Important architectural limitation discovered — feature-level access is
NOT observable at our current logging layer.** Every node in
`kineo_bridge.xml` is a plain, non-register-backed node (`<String>`/
`<Integer>`/`<Enumeration>` with a literal `<Value>`). Once `GCReadPort`
delivers the *entire* XML once (step 8 above), GenApi parses it entirely
into its own in-memory C++ object model — reading `DeviceVendorName` or
`Width` afterward is a pure in-process GenApi/C++ operation with **zero
further calls back into our CTI**. This means we cannot currently observe
*which* specific features Kineo reads or in what order — the ordered
feature-access list requested cannot be produced with the current node
implementation. **If this is wanted, the concrete next step is converting
specific nodes to register-backed types (`IntReg`/`StringReg` with distinct
virtual addresses)**, so each feature read triggers its own observable
`GCReadPort` call — not done this pass, flagged for the next round rather
than assumed.

**Answering the 8 requested report points directly:**
1. Full Machine-scope producer path correctly inherited: **yes**, confirmed
   via the printed scoped env var showing scratch dir + all 4 original
   segments.
2. Kineo progressed beyond `GCGetPortInfo`: **yes**, all the way through.
3. XML was requested/read: **yes** — full 2366-byte file, one `GCReadPort`
   call, byte-exact.
4. GenApi node map appears to have connected successfully: **very likely
   yes** — no error surfaced (contrast with the official-bindings test,
   where the *first* XML-without-`<Port>` attempt threw an explicit
   connect-failure exception; here, with the `<Port Name="Remote">` element
   already in place from M3c, nothing analogous fired), and Kineo's
   post-read behavior changed qualitatively (steady-state polling instead
   of frantic retry) — the behavior of a consumer that got what it needed.
   Not 100%-certain without direct node-map introspection (Kineo doesn't
   expose that to us the way the Python bindings did), but this is strong,
   consistent, convergent evidence with the official-bindings result.
5. Ordered list of feature nodes Kineo requested: **not observable** with
   current non-register-backed node types — see architectural limitation
   above.
6. First missing node/function/operation: **none observed** — nothing
   failed in this run.
7. Any `DS*`/`Event*` acquisition function reached: **no** — stopped
   cleanly at `DevGetNumDataStreams` → 0 (the expected M4 boundary).
8. Concise call sequence around first new failure: **N/A, no failure
   occurred** — see the 10-step trace above for the full sequence instead.

**This is CASE A** (per the interpretation guide) — or as close to it as
directly observable: no failure, full XML delivered, qualitative shift to
steady-state behavior. Per "do not preemptively add features" / "only
implement nodes confirmed by runtime behavior," **no new nodes are added
this pass** since none were shown to be missing.
