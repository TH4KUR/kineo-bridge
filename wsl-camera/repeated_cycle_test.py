#!/usr/bin/env python3
"""repeated_cycle_test.py -- validates the new bridge lifecycle (Step 3-6
of the lifecycle-hardening directive) at the protocol level, before
touching Kineo: one connection, OPEN+CONFIGURE once, then repeated
START -> capture a few frames -> STOP cycles with varying idle gaps,
exactly mirroring what the hardened CTI now does (pre-roll at
DevOpenDataStream = START, release at AcquisitionStop = STOP).
"""
import argparse
import socket
import sys
import time

import protocol as proto


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9494)
    ap.add_argument("--serial", default="4110010861")
    ap.add_argument("--cycles", type=int, default=10)
    ap.add_argument("--frames-per-cycle", type=int, default=5)
    args = ap.parse_args()

    sock = socket.create_connection((args.host, args.port), timeout=5.0)
    sock.settimeout(10.0)
    print(f"Connected to {args.host}:{args.port}")

    proto.send_json(sock, proto.MSG_HELLO, {})
    _, resp = proto.recv_json(sock)
    print(f"HELLO -> {resp}")

    proto.send_json(sock, proto.MSG_OPEN, {"serial": args.serial})
    _, resp = proto.recv_json(sock)
    print(f"OPEN -> {resp}")
    if not resp.get("ok"):
        print("FAILED: OPEN did not succeed")
        return 1

    proto.send_json(sock, proto.MSG_CONFIGURE, {
        "exposure_time_us": 1422.267, "gain": 1.0, "black_level": 1.75,
        "frame_rate": 60.0,  # deliberately request 60 like Kineo does -- the cap is bridge-side
    })
    _, resp = proto.recv_json(sock)
    print(f"CONFIGURE -> {resp}\n")

    idle_gaps = [5, 30, 60]
    total_bad = 0
    total_timeouts = 0
    failures = 0

    for cycle in range(1, args.cycles + 1):
        idle_s = idle_gaps[(cycle - 1) % len(idle_gaps)]
        t0 = time.monotonic()
        proto.send_json(sock, proto.MSG_START, {})
        _, resp = proto.recv_json(sock)
        start_latency_ms = (time.monotonic() - t0) * 1000.0
        if not resp.get("ok"):
            print(f"cycle {cycle:2d}: START FAILED -> {resp}")
            failures += 1
            continue

        received = 0
        bad = 0
        first_frame_ms = None
        for i in range(args.frames_per_cycle):
            try:
                msg_type, payload = proto.recv_message(sock)
            except socket.timeout:
                print(f"cycle {cycle:2d}: TIMEOUT waiting for frame {i}")
                bad += 1
                continue
            if msg_type != proto.MSG_FRAME:
                continue
            if first_frame_ms is None:
                first_frame_ms = (time.monotonic() - t0) * 1000.0
            fh = proto.unpack_frame_header(payload[:proto.FRAME_HEADER_SIZE])
            data = payload[proto.FRAME_HEADER_SIZE:]
            if len(data) == fh["data_len"] == 2304000 and fh["status"] == 0:
                received += 1
            else:
                bad += 1

        proto.send_json(sock, proto.MSG_STOP, {})
        # Drain any stray FRAME messages that arrive before the STOP ack
        # (same benign race test_client.py already handles).
        while True:
            msg_type, payload = proto.recv_message(sock)
            if msg_type == proto.MSG_STOP:
                stop_resp = proto.json.loads(payload.decode("utf-8"))
                break
        print(f"cycle {cycle:2d}: start_latency={start_latency_ms:6.1f}ms "
              f"first_frame={first_frame_ms if first_frame_ms is None else f'{first_frame_ms:.1f}ms':>9} "
              f"frames_ok={received} bad={bad} idle_next={idle_s}s STOP->{stop_resp}")
        total_bad += bad
        total_timeouts += stop_resp.get("frame_timeouts", 0)
        if received == 0:
            failures += 1

        time.sleep(idle_s)

    proto.send_json(sock, proto.MSG_CLOSE, {})
    _, resp = proto.recv_json(sock)
    print(f"\nCLOSE -> {resp}")
    sock.close()

    print(f"\n=== {args.cycles - failures}/{args.cycles} cycles succeeded, "
          f"total_bad_frames_this_test={total_bad} ===")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
