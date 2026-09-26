# test1_control_matrix.py — Test 1: known-producer control matrix.
# Calls ProducerLibrary.Open() identically on 7 candidates and records
# PASS/FAIL + exact error for each, using the same official ids_peak binding
# whose bundled ids_peak.dll is byte-identical (sha256-confirmed) to Kineo's.
import os
import sys
import hashlib

script_dir = os.path.dirname(os.path.abspath(__file__))
pkg_dir = os.path.join(script_dir, "pip_ids_peak_190")
os.add_dll_directory(os.path.join(pkg_dir, "ids_peak"))
sys.path.insert(0, pkg_dir)
import ids_peak.ids_peak as ids_peak  # noqa: E402

BASE = script_dir
T1 = os.path.join(BASE, "test1")

CANDIDATES = [
    ("1. original Kineo IDS ids_u3vgentlk.cti (read-only, original path)",
     r"C:\IMVapps\Kineo Software\resources\chiron-cpp-module\ExternalDependencies\IDS\ids_u3vgentlk.cti"),
    ("2. scratch copy, same filename", os.path.join(T1, "ids_u3vgentlk.cti")),
    ("3. scratch copy, renamed", os.path.join(T1, "renamed_totally_different.cti")),
    ("4. signature-stripped IDS copy (control, known-pass)", os.path.join(BASE, "real_ids_u3vgentlk_unsigned.cti")),
    ("5. TIS ic4-gentl-gev_x64.cti", os.path.join(T1, "ic4-gentl-gev_x64.cti")),
    ("6. TIS ic4-gentl-u3v_x64.cti", os.path.join(T1, "ic4-gentl-u3v_x64.cti")),
    ("7. kineo_probe_v2.cti (ours)", os.path.join(BASE, "kineo_probe_v2.cti")),
]


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def probe_log_path_for(path):
    # only meaningful for our own probe, which honors KINEO_BRIDGE_LOG
    return os.path.join(BASE, "kineo_probe_test1.log")


ids_peak.Library.Initialize()
results = []
try:
    for label, path in CANDIDATES:
        print(f"\n=== {label} ===")
        print(f"path: {path}")
        if not os.path.isfile(path):
            print("SKIP: file not found")
            results.append((label, path, None, "SKIP-NOT-FOUND", None))
            continue
        digest = sha256_of(path)
        print(f"sha256: {digest}")

        log_path = probe_log_path_for(path)
        os.environ["KINEO_BRIDGE_LOG"] = log_path
        if os.path.exists(log_path):
            os.remove(log_path)

        try:
            producer = ids_peak.ProducerLibrary.Open(path)
            print("RESULT: PASS")
            results.append((label, path, digest, "PASS", None))
            # touch System() to fully confirm it's usable, then let it go out of scope
            sd = producer.System()
            print(f"  System key: {sd.Key()!r}")
        except Exception as e:
            msg = str(e)
            print(f"RESULT: FAIL -- {msg}")
            results.append((label, path, digest, "FAIL", msg))

        if os.path.exists(log_path):
            with open(log_path) as f:
                lines = f.read().strip().splitlines()
            print(f"  probe-internal log ({len(lines)} lines): {lines}")
        else:
            print("  probe-internal log: N/A (not our probe / closed-source, no internal log)")

finally:
    ids_peak.Library.Close()

print("\n\n========== SUMMARY ==========")
for label, path, digest, status, err in results:
    print(f"{status:20s} | sha256={digest[:16] if digest else 'N/A':16s} | {label}")
    if err:
        print(f"                       error: {err}")
