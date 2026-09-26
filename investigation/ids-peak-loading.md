# Phase 3 — How `ids_peak.dll` Discovers GenTL Producers

Scope: static/binary inspection only (`strings`, `objdump`/`llvm-objdump`, config-file
grep, read-only registry reads). No process was launched; Kineo was not run.

Binaries inspected:
- `/mnt/c/IMVapps/Kineo Software/resources/chiron-cpp-module/ExternalDependencies/IDS/ids_peak.dll` (IDS, v1.9.0.0)
- `/mnt/c/IMVapps/Kineo Software/resources/chiron-cpp-module/x64/Release/KineoDeviceService.exe` (IMV Technologies, v1.2.0.0)

Raw dumps saved under `~/kineo-bridge/investigation/raw/`:
- `ids_peak_strings_full.txt` (27,039 lines)
- `ids_peak_objdump_p.txt` (empty — GNU `objdump -p` failed: "file format not recognized"; the DLL's PE/COFF layout wasn't parsed by this build of GNU objdump)
- `ids_peak_llvm_objdump_p.txt` (870 lines — used instead; succeeded, format `coff-x86-64`)
- `KineoDeviceService_strings_full.txt` (37,501 lines)

---

## 1. Discovery mechanism

### CONFIRMED

- `ids_peak.dll` strings contain, in sequence:
  `GENICAM_GENTL64_PATH` → `" environment variable not found! Please set this to the
  path of the CTIs."` → `.cti` (strings file lines ~23716–23718). This is the **only**
  GenTL-path-related environment variable name found anywhere in the binary. No
  `GENTL32_PATH`, no `GENICAM_GENTL32_PATH`, no IDS-specific variable name exists.
- Import table of `ids_peak.dll` (via `llvm-objdump -p`, since plain GNU `objdump -p`
  could not parse the file) shows imports from: `KERNEL32.dll`, `MSVCP140.dll`,
  `VCRUNTIME140.dll` / `VCRUNTIME140_1.dll`, several `api-ms-win-crt-*.dll` forwarder
  DLLs, and IDS's own `FirmwareUpdate_MD_VC141_v3_4.dll` / `GCBase_MD_VC141_v3_4.dll` /
  `GenApi_MD_VC141_v3_4.dll`. Specifically:
  - `api-ms-win-crt-environment-l1-1-0.dll` imports **exactly one function**: `_dupenv_s`
    — the CRT environment-variable read, which internally forwards to
    `GetEnvironmentVariableW`. This is the only environment-reading import in the file.
  - `KERNEL32.dll` imports include `FindFirstFileA` / `FindNextFileA` (directory
    enumeration) and `LoadLibraryA` / `GetProcAddress` / `FreeLibrary` (dynamic module
    loading/unloading).
  - **There is no `ADVAPI32.dll` import at all** — i.e., no `RegOpenKeyEx`,
    `RegQueryValueEx`, `RegGetValue`, or any other registry API is imported.
  - No `SHGetFolderPath`/`SHGetKnownFolderPath`/`Shell32.dll` import either (no
    special-folder resolution).
- Read-only registry check via PowerShell (`Get-ChildItem 'HKLM:\SOFTWARE\GenICam'`,
  `HKLM:\SOFTWARE\WOW6432Node\GenICam'`, and a filtered scan of all of
  `HKLM:\SOFTWARE` and `HKLM:\SOFTWARE\WOW6432Node` for child keys matching
  `GenTL|GenICam|EMVA|IDS|Kineo|IMV`) returned **zero matches**. No GenICam/GenTL-related
  registry keys exist on this machine at all. This is fully consistent with the
  import-table evidence — there is no registry-based discovery path.
- Config-file search of the entire `/mnt/c/IMVapps/Kineo Software` tree (json, ini, xml,
  yaml/yml, cfg, config, txt) for `.cti` / `GenTL` / `gentl` / `producer` found **no
  relevant config file**. The only hits were false positives (a TypeScript diagnostic
  message string, and the word "gentle" in OpenCV haarcascade XML comments). No explicit
  CTI path list or override file exists anywhere in the Kineo install tree.
- Verified the 4th `GENICAM_GENTL64_PATH` segment, `C:\Program Files\The Imaging Source
  Europe GmbH\IC4 GenTL Driver for USB3Vision Devices 1.4`: it **exists and is
  populated** — contains `bin\ic4-gentl-u3v_x64.cti`, `bin\ic4-gentl-u3v_arm64.cti`,
  an `arm64` bin subfolder, `kmdriver\{x64,arm64}` subfolders, and docs/changelog. This
  refines the background note: unlike the two empty IDS Peak SDK folders
  (`ids_peak\ids_u3vgentl\64`, `ids_gevgentl`), this TIS USB3Vision path is a real,
  valid CTI-bearing directory.

