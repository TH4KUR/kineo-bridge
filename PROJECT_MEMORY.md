# PROJECT_MEMORY.md — Kineo Bridge

This is the durable handoff document. Read this first in any new session
before touching code. It should let a fresh Claude Code session resume
without rediscovering anything below.

## 0. Repository

- Private GitHub repo: **https://github.com/TH4KUR/kineo-bridge** (branch
  `master`). All source, docs, and dev tooling described here are pushed
  there as of this writing.
- Tag **`v1.0.0-rc1`**: the first packaged-build validation state (see
  `RELEASE_MANIFEST.md`, "First clean multi-run result" and "Icon
  update"). Not yet full golden acceptance — see §23.
- If this repo directory (`~/kineo-bridge/` in this project's dev
  environment) is ever missing or a fresh session starts from scratch,
  `git clone` from the URL above recovers everything needed — **except**
  one external runtime dependency that is intentionally NOT in git (see
  next bullet).
- **Critical external dependency, not in this repo, must never be
  deleted**: `~/aravis-0.8.36/` (the built Aravis 0.8.36 library —
  specifically `~/aravis-0.8.36/build/src/`) is loaded at runtime by the
  WSL-side bridge via `GI_TYPELIB_PATH`/`LD_LIBRARY_PATH` (see §10, §19).
  It is a large third-party built C library, deliberately excluded from
  git — but without it, the camera bridge cannot function at all, and
  rebuilding it from source is nontrivial. If it's ever gone, it must be
  rebuilt from the Aravis 0.8.36 source release before anything camera-
  related will work again. This is NOT documented anywhere else — this
  is the one thing to check first if the bridge mysteriously stops
  working after any environment cleanup.

## 1. Purpose

Make a specific USB3 camera work with Kineo Software (a Windows Electron
app + native `KineoDeviceService.exe`, used for CASA — computer-assisted
sperm analysis) on a **Windows 11 ARM64** machine (Surface Pro 11), where
the camera's own vendor driver is **x64-only** and cannot run natively.

Solution: a custom Windows x64 **GenTL producer** (`.cti`) that Kineo's
real `ids_peak.dll` loads exactly like a real IDS driver, which bridges
to a **WSL2 Linux ARM64** side that owns the physical camera via Aravis
(an open-source GenICam/USB3Vision library), talking to our CTI over a
private TCP protocol.

## 2. Hardware

- Camera: **IDS U3-3560XCP-M**, VID:PID `1409:8000`, serial `4110010861`.
- Native mode: 1920×1200, Mono8, payload exactly 2,304,000 bytes/frame.
- Host: Windows 11 ARM64 (Surface Pro 11), running WSL2 (Ubuntu, ARM64).
- USB passthrough: `usbipd-win`, attaching the camera's USB bus to WSL2.

## 3. Why the native Windows IDS driver does not work

The IDS U3V kernel driver and SDK (`ids_peak.dll`, `ids_u3vcore.sys`) are
**x64-only**. This machine's Windows is ARM64. Kineo itself runs fine
under Windows-on-ARM's x64 emulation (Electron/x64 binaries), and
`ids_peak.dll` loads fine emulated — but the **kernel-mode USB driver**
cannot be emulated; ARM64 Windows cannot load an x64 kernel driver at
all. So the real camera can never be opened directly from Windows on
this machine, at any emulation level. WSL2 (Linux ARM64) with `usbipd`
passthrough and Aravis (a userspace GenICam library, no special kernel
driver needed beyond usbfs) is the only viable path to the physical
sensor.

## 4. Final architecture

```
Kineo UI (Electron)
    -> KineoDeviceService.exe (native x64, spawned by Kineo)
    -> ids_peak.dll (real IDS SDK, unmodified)
    -> our GenTL producer: probe/m5/m5_bridge.cti  (loaded via GENICAM_GENTL64_PATH)
    -> TCP (127.0.0.1:9494) private wire protocol
    -> WSL2 (Ubuntu ARM64): wsl-camera/kineo_camera_bridge.py
    -> Aravis 0.8.36 (async streaming) -> real camera over usbipd
```

The CTI is a **real, spec-following GenTL 1.5 producer** — not a shim
around the vendor driver. It implements the full GC*/TL*/IF*/Dev*/DS*/
Event* export surface, a real buffer/event lifecycle, and a GenApi XML
node map Kineo's own device-manager code queries and writes exactly as
it would a genuine camera.

## 5. Kineo 1.0.0 / 1.1.2 compatibility

The **exact same CTI binary** (unchanged, hash-verified) was proven
against both Kineo 1.0.0 and the 1.1.2 update. Full diff of the update:
`ids_peak.dll`, `ids_peak_ipl.dll`, both bundled IDS `.cti` files, and
the IDS kernel driver were all **byte-identical** across the update —
only Kineo's own app layer (Electron bundle, `KineoDeviceService.exe`
binary) changed. Conclusion: our CTI is compatible with the whole
IDS/GenTL stack as shipped, and future Kineo app-layer updates are not
expected to break it unless the update itself changes GenTL/IDS SDK
versions. See `investigation/kineo-update-diff.md` for the full record.

## 6. GenICam/GenTL contract (what Kineo actually needs)

Discovered empirically, in this order, across the whole project
(`investigation/kineo-workflow-trace.md` has the full blow-by-blow):

1. Full GenTL export table (missing exports = instant rejection before
   any of our logged functions are even called).
