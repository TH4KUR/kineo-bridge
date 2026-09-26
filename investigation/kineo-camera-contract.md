# Kineo Camera Contract — GenICam/GenApi Node Evidence

**Binary:** `/mnt/c/IMVapps/Kineo Software/resources/chiron-cpp-module/x64/Release/KineoDeviceService.exe`
(PE32+ x86-64, IMV Technologies, v1.2.0.0)

**Method:** `strings -n 4` (ASCII) and `strings -n 4 -e l` (UTF-16LE) dumped to
`~/kineo-bridge/investigation/raw/strings_ascii.txt` (37,501 lines) and
`raw/strings_wide.txt` (38 lines), then targeted `grep` against those dumps.
Raw dumps were not printed to context/markdown per instructions; only matched
lines are quoted below.

Adjacent install directory contents (read-only listing, no exe execution):
`temperature_pids.json`, `encrypted_message.bin`, `private_key.pem`,
`LICENSE.txt`, and the DLLs `bz2.dll`, `fmt.dll`, `libcrypto-3-x64.dll`,
`zip.dll`, `zlib1.dll`. No `camera_settings.json` default/template file was
found sitting in this directory — a wider search of the full Kineo Software
tree for a bundled template/config or GenICam XML file was **not** performed
in this pass (see UNKNOWNs / next steps below).

---

## 1. Confirmed strings / evidence

### 1.1 GenICam/SFNC-style node names found as exact standalone strings

Exact-line, case-insensitive grep of the ASCII string dump against the
requested target pattern list returned these standalone hits:

```
AcquisitionStart
AcquisitionStop
BlackLevel
ExposureStart
ExposureTime
Gain
Height
OffSetX
OffsetY
PayloadSize
TLParamsLocked
TriggerMode
Width
```

Notable detail: the offset-X node string is spelled `OffSetX` (capital `S`)
in the binary, not the canonical GenICam `OffsetX`. This is either (a) a
Kineo-internal identifier/log label rather than a literal GenApi node-name
lookup string, or (b) a case-insensitive lookup Kineo performs itself. It is
flagged here verbatim because a literal case-sensitive `NodeMap::FindNode`
call against a real GenICam XML would fail against this spelling if used
as-is — worth confirming dynamically.

These 13 hits directly overlap the already-confirmed `camera_settings.json`
schema fields (`width`/`height`/`ExposureTime`/`Gain`/`BlackLevel`/
`BrightnessAuto*`) plus core GenTL/GenICam acquisition control names
(`AcquisitionStart`, `AcquisitionStop`, `ExposureStart`, `PayloadSize`,
`TriggerMode`, `TLParamsLocked`).

### 1.2 Target patterns NOT found as exact standalone lines (first pass)

The following requested patterns did **not** appear as exact standalone
lines in the first-pass grep: `PixelFormat`, `Mono8`, `TriggerSource`,
`DeviceVendorName`, `DeviceModelName`, `DeviceSerialNumber`, `DeviceUserID`,
`StreamBufferHandlingMode`, `BrightnessAuto*` (as a literal node name;
`BrightnessAutoTarget`/`BrightnessAutoPercentile`/
`BrightnessAutoTargetTolerance` are already confirmed separately via the
known `camera_settings.json` schema, not via this binary-string grep),
`GevSCPS*`, `StreamID`, `PixelSize`.

This is a **first-pass, exact-line-only result** — it does not rule out
these names appearing as substrings inside longer strings (mangled C++
symbols, JSON fragments embedded in log format strings, etc.). A broader
substring sweep and IDS-peak-namespace symbol sweep (`peak::core::nodes`,
`NodeMap`, `FloatNode`/`IntegerNode`/`BooleanNode`/`EnumerationNode`/
`StringNode` class names) was planned but not yet executed in this pass —
see UNKNOWNs.

### 1.3 IDS peak C++ GenApi wrapper linkage (from legacy hint file, re-confirmed as a hint only)

The prior string dump preserved at
`~/kineo-bridge/legacy/websocket-service-probe/protocol-hints.txt` shows a
demangled/partially-mangled C++ RTTI symbol:

