# CLAUDE.md

Before doing anything in this repo, read **`PROJECT_MEMORY.md`** in full.
It is the authoritative project handoff document — architecture, the
GenTL/GenApi contract, the 60fps lifecycle, known bugs already fixed
(and why they must not come back), build commands, file map, and the
golden test procedure. Do not rediscover any of that from scratch.

## Standing rules (do not violate)

- READ-ONLY, no exceptions without explicit user approval, on:
  `/mnt/c/IMVapps/Kineo Software`, `/mnt/c/Program Files`,
  `/mnt/c/Program Files (x86)`, `/mnt/c/Windows`.
- Never patch, rename, or otherwise modify installed Kineo binaries or
  any installed `.cti` file.
- No registry, driver, or `.wslconfig` changes.
- No destructive commands (`rm -rf`, force-push, `git reset --hard`,
  etc.) without explicit confirmation.
- All project artifacts live under `~/kineo-bridge/` (this repo).
- `~/kineo-bridge/legacy/websocket-service-probe/` (if present) is
  historical-only — never modify.
- `probe/m3/m4h/` (the M4d golden synthetic baseline) must **never be
  edited again**.
- Do not commit or push to a remote without explicit user approval.
  Assume the intended GitHub repo is **private** (proprietary IP).

## Quick orientation

- CTI (GenTL producer): `probe/m5/m5_bridge.c` + `wsl_bridge_client.c/h`.
- Bridge (WSL/Aravis server): `wsl-camera/kineo_camera_bridge.py` +
  `camera_source.py` + `protocol.py`.
- Customer launcher: `launcher/kineobridge_launcher.c` →
  `KineoBridge.exe`.
- Dev tooling (bash, not shipped to customers): `scripts/`.
- Release packaging: `scripts/package_release.sh` → gitignored
  `release/`.
- Full history/investigation notes: `investigation/`.

Before changing CTI or bridge behavior, re-read `PROJECT_MEMORY.md`
§21 ("Do-not-regress rules") and run the golden test procedure in §23
after your change.
