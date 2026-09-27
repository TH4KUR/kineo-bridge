#!/usr/bin/env python3
"""transport_stress_test.py -- transport isolation stress test (no GenTL
CTI, no Kineo, no live control writes). Sustained async Aravis streaming
at a single fixed rate for a meaningful interval, reporting bounded
application-level metrics only (per instructions: no verbose USB/IP
kernel debugging during this -- it would hurt bulk-transfer performance
and skew the very thing being measured).

Run once per rate (10/20/30/60 fps), fresh camera open each run (matches
how the bridge itself opens fresh per bridge-connection), to build a
duration/throughput/failure profile independent of any Kineo/CTI timing
concerns.
"""
import argparse
import gc
import sys
import time

import gi
gi.require_version("Aravis", "0.8")
from gi.repository import Aravis  # noqa: E402

EXPECTED_SERIAL = "4110010861"
EXPECTED_PAYLOAD = 2304000
WIDTH, HEIGHT = 1920, 1200


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fps", type=float, required=True)
    ap.add_argument("--duration-s", type=float, default=90.0)
    ap.add_argument("--num-buffers", type=int, default=10)
    ap.add_argument("--report-every-s", type=float, default=10.0)
    args = ap.parse_args()

    Aravis.update_device_list()
    target_id = None
    for i in range(Aravis.get_n_devices()):
        if Aravis.get_device_serial_nbr(i) == EXPECTED_SERIAL:
            target_id = Aravis.get_device_id(i)
    if not target_id:
        print(f"FAILED: no device with serial {EXPECTED_SERIAL} found")
        return 1

    cam = Aravis.Camera.new(target_id)
    if cam.get_device_serial_number() != EXPECTED_SERIAL:
        print("FAILED: wrong camera")
        return 1
    print(f"Opened: {cam.get_vendor_name()} {cam.get_model_name()}")

    cam.set_pixel_format_from_string("Mono8")
    cam.set_region(0, 0, WIDTH, HEIGHT)
    payload = cam.get_payload()
    if payload != EXPECTED_PAYLOAD:
        print(f"FAILED: unexpected payload {payload}")
        return 1

    lo, hi = cam.get_frame_rate_bounds() if cam.is_frame_rate_available() else (0, 999)
    applied_fps = min(max(args.fps, lo), hi)
    if cam.is_frame_rate_available():
        cam.set_frame_rate(applied_fps)
    cam.set_acquisition_mode(Aravis.AcquisitionMode.CONTINUOUS)
    print(f"requested_fps={args.fps} applied_fps={applied_fps} "
          f"expected_bandwidth={applied_fps * payload / 1e6:.2f} MB/s")
    print(f"Running sustained stream for {args.duration_s:.0f}s -- NO control writes during this run.\n")

    stream = cam.create_stream(None, None)
    if stream is None:
        print("FAILED: create_stream returned None")
        return 1
    for _ in range(args.num_buffers):
        stream.push_buffer(Aravis.Buffer.new_allocate(payload))

    cam.start_acquisition()
    t_start = time.monotonic()
    last_report = t_start
    good = 0
    bad = 0
    timeouts = 0
    bytes_total = 0
    last_good_ts = None
    max_gap_s = 0.0

    while time.monotonic() - t_start < args.duration_s:
        buf = stream.timeout_pop_buffer(1_000_000)  # 1s
        now = time.monotonic()
        if buf is None:
            timeouts += 1
        else:
            status = buf.get_status()
            data = buf.get_data() if status == Aravis.BufferStatus.SUCCESS else b""
            ok = (status == Aravis.BufferStatus.SUCCESS and len(data) == EXPECTED_PAYLOAD)
            if ok:
                good += 1
                bytes_total += len(data)
                if last_good_ts is not None:
                    max_gap_s = max(max_gap_s, now - last_good_ts)
                last_good_ts = now
            else:
                bad += 1
            stream.push_buffer(buf)

        if now - last_report >= args.report_every_s:
            elapsed = now - t_start
            fps_actual = good / elapsed if elapsed > 0 else 0
            bw = bytes_total / elapsed / 1e6 if elapsed > 0 else 0
            print(f"  t={elapsed:6.1f}s good={good:6d} bad={bad:4d} timeouts={timeouts:4d} "
                  f"actual_fps={fps_actual:5.2f} bandwidth={bw:6.2f}MB/s")
            last_report = now

    cam.stop_acquisition()
    elapsed = time.monotonic() - t_start
    print(f"\n=== fps={applied_fps} duration={elapsed:.1f}s ===")
    print(f"  good frames:    {good}")
    print(f"  bad frames:     {bad}")
    print(f"  timeouts:       {timeouts}")
    print(f"  achieved fps:   {good / elapsed:.2f}")
    print(f"  achieved BW:    {bytes_total / elapsed / 1e6:.2f} MB/s")
    print(f"  max inter-frame gap: {max_gap_s:.2f}s")

    # Responsiveness check -- try a fresh enumeration + a quick re-open,
    # matching what the bridge does on the NEXT connection. Explicitly
    # release every reference to the stream/camera FIRST (a stale Python
    # reference holding the USB interface claim would falsely look like a
    # camera-side wedge otherwise -- LIBUSB_ERROR_BUSY, not a real
    # hardware failure).
    stream = None
    cam = None
    gc.collect()
    time.sleep(1.0)
    Aravis.update_device_list()
    still_enumerable = any(Aravis.get_device_serial_nbr(i) == EXPECTED_SERIAL
                            for i in range(Aravis.get_n_devices()))
    print(f"  camera still enumerable after run: {still_enumerable}")
    reopen_ok = False
    if still_enumerable:
        try:
            cam2 = Aravis.Camera.new(target_id)
            _ = cam2.get_payload()
            reopen_ok = True
            cam2 = None
            gc.collect()
        except Exception as e:
            print(f"  reopen FAILED: {e}")
    print(f"  camera reopenable after run: {reopen_ok}")

    return 0 if (still_enumerable and reopen_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
