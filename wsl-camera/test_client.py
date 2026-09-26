#!/usr/bin/env python3
"""test_client.py -- minimal test client for kineo_camera_bridge.py.

Purpose: isolate camera/bridge issues from GenTL/CTI issues by proving
the bridge works completely on its own first. Not polished -- just
enough to demonstrate correct serial, resolution, pixel format, exact
payload size, multiple frames, monotonically increasing IDs, and clean
START/STOP, at approximately 10 FPS.
"""
import argparse
import json
import socket
import sys
import time

import protocol as proto


def recv_json_skip_frames(sock, expected_msg_type):
    """Read messages until the expected control reply arrives, discarding
    any stray FRAME messages that the streaming thread may still emit
    between the client issuing STOP and the server noticing it -- a real
    GenTL consumer must tolerate exactly this kind of late buffer too."""
    skipped = 0
    while True:
        msg_type, payload = proto.recv_message(sock)
        if msg_type == expected_msg_type:
            obj = json.loads(payload.decode("utf-8")) if payload else {}
            if skipped:
                obj["_late_frames_skipped"] = skipped
            return obj
        if msg_type == proto.MSG_FRAME:
            skipped += 1
            continue
        raise RuntimeError(f"unexpected msg_type={msg_type} while waiting for {expected_msg_type}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9494)
    ap.add_argument("--serial", default="4110010861")
    ap.add_argument("--frames", type=int, default=20)
    ap.add_argument("--frame-rate", type=float, default=10.0)
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
    if resp.get("serial") not in (args.serial, "SYNTHETIC"):
        print(f"FAILED: wrong serial {resp.get('serial')!r}, expected {args.serial!r}")
        return 1

    proto.send_json(sock, proto.MSG_CONFIGURE, {
        "exposure_time_us": 1422.267, "gain": 1.0, "black_level": 1.75,
        "frame_rate": args.frame_rate,
    })
    _, resp = proto.recv_json(sock)
    print(f"CONFIGURE -> {resp}")

    proto.send_json(sock, proto.MSG_START, {})
    _, resp = proto.recv_json(sock)
    print(f"START -> {resp}")
    if not resp.get("ok"):
        print("FAILED: START did not succeed")
        return 1

    received = 0
    bad_size = 0
    last_frame_id = None
    monotonic_ok = True
    t0 = time.monotonic()
    for i in range(args.frames):
        msg_type, payload = proto.recv_message(sock)
        if msg_type != proto.MSG_FRAME:
            print(f"  unexpected msg_type={msg_type} while expecting FRAME")
            continue
        fh = proto.unpack_frame_header(payload[:proto.FRAME_HEADER_SIZE])
        data = payload[proto.FRAME_HEADER_SIZE:]
        ok_size = (len(data) == fh["data_len"] == 2304000)
        if not ok_size:
            bad_size += 1
        if last_frame_id is not None and fh["frame_id"] <= last_frame_id:
            monotonic_ok = False
        last_frame_id = fh["frame_id"]
        if i < 10:
            print(f"  frame {i}: id={fh['frame_id']} {fh['width']}x{fh['height']} "
                  f"len={len(data)} status={fh['status']} {'OK' if ok_size else 'BAD SIZE'}")
        received += 1
    elapsed = time.monotonic() - t0
    fps = received / elapsed if elapsed > 0 else 0

    proto.send_json(sock, proto.MSG_STOP, {})
    resp = recv_json_skip_frames(sock, proto.MSG_STOP)
    print(f"STOP -> {resp}")

    proto.send_json(sock, proto.MSG_CLOSE, {})
    resp = recv_json_skip_frames(sock, proto.MSG_CLOSE)
    print(f"CLOSE -> {resp}")

    print(f"\nReceived {received} frames ({bad_size} bad-size) in {elapsed:.2f}s "
          f"(~{fps:.1f} fps), monotonic_ids={'OK' if monotonic_ok else 'FAILED'}, "
          f"last_frame_id={last_frame_id}")
    sock.close()
    return 0 if (received > 0 and bad_size == 0 and monotonic_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
