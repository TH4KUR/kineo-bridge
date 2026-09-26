# nodemap_test.py — M3a follow-up: does RemoteDevice() actually construct a
# GenApi NodeMap, or is it a lazy wrapper that hasn't tried yet? Extends the
# M1a flow through OpenDevice -> RemoteDevice() -> NodeMaps().
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
    print(f"RemoteDevice(): OK, {remote!r}")

    try:
        node_maps = remote.NodeMaps()
        print(f"NodeMaps(): OK, count={len(node_maps)}")
        if node_maps:
            nm = node_maps[0]
            print(f"  nodeMap[0]: {nm!r}")
            try:
                nodes = nm.Nodes()
                print(f"  Nodes(): OK, count={len(nodes)}")
                for n in nodes[:20]:
                    print(f"    node: {n.DisplayName()!r} / {n.Name()!r}")
            except Exception as e:
                print(f"  Nodes() raised: {type(e).__name__}: {e}")
    except Exception as e:
        print(f"NodeMaps() raised: {type(e).__name__}: {e}")

except Exception as e:
    print(f"EARLIER STEP FAILED: {type(e).__name__}: {e}")
finally:
    ids_peak.Library.Close()
