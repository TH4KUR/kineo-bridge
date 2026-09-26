# GenTL Export Comparison (Phase 2)

Comparison of the exported GenTL C API surface across all 4 target `.cti`
producers (x64 builds; arm64 siblings used only as a secondary cross-check).

Tooling note: GNU `objdump -p` failed to recognize all 4 `.cti` files ("file
format not recognized") despite `file` correctly identifying them as PE32+
x86-64 DLLs. `llvm-objdump -p` parsed all of them cleanly and was used for
every export table below. `nm -D` was not needed since llvm-objdump succeeded.

## Files examined

| # | File | Vendor | Version | Exports (count) | Ordinal base |
|---|---|---|---|---|---|
| 1 | `ids_u3vgentlk.cti` | IDS | 1.16.1.0 | 81 | 100 |
| 2 | `ids_ueyegentl.cti` | IDS | 1.16.1.0 | 81 | 100 |
| 3 | `ic4-gentl-u3v_x64.cti` | TIS | 1.6.0.894 | 59 | 1 |
| 4 | `ic4-gentl-gev_x64.cti` | TIS | 1.6.0.1250 | 59 | 1 |

Cross-check: `ic4-gentl-u3v_arm64.cti` and `ic4-gentl-gev_arm64.cti` were also
dumped. Both have export name sets **identical** to their x64 siblings (same 59
names each, order matches) — confirms the export surface is arch-independent
per vendor, as expected.

The two IDS CTIs (`ids_u3vgentlk.cti`, `ids_ueyegentl.cti`) have **byte-for-byte
identical export name/ordinal sets** (81/81, same ordinals 100–750). The two
TIS CTIs (`u3v`, `gev`) likewise have **identical** export name sets (59/59,
same alphabetical ordinal ordering 1–59). So effectively there are only two
distinct *export surfaces* to compare: "IDS surface" vs "TIS surface".

## Export table by GenTL family

Legend: yes = exported, — = not exported. "IDS" column applies to both IDS
CTIs identically; "TIS" column applies to both TIS CTIs identically.

### GC* (GenICam/port/generic) family

| Export | IDS | TIS | Notes |
|---|---|---|---|
| GCGetInfo | yes | yes | common |
| GCGetLastError | yes | yes | common |
| GCInitLib | yes | yes | common |
| GCCloseLib | yes | yes | common |
| GCInitLibShared | — | yes | TIS-only, non-spec vendor extension (INFERENCE: likely lets multiple TIS CTIs in one process share init state) |
| GCReadPort | yes | yes | common |
| GCWritePort | yes | yes | common |
| GCReadPortStacked | yes | yes | common — GenTL >=1.1 batched port access |
| GCWritePortStacked | yes | yes | common |
| GCGetPortInfo | yes | yes | common |
| GCGetPortURL | yes | yes | common |
| GCGetNumPortURLs | yes | yes | common |
| GCGetPortURLInfo | yes | yes | common |
| GCRegisterEvent | yes | yes | common |
| GCUnregisterEvent | yes | yes | common |

### Event* family

| Export | IDS | TIS |
|---|---|---|
| EventGetData | yes | yes |
| EventGetInfo | yes | yes |
| EventFlush | yes | yes |
| EventGetDataInfo | yes | yes |
| EventKill | yes | yes |

All common. Full set (5/5) present in both vendors.

### TL* (Transport Layer) family

| Export | IDS | TIS |
|---|---|---|
| TLOpen | yes | yes |
| TLClose | yes | yes |
| TLGetInfo | yes | yes |
| TLGetNumInterfaces | yes | yes |
| TLGetInterfaceID | yes | yes |
| TLGetInterfaceInfo | yes | yes |
| TLOpenInterface | yes | yes |
| TLUpdateInterfaceList | yes | yes |

All common, all 8 present both sides.

### IF* (Interface) family

| Export | IDS | TIS |
|---|---|---|
| IFClose | yes | yes |
| IFGetInfo | yes | yes |
| IFGetNumDevices | yes | yes |
| IFGetDeviceID | yes | yes |
| IFUpdateDeviceList | yes | yes |
| IFGetDeviceInfo | yes | yes |
| IFOpenDevice | yes | yes |
| IFGetParentTL | yes | yes |

All common, all 8 present both sides.

### Dev* (Device) family

| Export | IDS | TIS |
|---|---|---|
| DevGetPort | yes | yes |
| DevGetInfo | yes | yes |
| DevClose | yes | yes |
| DevGetNumDataStreams | yes | yes |
| DevGetDataStreamID | yes | yes |
| DevOpenDataStream | yes | yes |
| DevGetParentIF | yes | yes |

All common, all 7 present both sides.

### DS* (Data Stream / buffer) family

| Export | IDS | TIS | Notes |
|---|---|---|---|
| DSAnnounceBuffer | yes | yes | common |
| DSAllocAndAnnounceBuffer | yes | yes | common |
| DSRevokeBuffer | yes | yes | common |
| DSQueueBuffer | yes | yes | common |
| DSGetParentDev | yes | yes | common |
| DSFlushQueue | yes | yes | common |
| DSStartAcquisition | yes | yes | common |
| DSStopAcquisition | yes | yes | common |
| DSGetInfo | yes | yes | common |
| DSGetBufferID | yes | yes | common |
| DSGetBufferInfo | yes | yes | common |
| DSGetBufferChunkData | yes | yes | common — GenTL >=1.3 chunk-data extension |
| DSClose | yes | yes | common |
| DSGetNumBufferParts | yes | — | **IDS-only** — GenTL "multi-part" buffer extension (>=1.5) |
| DSGetBufferPartInfo | yes | — | **IDS-only**, same extension |
| DSGetBufferInfoStacked | — | yes | **TIS-only** — batched buffer-info query; INFERENCE: not confirmed as an official GenTL-spec function name, likely a vendor performance extension analogous to GCReadPortStacked, but not verified against spec text |

Common DS exports: 13/13. IDS extra: 2 (multi-part streaming). TIS extra: 1
(stacked buffer info, uncertain spec status).

### Non-GenTL / internal exports (present but not part of the GenTL C API surface)

- IDS: 23 `OS_*` exports (`OS_EventClear`, `OS_EventCreateObject`,
  `OS_EventDeinitialize`, `OS_EventDestroyObject`, `OS_EventGetObjectSize`,
  `OS_EventInitialize`, `OS_EventSet`, `OS_EventWait`, `OS_EventWaitMultiple`,
  `OS_MemoryAlloc`, `OS_MemoryAllocWithTag`, `OS_MemoryCmp`, `OS_MemoryCompare`,
  `OS_MemoryCopy`, `OS_MemoryFree`, `OS_MemoryGetPageShift`,
  `OS_MemoryGetPageSize`, `OS_MemoryMove`, `OS_MemorySet`, `OS_MemoryZero`,
  `OS_StrGetCharStringLen`, `OS_StringCopy`, `OS_StringPrintf`) — these look
  like a shared internal OS-abstraction helper library statically linked in
  and incidentally exported, not GenTL API. Plus one mangled MSVC CRT symbol
  (`??4_Init_locks@std@@...`) exported by accident of static linking. **None
  of these are relevant to GenTL compatibility.**
- TIS: `HandleMapCount`, `ObjectTrackerCount` — look like internal
  debug/diagnostic counters (INFERENCE from name only; not GenTL API).

## Summary counts

- Common mandatory-looking GenTL exports across all 4 CTIs: **55**
  (GC 14 + Event 5 + TL 8 + IF 8 + Dev 7 + DS 13)
- IDS-only GenTL-named extras: 2 (`DSGetNumBufferParts`, `DSGetBufferPartInfo`)
- TIS-only GenTL-named extras: 2 (`GCInitLibShared`, `DSGetBufferInfoStacked`) —
  both of uncertain/likely-non-spec status (INFERENCE)
- IDS-only non-GenTL noise exports: 23 OS_* + 1 CRT symbol = 24
- TIS-only non-GenTL noise exports: 2 (`HandleMapCount`, `ObjectTrackerCount`)

## GenTL spec version signal

This section is backed by direct string evidence, not just export names.

Both IDS CTIs and both TIS CTIs contain embedded GenICam XML/description
strings referencing **`EVENT_REMOTE_DEVICE`** as the current name, and the TIS
binaries explicitly note "(named EVENT_FEATURE_DEVEVENT in GenTL **up to
version 1.4**)" (verbatim string found in both TIS binaries). The IDS binaries
use `EVENT_REMOTE_DEVICE` / `EVENT_REMOTE_DEVICE_CONNECTION_STATUS_CHANGE`
naming exclusively (no legacy `EVENT_FEATURE_DEVEVENT` string found),
consistent with the same post-1.4 naming.

