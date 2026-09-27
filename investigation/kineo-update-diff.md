# Kineo Update Diff — 1.0.0 -> 1.1.2

Captured 2026-09-26, immediately after the user triggered the in-app
update via Kineo's own UI. Compared against `investigation/golden-baseline.md`
(the frozen pre-update state).

## Version changes

| Component | Old | New |
|---|---|---|
| `kineo-application` (app.asar) | `1.0.0` | `1.1.2` |
| `@kineo/kineo-device-module` (unpacked package.json) | `1.0.0-rc.10` | `1.0.0-rc.10` (unchanged string, but the executing binary changed -- see below) |
| `ids_peak.dll` / `ids_peak_ipl.dll` | unchanged | **unchanged** (byte-identical hash) |
| `ids_u3vgentlk.cti` / `ids_ueyegentl.cti` | unchanged | **unchanged** (byte-identical hash) |
| `ids_u3vcore.sys` (kernel driver) | unchanged | **unchanged** (byte-identical hash) |

An installer artifact was left behind by the updater:
`Upgrades\Kineo\KineoInstaller1.1.2.exe` (not inspected further -- the
live before/after diff already answers what changed).

## Binary hash changes

```
Kineo Software.exe
  old: 4415ba95735aa6ce7339bf18d2e41bb7ad9de92a077baf4098af07fbb347e27d
  new: a95dbfbe3998d52c5a688aa06c1b22301d6ab3a5a6ef00cef4a0466c13e67baa
  CHANGED (expected -- main Electron binary, app version bump)

resources/app.asar
  old: 0ae8ba048a0040cc967c018a77eea9c19153028a51a56f97efa40a433b1df5c7
  new: 247e49c8b3a5426f51137135289af39ab13adbf1bfb2ffbb4a51a4da5b8f7cf4
  CHANGED (expected -- 1.0.0 -> 1.1.2, new frontend deps: @dnd-kit, more
  react-aria/react-stately grid/table modules, etc. -- UI-layer only)

resources/chiron-cpp-module/x64/Release/KineoDeviceService.exe
  old: 32e834b0c77f5923d91b2be5d5f1ea8ce183f593738d1a9e15b488c36ed02502  (2,819,584 bytes)
  new: 7bc52c0226566212deaea34c048584d8a836ee509eb6ce014394baf8b9915aba  (3,021,312 bytes)
  CHANGED -- this is the binary that actually loads our CTI (confirmed:
  our kineo_probe_cti.log always lands in this exact directory, i.e. this
  process's cwd). +201,728 bytes over the old build. THIS IS THE ONE
  COMPONENT THAT MATTERS FOR GENTL COMPATIBILITY.

resources/app.asar.unpacked/node_modules/@kineo/kineo-device-module/bin/KineoDeviceService.exe
  old: 32e834b0c77f5923d91b2be5d5f1ea8ce183f593738d1a9e15b488c36ed02502
  new: 32e834b0c77f5923d91b2be5d5f1ea8ce183f593738d1a9e15b488c36ed02502
  UNCHANGED -- this copy was NOT updated by the installer. It is not the
  copy that actually executes (see above), so this is a stale/inert
  leftover, not a compatibility concern -- but worth knowing the install
  tree now has two genuinely different KineoDeviceService.exe builds
  sitting side by side.

KineoUninstaller.exe
  UNCHANGED
```

## File-tree changes

- Total file count: 17,563 -> 28,264 (+10,701). Reviewed the full new-file
  list: essentially all of it is new Electron/React frontend dependencies
  (`@dnd-kit/*`, more `react-aria`/`react-stately` submodules, etc.) plus
  new `Upgrades/` and `Uninstaller.exe` artifacts from the updater itself.
- **No new files anywhere under `ExternalDependencies/IDS/`,
  `ExternalDependencies/` driver folders, or any TIS (The Imaging Source)
  path.** No ARM64-specific IDS components appeared. No new `.cti` files.
  No changed GenTL producer search-path layout observed.
- **Conclusion: this is an app/UI-layer + device-service-binary update.
  The GenTL/IDS SDK stack itself (ids_peak.dll, ids_peak_ipl.dll, IDS
  CTIs, kernel driver) is completely untouched.** Compatibility risk is
  narrowly scoped to whether the new `KineoDeviceService.exe` calls our
  CTI differently than the old one (new required export, new node
  lookups, different timing) -- not to any ABI change in ids_peak itself.