### INFERENCE

- The discovery mechanism is almost certainly: read `GENICAM_GENTL64_PATH` once via
  `_dupenv_s`; split on `;` (the standard GenTL-spec separator on Windows, matching the
  semicolon-joined value already observed in the env var); `FindFirstFile`/
  `FindNextFile`-scan each directory for `*.cti`; `LoadLibraryA` each match; then
  validate via the standard GenTL entry points (see §2). There is no apparent fallback
  default path and no registry fallback — if the env var is unset, the DLL emits the
  "not found" string and (presumably) has nothing left to enumerate.

---

## 2. Validation / whether a custom CTI would be accepted

### CONFIRMED

- `ids_peak.dll` strings contain, in sequence: `Could not initialize the library!
  [Function: GCInitLib | Error-Code: ` → `Provided cti is not supported!` →
  `GCGetInfo` → `Could not uninitialize the library! [Function: GCCloseLib |
  Error-Code: `. This is a per-candidate-file validation sequence: load module → call
  the standard GenTL entry point `GCInitLib` → call `GCGetInfo` → on failure, emit a
  generic "not supported" message.
- An exhaustive string search for hardcoded competitor vendor names (Basler, FLIR,
  Point Grey, Allied Vision, JAI, Teledyne, Imaging Source, Baumer, Matrix Vision,
  Cognex, Vieworks, Lucid Vision, Photonfocus, Hikrobot, Daheng, Sony) inside
  `ids_peak.dll` produced **zero hits**. The only vendor-name string present is IDS's
  own (`IDS Imaging Development Systems GmbH` / `...;IDS`, from the version resource).
  The `vendorName` / `PEAK_*_GetVendorName` strings are generic GenTL field accessors
  (they read whatever vendor string a loaded CTI reports at runtime) — not a filter
  list.
- The same vendor-name search against `KineoDeviceService.exe` (37,501 lines of
  strings) also produced **zero hits** for any hardcoded competitor vendor/model
  allowlist string.
- `KineoDeviceService.exe` strings contain the full `PEAK_RETURN_CODE_*` enum name list
  (`PEAK_RETURN_CODE_ERROR`, `_SUCCESS`, `_ABORTED`, `_NOT_INITIALIZED`, `_BAD_ALLOC`,
  `_BAD_ACCESS`, `_INVALID_ADDRESS`, `_BUFFER_TOO_SMALL`, `_INVALID_CAST`,
  `_INVALID_ARGUMENT`, `_NOT_FOUND`, `_INVALID_HANDLE`, `_TIMEOUT`, `_OUT_OF_RANGE`,
  `_NOT_IMPLEMENTED`, `_NOT_AVAILABLE`, `_IO`, **`_CTI_LOADING_ERROR`**, `_NO_DATA`),
  confirming (and matching) the prior `legacy/websocket-service-probe/protocol-hints.txt`
  finding that `PEAK_RETURN_CODE_CTI_LOADING_ERROR` is a distinct, named, non-fatal
  return code — not a crash/abort code. Interestingly, these `PEAK_RETURN_CODE_*`
  strings do **not** appear in `ids_peak.dll`'s own strings dump; they appear to be
  compile-time stringified in Kineo's own logging code (from the ids_peak C/C++ header
  enum), not stored as runtime strings inside `ids_peak.dll` itself.
- `KineoDeviceService.exe` strings contain the mangled C++ type names
  `class peak::core::ProducerLibrary` and `class peak::core::CTILoadingException` —
  i.e., Kineo's code is built against IDS's standard `peak::core` C++ wrapper API (the
  generic "peak" SDK layer designed to wrap arbitrary GenTL producers), not some
  IDS-only hardcoded internal path.

### INFERENCE

- The "Provided cti is not supported!" message is most likely emitted only when
  `GCInitLib`/`GCGetInfo` fails or returns something ids_peak's wrapper can't parse
  (e.g., wrong bitness, corrupt export table, unexpected GenTL version) — **not** a
  vendor-ID or vendor-name allowlist check. No vendor-ID constant table or allowlist
  was found anywhere in either binary to support a vendor-based rejection theory.
