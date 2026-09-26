#!/usr/bin/env python3
"""kineo_camera_bridge.py -- WSL-side camera bridge server.

Owns the physical IDS U3-3560XCP-M (via Aravis 0.8.36, async streaming)
or a synthetic frame source, and serves frames to a Windows-side TCP
client (our GenTL CTI) using the protocol in protocol.py.

One client at a time. Simple, explicit control-plane + frame-plane over
a single duplex TCP connection, per project instructions.
"""
import argparse
import socket
import sys
import threading
import time

import protocol as proto
from camera_source import RealCameraSource, SyntheticSource, CameraError, PAYLOAD_SIZE

LOG_DETAIL_LIMIT = 10


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


class ClientSession:
    def __init__(self, conn, addr, source_factory, default_serial):
        self.conn = conn
        self.addr = addr
        self.source = source_factory()
        self.default_serial = default_serial
        self.send_lock = threading.Lock()
        self.stream_thread = None
        self.stop_flag = threading.Event()
        self.opened = False
        self.streaming = False
        # counters
        self.frames_sent = 0
        self.frames_bad = 0
        self.frame_timeouts = 0
        self.detail_logged = 0

    def reply(self, msg_type, obj):
        proto.send_json(self.conn, msg_type, obj, lock=self.send_lock)

    def handle_hello(self, payload):
        self.reply(proto.MSG_HELLO, {"ok": True, "version": proto.VERSION,
                                      "source": type(self.source).__name__})

    def handle_open(self, payload):
        serial = payload.get("serial", self.default_serial)
        try:
            info = self.source.open(serial)
            self.opened = True
            log(f"OPEN OK: {info}")
            self.reply(proto.MSG_OPEN, {"ok": True, **info})
        except CameraError as e:
            log(f"OPEN FAILED: {e}")
            self.reply(proto.MSG_OPEN, {"ok": False, "error": str(e)})

    def handle_configure(self, payload):
        if not self.opened:
            self.reply(proto.MSG_CONFIGURE, {"ok": False, "error": "not opened"})
            return
        try:
            applied = self.source.configure(
                exposure_time_us=payload.get("exposure_time_us"),
                gain=payload.get("gain"),
                black_level=payload.get("black_level"),
                frame_rate=payload.get("frame_rate"),
            )
            log(f"CONFIGURE applied: {applied}")
            self.reply(proto.MSG_CONFIGURE, {"ok": True, "applied": applied})
        except Exception as e:
            log(f"CONFIGURE FAILED: {e}")
            self.reply(proto.MSG_CONFIGURE, {"ok": False, "error": str(e)})

    def handle_start(self, payload):
        if not self.opened:
            self.reply(proto.MSG_START, {"ok": False, "error": "not opened"})
            return
        if self.streaming:
            # idempotent, matches the GenTL side's own duplicate-start tolerance
            self.reply(proto.MSG_START, {"ok": True, "note": "already streaming"})
            return
        try:
            self.source.start()
            self.stop_flag.clear()
            self.streaming = True
            # Send the ack BEFORE launching the streaming thread -- otherwise
            # the new thread can race ahead and send the first FRAME message
            # before the client has even seen the START acknowledgment.
            self.reply(proto.MSG_START, {"ok": True})
            self.stream_thread = threading.Thread(target=self._stream_loop, daemon=True)
            self.stream_thread.start()
            log("START OK, streaming thread launched")
        except Exception as e:
            log(f"START FAILED: {e}")
            self.reply(proto.MSG_START, {"ok": False, "error": str(e)})

    def handle_stop(self, payload):
        self._stop_streaming()
        self.reply(proto.MSG_STOP, {"ok": True, "frames_sent": self.frames_sent,
                                     "frames_bad": self.frames_bad, "frame_timeouts": self.frame_timeouts})

    def handle_close(self, payload):
        self._stop_streaming()
        try:
            self.source.close()
        except Exception:
            pass
        self.opened = False
        self.reply(proto.MSG_CLOSE, {"ok": True})

    def handle_status(self, payload):
        self.reply(proto.MSG_STATUS, {
            "ok": True, "opened": self.opened, "streaming": self.streaming,
            "frames_sent": self.frames_sent, "frames_bad": self.frames_bad,
            "frame_timeouts": self.frame_timeouts,
        })

    def _stop_streaming(self):
        if not self.streaming:
            return
        self.stop_flag.set()
        if self.stream_thread is not None:
            self.stream_thread.join(timeout=5.0)
            if self.stream_thread.is_alive():
                log("WARNING: stream thread did not exit within 5s")
        self.stream_thread = None
        try:
            self.source.stop()
        except Exception:
            pass
        self.streaming = False
        log(f"STOP: frames_sent={self.frames_sent} frames_bad={self.frames_bad} "
            f"frame_timeouts={self.frame_timeouts}")

    def _stream_loop(self):
        """Runs on its own thread; only ever writes to the socket
        (protected by send_lock), never reads -- safe to run concurrently
        with the main handler thread's recv loop for control messages."""
        while not self.stop_flag.is_set():
            try:
                frame = self.source.get_frame(timeout_s=0.5)
            except Exception as e:
                log(f"get_frame() raised: {e}")
                break
            if frame is None:
                self.frame_timeouts += 1
                continue
            data = frame["data"]
            if frame["status"] != 0 or len(data) != PAYLOAD_SIZE:
                self.frames_bad += 1
                if self.detail_logged < LOG_DETAIL_LIMIT:
                    log(f"  bad frame: frame_id={frame['frame_id']} len={len(data)} status={frame['status']}")
                    self.detail_logged += 1
                continue
            header = proto.pack_frame_header(
                frame["frame_id"], frame["width"], frame["height"],
                frame["pixel_format"], len(data), frame["timestamp_ns"], 0)
            try:
                proto.send_message(self.conn, proto.MSG_FRAME, header + data, lock=self.send_lock)
            except (BrokenPipeError, ConnectionError, OSError) as e:
                log(f"client disconnected during frame send: {e}")
                self.stop_flag.set()
                break
            self.frames_sent += 1
            if self.detail_logged < LOG_DETAIL_LIMIT:
                log(f"  frame sent: frame_id={frame['frame_id']} size={len(data)}")
                self.detail_logged += 1
            elif self.detail_logged == LOG_DETAIL_LIMIT:
                log("  further frames logged only in STOP summary")
                self.detail_logged += 1

    def run(self):
        log(f"client connected: {self.addr}")
        try:
            while True:
                msg_type, payload = proto.recv_json(self.conn)
                handler = {
                    proto.MSG_HELLO: self.handle_hello,
                    proto.MSG_OPEN: self.handle_open,
                    proto.MSG_CONFIGURE: self.handle_configure,
                    proto.MSG_START: self.handle_start,
                    proto.MSG_STOP: self.handle_stop,
                    proto.MSG_CLOSE: self.handle_close,
                    proto.MSG_STATUS: self.handle_status,
                }.get(msg_type)
                if handler is None:
                    log(f"unknown/unsupported msg_type={msg_type} from client, replying ERROR")
                    self.reply(proto.MSG_ERROR, {"ok": False, "error": f"unsupported msg_type {msg_type}"})
                    continue
                handler(payload)
                if msg_type == proto.MSG_CLOSE:
                    break
        except (ConnectionError, proto.ProtocolError) as e:
            log(f"client session ending: {e}")
        finally:
            self._stop_streaming()
            try:
                self.source.close()
            except Exception:
                pass
            try:
                self.conn.close()
            except Exception:
                pass
            log(f"client disconnected: {self.addr}")


def main():
    ap = argparse.ArgumentParser(description="Kineo WSL camera bridge server")
    ap.add_argument("--host", default="0.0.0.0", help="bind address (default 0.0.0.0, narrow via firewall/network mode as needed)")
    ap.add_argument("--port", type=int, default=9494)
    ap.add_argument("--source", choices=["camera", "synthetic"], default="camera")
    ap.add_argument("--serial", default="4110010861")
    args = ap.parse_args()

    source_factory = RealCameraSource if args.source == "camera" else SyntheticSource
    log(f"kineo_camera_bridge starting: host={args.host} port={args.port} source={args.source}")

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(1)
    log(f"listening on {args.host}:{args.port}")

    try:
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            session = ClientSession(conn, addr, source_factory, args.serial)
            session.run()  # one client at a time, per instructions
    except KeyboardInterrupt:
        log("interrupted, shutting down")
    finally:
        srv.close()


if __name__ == "__main__":
    sys.exit(main())
