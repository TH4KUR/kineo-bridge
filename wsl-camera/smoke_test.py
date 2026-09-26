#!/usr/bin/env python3
# smoke_test.py -- minimal async-streaming smoke test against the real
# IDS U3-3560XCP-M over usbipd/WSL, using the locally-built Aravis 0.8.36.
# Purpose: validate the camera can actually stream real frames through
# this USB/IP path before building the full bridge server around it.
import sys
import time

import gi
gi.require_version("Aravis", "0.8")
from gi.repository import Aravis  # noqa: E402

EXPECTED_SERIAL = "4110010861"
EXPECTED_PAYLOAD = 2304000
NUM_FRAMES = 20

def main():
    Aravis.update_device_list()
    n = Aravis.get_n_devices()
    print(f"devices found: {n}")
    target_id = None
    for i in range(n):
        serial = Aravis.get_device_serial_nbr(i)
        dev_id = Aravis.get_device_id(i)
        print(f"  [{i}] id={dev_id!r} serial={serial!r}")
        if serial == EXPECTED_SERIAL:
            target_id = dev_id
    if not target_id:
        print(f"FAILED: no device with serial {EXPECTED_SERIAL} found")
        return 1

    cam = Aravis.Camera.new(target_id)
    actual_serial = cam.get_device_serial_number()
    if actual_serial != EXPECTED_SERIAL:
        print(f"FAILED: opened wrong camera (serial {actual_serial!r}, expected {EXPECTED_SERIAL!r})")
        return 1
    print(f"Opened: {cam.get_vendor_name()} {cam.get_model_name()} serial={actual_serial}")

    cam.set_pixel_format_from_string("Mono8")
    cam.set_region(0, 0, 1920, 1200)
    payload = cam.get_payload()
    print(f"payload={payload} (expected {EXPECTED_PAYLOAD})")
    if payload != EXPECTED_PAYLOAD:
        print("FAILED: unexpected payload size")
        return 1

    cam.set_acquisition_mode(Aravis.AcquisitionMode.CONTINUOUS)
    if cam.is_frame_rate_available():
        cam.set_frame_rate(10.0)
        print(f"frame_rate set to 10.0, readback={cam.get_frame_rate()}")

    stream = cam.create_stream(None, None)
    if stream is None:
        print("FAILED: create_stream returned None")
        return 1

    NUM_BUFFERS = 10
    for _ in range(NUM_BUFFERS):
        stream.push_buffer(Aravis.Buffer.new_allocate(payload))
    print(f"pushed {NUM_BUFFERS} buffers, payload={payload} each")

    cam.start_acquisition()
    print("start_acquisition: OK")

    received = 0
    bad = 0
    last_frame_id = None
    t0 = time.monotonic()
    for i in range(NUM_FRAMES):
        buf = stream.timeout_pop_buffer(3_000_000)  # 3s timeout, microseconds
        if buf is None:
            print(f"  frame {i}: TIMEOUT (no buffer within 3s)")
            bad += 1
            continue
        status = buf.get_status()
        if status != Aravis.BufferStatus.SUCCESS:
            print(f"  frame {i}: bad status={status}")
            bad += 1
            stream.push_buffer(buf)
            continue
        data = buf.get_data()
        size = len(data)
        fid = buf.get_frame_id()
        w = buf.get_image_width()
        h = buf.get_image_height()
        ok = (size == EXPECTED_PAYLOAD and w == 1920 and h == 1200)
        if ok:
            received += 1
        else:
            bad += 1
        if i < 10:
            print(f"  frame {i}: frame_id={fid} size={size} {w}x{h} status={status} {'OK' if ok else 'MISMATCH'}")
        last_frame_id = fid
        stream.push_buffer(buf)  # recycle immediately

    elapsed = time.monotonic() - t0
    fps = received / elapsed if elapsed > 0 else 0
    print(f"\nReceived {received}/{NUM_FRAMES} good frames, {bad} bad, in {elapsed:.2f}s (~{fps:.1f} fps), last_frame_id={last_frame_id}")

    cam.stop_acquisition()
    print("stop_acquisition: OK")
    return 0 if received > 0 else 1

if __name__ == "__main__":
    sys.exit(main())