- Given the generic (non-vendor-filtered) validation path and the presence of
  `peak::core::ProducerLibrary` / `CTILoadingException`, a well-formed third-party/
  custom `.cti` placed in one of the `GENICAM_GENTL64_PATH` directories would very
  likely be picked up and loaded by `ids_peak.dll`'s enumeration pass, and would likely
  also be enumerable by Kineo's `peak::core::ProducerLibrary`-based code — **provided**
  it exports the standard GenTL v1.x C entry points correctly and the load doesn't
  throw `CTILoadingException` (which appears to be caught/skipped per-CTI, given that
  `PEAK_RETURN_CODE_CTI_LOADING_ERROR` is a distinct, non-fatal return code rather than
  part of a hard-abort code family).
- No evidence was found (in strings alone) of a separate, higher-layer Kineo device
  allowlist that filters by vendor/model name *after* ids_peak enumeration. This is a
  **weak negative** — string search alone cannot rule out numeric/binary comparisons
  (e.g., comparing a `uint16` vendor ID, a GUID, or a compiled-in filename list rather
  than a human-readable string).

### UNKNOWN (needs a live-process test)

- Whether `KineoDeviceService.exe`, at runtime, actually iterates *all* GenTL producers
  found via `GENICAM_GENTL64_PATH` (including the TIS ones), or whether it internally
  restricts itself to only the IDS-authored `.cti` files it ships alongside
  `ids_peak.dll` (`ids_u3vgentlk.cti`, `ids_ueyegentl.cti`), via some in-process filter
  not visible in strings (e.g., comparing `TL_INFO_VENDOR` against a compiled-in
  constant, or only calling `ProducerLibrary` on specific hardcoded filenames stored as
  non-printable/obfuscated data).
- Whether a custom/bridge `.cti` dropped into one of the four `GENICAM_GENTL64_PATH`
  directories would actually surface a device in Kineo's UI/API end-to-end — this
  cannot be determined from static analysis alone and is exactly the next experiment.
- Whether the two empty `ids_peak`/`ids_gevgentl` SDK folders (first `PATH` segment)
  cause any startup error/log line, versus being silently skipped when
  `FindFirstFile` finds zero `*.cti` matches there.

---

## 3. Config files / registry (task items 3–4)

- **Config files:** none found anywhere in the Kineo tree that mention `.cti`, `GenTL`,
  `gentl`, or `producer` in a relevant way (see CONFIRMED above). Kineo does not appear
  to ship any override/explicit CTI path list.
- **Registry:** no GenICam/GenTL-related keys exist under `HKLM:\SOFTWARE` or
  `HKLM:\SOFTWARE\WOW6432Node` (checked both a targeted `HKLM:\SOFTWARE\GenICam` /
  `...\WOW6432Node\GenICam` lookup and a broader filtered scan for
  `GenTL|GenICam|EMVA|IDS|Kineo|IMV` child keys). Registry plays no role in producer
  discovery, consistent with the binary import evidence.

---

## 4. Post-enumeration filtering by Kineo (task item 6)

### CONFIRMED

- No hardcoded competitor vendor/model name strings, and no strings resembling
  "supported vendor list" / "allowlist" / "blocklist" / "device filter" wording, were
  found anywhere in `KineoDeviceService.exe`'s 37,501 lines of strings.

### INFERENCE

- If Kineo filters devices after ids_peak enumeration, it likely does so via numeric/
  binary comparison (vendor ID, compiled-in filename, or similar) rather than a
  human-readable string table, since no such table is visible statically. This can't be
  confirmed without dynamic analysis or decompilation of the actual comparison logic.

---

## 5. Highest-value next experiment (live test, for the user to run)

This agent did **not** launch Kineo or any Windows binary, per the investigation's hard
safety rules. The single most valuable next step is a **live-process check**, to be run
by the user:

- **Option A (preferred, most informative):** Run Process Monitor (Sysinternals
  ProcMon) filtered to `KineoDeviceService.exe`, capturing `LoadLibrary`/`CreateFile`/
  `QueryOpen` events, while starting Kineo normally. This directly shows which of the
  4 `GENICAM_GENTL64_PATH` directories and which `.cti` files actually get touched at
  runtime — resolving whether Kineo enumerates all producers on the path or only the
  two IDS-shipped ones.
- **Option B (simpler, more direct proof for the bridge question):** Temporarily add a
  copy of a known-good third-party `.cti` (e.g., the TIS USB3Vision
  `ic4-gentl-u3v_x64.cti`, confirmed present and populated above) to one of the
  `GENICAM_GENTL64_PATH` directories (or append a new directory to the env var),
  restart Kineo's service, and check whether a TIS device (if physically connected)
  appears in Kineo's device list. This is the most direct end-to-end proof of "will
  Kineo accept a non-IDS CTI's devices," and is the natural precursor test before
  building any custom bridge CTI.