2. A real, parseable GenApi XML node map served via `local:` URL scheme
   (register-mapped, not `file:` — that scheme is silently ignored by
   this consumer despite being in the GenTL 1.5 spec).
3. `AcquisitionFrameRate`, `AcquisitionMode`, `TriggerSelector` GenApi
   nodes — Kineo queries/writes these; their absence blocks progress
   long before real streaming is ever attempted.
4. A real buffer lifecycle (`DSAllocAndAnnounceBuffer`/`DSQueueBuffer`/
   `DSRevokeBuffer`) with a **natural pool size of 81 buffers** — Kineo
   allocates until it naturally stops there; this is not a cap we chose,
   it's what the real consumer does. Never impose an artificial ceiling.
5. Real `GCRegisterEvent`/`EventGetData` for `EVENT_NEW_BUFFER` (value
   **1**, per the official EMVA GenTL header — verified directly from
   source after an earlier bad web-fetch claimed 1000; this was a
   documented self-correction, see the workflow trace).
6. Two-layer acquisition state: `DSStartAcquisition` arms the **stream**
   layer; the separate GenApi `AcquisitionStart` command arms the
   **camera** layer. Real frame delivery needs both.
7. `BUFFER_INFO_IMAGEOFFSET` (cmd=18) — one specific info command that,
   if missing, causes `GrabFrame failed` after exactly one delivered
   frame. Found via a real-Kineo trace, not guessed.

## 7. Custom CTI design

Single source file `probe/m5/m5_bridge.c` (+ `wsl_bridge_client.c/h` for
the WSL bridge TCP client), built with `mingw-w64`
(`x86_64-w64-mingw32-gcc`) from WSL, producing a genuine PE32+ x86-64 DLL.

