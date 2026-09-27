# Golden Baseline — M5 Real-Camera Bridge (pre Kineo-update)

Frozen 2026-09-26, immediately after M5/M6 was accepted as COMPLETE (real
video in Kineo's UI, real Analysis, real report PDF, via the WSL2/Aravis
bridge). This document exists so a Kineo software update can be evaluated
against a precisely-known "before" state, with a verified rollback path.

**Do not modify `probe/m5/`, `wsl-camera/`, or `probe/m3/m4h/` (the M4d
synthetic baseline) until Kineo-update compatibility testing is complete.**

## 1. Preserved artifacts (this repo, native WSL filesystem)

| Item | Location |
|---|---|
| Working M5 CTI + bridge client source | `probe/m5/` |
| M4d synthetic golden baseline (untouched, protected since M4d) | `probe/m3/m4h/` |
| WSL bridge server | `wsl-camera/` |
| Launch scripts (session-scoped env vars, no permanent changes) | `probe/m5/launch_kineo_wsl.ps1`, `probe/m5/launch_kineo_synthetic.ps1` |
| Successful real-camera CTI log (full trace of the passing run) | `investigation/golden-baseline-m5/kineo_probe_cti_m5_success.log` |
| Successful Analysis report PDF (copy) | `investigation/golden-baseline-m5/Analysis_33_Abcd_2026-09-26.pdf` |
| Pre-update `camera_settings.json` snapshot | `investigation/golden-baseline-m5/camera_settings_pre_update.json` |
| Narrative history of the whole M5 arc | `investigation/kineo-workflow-trace.md` |

Original locations on the Windows side (read-only references, not
archived copies): live report at
`C:\Users\IMV\Documents\Kineo\Reports\Analysis_33_Abcd_2026-09-26.pdf`;
live CTI log at
`C:\IMVapps\Kineo Software\resources\chiron-cpp-module\x64\Release\kineo_probe_cti.log`
(this one gets overwritten/appended on every run, hence the archived copy
above).

## 2. Existing installation backup — verified complete

`C:\IMVapps\Kineo Software BACKUP 1.0.0` already existed (made
2026-09-25, before any M5 work) and was verified against the live
`C:\IMVapps\Kineo Software` install rather than duplicated:

- Same total size (2.8 GB) and near-identical file count (17,563 backup
  vs. 17,643 live).
- **Every file in the backup also exists in the live install** (a diff of
  full recursive file listings, backup vs. live, produced zero
  backup-only entries).
- The 80 live-only files are all runtime-generated data, not
  application files: `Data/AnalysisFolder/CurrentAnalysis/*`,
  `Data/HttpResources/*.mp4`, `Data/Kineo-software/chironbase_backup_*.sqlite`,
  `Data/Logs/**/*.log`, `Data/camera_settings.json`, and our own
  `resources/chiron-cpp-module/x64/Release/kineo_probe_cti.log`.
- Spot-hash-verified byte-identical: `Kineo Software.exe`,
  `KineoUninstaller.exe`, `KineoDeviceService.exe` (all SHA-256 match,
  see table below).

**Conclusion: this backup is a complete, valid rollback target.** No new
backup copy was made (would have duplicated 2.8 GB of identical data).

## 3. Version metadata (pre-update)