```
.?AV?$ClassCreator@VCommandNode@nodes@core@peak@@@?A0x44354e43@@
.?AV?$_Ref_count_obj2@V?$ClassCreator@VCommandNode@nodes@core@peak@@@?A0x44354e43@@@std@@
```

This decodes to a `ClassCreator<peak::core::nodes::CommandNode>` — i.e.
Kineo statically links against IDS peak's C++ GenApi node-map wrapper
(`peak::core::nodes::CommandNode`, part of the `ids_peak` / `ids_peak_ipl`
SDK's node-map object model), not merely raw GenTL byte-stream access. It
also shows `CameraException`, `InternalErrorException@core@peak`, and
`ISerialCom`/`SerialCom`/`SerialComException` classes (the serial-com
classes appear to be for peripheral hardware — fan/relay/temperature board
control — a separate subsystem from the camera GenICam contract; see
`temperature_pids.json` in the install dir, which is consistent with this).

This linkage evidence was taken from the legacy hint file as a starting
pointer per instructions, not independently re-extracted from the current
binary's raw dump in this pass (the current dump's `ClassCreator` /
`peak::core::nodes` substring sweep is one of the planned-but-not-yet-run
steps below).

---

## 2. Inferred required vs. optional nodes

**Caveat on method:** classifying a node as "required" vs "optional" from
static strings alone is inherently weak evidence. The intended
context-grep step (`-B2/-A2` around each hit, to see whether it's paired
with hard-error/abort wording vs. best-effort/warning wording) was **not**
executed in this pass — see UNKNOWNs. The list below is therefore based on
(a) which nodes are core to the confirmed `camera_settings.json` schema and
GenICam's own SFNC acquisition lifecycle (these are near-certainly required
for the device to stream at all), and (b) general GenICam/GenTL protocol
knowledge of what a producer cannot omit, rather than confirmed
error-string pairing.

### Likely REQUIRED (hard runtime dependency inferred)

- `Width`, `Height` — directly mirrored in the confirmed camera_settings.json
  schema (`width`, `height`); needed to size the acquisition buffer
  (2,304,000 bytes for 1920x1200 Mono8 is already confirmed).
- `ExposureTime`, `Gain`, `BlackLevel` — directly mirrored in
  camera_settings.json; these are user-facing settings Kineo writes.
- `AcquisitionStart` / `AcquisitionStop` — canonical GenICam commands
  required to start/stop streaming; no GenTL/GenApi camera driver path
  omits these.
- `ExposureStart` — appears as a standalone string; likely used either as
  a trigger-related command node or an event; presence alongside
  `TriggerMode` suggests Kineo may support/require a triggered-exposure
  acquisition mode, not just free-run.
- `PayloadSize` — required by any GenTL consumer to size stream buffers;
  near-certain hard dependency.
- `TLParamsLocked` — a GenICam/GenTL-standard node that must be settable to
  1 before acquisition start (a licensing/consistency-lock node in the SFNC
  transport-layer category); its presence as a literal string suggests
  Kineo actively sets it, which would be a **hard requirement** for a
  bridge producer's XML to expose (many minimal/synthetic producers forget
  this node and cameras/clients that check it will fail to start
  acquisition).
- `TriggerMode` — presence strongly implies Kineo queries/sets this node;
  likely required at least to confirm/force free-run mode even if
  triggered capture isn't used.
- `PixelFormat`/`Mono8` — **not found as literal strings**, yet the
  confirmed camera_settings.json and payload-size math (1920×1200 = 1
  byte/px Mono8) show Kineo/IDS peak resolves pixel format somewhere. Two
  plausible explanations: Kineo hardcodes an assumption (reads raw buffer
  as Mono8) rather than querying the `PixelFormat` enum node by name, or it
  goes through the IDS peak C++ object API (e.g. `peak::core::PixelFormat`
  as a typed enum class rather than the literal GenApi string name), which
  would not show up as the literal ASCII string `"PixelFormat"`. This is
  flagged as an explicit UNKNOWN requiring dynamic verification.

