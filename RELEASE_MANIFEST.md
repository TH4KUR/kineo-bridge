# RELEASE_MANIFEST.md — Kineo Bridge

Private release record. Not shipped to customers. Regenerate this file
(hashes especially) any time `scripts/package_release.sh` is re-run
against changed source.

## Product

- Product: Kineo Bridge (`KineoBridge.exe`)
- Version: **1.0.0** (internal)
- Build date: 2026-09-27
- Company: IMV Technologies

## Hardware / software validated against

- Camera: IDS U3-3560XCP-M, VID:PID `1409:8000`
- Camera serial used throughout development and validation: `4110010861`
- Native resolution/format: 1920×1200, Mono8, 2,304,000 bytes/frame
- Kineo versions this CTI has been proven compatible with:
  **1.0.0** and **1.1.2** (see `PROJECT_MEMORY.md` §5 — the full IDS/
  GenTL stack the update ships is byte-identical between these two
  Kineo versions; only Kineo's own app layer changed)
- Target OS: Windows 11 ARM64 (Surface Pro 11), WSL2 (Ubuntu ARM64)

## Build-time verification performed (2026-09-27)

These were verified directly by building and inspecting the actual
release artifacts — not assumed:

- Release CTI: 81 GenTL exports confirmed (`llvm-objdump` export table
  count), matching the dev CTI's export count exactly.
- Release CTI: no embedded `/home/*/kineo-bridge` dev source paths
  (`strings` scan, clean).
- Release CTI, `KineoBridge.exe`, and all four shipped `.pyc` bridge
  files are **byte-for-byte reproducible**: each was built twice from
  identical source and the sha256 hashes matched exactly (required
  fixing two real non-determinism sources — a PE linker timestamp, and
  `.pyc` `co_filename`/mtime metadata; see `PROJECT_MEMORY.md` §17).
- Shipped `.pyc` bridge files carry no dev-machine path in their
  `co_filename` metadata (verified via `strings`) — only bare module
  names (`camera_source.py`, etc.).
- Shipped `.pyc`-only bridge (no `.py` source present) starts and
  imports correctly end-to-end in isolation (smoke-tested against
  `--source synthetic`, confirmed clean startup/listen/accept/protocol
  handshake in the log).
- `KineoBridge.exe`'s embedded manifest confirmed via `strings` to
  declare `requestedExecutionLevel level="requireAdministrator"`.
- **Static source-path audit of `kineobridge_launcher.c`** (proof, not
  assumption, that the launcher cannot silently fall back to dev/
  synthetic):
  - The bridge is started with `--source camera` — this exact string
    literal appears exactly once in the source, hardcoded; there is no
    flag, branch, or code path anywhere that can pass `--source
    synthetic` or otherwise select the synthetic frame source.
  - `GENICAM_GENTL64_PATH` is always set to the single `RUNTIME_DIR`
    constant (`C:\ProgramData\KineoBridge\runtime`) — one `#define`,
    referenced in exactly one place; never conditionally pointed at a
    dev path (e.g. `probe\m5`).
  - `RUNTIME_DIR` is populated only by `deploy_runtime_payload()`,
    which copies from `<exe_dir>\payload\` — and the release payload
    (built by `scripts/package_release.sh`) contains only
    `m5_bridge.release.cti`, never the dev `m5_bridge.cti`.
  - **Caveat found and documented** (`PROJECT_MEMORY.md` §19): if no
    `payload\` folder exists next to the exe, `deploy_runtime_payload()`
    silently no-ops, leaving whatever is already in `RUNTIME_DIR`
    untouched. This cannot happen with a real release build (which
    always ships a `payload\` folder) but means the live validation
    below must confirm the *deployed* CTI hash on disk, not just that
    the launch succeeded.

## SHA256 — shipped release artifacts (this build)

Verified reproducible: the whole `scripts/package_release.sh` pipeline
was run twice back-to-back and every one of these hashes matched
exactly both times.

```
KineoBridge.exe                    41a57efeb4660780a50022387f9bbad46413993d9a6f00a7bd97b866c023c9ff
payload/m5_bridge.release.cti      5dbc879e76270987d755090292c0b225d6eb88ef9d4b346bd156d05620c5a5ea
payload/kineo_bridge_m5.xml        f62c1c36e5c203954d3db8ab4276aa2f3b14395f0d8c87cca3d40d774c8b2c01
payload/bridge/protocol.pyc        2df91c6257a7c277f35c3e40e964eb06392ce56e0c96263b1fda6b509ced360d
payload/bridge/camera_source.pyc   a23c1d2987d7d14a94500cb231fc8be7dc56309c0f9b6ebf9317b8c0b63c5e42
payload/bridge/kineo_camera_bridge.pyc  720a886bdb93fcbf5d1d637fd53f40c33f423b132a298edfa22a906915d3630c
payload/bridge/bridge_status.pyc   8d0dbaace5fade577198b22caf72127fdaba87057a951e8cd9d62029c2018984
```

Deployed for live testing at:
`C:\Users\IMV\kineo-bridge-test\release_v1.0.0\KineoBridge.exe`
(hash-verified identical to the above after copy).

For reference, the matching **dev** (unstripped, verbose-logging) CTI
built from the identical `m5_bridge.c` at the same time:

```
probe/m5/m5_bridge.cti  d49a7045d6d080da042e79fa0f65ba9b1993a845aeec1aff21b2a3822628145f
```

## Acceptance tests performed

**On the dev tooling (`start-kineo.sh`, dev CTI, real camera, real
Kineo), earlier in this project's history — informative but NOT a
substitute for a customer-build test:**
- Multiple real Kineo sessions with 2, 5, and 10 consecutive real
  Analyses; cancel-during-analysis followed by a successful analysis;
  idle gaps between analyses — all passed cleanly at physical 60 FPS
  after the lifecycle/EventGetData-grace/logging fixes described in
  `PROJECT_MEMORY.md` §12/§13.
- Synthetic-mode regression checks after every CTI change.

**NOT yet performed — requires the actual customer-facing build on the
real Windows hardware, which this session cannot do itself** (no
ability to click a UAC prompt, watch Kineo's Electron UI, or judge
real video content from here):
- Session 1 (5 analyses + cancel/retry + ≥3 min idle) launched via
  `KineoBridge.exe` itself, with the `requireAdministrator` UAC prompt,
  the release CTI, and the compiled bridge.
- Session 2 (cold restart, automatic USB/IP recovery, one more real
  Analysis).
- Controlled failure UX (camera disconnected -> concise error dialog
  with a support code -> reconnect -> normal launch).
- Confirmation, read directly off the deployed files, that
  `C:\ProgramData\KineoBridge\runtime\m5_bridge.release.cti` matches
  the hash recorded above (guards against the payload-fallback caveat
  above).

**These are tracked as open items — see the live test script provided
in this session's conversation. This manifest and `PROJECT_MEMORY.md`
must be updated with the real results once that test is run.**

**Bug found and fixed during the first live launch attempt
(2026-09-27)**: the very first real double-click of `KineoBridge.exe`
failed immediately with a native Windows error — *"The application has
failed to start because its side-by-side configuration is incorrect"*
(`STATUS_SXS_CANT_GEN_ACTCTX`). Root cause: `launcher/kineobridge.manifest`
contained a descriptive `<!-- ... -->` XML comment whose prose used a
bare `--` as an em-dash substitute. The XML spec forbids `--` anywhere
inside a comment body (only as part of the closing `-->`); Windows'
strict SxS manifest parser rejected the whole manifest outright, so the
executable could not even start — this affected every build of
`KineoBridge.exe` produced so far, independent of the elevation-model
change, and would have affected the earlier `asInvoker` manifest too had
it ever been launched live. Fixed by removing the offending comment
text from the manifest (rationale kept in source-code comments and
`PROJECT_MEMORY.md` instead, where XML comment rules don't apply).
Verified fix: manifest parses as valid XML (`xml.dom.minidom`), no bare
`--` remains, and the exe was rebuilt and hash-verified reproducible
again after the fix. **Confirmed on real hardware**: the next launch
got past UAC and into the app (the SxS error did not recur).

**Second bug found and fixed, same day, second live launch attempt**:
after the manifest fix, the launcher got through UAC and elevation
correctly but failed camera setup every time with `Error KB-USB-003`
("Camera connection failed"). Added diagnostic logging
(`C:\ProgramData\KineoBridge\logs\launcher.log`, read directly from
this session over `/mnt/c/ProgramData/...`) and found the actual
`usbipd.exe list` output showed the camera correctly as `Shared` — but
`usbip_find()`'s state-parsing bug (see `PROJECT_MEMORY.md` §21) caused
it to be misread as `NotShared` because an unrelated second USB device
("USB Serial Converter", `0403:6015`) happened to sit on the very next
line with a genuine "Not shared" state, and the parser's substring
search wasn't bounded to the camera's own line. Fixed by snapshotting
the full line into a fixed buffer before `strtok` mutates it and
searching only that. Verified against the exact real captured log
output with a standalone native unit test before rebuilding
(confirmed: now parses as `SHARED`, state=1). Rebuilt, reproducibility
re-verified, redeployed. **Not yet confirmed on real hardware** — that
is the next thing to test.

**Third bug found and fixed, same day, third live launch attempt**:
after both prior fixes, camera setup fully succeeded (bind → attach →
WSL `lsusb` → Aravis serial match, all confirmed in the log) but the
launcher then failed starting the bridge itself (`Error KB-BRIDGE-001`).
Diagnostic log showed the bridge-start command returned a signal-like
exit code with zero output. Root cause: the exact same self-matching
`pkill -f` bug already known from this project's dev tooling (see
`PROJECT_MEMORY.md` §21) — the command's own invoking shell text
contained the search pattern, so it killed itself before ever starting
the bridge, while an unrelated stale bridge process from earlier manual
dev testing (holding the same port from a `.py`, not `.pyc`, invocation
that the pattern didn't even match) sat there blocking the port anyway.
Fixed with PID-file-based process management; a related subtlety (`$!`
capturing a subshell's PID instead of the real python3 PID when `cd` is
part of the backgrounded `&&` chain) was found and fixed in the same
pass. Verified directly against real WSL/Aravis: single start, and a
start→restart→restart cycle, leave exactly one bridge process alive
each time with no orphans. **Not yet confirmed via the actual launcher
exe on real hardware** — that is the next thing to test.

**Fourth bug found and fixed, same day, fourth live launch attempt**:
after all three prior fixes, camera and bridge setup both succeeded
cleanly (confirmed in the log — bind, attach, WSL, Aravis, bridge
start and health check all green), but Kineo itself then failed to
verify as started (`Error KB-KINEO-002`) on both retry attempts. Added
logging to `launch_and_verify_kineo()` (it previously logged nothing)
and, while adding it, found the actual bug by code inspection:
`CreateProcessA` was called with the `CREATE_UNICODE_ENVIRONMENT` flag
alongside an environment block built as plain ANSI bytes by
`build_kineo_env()` (`GetEnvironmentStringsA()`) — Windows then
misinterprets that byte buffer as UTF-16, corrupting every environment
variable the child process receives (including our own
`GENICAM_GENTL64_PATH`/`KINEO_BRIDGE_SOURCE`), even though
`CreateProcessA` can still report success. Fixed by removing that flag
(the ANSI API with an ANSI block needs no such flag). **Not yet
confirmed on real hardware** — that is the next thing to test.

**Fifth bug found and fixed, same day, fifth live launch attempt**: this
time Kineo actually started, connected to the camera, and streamed real
video into a live analysis — then failed mid-analysis with Kineo's own
"Connection lost / Connection to Kineo Start was lost" dialog. The
bridge's own log came back completely empty (a related bug, fixed:
`python3 -u` now required — see `PROJECT_MEMORY.md`). Investigation
found a persistent MACHINE-level `GENICAM_GENTL64_PATH` already set on
this machine, pointing at Kineo's own bundled vendor IDS CTI directory.
`build_kineo_env()` only ever appended its own value after the
inherited base block, leaving a **duplicate** `GENICAM_GENTL64_PATH`
key whose resolution by `ids_peak.dll` is not reliably ours — plausible
root cause for intermittent/unstable GenTL producer behavior mid-session.
Fixed by stripping any existing `GENICAM_GENTL64_PATH`/
`KINEO_BRIDGE_SOURCE` entries before inserting a single authoritative
value (ours first, semicolon-separated, ahead of the preserved genuine
vendor path). Verified the dedup logic with a standalone native unit
test against a synthetic duplicate-key scenario matching the real
machine's actual value. **Not yet confirmed on real hardware that this
resolves the mid-analysis disconnect** — that is the next thing to
test, and given this bug could plausibly cause an INTERMITTENT rather
than immediate failure, a full Session-1-style multi-analysis run (not
just one single analysis) is warranted before concluding it's fixed.

**Sixth bug found and fixed, same day, sixth live launch attempt**: with
the env-block fix in place, Kineo again started, connected, and failed
mid-analysis with "start capture" / "GrabFrame failed" — traced (via the
CTI's own log showing `WSAECONNREFUSED` connecting to the bridge) to the
launcher's health check only testing for the status file's *existence*;
a stale file left over from earlier manual testing made it report
healthy without ever confirming the fresh process actually started.
Fixed by deleting the stale file before starting and requiring a couple
of confirming polls. Also clarified: the camera showing `Shared` (not
`Attached`) afterward is the already-documented, harmless between-session
USB/IP behavior, not a new issue.

**Seventh bug found and fixed, same day, seventh live launch attempt**:
with the health-check fix in place, the launcher now correctly reported
`KB-BRIDGE-001` (a real failure, not a false positive) — the bridge
genuinely never came up. Root-caused directly by reproducing the exact
failure repeatedly via `wsl.exe -- bash -lc` from this session: a
one-shot `wsl.exe` invocation tears down its own session the instant it
exits, killing any backgrounded/nohup'd/disowned descendant process
still tied to it — a known WSL quirk, distinct from anything Windows- or
Kineo-specific. Fixed with `setsid` (full session detachment) plus a
brief settle delay after backgrounding before the script exits; verified
directly that `setsid` alone was insufficient without the delay, and
that the combination survives 8+ seconds past the invoking `wsl.exe`
process exiting. **Not yet confirmed via the actual launcher exe on real
hardware** — that is the next thing to test, and given this bug could
explain intermittent-looking failures generally, a full multi-analysis
Session-1-style run is warranted once this passes a single analysis.

Current sha256 (after all seven fixes):
```
KineoBridge.exe                    f7c5581740c71b8c046e9fac36ba4545a69f958caa6906ebbf5b8e04d3dc524f
```
(CTI, XML, and `.pyc` hashes are unchanged from the table above — only
the launcher changed.)

## First clean multi-run result (2026-09-27, eighth live launch attempt)

With all seven bugs above fixed, the exact customer-facing build
(`KineoBridge.exe`, requireAdministrator, release CTI, compiled bridge)
completed a 3-analysis session with no errors, verified independently
across all three logs (launcher, CTI, bridge) plus Kineo's own
ChironLog — cross-referenced and consistent once the WSL-clock-is-UTC
vs Windows-is-IST (+5:30) offset is accounted for (not a bug; WSL2's
default clock is UTC while Windows shows local time — worth remembering
for any future log correlation).

- Analysis 76: 56 GenTL frames, 0 bad, 0 timeouts, ~59.85 fps — succeeded.
- Analysis 77: 53 GenTL frames, 0 bad, 0 timeouts, ~59.85 fps physically
  completed, then cancelled by the user during Kineo's own post-capture
  processing (not during the physical capture itself) — Kineo correctly
  suppressed its result ("cancelled by user" → later "is cancelled,
  skipping update" → "suppressing COMPLETED event"), no corruption, no
  crash, matches previously-documented cancel behavior.
- Analysis 78: 51 GenTL frames, 2 bad, 0 timeouts, ~57.63 fps — succeeded
  (the 2 bad frames are minor and followed an unusually quick ~13s
  restart right after the cancel; well within historical norms).
- Zero USB/control-channel errors, zero WSAECONNREFUSED, zero reconnect
  attempts, zero camera reattach needed mid-session — one single
  `ensure_camera_ready()` pass at launch was sufficient for the whole
  session.
- EventGetData grace period worked correctly all three times (~504-520ms
  real first-frame latency, well inside the 1000ms grace, reverting to
  normal timeout handling immediately after).

This is the first time the actual packaged release build has completed
a clean multi-analysis session including a cancel-then-retry. **Still
outstanding before formal golden-acceptance sign-off**: 5 consecutive
analyses in one session (only 2 complete + 1 cancelled done so far) and
a ≥3-minute idle gap (longest gap seen so far is ~2m42s, between
Analysis 76 and 77).

## Known limitations (carried from PROJECT_MEMORY.md §20)

- WSL-side Aravis 0.8.36 build and one-time `usbipd bind` are the only
  remaining manual-setup dependencies not yet automated end-to-end by
  the installer (the launcher can now perform `bind` itself at runtime
  since it runs elevated — but the Aravis build inside WSL is still a
  manual prerequisite).
- No MSI installer yet (manual folder copy).
- No licensing/activation enforcement yet.
- Kineo's own multi-minute cancel-cleanup delay is inside Kineo's own
  app, not fixable here.
- Full Kineo timestamp/FPS-semantics investigation only partially done
  (see PROJECT_MEMORY.md §20).

## Rollback notes

- Previous (asInvoker, non-elevated) launcher design is fully described
  in git history / `PROJECT_MEMORY.md` git blame if a return to
  least-privilege is ever wanted; the only functional loss from
  reverting would be automatic `usbipd bind` on a never-shared device
  (that case would go back to showing a manual one-time admin command).
- Reverting the reproducible-build changes (`--no-insert-timestamp`,
  `py_compile`/`UNCHECKED_HASH`) is safe functionally but would
  reintroduce non-reproducible hashes across rebuilds — no reason to
  do this.
- If the release CTI is ever found to behave differently from the dev
  CTI against real Kineo, do not ship: diff only the build-flag deltas
  between `build_m5.sh` and `build_m5_release.sh` (currently: `-Wall
  -Wextra` dropped, `-s` added, `--no-insert-timestamp` added) until
  they behave identically, per the standing instruction not to ship a
  release CTI that hasn't been proven equivalent.

## Icon update (2026-09-27)

`KineoBridge.exe` now shows Kineo's own application icon (extracted
from `Kineo Software.exe`, see `PROJECT_MEMORY.md` §19). Verified
byte-identical by re-extracting the icon back out of the rebuilt exe
and diffing against the source `.ico`. Reproducibility re-verified
(unchanged). This changed only `launcher/kineobridge.rc` (added the
icon statement) — no functional/behavioral code changed, so the seven
bug fixes and the 3-analysis validation result above still apply
unchanged to this build.

Current sha256 (icon added):
```
KineoBridge.exe                    4c4c96e26255e4e0c653a7181234778f9316d7f5d7f27e7c7a9d4632a4550a54
```
(CTI, XML, and `.pyc` hashes unchanged.)

## Status-window attribution footer (2026-09-27)

Added a small credits footer to the status window: "System Integration
and Infrastructure Solutions (siis.in)" and "Eashaaan
(github.com/th4kur)", gray centered text below a thin separator.
Cosmetic only, no functional/behavioral change — the window grew
slightly taller (90px -> 132px) to fit it. Reproducibility re-verified,
icon re-verified byte-identical.

Current sha256 (footer added):
```
KineoBridge.exe                    8f2b88b2b5a3716566979b65e68edb92561d4cd5b93a0f6e670c0bb45beb8a40
```
(CTI, XML, and `.pyc` hashes unchanged.)

## Status-window redesign (2026-09-27)

Window grew 400×132 → 460×200; typography reworked for clearer title
vs. status hierarchy (see `PROJECT_MEMORY.md` §19); attribution changed
to "Made with ♥ by" plus two real clickable hyperlinks (Website →
siis.in, LinkedIn → Eashaan Thakur's profile), styled as classic
underlined blue links with a hand cursor on hover. Cosmetic/UI only —
no functional change. Reproducibility and the embedded icon both
re-verified unaffected. Text-width margin for the longest link line
was estimated (no way to render/preview a live Win32 window from this
dev environment) rather than visually confirmed — worth a quick look
on real hardware.

Current sha256 (redesign):
```
KineoBridge.exe                    156ebff3643884ece8bfd59846765fd1a35f46f94c0e7107cb8e8f136e856957
```
(CTI, XML, and `.pyc` hashes unchanged.)

## Status-window refinement round 2 (2026-09-27)

User feedback on the round-1 redesign: only "Website"/"LinkedIn" should
be the actual clickable text (not the whole line); title too heavy,
status too faint (inverted from what it should be — status is the more
important, moment-to-moment line); title/status should read as one
tight group; footer should be pushed further down for separation; 460px
width "looks awkward".

Addressed all five: switched the two attribution rows from plain
`SS_NOTIFY` statics to real `SysLink` controls with `<A HREF>` markup so
only the tagged word is a link; title is now medium-weight/gray
(de-emphasized), status is now bold/near-black (emphasized — this was
an intentional hierarchy inversion from round 1); title/status gap
shrunk to near zero, gap before the footer widened; window narrowed
460×200 → 400×190. Each `SysLink` row is measured at its own real
rendered width at runtime (`LM_GETIDEALHEIGHT`/`LM_GETIDEALSIZE`) and
centered exactly there, rather than guessed — see `PROJECT_MEMORY.md`
§19 for the full mechanism. Added the standard comctl32 v6 manifest
dependency so `SysLink` themes correctly. Reproducibility and the
embedded icon both re-verified unaffected. **Still not visually
confirmed on real hardware** — I have no way to render a Win32 window
from this dev environment; the sizing/centering is now measured live
by the code itself at runtime rather than guessed by me, which should
make it robust regardless, but an actual look is still worth doing.

Current sha256:
```
KineoBridge.exe                    caa79166daca0c180c73952ce3f2b1d8330013269fafe7d6c8cff3363cbe5236
```
(CTI, XML, and `.pyc` hashes unchanged.)

## PID-capture reliability fix (2026-09-27)

`$!` turned out to be unreliable specifically through the `wsl.exe --
bash -lc "..."` interop path in this environment — the shipped
`bridge.pid` was found to be a bare newline (empty), meaning
`stop_bridge()`'s kill had been silently a no-op. Fixed with
`bash -c 'echo $$ > bridge.pid; exec python3 ...'` instead (`$$` read
from inside the new process itself, `exec` keeps the same PID — see
`PROJECT_MEMORY.md` §21 for the full repro/fix). Also added
`scripts/wsl_warmup.ps1`, an optional login-time script addressing two
further `KB-USB-003` causes (cold WSL2 boot race, WSL2 idle-shutdown
silently dropping the attach) — not yet wired into the installer.
Reproducibility and the embedded icon both re-verified unaffected.

Current sha256:
```
KineoBridge.exe                    9e3b9f84fee5a6d5ce8553ad17a09b837caaefbc99e3aad063143bf4682178ca
```
(CTI, XML, and `.pyc` hashes unchanged.)

## Machine lock added (2026-09-28)

User-directed, twice-confirmed (once generally, once specifically
extended to the shipped exe after I flagged that this conflicts with
future multi-customer deployability as currently architected): this
build now refuses to run outside this one machine/account. Two checks,
both hardcoded absolute paths instead of anything dynamic/portable:
`WinMain()` requires `C:\Users\IMV` to exist as a directory (checked
before anything else, including creating the status window — fails
immediately with `KB-ENV-001` otherwise), and every WSL-side command
now uses the hardcoded `/home/imv` instead of `~`. Both constants
verified true on this actual machine before shipping (so this build
still works here); full rationale and the explicit "don't remove this
without checking first" note are in `PROJECT_MEMORY.md` §0/§19/§21.
Reproducibility and the embedded icon both re-verified unaffected.

**This is the first release build that will NOT run correctly if
handed to a different machine/account, even a real future customer
one.** If that's ever needed, this is the first thing to revisit —
replace with real licensing/hardware-ID binding (already flagged as a
future item above) rather than hardcoding more machines.

Current sha256:
```
KineoBridge.exe                    27e5e4c531061d296a02a5db0cf82405f51d99bc85d7b12431b4b0c310aac89f
```
(CTI, XML, and `.pyc` hashes unchanged.)

## Hardware fingerprint lock added (2026-09-28)

User-directed: the path-based lock alone is trivially defeated by
recreating the expected username/directories, so a real backstop was
added — SHA-256 (via Windows BCrypt, no hand-rolled crypto) of this
machine's motherboard serial + BIOS UUID + first disk serial, hardcoded
as `EXPECTED_HW_HASH` and checked in `WinMain()` right after the path
check (`KB-ENV-002` on mismatch). Deliberately no private/signing key
anywhere — that would be a real security mistake in shipped code, not
a style choice (see `PROJECT_MEMORY.md` §19 "Machine lock" for the
full reasoning). Verified: only the hash is embedded in the binary
(`strings` scan — raw hardware identifiers are not present), the log
never records the fingerprint/hash values, and the build-time fingerprint
matches the runtime trim logic byte-for-byte. Reproducibility and the
embedded icon both re-verified unaffected.

Current sha256:
```
KineoBridge.exe                    1257785aaeebd4b3d290df04227bba8c6620cf7e65850bfa8d09e23dfcd86f9c
```
(CTI, XML, and `.pyc` hashes unchanged.)

## Retry-loop bug found and fixed (2026-09-28)

Live-tested the hardware lock (passed correctly, `match=1`); the very
next thing observed was a real Kineo-side crash on a retry
(`KB-KINEO-002`, `Child process exited with code: 3221226505` =
`STATUS_STACK_BUFFER_OVERRUN` in `KineoDeviceService.exe`, preceded by
Kineo's own log warning about an unexpected leftover instance) —
unrelated to the hardware lock itself. Root-caused to a real bug in the
2-attempt retry loop: the ChironLog scan `mark` was computed once
before the loop instead of fresh per attempt (so a retry's scan window
still included the previous attempt's own failure line, causing a
near-instant false failure ~1s in), and nothing killed the first
attempt's process tree before the second attempt launched a new "Kineo
Software.exe" on top of it. Fixed both; verified with a subsequent
clean run succeeding on attempt 1, no retry needed. Reproducibility and
the embedded icon both re-verified unaffected.

Current sha256:
```
KineoBridge.exe                    3fccb6631acfe89f396fa7358b57719890760f9b2e11ff497777585b15a2b195
```
(CTI, XML, and `.pyc` hashes unchanged.)