- Fixed sentinel handles into static storage (one TL, one interface, one
  device, one stream — that's all this project ever needs).
- GenApi XML: wrapper (`Integer`/`Float`/`Enumeration`) + register
  (`IntReg`/`FloatReg`) two-node pattern, register-backed so every real
  read/write is directly observable in the log.
- All logging goes to a file (`kineo_probe_cti.log`, path overridable via
  `KINEO_BRIDGE_LOG`), never stdout (there is no console attached to
  `KineoDeviceService.exe`).

## 8. Buffer / event lifecycle

- `BUFFER_HANDLE`/`EVENT_HANDLE` are the malloc'd objects' own addresses
  (individually `malloc`'d, so a separate growable *pointer* registry can
  be `realloc`'d safely — only pointers move, never the objects).
  Validated by identity scan, never address-range math.
- Buffer states: `ANNOUNCED -> QUEUED -> FILLING -> COMPLETED -> requeue`.
- Events are refcounted (`event_t.refcount`/`pending_free`) to survive a
  real race: `GCUnregisterEvent` can run on a different thread than a
  blocked `EventGetData` — mark `killed` + broadcast immediately, defer
  the actual `free()` until the last reference exits.
- Single `CRITICAL_SECTION` + `CONDITION_VARIABLE` for all of this —
  native Win32 primitives only, no external runtime dependency.

## 9. Required GenApi nodes

`Width`, `Height`, `PixelFormat` (Mono8), `PayloadSize`, `ExposureTime`,
`Gain`, `BlackLevel`, `AcquisitionMode`, `AcquisitionFrameRate`,
`TriggerSelector`, `TriggerMode`, `AcquisitionStart`/`AcquisitionStop`
(commands). See `probe/m5/kineo_bridge_m5.xml` for the exact XML and the
register address map at the top of `m5_bridge.c` (`reg_desc_t g_regs[]`).

## 10. WSL / Aravis camera bridge

`wsl-camera/kineo_camera_bridge.py` (+ `camera_source.py`,
`protocol.py`, `bridge_status.py`). One client (our CTI) at a time.
`RealCameraSource` uses Aravis 0.8.36's **async** streaming pattern
(`create_stream` + `push_buffer` + `start_acquisition` +
`timeout_pop_buffer` loop) — the synchronous convenience call was
established early on to behave badly over WSL/USB-IP and is never used.

Aravis 0.8.36 (built from source, at `~/aravis-0.8.36/build/src` on the
dev machine) is used via its GObject-Introspection Python bindings
(`GI_TYPELIB_PATH`/`LD_LIBRARY_PATH` pointed at the local build tree —
**not installed system-wide**). This is a real, currently-undocumented
**deployment prerequisite** for any new machine — see §20.

## 11. IPC protocol

Custom binary framing, `wsl-camera/protocol.py` is the single source of
truth:
- 10-byte header: `4s magic("KCB1") + 1B version + 1B msg_type + 4B LE payload_len`.
- Control messages (HELLO/OPEN/CONFIGURE/START/STOP/CLOSE/STATUS/ERROR):
  UTF-8 JSON payload.
- `FRAME` messages (server→client only): 33-byte binary header
  (`frame_id`(8) `width`(4) `height`(4) `pixel_format`(4) `data_len`(4)
  `timestamp_ns`(8) `status`(1)) + raw pixel bytes.
- A real consumer must tolerate a stray `FRAME` arriving between a
  request and its control-message ack (a benign race, handled on both
  sides).

## 12. 60 FPS final lifecycle (the current production candidate)

**Do not regress this.** Reaching it took several real, hard-won fixes:

```
DevOpenDataStream        -> TCP connect + camera OPEN + CONFIGURE only
                             (NOT physical acquisition -- no pre-roll)
AcquisitionStart (GenApi) -> physical acquisition starts HERE, exactly
                             when Kineo asks (wsl_bridge_notify_acquisition_start)
first EventGetData        -> absorbed by the startup grace period (§13)
frames flow                -> normal buffer/event delivery, unchanged
AcquisitionStop (GenApi)   -> physical stream stopped AND released
                             (wsl_bridge_notify_acquisition_stop);
                             camera object stays open, stream/acquisition
                             torn down -- genuinely zero image traffic
                             until the next AcquisitionStart
```

Camera physical rate: capped independently of whatever Kineo's GenApi
node requests, via `KINEO_BRIDGE_MAX_FPS` (bridge-side, in
`camera_source.py`). **Currently validated and in use: 60** (native
camera bounds are 1.29–60.38 fps; readback at 60 ≈ 59.99; real measured
source FPS across many real cycles ≈ 59.85–59.90). A real Analysis burst
is consistently **~1.05–1.07 seconds**, ~53–65 frames, zero bad frames,
zero timeouts, every genuine cycle measured.

### What this replaced, and why (do not go back)

- **Original M5 design**: physical camera streamed continuously for the
  whole bridge-connection lifetime (chosen to dodge a ~150ms timeout —
  see §13). Proven fine for a *single* analysis per session, but never
  tested for multiple. When multiple real analyses in one session were
  finally tried: sustained high-bandwidth streaming for several minutes
  measurably degraded the USB3Vision link (bad-frame counts climbing,
  eventual `GrabFrame failed`, camera unresponsive to even plain
  enumeration afterward — recoverable only via a USB-level detach/
  reattach).
- Two further real bugs were found and fixed while getting to the
  current lifecycle:
  1. A **leftover initial-START** in the bridge connection's own
     handshake (`run_one_session()` in `wsl_bridge_client.c`) — a relic
     from the original design that survived every later redesign by
     accident, causing the physical camera to stream unattended for up
     to **7.5 minutes** between connecting (`DevOpenDataStream`, which
     happens early) and the user's actual first click. Fixed: the
     handshake now settles into a genuinely idle state; only
     `AcquisitionStart` ever starts physical acquisition.
  2. A **logging bug** (not a real one): Kineo can send a duplicate
     `AcquisitionStop` on cancel with no new `AcquisitionStart` in
     between; the per-cycle instrumentation (added for the FPS
     investigation) computed a bogus multi-minute "duration" for that
     duplicate using stale timing data. Fixed on both sides (CTI +
     bridge) to log a clean one-line no-op instead.
- After both fixes, a user-reported "cancel takes a long time" was
  traced precisely via Kineo's own `ChironLog`: **8 minutes 28 seconds**
  between `Analysis N cancelled by user` and Kineo's own cleanup
  finishing, while our own logs show the camera was already idle the
  entire time. **That delay is entirely inside Kineo's own
  cancel/cleanup pipeline, not ours** — do not try to fix it here.

## 13. EventGetData first-frame grace period

Real, measured problem: even with the camera already open/configured,
`AcquisitionStart`-to-first-real-frame latency is **~300-390ms**
(measured both by recreating the Aravis stream each cycle and by
reusing one) — the camera's own startup sequence, not fixable by our
bookkeeping. Kineo's own first `EventGetData` after `AcquisitionStart`
uses a timeout of only **~150ms**.

Fix (in `m5_bridge.c`'s `EventGetData`): a narrowly-scoped grace period.
Only the *first* wait after a fresh physical `AcquisitionStart` is
allowed to run up to `EVENTGETDATA_GRACE_MS` (1000ms) internally,
regardless of what timeout the caller actually requested. The instant a
real frame arrives, every subsequent call goes back to honoring the
caller's exact requested timeout. Logged clearly (`requested_timeout`,
`effective_wait`, time from physical `AcquisitionStart` to first real
frame) — never silently expand timeouts elsewhere. **Kineo tolerates
this fine** (confirmed empirically across many real analyses) — this was
the deciding experiment that made the whole "camera idle until
AcquisitionStart" lifecycle viable at all, replacing an earlier,
much riskier "pre-roll" approach.

## 14. USB/IP known behavior

- `usbipd.exe` is directly invocable from WSL (or from a native Windows
  launcher) via interop — no PowerShell wrapper needed.
- `usbipd attach --wsl --busid <id>` for an **already-Shared** device
  needs **no elevation** (empirically verified repeatedly this project).
- The **only** operation needing elevation is the one-time
  `usbipd bind --busid <id>` (NotShared -> Shared).
- **v1 product decision (2026-09-27, supersedes the earlier
  least-privilege-only design)**: `KineoBridge.exe` now runs elevated
  (manifest `requireAdministrator`, one UAC prompt at launch) so it can
  perform `usbipd bind` itself, with zero manual admin command ever
  shown to the customer, covering a never-shared device (fresh machine,
  driver reinstall, a Windows update that resets share state) as well as
  the steady-state attach case. See §19/§21.
- BUSID is **never hardcoded** — always parsed fresh from
  `usbipd.exe list`, keyed by VID:PID `1409:8000`.
- `usbipd.exe list`/`tasklist.exe` via interop have occasionally returned
  empty/stale output for no real reason (not a state change) — both
  `lib_usbip.sh`'s `usbip_find_device` and the launcher's process checks
  use a bounded retry (not unbounded) to tolerate this.
- **Known, accepted limitation**: after a whole Kineo session ends, the
  camera often needs a fresh `usbipd attach` before the *next* session
  starts (recovers instantly, no reboot, no Kineo restart needed). This
  is NOT true within a live session anymore (the 60fps lifecycle fixed
  that) — only between sessions. Not a priority to chase further; the
  launcher handles it automatically and invisibly to the user.

## 15. Start / cancel / stop behavior

- `DSStartAcquisition` (stream layer) and GenApi `AcquisitionStart`
  (camera layer) are logged and handled separately (two-layer model);
  real Kineo always follows `DSStartAcquisition` with the GenApi command
  almost immediately.
- `AcquisitionStart`/`AcquisitionStop` writes are idempotent by
  construction — Kineo has been observed sending duplicates of both; the
  CTI must never treat a duplicate as an error or compute misleading
  derived stats from one (see §12's logging-bug fix).
- A real user cancel produces a duplicate `AcquisitionStop` with no
  corresponding new `AcquisitionStart` — expected, harmless, must be a
  clean no-op on our side.

## 16. Synthetic regression mode

`KINEO_BRIDGE_SOURCE=synthetic` (vs. `wsl`) switches the frame source
with **no rebuild** — a deterministic moving vertical bar, generated
in-process on both sides (CTI worker thread fallback path, and
`SyntheticSource` in `camera_source.py`), used to isolate "is this a
GenTL/Kineo regression" from "is this a camera/bridge/USB-IP problem."
**Never remove this.** If real-camera behavior ever fails, check
synthetic first: synthetic broken -> CTI/Kineo regression; synthetic
fine, real fails -> camera/bridge/USB-IP problem.

The pre-hardening `probe/m3/m4h/` directory is the **original golden
synthetic baseline** and must never be edited again, by explicit
standing instruction from early in this project.

## 17. Build commands

```bash
# Dev CTI (debug-friendly, verbose logging retained)
probe/m5/build_m5.sh

# Release CTI (stripped, same behavior/exports, verified no embedded dev paths)
probe/m5/build_m5_release.sh

# Customer-facing launcher (KineoBridge.exe)
launcher/build_launcher.sh

# Full release bundle (CTI + launcher + compiled bridge, into release/)
scripts/package_release.sh
```

All CTI builds use `x86_64-w64-mingw32-gcc` from WSL, producing real
PE32+ x86-64 DLLs. Export count must always be **81** (verified by every
build script) — if it changes, something in the `.def` file or export
surface broke.

**Reproducible builds**: the release CTI, the launcher, and the
compiled bridge `.pyc` files are all byte-for-byte reproducible across
repeated builds of unchanged source (verified 2026-09-27 by building
each twice and diffing sha256). This required three explicit fixes,
kept in the build scripts — do not remove them:
- `-Wl,--no-insert-timestamp` on both `build_m5_release.sh` and
  `launcher/build_launcher.sh` (mingw's linker otherwise embeds the
  wall-clock build time in the PE header, changing the hash on every
  rebuild even with identical source).
- `scripts/package_release.sh` compiles the bridge `.pyc` files with
  `py_compile` directly (not `compileall` on a `mktemp -d` copy),
  passing `dfile=` so the embedded `co_filename` is always the bare
  module name (`camera_source.py`, never an absolute dev path), and
  `invalidation_mode=UNCHECKED_HASH` (a content hash, not a mtime) —
  both needed for the shipped `.pyc` bytes to be reproducible and to
  never leak a dev-machine path.

## 18. Release artifacts

`scripts/package_release.sh` produces `release/` (gitignored, a build
output — never committed):
```
release/
  KineoBridge.exe
  VERSION.txt
  README_INSTALL.txt
  payload/
    m5_bridge.release.cti
    kineo_bridge_m5.xml
    bridge/*.pyc            (compiled, no .py source shipped)
```
Customer install (v1, manual — no MSI yet): copy the whole `release/`
folder to `C:\Program Files\IMV Technologies\KineoBridge\`, then
double-click `KineoBridge.exe`.

## 19. Launcher behavior (KineoBridge.exe)

`KineoBridge.exe`'s icon (Explorer/taskbar) is Kineo's own app icon,
extracted directly from `C:\IMVapps\Kineo Software\Kineo Software.exe`
via `launcher/extract_pe_icon.py` (a small dependency-free PE resource
parser written because `icoutils`/`pefile` weren't available in this
environment) into `launcher/kineo_icon.ico`, embedded via `1 ICON
"kineo_icon.ico"` in `kineobridge.rc`. Verified byte-identical by
re-extracting it back out of the built `KineoBridge.exe` and diffing.
To refresh it after a Kineo update: re-run
`python3 launcher/extract_pe_icon.py "<path to Kineo Software.exe>"
launcher/kineo_icon.ico` and rebuild.

Source: `launcher/kineobridge_launcher.c` (native Win32 GUI app, no
console window, `mingw-w64`-built). Sequence: check Kineo install exists
-> close any stale Kineo process -> deploy its own `payload\` (next to
the exe) into `C:\ProgramData\KineoBridge\runtime\` (every launch,
unconditional overwrite — no separate version check to get wrong) ->
detect/repair camera USB/IP (dynamic BUSID, never hardcoded; can bind
AND attach, see below) -> deploy the bridge's `.pyc` files into WSL and
start it (`wsl.exe -- bash -lc "..."`, always `--source camera`, never
`synthetic` — hardcoded, no flag/branch selects otherwise) -> poll the
bridge's own status file for real readiness -> launch Kineo via
`CreateProcess` with **working directory exactly `C:\IMVapps\Kineo
Software`** (mandatory — Kineo spawns `KineoDeviceService.exe` via a
relative path) and an **explicit environment block** (current
environment + `KINEO_BRIDGE_SOURCE=wsl` +
`GENICAM_GENTL64_PATH=C:\ProgramData\KineoBridge\runtime`) — never a
persistent User/Machine environment change -> verify Kineo actually
stays alive via its own **ChironLog** (not OS process polling alone; see
§20 for the false-positive that made this necessary) -> hand off to the
normal Kineo UI (status window closes).

The status window is 400×190. Title and status are deliberately treated
as one visual group (near-zero gap between them, then a wide gap before
the footer): title is medium-weight (`FW_MEDIUM`) 19px in medium gray
(RGB 95,95,95) — de-emphasized, since it barely changes after the first
glance — while status is bold 16px near-black (RGB 15,15,15) — the line
that actually matters moment to moment, so it needs to be the most
visible text in the window, not the title. Below a thin inset
separator (pushed well clear of the title/status group): an italic
gray "Made with ♥ by" line (the ♥ is rendered via one
`CreateWindowExW`/Unicode child STATIC control — the only Unicode
window in an otherwise all-ANSI ("A" API) codebase, needed because the
heart glyph isn't representable in the ANSI/Windows-1252 codepage;
classic GDI static text can't render full-color emoji regardless of
encoding, so this is the plain U+2665 heart symbol, not a colored
emoji), then two attribution rows where only "Website"/"LinkedIn" are
the actual link (the rest is plain gray text):
- "System Integration and Infrastructure Solutions · **Website**" → https://siis.in
- "Eashaan Thakur · **LinkedIn**" → https://www.linkedin.com/in/eashaan-thakur/

Implemented as **`SysLink`** controls (class `"SysLink"`, requires
`InitCommonControlsEx(ICC_LINK_CLASS)` plus a `Microsoft.Windows.Common-
Controls` v6 dependency in `kineobridge.manifest` for proper themed
rendering) using `<A HREF="...">text</A>` markup, so only the tagged
substring renders as a link (blue, underlined, its own hand cursor —
SysLink handles all of that natively) while the surrounding text stays
plain. Each row is created and then measured with its OWN real
rendered width via `LM_GETIDEALHEIGHT` (mingw's header only has the
older name for what Microsoft's docs call `LM_GETIDEALSIZE` — same
message) and centered at that exact width in `create_centered_link()`
— measured live on whatever machine it actually runs on, not guessed at
build time, since this dev environment can't render a real Win32
window to check text metrics directly. A click fires `NM_CLICK`/
`NM_RETURN` via `WM_NOTIFY`, handled in `WndProc()` by calling
`ShellExecuteA(..., "open", <url>, ...)`. Colors/fonts are set via
`WM_CTLCOLORSTATIC`, matched by control ID (`IDC_TITLE`, `IDC_STATUS`,
`IDC_MADE_WITH`, `IDC_LINK_WEBSITE`, `IDC_LINK_LINKEDIN`). Purely
cosmetic; if any of this is ever removed or restyled, nothing else
depends on it.

Manifest: **`requireAdministrator`** (v1 product decision, 2026-09-27 —
supersedes an earlier `asInvoker`-only design). One UAC prompt at
launch; this lets `ensure_camera_ready()` run `usbipd bind` itself when
a device is ever seen `NotShared`, as well as `usbipd attach` — no
manual admin command is ever shown to the customer for either case. On
any real failure, a native `MessageBoxA` shows a short, non-technical
message plus a short support code (`KB-USB-000/001/003`,
`KB-BRIDGE-001`, `KB-KINEO-001/002`) — never a console/log dump.

**Known gap in `deploy_runtime_payload()`**: if no `payload\` folder
exists next to the exe (only a dev-testing scenario — every real
release build ships one), the function silently no-ops and leaves
`C:\ProgramData\KineoBridge\runtime\` untouched. On a machine that has
ever been used for manual dev testing, this means a *stale* CTI could
theoretically remain in place if a customer build were ever run without
its `payload\` folder. Mitigation: always verify the deployed
`C:\ProgramData\KineoBridge\runtime\m5_bridge.release.cti` hash matches
the release manifest's recorded hash during release validation — never
assume from a successful launch alone.

## 20. Known limitations

- **The WSL-side Aravis 0.8.36 build is a real, unaddressed deployment
  dependency.** This project's launcher/packaging assumes a target
  machine already has WSL2 + a default distro + `usbipd-win` + Aravis
  0.8.36 (with GI bindings) built at `~/aravis-0.8.36/build/src` inside
  WSL, and the camera already `usbipd bind`-shared once. None of that is
  bundled or auto-installed by `KineoBridge.exe` in this v1. A genuinely
  fresh customer machine needs all of this set up by hand (or by a
  future installer enhancement) before `KineoBridge.exe` will work.
- **No true single-file EXE with embedded resources.** The release
  bundle is `KineoBridge.exe` + a `payload\` folder shipped alongside it,
  not literal Win32 resources baked into the binary. Functionally
  equivalent for the "no readable source" requirement (only `.pyc` +
  compiled `.cti` ship, never `.py`/`.c`), but not literally one file.
  Documented as a reasonable v1 tradeoff, not a limitation that blocks
  release.
- **No installer (MSI) yet** — manual folder-copy install only (see
  `release/README_INSTALL.txt`).
- **No licensing/activation implemented yet** — the architecture is
  licensing-ready (a signed-license verification step could be inserted
  early in `WinMain`) but nothing is enforced in v1.
- **Kineo's own cancel-cleanup delay** (§12) is real and user-visible
  but entirely inside Kineo's own app — not something this project can
  fix.
- **"Visualize Sperm Path" checkbox** was observed greyed-out during
  testing — likely because no trackable sperm content exists in
  whatever the camera was pointed at during testing (consistent with
  every "Tracking image not found" warning seen throughout this
  project), not a bridge/CTI bug. Not independently confirmed with a
  real prepared sample.
- **60 FPS is the current validated production candidate; 30/20/10 were
  never needed** (60 passed cleanly on the first top-down attempt, per
  explicit instruction not to descend unless it failed).
- **Full Kineo-timing-semantics investigation (does Kineo's own
  motion/velocity math use real buffer timestamps, the configured
  `AcquisitionFrameRate`, or something else) was only partially done.**
  Concrete finding so far: Kineo/`ids_peak` has **never once** queried
  `DEVICE_INFO_TIMESTAMP_FREQUENCY` across this entire project's logs —
  suggestive (not conclusive) that it doesn't actively use our raw
  buffer timestamps. The planned synthetic A/B/C cadence-vs-timestamp
  experiment was not run.

## 21. Do-not-regress rules

- Never modify `probe/m3/m4h/` (the M4d golden synthetic baseline).
- Never remove `KINEO_BRIDGE_SOURCE=synthetic` mode.
- Never reintroduce continuous/pre-roll physical streaming while idle —
  the camera must be at zero image traffic between analyses (§12).
- Never let a duplicate `AcquisitionStart`/`AcquisitionStop` compute or
  log derived stats from stale cycle state (§12/§15).
- Never expand `EventGetData`'s timeout beyond the one narrowly-scoped
  first-wait grace period (§13).
- Never hardcode a USB BUSID (§14).
- Never let an uncaught exception in the bridge's `open()`/handler path
  escape and crash the whole server process — every handler must convert
  exceptions to a clean error reply, never let one kill the accept loop
  (this exact bug caused real, hours-long "GrabFrame failed" outages
  before it was found and fixed at three layers).
- Never assume `CreateProcess`/launch success without verifying the
  child (Kineo) actually stays alive via its own log — OS-level process
  polling alone produced a real false positive (Electron's own
  single-instance-lock bootstrap launches/exits a redundant process in
  under a second, which a tight poll can misread as a crash).
- Never persist `GENICAM_GENTL64_PATH`/`KINEO_BRIDGE_SOURCE` to the
  User/Machine environment — process-local only, every time.
- Never modify files under `C:\IMVapps\Kineo Software`, `C:\Program
  Files`, `C:\Program Files (x86)`, or `C:\Windows` without explicit
  approval.
- Never let the release CTI, the launcher, or the shipped `.pyc` bridge
  files become non-reproducible again (no re-embedded PE timestamps, no
  absolute dev paths in `.pyc` `co_filename`) — see §17.
- Never remove the `-Wl,--no-insert-timestamp` linker flag or the
  `py_compile`/`UNCHECKED_HASH` bridge-compile approach in
  `scripts/package_release.sh` without re-verifying reproducibility.
- In `build_kineo_env()` (launcher/kineobridge_launcher.c): never just
  append `GENICAM_GENTL64_PATH=`/`KINEO_BRIDGE_SOURCE=` after a copied
  base environment block without first removing any existing entries
  of the same name. A real bug (found 2026-09-27, fifth live launch
  attempt): this machine has a persistent MACHINE-level
  `GENICAM_GENTL64_PATH` already set (pointing at Kineo's own bundled
  vendor IDS CTI directory, `...\resources\chiron-cpp-module\
  ExternalDependencies\IDS` — confirmed via `reg query
  "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment"`),
  which `GetEnvironmentStringsA()` inherits into the base block. The
  old code left a DUPLICATE `GENICAM_GENTL64_PATH` key (the inherited
  one, followed later by ours) — which of the two ids_peak.dll's GenTL
  producer scan actually honored was inconsistent, and real evidence
  (Kineo connecting, streaming real video, then failing with "Connection
  lost" mid-analysis) pointed at this being live-but-unstable, consistent
  with intermittently picking up the wrong producer path. Fixed: strip
  any existing `GENICAM_GENTL64_PATH`/`KINEO_BRIDGE_SOURCE` entries from
  the copied base block first, then insert single, authoritative
  replacements — with our runtime dir listed first in a semicolon-list
  ahead of whatever machine value already existed (preserves the genuine
  vendor CTI directory's reachability without ambiguity). Verified with
  a standalone native unit test against a synthetic duplicate-key
  scenario before rebuilding. Also fixed in the same pass: the bridge
  is now started with `python3 -u` (unbuffered) instead of buffered
  stdout — its own log file came back completely empty after this
  crash, consistent with buffered output being lost when the process
  exits abruptly; unbuffered mode is required for any future crash to
  actually leave a trace.
- In `launch_and_verify_kineo()` (launcher/kineobridge_launcher.c): never
  pass `CREATE_UNICODE_ENVIRONMENT` to `CreateProcessA` alongside the
  environment block from `build_kineo_env()`. A real bug (found
  2026-09-27, fourth live launch attempt, `KB-KINEO-002`):
  `build_kineo_env()` builds a plain ANSI (narrow) block via
  `GetEnvironmentStringsA()`, but that flag tells Windows the block is
  UTF-16 -- every variable the child (Kineo) receives, including
  `PATH`/`APPDATA` and our own `GENICAM_GENTL64_PATH`/
  `KINEO_BRIDGE_SOURCE`, gets corrupted even when `CreateProcessA`
  itself still reports success, so Kineo never came up correctly and
  its ChironLog never showed a success marker within the wait window.
  If a genuinely Unicode environment block is ever wanted, build one
  with `GetEnvironmentStringsW`/wide strings and switch to
  `CreateProcessW` -- do not mix the ANSI API with that flag again.
- In `deploy_and_start_bridge()`/`stop_bridge()`
  (launcher/kineobridge_launcher.c): never use `pkill -f <pattern>` to
  stop the bridge. A real bug (found 2026-09-27, second live launch
  attempt): the whole kill+start command is itself invoked as `bash -lc
  "<the command text>"`, and that text contains the literal substring
  "kineo_camera_bridge.pyc" (both in the pkill pattern and again in the
  nohup command later in the same string) — so `pkill -f
  'kineo_camera_bridge.pyc'` run from inside that invocation matches
  its own invoking shell's command line and kills it (a signal-related
  exit code, empty captured output) before the script ever reaches the
  nohup/echo STARTED lines — the bridge never actually started. This is
  the same class of self-matching pkill bug already documented in this
  project's dev tooling, now fixed properly with a PID file
  (`~/.local/lib/kineobridge/bridge.pid`) instead: no pattern matching,
  no self-match risk. Also: the `cd dir && env... nohup ... &` shape
  must never be used to capture `$!` — backgrounding a `&&`-joined
  compound list makes bash fork a subshell to run the whole list, and
  `$!` captures that subshell's PID, not the real python3 PID (verified
  directly: two live processes, killing the subshell PID left the real
  bridge running forever, protected by `nohup` from the resulting
  SIGHUP). Fix: `cd` must be its own prior statement, so the
  backgrounded command is a single simple command and `$!` matches the
  real process exactly (verified with a full start/restart/restart
  cycle: exactly one bridge process alive throughout, no orphans).
- In `deploy_and_start_bridge()`: the bridge MUST be started with
  `setsid nohup ... < /dev/null & ...; disown; sleep 1; echo STARTED`,
  never plain `nohup ... & disown; echo STARTED`. A real bug (found
  2026-09-27, seventh live launch attempt, `KB-BRIDGE-001`, health-check
  correctly reported the bridge never came up): the bridge-start command
  is one-shot (`wsl.exe -- bash -lc "..."`), and WSL tears down the
  entire session belonging to that one-shot client the moment it exits
  -- killing any descendant process still in that session, INCLUDING
  one started with `nohup ... & disown` (those only protect against
  SIGHUP and bash's own job-table tracking, not WSL's own session-level
  cleanup). Verified directly and reproducibly: the exact same command
  survives fine when run inside an already-open, persistent WSL shell,
  but reliably dies within ~1-2s when run via a fresh `wsl.exe -- bash
  -lc` invocation; `setsid` alone (confirmed via `ps` showing the
  process's own session leadership, state `Ssl`) was NOT sufficient by
  itself -- the script must also sleep briefly after backgrounding so
  the fork/setsid genuinely completes before the one-shot session ends.
  Confirmed stable 8+ seconds after the invoking `wsl.exe` process
  exited, with both `setsid` and the trailing `sleep 1` in place. Any
  future WSL-side background process this launcher starts (not just
  the bridge) needs the same treatment.
- In `usbip_find()` (launcher/kineobridge_launcher.c): never search
  the unbounded `line` pointer (which runs into every subsequent line
  of `usbipd.exe list`'s multi-line output) for the state substrings
  ("Shared"/"Not shared"/"Attached") — always search a bounded,
  single-line copy taken *before* `strtok` mutates it. A real bug
  (found 2026-09-27, first live launch attempt) had the camera's own
  `Shared` line matched against a *different*, unrelated device's
  `Not shared` line that happened to appear immediately after it in the
  list, misreporting an already-Shared camera as NotShared and sending
  the launcher into a pointless bind-retry loop that always "succeeded"
  (bind is a no-op on an already-shared device) but never progressed,
  eventually failing with KB-USB-003. This was invisible in every
  earlier manual dev-tooling test because no other USB device happened
  to be adjacent to the camera in `usbipd list`'s output at the time.
- Never put a bare `--` inside an XML comment in
  `launcher/kineobridge.manifest` (or any other embedded manifest). The
  XML spec forbids it inside comments; Windows' SxS parser rejects the
  whole manifest if it's there, and the executable then refuses to
  start at all with `STATUS_SXS_CANT_GEN_ACTCTX` ("side-by-side
  configuration is incorrect") — a real bug found on the first live
  launch attempt (2026-09-27). Validate any manifest edit with
  `xmllint --noout` (or `python3 -c "import xml.dom.minidom as m;
  m.parse(path)"`) before rebuilding.

## 22. Important file paths

```
probe/m5/m5_bridge.c              -- the CTI, GenTL producer source
probe/m5/wsl_bridge_client.c/.h    -- CTI-side TCP client to the bridge
probe/m5/kineo_bridge_m5.xml       -- GenApi XML node map
probe/m5/gentl_v2.h                -- GenTL 1.5 constants (EMVA-verified)
probe/m5/build_m5.sh               -- dev build
probe/m5/build_m5_release.sh       -- release (stripped) build
probe/m3/m4h/                      -- PROTECTED, never edit -- synthetic golden baseline

wsl-camera/kineo_camera_bridge.py  -- bridge server
wsl-camera/camera_source.py        -- RealCameraSource / SyntheticSource
wsl-camera/protocol.py             -- wire protocol (source of truth)
wsl-camera/bridge_status.py        -- health-status file writer/reader

launcher/kineobridge_launcher.c    -- KineoBridge.exe source
launcher/build_launcher.sh         -- launcher build
launcher/kineobridge.manifest      -- requireAdministrator manifest
launcher/kineo_icon.ico            -- Kineo's own icon, extracted (see section 19)
launcher/extract_pe_icon.py        -- dependency-free PE icon extractor

scripts/lib_common.sh              -- shared constants + show_modal()
scripts/lib_usbip.sh               -- USB/IP detection/recovery (bash, dev tooling)
scripts/start-kineo.sh             -- dev one-command launcher (bash, NOT the customer build)
scripts/stop-kineo.sh              -- dev stop/cleanup
scripts/diagnose.sh                -- dev health check, layered (USB/IP vs Aravis vs bridge vs CTI)
scripts/package_release.sh         -- assembles release/
RELEASE_MANIFEST.md                -- private release record (per-build hashes, tests performed)

investigation/kineo-workflow-trace.md  -- full milestone-by-milestone history (M1-M4d)
investigation/golden-baseline.md       -- pre-1.1.2-update hashes/versions
investigation/kineo-update-diff.md     -- 1.0.0 -> 1.1.2 diff, compatibility proof
investigation/hardening.md             -- the whole lifecycle/FPS investigation arc

C:\IMVapps\Kineo Software\                              -- Kineo install (read-only, vendor's)
C:\IMVapps\Kineo Software BACKUP 1.0.0\                 -- verified-complete pre-update backup
C:\ProgramData\KineoBridge\runtime\                      -- deployed private runtime (launcher target)
C:\Users\IMV\kineo-bridge-test\m1\m5\                    -- dev-only staging dir (start-kineo.sh target)
~/aravis-0.8.36/build/src                                -- WSL-side Aravis build (GI_TYPELIB_PATH/LD_LIBRARY_PATH)
```

## 23. Golden test procedure

Before trusting any change to the CTI or bridge:

1. **Synthetic regression first.** `KINEO_BRIDGE_SOURCE=synthetic`,
   confirm the app-facing behavior is unchanged (video appears, Analysis
   completes). If this breaks, it's a CTI/Kineo-facing regression —
   fix that before touching anything camera-related.
2. **Real camera, single analysis.** Confirm serial matches
   (`4110010861`), exact payload size (2,304,000 bytes), real video in
   Kineo's UI, Analysis completes.
3. **Real camera, repeated-analysis session** (the actual golden
   acceptance bar): in ONE Kineo session —
   - 5 consecutive completed analyses
   - 1 cancel during analysis, then a successful analysis immediately after
   - at least one idle gap of 3+ minutes between analyses
   - zero image traffic while idle (check the bridge's own
     `cycle_source_span_s`/`frames_captured` — must stay near-zero
     between analyses, not grow continuously)
   - zero bad frames, zero timeouts, zero USB/control-channel errors
   - zero USB reattach needed **within** the session
   - camera still healthy (plain Aravis enumeration succeeds) right
     after the session ends (a reattach may be needed **before the
     next** session — known, see §14/§20 — but the camera must not be
     wedged mid-session)
4. Only after that passes: build+test the **release** CTI the same way
   (not just the dev build) before packaging.

### Packaged-build validation status (2026-09-27)

The exact customer-facing packaged build (`KineoBridge.exe` with
`requireAdministrator`, the release CTI, and the compiled `.pyc`
bridge — not dev tooling) completed a real 3-analysis session with no
errors: 2 complete analyses + 1 cancel-then-immediate-retry, zero bad
frames beyond 2 minor ones in the post-cancel cycle, zero timeouts,
zero USB/control-channel errors, zero mid-session reattach. Full
evidence (per-cycle frame counts/FPS, cross-referenced across the
launcher/CTI/bridge/ChironLog) is in `RELEASE_MANIFEST.md`, "First
clean multi-run result." This is the first time the actual packaged
build — not the dev bash tooling — has been validated end-to-end.

**This does not yet fully satisfy step 3 above**: only 2 consecutive
completed analyses (not 5) and no ≥3-minute idle gap were exercised
(longest gap seen was ~2m42s). Getting through this partial run
required finding and fixing seven real bugs in the launcher itself
(none in the CTI/bridge core logic) — see the do-not-regress entries
below for each. Treat this as a strong positive signal, not a
substitute for the full golden acceptance run, if further launcher or
CTI changes are made.