### Likely OPTIONAL / best-effort (weaker inference)

- `OffSetX` / `OffsetY` — present, but the non-canonical capitalization of
  `OffSetX` suggests this may be a Kineo-internal parameter name/label
  rather than a strict pass-through GenApi node query; ROI/offset support
  may be best-effort or UI-only rather than a hard runtime requirement for
  basic acquisition.
- `BrightnessAutoTarget` / `BrightnessAutoPercentile` /
  `BrightnessAutoTargetTolerance` — confirmed only via the already-known
  camera_settings.json schema, not via this binary's string dump; these
  read as Kineo/IDS peak auto-exposure convenience features layered on top
  of the base exposure/gain controls, plausibly with graceful fallback if
  the camera lacks IDS-specific auto-brightness nodes.

### Not found at all (status unclear — see UNKNOWNs)

`TriggerSource`, `DeviceVendorName`, `DeviceModelName`,
`DeviceSerialNumber`, `DeviceUserID`, `StreamBufferHandlingMode`,
`GevSCPS*`, `StreamID`, `PixelSize`. Device-identity nodes
(`DeviceVendorName`/`DeviceModelName`/`DeviceSerialNumber`/`DeviceUserID`)
are SFNC-standard and almost universally queried by any GenTL-based
application at enumeration time (to populate a device-picker UI, log
identity, etc.) — their absence as literal strings in this pass is more
likely a limitation of the exact-line grep (they may be present as
substrings in mangled symbols, or resolved through IDS peak's typed C++
accessors rather than string-keyed `NodeMap::FindNode` calls) than genuine
evidence Kineo never touches them.

---

## 3. Read vs. written (INFERENCE — static-analysis limited)

Static string evidence cannot reliably distinguish reads from writes for
any of the nodes above. Reasoned INFERENCE only, not confirmed:

- Likely **written** by Kineo (user/settings-driven): `ExposureTime`,
  `Gain`, `BlackLevel`, `Width`/`Height` (or `OffsetX`/`OffsetY` if ROI is
  supported), `TriggerMode`, `TLParamsLocked`, `AcquisitionStart`/
  `AcquisitionStop` (commands, inherently "written"/executed).
- Likely **read** by Kineo (status/identity/sizing): `PayloadSize`,
  `PixelFormat` (if queried at all), device-identity nodes (if queried).
- `BrightnessAuto*` fields are ambiguous — could be write-only (Kineo
  pushes a target/percentile into an IDS-specific auto-exposure node) or
  read-modify-write (Kineo reads current auto-exposure state then adjusts).

**This section is explicitly flagged as the weakest part of this
investigation.** A live GenApi node-map dump via ids_peak while Kineo is
actually running and driving the IDS U3-3560XCP-M camera is the definitive
way to resolve which nodes are read, written, or both, and in what order
relative to `AcquisitionStart`.

---

## 4. Explicit UNKNOWNs

1. **Whether `PixelFormat`/`Mono8` are queried by name at all**, or whether
   Kineo/IDS-peak resolves pixel format through a typed C++ enum API that
   never surfaces the literal string in the binary. Needs dynamic
   verification (live node-map dump, or a broader substring/symbol sweep
   that was not completed in this pass).
2. **Whether device-identity nodes (`DeviceVendorName`, `DeviceModelName`,
   `DeviceSerialNumber`, `DeviceUserID`) are used at all**, and if so
   whether Kineo requires exact/plausible values (e.g. matching an IDS
   vendor string) or accepts arbitrary values from a bridge producer.
3. **The full extent of IDS-peak-specific (non-SFNC) node names** Kineo may
   depend on — the planned broader sweep for `peak::core::nodes` /
   `NodeMap` / `FloatNode`/`IntegerNode`/`BooleanNode`/`EnumerationNode`/
   `StringNode` symbol names, and for IDS-proprietary feature names beyond
   standard SFNC, was **not executed** in this investigation pass. This is
   the single highest-value gap: it directly determines whether Kineo is
   tightly IDS-coupled or SFNC-generic-enough to fake.
