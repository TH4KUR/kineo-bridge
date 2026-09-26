# read_values_test.py — verify actual feature VALUES are readable via the
# GenApi node map, not just node names/existence.
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
    remote = dev.RemoteDevice()
    nm = remote.NodeMaps()[0]

    for name in ["DeviceVendorName", "DeviceModelName", "DeviceSerialNumber",
                 "Width", "Height", "PixelFormat", "PayloadSize"]:
        try:
            node = nm.FindNode(name)
            val = node.Value() if hasattr(node, "Value") else node.CurrentValue() if hasattr(node, "CurrentValue") else "(no Value accessor)"
            print(f"{name} = {val!r}  (type: {type(node).__name__})")
        except Exception as e:
            print(f"{name}: FindNode/Value raised {type(e).__name__}: {e}")

except Exception as e:
    print(f"FAILED: {type(e).__name__}: {e}")
finally:
    ids_peak.Library.Close()