| Component | Version |
|---|---|
| `kineo-application` (app.asar `package.json`) | `1.0.0` |
| `@kineo/kineo-device-module` (both app.asar `package.json` and the unpacked module's own `package.json` agree) | `1.0.0-rc.10` |
| IDS external SDK DLL family (`GCBase`/`GenApi`/`NodeMapData`/`XmlParser`/etc., all `_MD_VC141_v3_4`) | file mtime 2025-01-09 (no embedded version string found via `strings`; same build batch as `ids_peak.dll`/`ids_peak_ipl.dll`) |
| `ids_u3vgentlk.cti`, `ids_ueyegentl.cti` | file mtime 2024-08-29 |

No TIS (The Imaging Source) components found anywhere in this install —
IDS-only, note for the update-diff comparison.

## 4. SHA-256 hashes (pre-update, live install == backup for all binaries)

```
Kineo Software.exe
  4415ba95735aa6ce7339bf18d2e41bb7ad9de92a077baf4098af07fbb347e27d

KineoUninstaller.exe
  5545a02f8cff45108293e272a366b3a9ab977332533af1fa5b9bf4e3fabb824a

resources/app.asar
  0ae8ba048a0040cc967c018a77eea9c19153028a51a56f97efa40a433b1df5c7

resources/chiron-cpp-module/x64/Release/KineoDeviceService.exe
resources/app.asar.unpacked/node_modules/@kineo/kineo-device-module/bin/KineoDeviceService.exe
  32e834b0c77f5923d91b2be5d5f1ea8ce183f593738d1a9e15b488c36ed02502
  (both copies identical, as expected)

resources/chiron-cpp-module/ExternalDependencies/IDS/ids_peak.dll
  5a217c28f8c56ba51525e924a1fa75cc44e351aa27032589a0b1e8e0c28be15e

resources/chiron-cpp-module/ExternalDependencies/IDS/ids_peak_ipl.dll
  88773ce85fef1a4cc57d651416f8e0394b0029b403e4108473d589909c9f3da3

resources/chiron-cpp-module/ExternalDependencies/IDS/ids_u3vgentlk.cti
  10a0e429a6ed9c9ccc1e7a8955c38c2b7fd08e62f08104010abb11083cbc5e0a

resources/chiron-cpp-module/ExternalDependencies/IDS/ids_ueyegentl.cti
  bfbd5cf6140108707b6c1c97a5d3ab2f2d03a9c0132e393b14b2c9b40515ff95
```

### Our own working artifacts (native WSL filesystem, `~/kineo-bridge/`)

```
probe/m5/m5_bridge.cti
  fc51fb4a4c8f71a9a05dc8579b2a55ce4dedb4bbd144380f3d9f34ecc1b443cc

probe/m5/m5_bridge.c
  f85f0d640204de74a17ca5d5dfcda66320a6c34799584441cd17608fdd143f3d

probe/m5/wsl_bridge_client.c
  30ec0c585ecc9c8d4f77e3e9b7c836a10c0f34cd81115ac6056fdaa5fce08bdd

probe/m5/wsl_bridge_client.h
  f805bc6c7bfb808c5df148245cb8ca50cc609e1f745b7fe2a445c0b23a3ea50c

probe/m5/gentl_v2.h
  c1c4578f5693551bae9116993f8b4c71577773a34108bd7061b1d1e9610596a1

probe/m5/kineo_bridge_m5.xml
  f62c1c36e5c203954d3db8ab4276aa2f3b14395f0d8c87cca3d40d774c8b2c01

probe/m5/m5_bridge.def
  43ff01511bee2210d05c5cae767115f75ecc5a33b5ba719cca94fba0c6785aae

probe/m3/m4h/m4h_standalone.cti  (M4d protected baseline, for reference)
  5929750f10a58ca795fcc60c123b0a5c95d5dcdf8df1b356e0c90d9521f1b902

probe/m3/m4h/m4h_standalone.c
  8d4771be50e912056f80f5af26377aad51f9020ac018b26b3d0aa54d7fc192da

wsl-camera/kineo_camera_bridge.py
  aeca9fe28cff0fe51adf351c864619d314715413eaa68d24aaa35a855e7ec30b

wsl-camera/protocol.py
  1879172f312ddf9b25e8e858a74082b8f398fadabbcc7b636263924ce52427b2

wsl-camera/camera_source.py
  135cf43c7bdae1c7eb3553bbf3e676e3649bc3271bcda7a76618592c3d618712

wsl-camera/test_client.py
  e23edb1d56261090952e1577fd38ab789f8c90e9855d1cb946aacb5d2f615bf7
```

## 5. Known-good behavioral trace (for compatibility diffing)

From `kineo_probe_cti_m5_success.log`, the exact sequence a compatible
Kineo build should reproduce, in order, once our CTI is loaded via
`GENICAM_GENTL64_PATH`:

1. `GCInitLib` called 3x (Kineo's own capability-probe pattern) before
   `TLOpen`.
2. `TLOpen` -> `TLUpdateInterfaceList` -> `IFOpenDevice` -> full GenApi
   XML read via `GCReadPort` (13,804 bytes) -> `DevGetInfo` x4 ->
   `DevGetNumDataStreams`/`DevGetDataStreamID` -> register writes
   (`ExposureTime`, `Gain`, `BlackLevel`, `Width`, `Height`,
   `AcquisitionFrameRate`) -> `DevOpenDataStream`.
3. This same `IFOpenDevice`->`DevOpenDataStream` sequence repeats 2-3
   times (Kineo's own retry/re-probe pattern) before the real streaming
   pass.
4. Real pass: `DSAllocAndAnnounceBuffer`/`DSQueueBuffer` x81 (natural
   pool size, not an artificial cap) -> `GCRegisterEvent(EVENT_NEW_BUFFER)`
   -> `DSStartAcquisition` -> `COMMAND AcquisitionStart` (written twice,
   ~0ms apart) -> `EventGetData` polls (first poll historically used a
   short ~150ms timeout -- this is why the WSL bridge connection is
   triggered at `DevOpenDataStream`, not `DSStartAcquisition`, giving it
   tens of seconds of lead time) -> frames delivered/requeued
   continuously -> `COMMAND AcquisitionStop` (Kineo-initiated, ~1s later
   in the observed successful run) -> `DSStopAcquisition` ->
   `GCUnregisterEvent` -> `DevClose`.
5. Any compatibility test against an updated Kineo should be compared
   against this exact shape. The first point of divergence is the
   evidence to act on (per the update-compatibility directive already in
   effect).

## 6. Next steps (not yet done)

- Phase 2: locate/inspect the available Kineo update offline (read-only),
  without installing.
- Phase 4 (after backup+rollback confirmed, which is now done): install
  the update via Kineo's own UI (user-driven, not automatable from this
  WSL session).
- Phase 5/6: synthetic-then-real compatibility test of this exact,
  unmodified `probe/m5/m5_bridge.cti` against the updated Kineo.