4. **Whether a bundled `camera_settings.json` template or a GenICam XML
   file ships anywhere else in the wider Kineo Software install tree** —
   not searched in this pass (only the immediate exe directory was
   listed).
5. **Read vs. write semantics and call ordering** for every node above —
   see Section 3; unresolved by static analysis alone.
6. **The true meaning of `OffSetX`'s non-standard capitalization** — label
   text vs. literal case-sensitive node lookup.
7. **Whether `TriggerMode`/`TriggerSource`/`ExposureStart` together imply
   Kineo actually drives external/software triggering in normal operation,
   or whether these are vestigial/optional code paths** unused in the
   currently-observed free-run acquisition flow.

---

## 5. Recommendation: what a minimal producer's GenICam XML must expose

Given the evidence above, a minimal-but-safe GenTL producer's GenICam XML
description should, at minimum, expose (as real, functioning nodes, not
stubs that error out):

- `Width`, `Height` (writable, matching the native 1920x1200 Mono8 sensor
  geometry already confirmed, or a documented subset)
- `PixelFormat` (even though not confirmed as a literal string dependency,
  it is SFNC-mandatory and cheap to expose correctly as `Mono8`)
- `ExposureTime`, `Gain`, `BlackLevel` (writable float/int nodes)
- `AcquisitionStart`, `AcquisitionStop` (command nodes)
- `PayloadSize` (integer node, correctly reflecting the real/emulated
  buffer size, i.e. 2,304,000 bytes for the confirmed geometry)
- `TLParamsLocked` (boolean/integer node — **do not omit**; several GenTL
  producers get overlooked here and clients that set it before
  `AcquisitionStart` will fail silently or hard-fail if the node is
  missing)
- `TriggerMode` (at minimum settable to `Off`, to satisfy any startup
  query/reset Kineo performs, since its presence as a literal string
  suggests active use)
- Device-identity nodes (`DeviceVendorName`, `DeviceModelName`,
  `DeviceSerialNumber`, `DeviceUserID`) as read-only strings — SFNC-cheap
  to add and reduces risk given UNKNOWN #2 above
- `OffsetX`/`OffsetY` as writable integer nodes defaulting to 0, in case
  Kineo's `OffSetX`/`OffsetY` references are literal ROI node
  reads/writes rather than internal labels

Because Kineo statically links IDS peak's C++ GenApi node-map wrapper
(`peak::core::nodes::CommandNode`, confirmed via the legacy hint file's
`ClassCreator<peak::core::nodes::CommandNode>` RTTI symbol) rather than
only using raw GenTL streaming calls, this is evidence Kineo walks/queries
a **real GenApi node-map object model** at some point in its startup or
settings-apply path — not just a byte-oriented GenTL data stream. This
weighs toward needing a **reasonably complete, well-formed GenICam XML**
(valid SFNC categories/visibility/access-mode metadata, not merely the
7-13 confirmed node names as bare leaves) rather than a bare-minimum
synthetic XML with only the handful of nodes explicitly seen in strings —
IDS peak's C++ wrapper layer may perform its own node-map validation or
category traversal that a too-sparse/malformed XML could fail even before
Kineo's own code touches it. However, this is inference from linkage
evidence, not confirmed by tracing actual node-map access calls — **the
single most valuable next experiment is a live GenApi node-map dump (via
ids_peak) while Kineo is running against the real IDS camera**, to see
exactly which nodes it enumerates/reads/writes and in what order. That
live dump should be treated as the authoritative source; this document's
required/optional classification should be revised once it's available.

---

## 6. Method notes / what was NOT done in this pass

- Broader substring (non-exact-line) greps for the remaining target
  patterns and for IDS-peak namespace symbols were planned but not
  executed — flagged throughout as UNKNOWNs above.
- Context greps (`-B2/-A2`) around each hit to classify hard-error vs.
  best-effort wording were planned but not executed.
- A wider read-only search of the full Kineo Software install tree for a
  bundled camera_settings template or GenICam XML file was not performed.
- No binary was executed, patched, renamed, or modified at any point.