## Camera configuration schema

`Data/camera_settings.json` (runtime-generated, not part of the
installer) -- pre-update snapshot archived at
`investigation/golden-baseline-m5/camera_settings_pre_update.json` for
comparison if the schema changes after the next real-camera run.

## Logging

User-observed: "it seems to have better logging than the previous one."
This refers to Kineo's own application logs (`ChironLog`,
`KineoDeviceManagerLogs`), which live in `Data/Logs/` and are generated
by the updated `KineoDeviceService.exe`/app code -- unrelated to our own
`kineo_probe_cti.log` (which we generate ourselves and is unaffected by
this). Consistent with a genuine `KineoDeviceService.exe` binary update
adding more detailed internal logging. Not yet inspected in detail; will
revisit if the compatibility test surfaces something the new logs can
help explain.

## Next: Phase 5 — synthetic compatibility test

Testing our existing, byte-for-byte unchanged `probe/m5/m5_bridge.cti`
against this updated Kineo, `KINEO_BRIDGE_SOURCE=synthetic` first.

## Phase 5 result: SYNTHETIC compatibility test — PASSED

Our unchanged `probe/m5/m5_bridge.cti` loaded cleanly under Kineo 1.1.2.
Camera enumerated, device opened, DataStream opened, synthetic
moving-bar video appeared in the viewer, Start-Analysis completed
(Analysis No. 34, `succeed:true` at the ChironLog level, PDF export
available). Warnings (`Focus out of range`, concentration below
range, 0.0% motility) are the expected synthetic-pattern signature, not
errors -- identical class of result to the pre-update M4d/M5 baseline.

CTI-log-level check: only 2 unique `NOT_IMPLEMENTED` call sites hit
(`DSFlushQueue`, the interface-level `GCRegisterEvent` for an
unsupported event id) -- both already-known stubs from every prior
successful run. No new required GenTL calls, no new node lookups.

**Per instructions: CTI marked compatible with the updated Kineo/IDS
Peak stack. No CTI changes made.**

## Phase 6 result: REAL CAMERA test — PASSED (after two unrelated USB/IP hiccups)

Two failed attempts first, both root-caused as **USB/IP attachment drops
in WSL, not Kineo/CTI compatibility issues** (`usbipd list` showed the
camera `Shared`, not `Attached`; Aravis saw zero devices, bridge OPEN
correctly reported `"no device with serial '4110010861' found (saw:
[])"`, and correctly retried with backoff the whole time -- no bridge or
CTI bug). Re-running `usbipd attach --wsl --busid 3-2` fixed it each
time, matching the exact pre-update pattern from the M5 arc.

**Third attempt: full success**, confirmed at every level:
- CTI log: `DevOpenDataStream` triggered the bridge connect early (per
  the M5 fix), real camera opened (`vendor: IDS Imaging Development
  Systems GmbH`, `model: U3-356xXCP-M`, `serial: 4110010861`),
  CONFIGURE/START succeeded, real 1920x1200/2304000-byte frames streamed
  continuously well before `DSStartAcquisition`. `DSStartAcquisition`
  succeeded with the natural 81-buffer pool, `AcquisitionStart` fired,
  clean ~1.055s capture window (Kineo-initiated `AcquisitionStop`,
  matching the same short-clip design behavior observed pre-update),
  clean `DSStopAcquisition`.
- Screenshot confirmation: Analysis No. 35, real camera-grain/noise
  texture visible in the frame (not the synthetic moving-bar pattern),
  title bar reads "Kineo Software 1.1.2".
- ChironLog confirmation (the improved logging the user noticed --
  unrelated to our CTI, just Kineo's own updated device-service binary):
  `Analysis 35 result: {...,"succeed":true,...}`, followed by
  `resultVideo35.mp4` generation. Same warning class as every prior real
  and synthetic run (nonsensical motility numbers -- expected when the
  camera isn't pointed at an actual sample, not a defect).
- PDF export mechanism confirmed available (the same "PDF" button that
  produced `Analysis_33_Abcd_2026-09-26.pdf` pre-update); not yet
  clicked for Analysis 35 specifically, but nothing suggests it would
  behave differently.

**STOP CONDITION B MET: updated Kineo + real camera completes Analysis,
using our completely unmodified M5 CTI/bridge.**
