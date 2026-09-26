# Phase 1 — Inventory

Status: PARTIAL (foundational facts confirmed; full export/dependency analysis delegated to Phase 2/3 agents)
Generated: 2026-09-26

## 1. File identity and version metadata (CONFIRMED via `file` + PowerShell `Get-Item .VersionInfo`)

| File | Path | Arch | FileVersion | Company |
|---|---|---|---|---|
| KineoDeviceService.exe | `C:\IMVapps\Kineo Software\resources\chiron-cpp-module\x64\Release\KineoDeviceService.exe` | PE32+ x86-64, console, 6 sections | 1.2.0.0 | IMV Technologies |
| ids_peak.dll | `...\ExternalDependencies\IDS\ids_peak.dll` | x86-64 | 1.9.0.0 | IDS Imaging Development Systems GmbH |
| ids_peak_ipl.dll | `...\ExternalDependencies\IDS\ids_peak_ipl.dll` | x86-64 | 1.13.0.0 | IDS Imaging Development Systems GmbH |
| ids_u3vgentlk.cti | `...\ExternalDependencies\IDS\ids_u3vgentlk.cti` | PE32+ x86-64 DLL, 7 sections | 1.16.1.0 | IDS Imaging Development Systems GmbH |
| ids_ueyegentl.cti | `...\ExternalDependencies\IDS\ids_ueyegentl.cti` | PE32+ x86-64 DLL, 7 sections | 1.16.1.0 | IDS Imaging Development Systems GmbH |
| ic4-gentl-gev_x64.cti | `C:\Program Files\The Imaging Source Europe GmbH\IC4 GenTL Driver for GigEVision Devices 1.6\bin\ic4-gentl-gev_x64.cti` | PE32+ x86-64 DLL | 1.6.0.1250 | The Imaging Source Europe GmbH |
| ic4-gentl-gev_arm64.cti | same dir, `_arm64.cti` | PE32+ ARM64 DLL | 1.6.0.1250 | The Imaging Source Europe GmbH |
| ic4-gentl-u3v_x64.cti | `C:\Program Files\...\IC4 GenTL Driver for USB3Vision Devices 1.6\bin\ic4-gentl-u3v_x64.cti` | PE32+ x86-64 DLL | 1.6.0.894 | The Imaging Source Europe GmbH |
| ic4-gentl-u3v_arm64.cti | same dir, `_arm64.cti` | PE32+ ARM64 DLL | 1.6.0.894 | The Imaging Source Europe GmbH |

Note: env var also references a "USB3Vision Devices 1.4" TIS install (see §3) — not yet located/confirmed to exist on disk; flagged UNKNOWN for Phase 3 agent to check.

KineoDeviceService.exe directory also contains **91 DLLs total** under `chiron-cpp-module/` (OpenCV, OpenVINO, websocket++, boost, etc. — not individually catalogued here; see Phase 2/4 agents for targeted exports/strings).

## 2. GENICAM_GENTL64_PATH (CONFIRMED, both Machine and User scope — identical value)

```
C:\Program Files\IDS\ids_peak\ids_u3vgentl\64;
C:\IMVapps\Kineo Software\resources\chiron-cpp-module\ExternalDependencies\IDS;
C:\Program Files\The Imaging Source Europe GmbH\IC4 GenTL Driver for GigEVision Devices 1.6\bin;
C:\Program Files\The Imaging Source Europe GmbH\IC4 GenTL Driver for USB3Vision Devices 1.4\bin
```

**Important finding:** the first path segment, `C:\Program Files\IDS\ids_peak\ids_u3vgentl\64`, does **not exist** — checked, and in fact `C:\Program Files\IDS\ids_peak\ids_u3vgentl` and `...\ids_gevgentl` are both present as directories but are **completely empty** (0 files), last modified 2026-09-25 (one day before this investigation). This looks like a partially-uninstalled or partially-installed standalone IDS peak SDK. The `\64` subfolder referenced in the env var doesn't exist at all.

INFERENCE: this dead path segment is harmless for producer discovery (GenTL loaders typically skip missing paths) but means there is currently **no standalone IDS Peak SDK with headers/docs** on this machine — only the redistributable DLLs/CTIs bundled inside Kineo's install and inside the TIS IC4 installs.

The fourth segment, TIS "USB3Vision Devices 1.4", was not independently confirmed to exist — Phase 3 agent should check `C:\Program Files\The Imaging Source Europe GmbH\IC4 GenTL Driver for USB3Vision Devices 1.4` for presence/emptiness the same way.

This env var is the standard GenTL producer discovery mechanism (semicolon-delimited dirs scanned for `*.cti`). Its presence and scope (both Machine + User) is strong evidence GenTL-standard discovery is in play for at least *some* consumer on this box — Phase 3 agent must determine whether `ids_peak.dll` itself honors this var or does its own internal/hardcoded discovery, and whether Kineo overrides it before launching KineoDeviceService.exe.

