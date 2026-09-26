# m1a_test.py — M1a: explicit-path producer open via IDS's own official
# ids_peak Python bindings (package version 1.9.0.0.2, whose bundled
# ids_peak.dll is confirmed byte-identical, sha256-verified, to the copy
# Kineo actually ships). No GENICAM_GENTL64_PATH involved (that's M1b).
#
# Run on the Windows side:
#   pyembed\python.exe m1\m1a_test.py <path-to-kineo_probe.cti>
import os
import sys

if len(sys.argv) < 2:
    print("usage: m1a_test.py <path-to-kineo_probe.cti>")
    sys.exit(2)

cti_path = os.path.abspath(sys.argv[1])
script_dir = os.path.dirname(os.path.abspath(__file__))
pkg_dir = os.path.join(script_dir, "pip_ids_peak_190")

# Point the probe's own logger at a dedicated M1 log file (distinct from M0's),
# set before the probe DLL is ever loaded (its DllMain reads this at
# PROCESS_ATTACH time).
log_path = os.path.join(script_dir, "kineo_probe_m1a.log")
os.environ["KINEO_BRIDGE_LOG"] = log_path
if os.path.exists(log_path):
    os.remove(log_path)

# Python 3.8+ no longer implicitly searches the importing package's own
# directory for dependent DLLs (the ids_peak.dll, GCBase_*.dll, etc. that
# _ids_peak_python_interface.pyd needs) -- must add it explicitly.
os.add_dll_directory(os.path.join(pkg_dir, "ids_peak"))
sys.path.insert(0, pkg_dir)

import ids_peak.ids_peak as ids_peak  # noqa: E402  (the actual API lives in the ids_peak submodule; __init__.py only does DLL-directory setup)

print(f"=== M1a: python={sys.version.split()[0]} ({'64-bit' if sys.maxsize > 2**32 else '32-bit'}) ===")
print(f"=== ids_peak package dir: {pkg_dir} ===")
print(f"=== probe CTI: {cti_path} ===")
print(f"=== probe log: {log_path} ===")

ok = True

print("\n-- Library.Initialize() --")
ids_peak.Library.Initialize()
print("OK")

try:
    print("\n-- ProducerLibrary.Open(cti_path) --")
    producer = ids_peak.ProducerLibrary.Open(cti_path)
    print("OK: producer library opened")

    print("\n-- producer.System() -> SystemDescriptor --")
    sys_desc = producer.System()
    print(f"OK: key={sys_desc.Key()!r}")

    print("\n-- sys_desc.OpenSystem() --")
    system = sys_desc.OpenSystem()
    print("OK: system opened")

    print("\n-- system.UpdateInterfaces(1000) --")
    system.UpdateInterfaces(1000)
    print("OK")

    print("\n-- system.Interfaces() --")
    ifaces = system.Interfaces()
    print(f"count={len(ifaces)}  {'OK (expected 1)' if len(ifaces) == 1 else 'UNEXPECTED'}")
    if len(ifaces) != 1:
        ok = False

    if ifaces:
        ifd = ifaces[0]
        print(f"  interface[0]: key={ifd.Key()!r} displayName={ifd.DisplayName()!r} TLType={ifd.TLType()!r}")

        print("\n-- interface.OpenInterface() --")
        iface = ifd.OpenInterface()
        print("OK: interface opened")

        print("\n-- iface.UpdateDevices(1000) --")
        iface.UpdateDevices(1000)
        print("OK")

        print("\n-- iface.Devices() --")
        devs = iface.Devices()
        print(f"count={len(devs)}  {'OK (expected 1)' if len(devs) == 1 else 'UNEXPECTED'}")
        if len(devs) != 1:
            ok = False

        if devs:
            dd = devs[0]
            print(f"  device[0]: key={dd.Key()!r}")
            print(f"    VendorName    = {dd.VendorName()!r}")
            print(f"    ModelName     = {dd.ModelName()!r}")
            print(f"    SerialNumber  = {dd.SerialNumber()!r}")
            print(f"    DisplayName   = {dd.DisplayName()!r}")
            print(f"    UserDefinedName = {dd.UserDefinedName()!r}")
            print(f"    Version       = {dd.Version()!r}")
            print(f"    TLType        = {dd.TLType()!r}")
            print(f"    AccessStatus  = {dd.AccessStatus()!r}")
            print(f"    TimestampTickFrequency = {dd.TimestampTickFrequency()!r}")

            print("\n-- device_descriptor.IsOpenable(Control) --")
            try:
                openable = dd.IsOpenable(ids_peak.DeviceAccessType_Control)
                print(f"IsOpenable(Control) = {openable}")
            except Exception as e:
                print(f"IsOpenable raised: {e!r}  (non-fatal, continuing to try OpenDevice anyway)")

            print("\n-- device_descriptor.OpenDevice(Control) --")
            dev = dd.OpenDevice(ids_peak.DeviceAccessType_Control)
            print("OK: DEVICE OPENED")

            print("\n-- dev.RemoteDevice() / Key() (post-open info) --")
            print(f"  device.Key() = {dev.Key()!r}")
            try:
                port = dev.RemoteDevice()
                print(f"  device.RemoteDevice() -> port object obtained: {port!r}")
            except Exception as e:
                print(f"  device.RemoteDevice() raised: {e!r}  (expected -- GenICam XML/port is M3, not implemented yet)")

except Exception as e:
    print(f"\n!!! EXCEPTION: {type(e).__name__}: {e}")
    ok = False

finally:
    print("\n-- Library.Close() --")
    try:
        ids_peak.Library.Close()
        print("OK")
    except Exception as e:
        print(f"Library.Close() raised: {e!r}")

print(f"\n=== M1a result: {'PASS' if ok else 'FAIL/PARTIAL — see output above'} ===")
sys.exit(0 if ok else 1)
