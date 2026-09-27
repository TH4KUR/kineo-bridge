"""bridge_status.py -- shared, thread-safe health status for the bridge
server, periodically dumped to a JSON file so diagnose.sh (and anything
else) can read it directly without opening a TCP connection -- the bridge
serves exactly one client at a time, so a second network connection just
to ask "are you healthy?" would queue behind whatever Kineo is doing.

Not part of the wire protocol; this is purely a local diagnostic side
channel. Never read by kineo_camera_bridge.py itself.
"""
import json
import os
import threading
import time

STATUS_DIR = os.path.dirname(os.path.abspath(__file__))
WRITE_INTERVAL_S = 1.0


def status_path_for_port(port):
    return os.path.join(STATUS_DIR, f"bridge_status.{port}.json")


# Default path (port 9494, the standard bridge port) -- what diagnose.sh
# looks for unless told otherwise.
STATUS_PATH = status_path_for_port(9494)


class BridgeStatus:
    def __init__(self, source_type, port=9494):
        self.path = status_path_for_port(port)
        self._lock = threading.Lock()
        self._data = {
            "pid": os.getpid(),
            "source_type": source_type,
            "started_at": time.time(),
            "camera_connected": False,
            "serial": None,
            "streaming": False,
            "width": None,
            "height": None,
            "pixel_format": None,
            "frames_captured": 0,     # produced by the source, including ones dropped before send
            "frames_transmitted": 0,  # actually sent to the client over the wire
            "frames_bad": 0,
            "frame_timeouts": 0,
            "last_frame_ts": None,    # time.time() of the last good frame
            "last_error": None,
        }
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._writer_loop, daemon=True)
        self._thread.start()

    def update(self, **kwargs):
        with self._lock:
            self._data.update(kwargs)

    def incr(self, field, by=1):
        with self._lock:
            self._data[field] = self._data.get(field, 0) + by

    def snapshot(self):
        with self._lock:
            d = dict(self._data)
        d["now"] = time.time()
        d["last_frame_age_ms"] = (
            int((d["now"] - d["last_frame_ts"]) * 1000) if d["last_frame_ts"] else None
        )
        return d

    def _writer_loop(self):
        tmp_path = self.path + ".tmp"
        while not self._stop.is_set():
            try:
                snap = self.snapshot()
                with open(tmp_path, "w") as f:
                    json.dump(snap, f)
                os.replace(tmp_path, self.path)  # atomic on POSIX
            except Exception:
                pass  # diagnostics must never crash the bridge
            self._stop.wait(WRITE_INTERVAL_S)

    def close(self):
        self._stop.set()
        try:
            os.remove(self.path)
        except OSError:
            pass


def read_status(port=9494, max_age_s=5.0):
    """For external readers (diagnose.sh via a tiny python helper). Returns
    the parsed dict, or None if the file is missing/unreadable/stale
    (stale = the writer thread hasn't updated it recently -> bridge is
    probably dead even though the file is still on disk)."""
    try:
        with open(status_path_for_port(port)) as f:
            d = json.load(f)
    except Exception:
        return None
    if time.time() - d.get("now", 0) > max_age_s:
        return None
    return d
