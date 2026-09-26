# m3f_test.py — M3f verification: node map builds, values readable, and
# writable register-backed nodes can actually be written and read back.
import os
import sys

cti_path = os.path.abspath(sys.argv[1])
script_dir = os.path.dirname(os.path.abspath(__file__))
pkg_dir = os.path.join(script_dir, "..", "pip_ids_peak_190") if os.path.isdir(os.path.join(script_dir, "..", "pip_ids_peak_190")) else os.path.join(script_dir, "pip_ids_peak_190")
os.add_dll_directory(os.path.join(pkg_dir, "ids_peak"))
sys.path.insert(0, pkg_dir)
import ids_peak.ids_peak as ids_peak  # noqa: E402

ids_peak.Library.Initialize()
try:
    producer = ids_peak.ProducerLibrary.Open(cti_path)
    system = producer.System().OpenSystem()
    system.UpdateInterfaces(1000)
    iface = system.Interfaces()[0].OpenInterface()
    iface.UpdateDevices(1000)
    dd = iface.Devices()[0]
    dev = dd.OpenDevice(ids_peak.DeviceAccessType_Control)
    print("OpenDevice: OK")
    remote = dev.RemoteDevice()
    nm = remote.NodeMaps()[0]
    nodes = nm.Nodes()
    print(f"Nodes(): OK, count={len(nodes)}")
    for n in nodes:
        print(f"  node: {n.DisplayName()!r} / {n.Name()!r} (type={type(n).__name__})")

    print("\n-- read initial values --")
    for name in ["Width", "Height", "PixelFormat", "PayloadSize", "ExposureTime",
                 "Gain", "BlackLevel", "BrightnessAutoTarget", "BrightnessAutoPercentile",
                 "BrightnessAutoTargetTolerance", "OffsetX", "OffsetY", "TriggerMode",
                 "TLParamsLocked"]:
        try:
            node = nm.FindNode(name)
            if hasattr(node, "Value"):
                print(f"  {name} = {node.Value()!r}")
            elif hasattr(node, "CurrentEntry"):
                print(f"  {name} = {node.CurrentEntry().SymbolicValue()!r}")
            else:
                print(f"  {name}: (no read accessor found on {type(node).__name__})")
        except Exception as e:
            print(f"  {name}: FAILED {type(e).__name__}: {e}")

    print("\n-- write/read-back test on writable nodes --")
    write_tests = [
        ("ExposureTime", 2000.0),
        ("Gain", 2.5),
        ("BlackLevel", 3.0),
        ("BrightnessAutoTarget", 180),
        ("Width", 1920),
        ("Height", 1200),
        ("TLParamsLocked", 1),
    ]
    for name, val in write_tests:
        try:
            node = nm.FindNode(name)
            node.SetValue(val)
            readback = node.Value()
            print(f"  {name}: wrote {val!r}, read back {readback!r} {'OK' if readback == val else 'MISMATCH'}")
        except Exception as e:
            print(f"  {name}: write FAILED {type(e).__name__}: {e}")

    print("\n-- PayloadSize after writes (should reflect any Width/Height change) --")
    print(f"  PayloadSize = {nm.FindNode('PayloadSize').Value()!r}")

    print("\n-- command node test --")
    for name in ["AcquisitionStart", "AcquisitionStop", "ExposureStart"]:
        try:
            node = nm.FindNode(name)
            node.Execute()
            print(f"  {name}.Execute(): OK, IsDone={node.IsDone()}")
        except Exception as e:
            print(f"  {name}: FAILED {type(e).__name__}: {e}")

except Exception as e:
    print(f"FAILED: {type(e).__name__}: {e}")
finally:
    ids_peak.Library.Close()
