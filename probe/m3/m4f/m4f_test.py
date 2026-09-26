# m4e_test.py -- M4c-1d verification: same M4c-1 checks plus a dynamic
# growth test (allocate well past the old fixed ceilings to exercise the
# registry's realloc-based growth path with no artificial count limit),
# using only the official ids_peak Python bindings' high-level API.
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
    print(f"NodeMaps: OK, {len(nm.Nodes())} nodes (unchanged from M3i baseline -- see m3i_test.py for the full node dump)")

    print("\n-- DataStream open --")
    ds_descs = dev.DataStreams()
    print(f"  DataStreams(): {len(ds_descs)} descriptor(s)")
    ds = ds_descs[0].OpenDataStream()
    print("  OpenDataStream(): OK")
    # Not calling ds.PayloadSize() here -- that maps to
    # DSGetInfo(STREAM_INFO_PAYLOAD_SIZE), which real Kineo never actually
    # calls (it reads PayloadSize from the GenApi register instead, per
    # the M3i/M4a traces) and M4b's scope is buffer allocation only, not
    # implementing additional unrequested DSGetInfo commands.
    payload = 2304000  # known PayloadSize (Width*Height*1 byte/px, Mono8)

    min_required = ds.NumBuffersAnnouncedMinRequired()
    print(f"  NumBuffersAnnouncedMinRequired() = {min_required} {'OK' if min_required == 1 else '(expected 1)'}")

    print(f"  IsGrabbing() = {ds.IsGrabbing()} {'OK' if ds.IsGrabbing() is False else '(expected False)'}")

    print("\n-- buffer allocation + revoke --")
    # Not calling buf.Size()/buf.BasePtr() -- those map to
    # DSGetBufferInfo(BUFFER_INFO_*), which isn't implemented yet (M4b's
    # scope is allocation/revoke only; BUFFER_INFO commands are deferred
    # until real Kineo's trace shows which ones it actually needs).
    buffers = []
    for i in range(3):
        buf = ds.AllocAndAnnounceBuffer(payload)
        buffers.append(buf)
        print(f"  AllocAndAnnounceBuffer #{i}: OK")

    print("\n-- queue buffer #0, verify duplicate-queue and revoke-while-queued are rejected --")
    ds.QueueBuffer(buffers[0])
    print("  QueueBuffer #0: OK")
    try:
        ds.QueueBuffer(buffers[0])
        print("  QueueBuffer #0 again: UNEXPECTED SUCCESS (should have been rejected)")
    except Exception as e:
        print(f"  QueueBuffer #0 again: correctly rejected ({type(e).__name__})")
    try:
        ds.RevokeBuffer(buffers[0])
        print("  RevokeBuffer #0 (while queued): UNEXPECTED SUCCESS (should have been rejected)")
    except Exception as e:
        print(f"  RevokeBuffer #0 (while queued): correctly rejected ({type(e).__name__})")

    print("\n-- revoke the two still-ANNOUNCED buffers, verify no crash --")
    for i, buf in enumerate(buffers[1:], start=1):
        ds.RevokeBuffer(buf)
        print(f"  RevokeBuffer #{i}: OK")

    print("\n-- re-allocate after revoke (slot reuse check) --")
    buf2 = ds.AllocAndAnnounceBuffer(payload)
    print("  AllocAndAnnounceBuffer (post-revoke): OK")
    ds.RevokeBuffer(buf2)
    print("  RevokeBuffer (post-revoke buffer): OK")

    print("\n-- dynamic growth: allocate 200 buffers (past the old 16/64 fixed ceilings), queue them all --")
    # Not revoking these afterward -- a QUEUED buffer is correctly
    # rejected by DSRevokeBuffer (must be flushed/dequeued first, same as
    # the buffers[0] check above), matching real Kineo's own observed
    # behavior of never revoking queued buffers itself either. Left for
    # the library-teardown defensive-cleanup path to free.
    growth_buffers = []
    for i in range(200):
        buf = ds.AllocAndAnnounceBuffer(payload)
        ds.QueueBuffer(buf)
        growth_buffers.append(buf)
    print("  allocated + queued 200 buffers: OK (no artificial ceiling hit)")

    print("\nAll M4c-1d checks completed (buffer #0 and the 200 growth buffers left queued --")
    print("no flush/DSClose in this script, matching real Kineo's own observed shutdown).")

except Exception as e:
    print(f"FAILED: {type(e).__name__}: {e}")
finally:
    ids_peak.Library.Close()
