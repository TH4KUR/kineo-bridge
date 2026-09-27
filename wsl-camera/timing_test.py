#!/usr/bin/env python3
"""timing_test.py -- isolated WSL/Aravis timing test (Step 1 of the
lifecycle-hardening directive). No Kineo, no bridge -- pure Aravis.

Opens and configures the real camera ONCE, then for N cycles: creates a
fresh stream, starts acquisition, measures time to the first valid
2304000-byte frame, captures a few more, stops acquisition, destroys the
stream, waits briefly, repeats. Camera object itself stays open/idle
between cycles the whole time (this measures the "camera stays open,
only stream/acquisition cycles" architecture -- the question is whether
that's fast and stable enough to skip full camera close/reopen).

Reports min/median/p95/max of AcquisitionStart-to-first-frame latency,
and bad/timeout counts, so this can be compared directly against Kineo's
observed ~150ms EventGetData timeout.
"""
import argparse
import statistics
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
    ap.add_argument("--cycles", type=int, default=20)
    ap.add_argument("--fps", type=float, default=10.0)
    ap.add_argument("--frames-per-cycle", type=int, default=3)
    ap.add_argument("--idle-s", type=float, default=1.0, help="wait between cycles")
    ap.add_argument("--num-buffers", type=int, default=10)
    args = ap.parse_args()

    Aravis.update_device_list()
    target_id = None
    for i in range(Aravis.get_n_devices()):
        if Aravis.get_device_serial_nbr(i) == EXPECTED_SERIAL:
            target_id = Aravis.get_device_id(i)
    if not target_id:
        print(f"FAILED: no device with serial {EXPECTED_SERIAL} found")
        return 1

    print(f"Opening camera once (serial={EXPECTED_SERIAL})...")
    cam = Aravis.Camera.new(target_id)
    actual = cam.get_device_serial_number()
    if actual != EXPECTED_SERIAL:
        print(f"FAILED: opened wrong camera (serial={actual!r})")
        return 1
    print(f"Opened: {cam.get_vendor_name()} {cam.get_model_name()} serial={actual}")

    cam.set_pixel_format_from_string("Mono8")
    cam.set_region(0, 0, WIDTH, HEIGHT)
    payload = cam.get_payload()
    print(f"payload={payload} (expected {EXPECTED_PAYLOAD})")
    if payload != EXPECTED_PAYLOAD:
        print("FAILED: unexpected payload size")
        return 1

    requested_fps = args.fps
    applied_fps = requested_fps
    if cam.is_frame_rate_available():
        lo, hi = cam.get_frame_rate_bounds()
        applied_fps = min(max(requested_fps, lo), hi)
        cam.set_frame_rate(applied_fps)
        readback = cam.get_frame_rate()
        print(f"requested physical FPS={requested_fps} applied={applied_fps} readback={readback}")
        if applied_fps != requested_fps:
            print(f"NOTE: requested FPS clamped to camera bounds [{lo}, {hi}]")
    cam.set_acquisition_mode(Aravis.AcquisitionMode.CONTINUOUS)
    print("Camera configured. Camera object stays open/idle for the whole test.\n")

    first_frame_latencies_ms = []
    bad_cycles = 0
    total_bad_frames = 0
    total_timeouts = 0

    for cycle in range(1, args.cycles + 1):
        stream = cam.create_stream(None, None)
        if stream is None:
            print(f"cycle {cycle}: FAILED create_stream returned None")
            bad_cycles += 1
            continue
        for _ in range(args.num_buffers):
            stream.push_buffer(Aravis.Buffer.new_allocate(payload))

        t0 = time.monotonic()
        cam.start_acquisition()

        first_latency_ms = None
        got_first = False
        frames_this_cycle = 0
        cycle_bad = 0
        cycle_timeouts = 0
        # Generous 3s timeout for the first frame specifically (we're
        # measuring it, not enforcing Kineo's budget here), then normal
        # frame pacing for the rest of this cycle's requested frames.
        deadline = time.monotonic() + 3.0
        while frames_this_cycle < args.frames_per_cycle and time.monotonic() < deadline:
            buf = stream.timeout_pop_buffer(500_000)  # 500ms, microseconds
            if buf is None:
                cycle_timeouts += 1
                continue
            status = buf.get_status()
            size = len(buf.get_data()) if status == Aravis.BufferStatus.SUCCESS else 0
            ok = (status == Aravis.BufferStatus.SUCCESS and size == EXPECTED_PAYLOAD)
            if not got_first:
                first_latency_ms = (time.monotonic() - t0) * 1000.0
                got_first = True
            if ok:
                frames_this_cycle += 1
            else:
                cycle_bad += 1
            stream.push_buffer(buf)

        cam.stop_acquisition()
        stream = None  # release -- new stream created fresh next cycle

        total_bad_frames += cycle_bad
        total_timeouts += cycle_timeouts
        if first_latency_ms is None:
            print(f"cycle {cycle:2d}: FAILED -- no frame arrived at all within 3s")
            bad_cycles += 1
        else:
            first_frame_latencies_ms.append(first_latency_ms)
            flag = " <-- OVER 150ms BUDGET" if first_latency_ms > 150 else ""
            print(f"cycle {cycle:2d}: first-frame latency={first_latency_ms:7.1f}ms "
                  f"frames_ok={frames_this_cycle} bad={cycle_bad} timeouts={cycle_timeouts}{flag}")

        time.sleep(args.idle_s)

    print()
    if first_frame_latencies_ms:
        lat = sorted(first_frame_latencies_ms)
        n = len(lat)
        p95_idx = min(n - 1, int(round(0.95 * (n - 1))))
        print(f"=== first-frame latency over {n} successful cycles ({bad_cycles} failed cycles) ===")
        print(f"  min:    {lat[0]:.1f} ms")
        print(f"  median: {statistics.median(lat):.1f} ms")
        print(f"  p95:    {lat[p95_idx]:.1f} ms")
        print(f"  max:    {lat[-1]:.1f} ms")
        print(f"  physical FPS applied: {applied_fps}")
        print(f"  total bad frames: {total_bad_frames}, total timeouts: {total_timeouts}")
        over_budget = sum(1 for x in lat if x > 150)
        print(f"  cycles over Kineo's ~150ms EventGetData budget: {over_budget}/{n}")
    else:
        print("FAILED: no successful cycles at all")
        return 1

    return 0 if bad_cycles == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