**Directly observed:** the `EVENT_FEATURE_DEVEVENT` -> `EVENT_REMOTE_DEVICE`
rename happened after GenTL 1.4, so both vendors' XML/producer text targets
**GenTL 1.5 or later**.

**INFERENCE from export set:** the presence of `DSGetBufferChunkData` (chunk
data, GenTL >=1.3), `GCReadPortStacked`/`GCWritePortStacked` (stacked port
access, GenTL >=1.1), and IDS's `DSGetNumBufferParts`/`DSGetBufferPartInfo`
(multi-part buffer support, generally associated with GenTL 1.5's
multi-part/PFNC extension) is consistent with, and does not contradict, a
GenTL 1.5-family implementation for IDS. TIS not exporting the multi-part pair
is **UNKNOWN whether that means TIS implements an older effective feature
level, or simply doesn't use multi-part buffers for GigE/USB3 area-scan
devices and omits the optional exports** — both are plausible; this could not
be resolved from exports/strings alone (would need to call
`GCGetInfo`/`TLGetInfo` with `GenTLVersion`/`GenTLSFNCVersion` info command IDs
at runtime, or find explicit numeric version strings, neither of which turned
up in a static string search).

No explicit numeric "GenTL 1.x" version string was found in any of the 4
binaries via `strings` search (checked patterns like `GenTL 1.`, `GenTL_1`,
`up to version 1.`). So the **exact minor version (1.5 vs 1.6) is UNKNOWN** —
only "≥1.5" is directly supported by string evidence.