## 3. GenTL SDK / headers / docs already installed locally

- No standalone GenTL SDK headers found under `C:\Program Files\IDS\ids_peak\*` (dirs empty — see §2).
- No headers/SDK found bundled inside Kineo's `ExternalDependencies\IDS` beyond the runtime DLLs/CTIs themselves (Phase 2 agent should double check for a `include/` or `sdk/` subfolder while dumping exports).
- Not yet checked: whether TIS IC4 installs ship a GenTL SDK/headers alongside their CTIs (`C:\Program Files\The Imaging Source Europe GmbH\...`) — delegated to Phase 1/2 agent follow-up.
- `~/aravis-0.8.36/src` likely contains a vendored `GenTL.h`/`GenTL_v1_5.h` (Aravis implements/consumes GenTL) — delegate confirmation + path to Phase 6 agent (WSL bridge design), since it's the same codebase they're already inspecting.

## 4. Which CTIs are actually loaded by KineoDeviceService at runtime

UNKNOWN — no Kineo process was running at investigation time (`Get-Process` for `*Kineo*` returned nothing). Confirming actual runtime CTI loads requires either:
  - launching Kineo and inspecting loaded modules (Windows `Get-Process -Module` / Sysinternals `Handle`/`listdlls` if available), or
  - static inspection of KineoDeviceService.exe's config/import behavior (delegated to Phase 3 agent).

This is a live-system observation Phase 3 should attempt non-destructively (read-only process/module listing while Kineo is running, if the user is willing to launch it) — flagged as a specific ask for the user rather than assumed.

## 5. Host toolchain facts relevant to later phases (CONFIRMED)

- Windows: `Microsoft Windows NT 10.0.26200.0`, `PROCESSOR_ARCHITECTURE=ARM64`.
- No Visual Studio found under `Program Files` or `Program Files (x86)`.
- No Windows SDK found under `Program Files (x86)/Windows Kits`.
- `cl.exe` not found via `where.exe`.
- On the WSL/Linux side: no `clang`, no `x86_64-w64-mingw32-*` mingw cross toolchain installed yet. `objdump`, `llvm-objdump`, `nm`, `strings`, `python3` are present.
- Full toolchain investigation (cross-compile options for producing a PE32+ x86-64 DLL from this ARM64 Linux host) delegated to Phase 5 agent.

## 6. Legacy artifact note

`~/kineo-bridge/legacy/websocket-service-probe/` contains a prior WebSocket-emulation experiment (`server.py`, `ws.log`, `protocol-hints.txt`). Per project instructions this is historical evidence only — not the current architecture, not to be extended or modified.

`protocol-hints.txt` (already-extracted strings from a prior KineoDeviceService.exe binary scan) independently confirms the WS protocol verbs already known (`Device-Get-State`, `Analysis-Start`, `PerfTest-Start`, etc.) and adds useful new evidence for Phase 4 (camera contract) and general architecture understanding:
  - Uses **websocketpp**, **boost.asio**, **nlohmann::json**, **spdlog**, **OpenVINO** (`ov::CompiledModel`, `ov::InferRequest`), **OpenCV**, **libzip**, and IDS peak's own `peak::core::nodes::CommandNode` (`ClassCreator<CommandNode>` — confirms Kineo links against ids_peak's C++ GenApi node wrapper, not just the C API).
  - A `SerialCom` subsystem (separate from the camera/GenTL path) talks to a "Platinum board" over serial — handles fan speed, relay, heating, temperature, LED — i.e. Kineo also manages non-camera hardware (thermal/illumination control unit) via serial, independent of the GenTL camera path. Relevant context, not in scope for the camera bridge itself.
  - Confirms `camera_settings.json` is the on-disk settings cache Kineo loads/saves, with the already-known width/height/ExposureTime/Gain/BlackLevel/BrightnessAuto* schema.
  - `ExposureStart` string appears alongside `AcquisitionStart`/`AcquisitionStop` — an additional GenICam-ish feature name for Phase 4 to account for.
  - `PEAK_RETURN_CODE_CTI_LOADING_ERROR` string confirms ids_peak.dll has an explicit, named error path for CTI load failures — useful signal for Phase 3 (ids_peak surfaces CTI loading errors distinctly, implying it does its own dlopen/LoadLibrary of `.cti` files rather than delegating entirely to another layer).

## Open items carried into later phases

- [ ] Confirm/deny existence of TIS "USB3Vision Devices 1.4" install (Phase 3).
- [ ] Confirm whether ids_peak.dll reads `GENICAM_GENTL64_PATH` directly (exports/strings/behavior) vs. Kineo setting an absolute/overridden path before spawning KineoDeviceService.exe (Phase 3).
- [ ] Full export tables for the 4 primary CTIs (Phase 2).
- [ ] Runtime module-load confirmation while Kineo is actually running (needs user to launch Kineo; non-destructive `Get-Process -Module` read) — ask user before phase 3 agent attempts, since it requires the app to be running.