## Minimal loadable export set (INFERENCE)

Based on what's common to all 4 real producers, a minimal producer that just
needs to be *recognized and opened* by a GenTL Consumer (not necessarily
stream data) would need at least:

- `GCGetInfo`, `GCInitLib`, `GCCloseLib`, `GCGetLastError` (library lifecycle)
- `TLOpen`, `TLClose`, `TLGetInfo` (System/TL module)
- `TLGetNumInterfaces`, `TLGetInterfaceID`, `TLGetInterfaceInfo`,
  `TLOpenInterface`, `TLUpdateInterfaceList` (interface enumeration — most
  consumers, e.g. GenICam Consumer reference impl, call
  `TLUpdateInterfaceList` before enumerating)
- `IFGetNumDevices`, `IFGetDeviceID`, `IFGetDeviceInfo`, `IFOpenDevice`,
  `IFUpdateDeviceList`, `IFClose`, `IFGetInfo`, `IFGetParentTL` (device
  enumeration/open)
- `DevGetInfo`, `DevClose`, `DevGetParentIF`, `DevGetPort` (minimal device
  handle — `DevGetPort` needed if the consumer wants the GenApi XML at all)
- `GCGetPortURL`, `GCGetNumPortURLs`, `GCGetPortURLInfo`, `GCReadPort`,
  `GCWritePort`, `GCGetPortInfo` (register/XML access via port — needed for
  the consumer to load the GenApi feature XML at all, which is how most tools
  identify a device as "real")

Actually opening a data stream (`DevGetNumDataStreams`, `DevOpenDataStream`,
`DS*`, `Event*`) is likely NOT required just to be *listed and opened* as a
device by a generic viewer — those become necessary once the consumer tries
to acquire images. This split (enumerate/open vs. stream) is an INFERENCE from
GenTL's own module layering (System -> Interface -> Device -> DataStream), not
something directly observed by exercising a consumer.
