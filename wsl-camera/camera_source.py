"""camera_source.py -- frame source abstraction for the WSL camera bridge.

Two implementations, selected by the bridge server's --source flag:
    RealCameraSource   -- the actual IDS U3-3560XCP-M via Aravis 0.8.36
    SyntheticSource    -- in-process generated pattern, no camera/USB
                          needed at all; isolates transport bugs from
                          camera bugs (regression tool), and lets the
                          bridge/protocol be developed and tested even
                          when the real camera isn't attached.

Both expose the same small interface: open(), configure(**kwargs),
start(), get_frame(timeout_s) -> dict|None, stop(), close().
"""
import threading
import time

WIDTH = 1920
HEIGHT = 1200
PAYLOAD_SIZE = WIDTH * HEIGHT  # Mono8, 1 byte/px
PFNC_MONO8 = 0x01080001
EXPECTED_SERIAL = "4110010861"


class CameraError(Exception):
    pass


class RealCameraSource:
    """Real IDS U3-3560XCP-M via Aravis 0.8.36, async streaming
    (create_stream + push_buffer + start_acquisition + pop_buffer loop --
    NOT the arv_camera_acquisition() synchronous convenience call, which
    was already established to behave badly over WSL USB/IP)."""

    def __init__(self):
        import gi
        gi.require_version("Aravis", "0.8")
        from gi.repository import Aravis
        self._Aravis = Aravis
        self._cam = None
        self._stream = None
        self._num_buffers = 10
        self._acquiring = False

    def open(self, serial=EXPECTED_SERIAL):
        Aravis = self._Aravis
        Aravis.update_device_list()
        n = Aravis.get_n_devices()
        target_id = None
        seen = []
        for i in range(n):
            dev_serial = Aravis.get_device_serial_nbr(i)
            seen.append(dev_serial)
            if dev_serial == serial:
                target_id = Aravis.get_device_id(i)
        if not target_id:
            raise CameraError(f"no device with serial {serial!r} found (saw: {seen})")
        self._cam = Aravis.Camera.new(target_id)
        actual = self._cam.get_device_serial_number()
        if actual != serial:
            raise CameraError(f"opened wrong camera: serial={actual!r}, expected {serial!r}")
        return {
            "vendor": self._cam.get_vendor_name(),
            "model": self._cam.get_model_name(),
            "serial": actual,
        }

    def configure(self, exposure_time_us=None, gain=None, black_level=None,
                  frame_rate=None, width=WIDTH, height=HEIGHT):
        cam = self._cam
        cam.set_pixel_format_from_string("Mono8")
        cam.set_region(0, 0, width, height)
        applied = {}
        # Apply what's requested, clamped to the real camera's own bounds
        # -- log clamps clearly rather than silently failing or pretending.
        if exposure_time_us is not None:
            lo, hi = cam.get_exposure_time_bounds()
            v = min(max(exposure_time_us, lo), hi)
            cam.set_exposure_time(v)
            applied["exposure_time_us"] = v
            if v != exposure_time_us:
                applied["exposure_time_clamped_from"] = exposure_time_us
        if gain is not None:
            lo, hi = cam.get_gain_bounds()
            v = min(max(gain, lo), hi)
            cam.set_gain(v)
            applied["gain"] = v
            if v != gain:
                applied["gain_clamped_from"] = gain
        if black_level is not None and cam.is_black_level_available():
            lo, hi = cam.get_black_level_bounds()
            v = min(max(black_level, lo), hi)
            cam.set_black_level(v)
            applied["black_level"] = v
            if v != black_level:
                applied["black_level_clamped_from"] = black_level
        if frame_rate is not None and cam.is_frame_rate_available():
            lo, hi = cam.get_frame_rate_bounds()
            v = min(max(frame_rate, lo), hi)
            cam.set_frame_rate(v)
            applied["frame_rate"] = v
            if v != frame_rate:
                applied["frame_rate_clamped_from"] = frame_rate
        cam.set_acquisition_mode(self._Aravis.AcquisitionMode.CONTINUOUS)
        applied["payload"] = cam.get_payload()
        return applied

    def start(self):
        if self._acquiring:
            return  # idempotent
        cam = self._cam
        payload = cam.get_payload()
        if payload != PAYLOAD_SIZE:
            raise CameraError(f"unexpected payload size {payload}, expected {PAYLOAD_SIZE}")
        self._stream = cam.create_stream(None, None)
        if self._stream is None:
            raise CameraError("create_stream returned None")
        for _ in range(self._num_buffers):
            self._stream.push_buffer(self._Aravis.Buffer.new_allocate(payload))
        cam.start_acquisition()
        self._acquiring = True

    def get_frame(self, timeout_s=1.0):
        """Returns a dict with frame_id/width/height/pixel_format/
        timestamp_ns/data (bytes)/status, or None on timeout. Recycles
        the buffer back to the stream immediately (per instructions --
        do not silently recycle GenTL-side buffers ourselves, but this
        is purely the internal Aravis buffer pool, unrelated to the
        Kineo-side buffer lifecycle the CTI owns)."""
        if not self._acquiring or self._stream is None:
            return None
        buf = self._stream.timeout_pop_buffer(int(timeout_s * 1_000_000))
        if buf is None:
            return None
        Aravis = self._Aravis
        status = buf.get_status()
        ok = (status == Aravis.BufferStatus.SUCCESS)
        data = bytes(buf.get_data()) if ok else b""
        result = {
            "frame_id": buf.get_frame_id(),
            "width": buf.get_image_width() if ok else 0,
            "height": buf.get_image_height() if ok else 0,
            "pixel_format": PFNC_MONO8,
            "timestamp_ns": buf.get_timestamp(),
            "data": data,
            "status": 0 if (ok and len(data) == PAYLOAD_SIZE) else 1,
        }
        self._stream.push_buffer(buf)  # recycle into Aravis's own pool
        return result

    def stop(self):
        if self._cam is not None and self._acquiring:
            self._cam.stop_acquisition()
        self._acquiring = False

    def close(self):
        self.stop()
        self._stream = None
        self._cam = None


class SyntheticSource:
    """In-process synthetic frame generator -- no camera/USB involved at
    all. Same moving-vertical-bright-bar pattern as the M4d CTI-side
    generator, so behavior is easy to reason about across both halves of
    the stack. Used to isolate transport/bridge bugs from real-camera
    bugs, and to develop/test the bridge without the camera attached."""

    def __init__(self):
        self._running = False
        self._frame_id = 0
        self._fps = 10.0
        self._lock = threading.Lock()
        self._last_emit = 0.0

    def open(self, serial=None):
        return {"vendor": "Synthetic", "model": "SyntheticSource", "serial": "SYNTHETIC"}

    def configure(self, exposure_time_us=None, gain=None, black_level=None,
                  frame_rate=None, width=WIDTH, height=HEIGHT):
        if frame_rate:
            self._fps = frame_rate
        return {"frame_rate": self._fps, "payload": PAYLOAD_SIZE, "note": "synthetic source, values not physically applied"}

    def start(self):
        with self._lock:
            self._running = True
            self._last_emit = 0.0

    def get_frame(self, timeout_s=1.0):
        if not self._running:
            return None
        interval = 1.0 / self._fps if self._fps > 0 else 0.1
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now - self._last_emit >= interval:
                break
            time.sleep(min(0.01, deadline - now))
        else:
            return None
        with self._lock:
            self._last_emit = time.monotonic()
            fid = self._frame_id
            self._frame_id += 1
        data = bytearray(PAYLOAD_SIZE)
        bar_width = 60
        pos = (fid * 12) % WIDTH
        row = bytearray(WIDTH)
        for x in range(WIDTH):
            row[x] = 255 if ((x - pos) % WIDTH) < bar_width else 32
        row = bytes(row)
        for y in range(HEIGHT):
            data[y * WIDTH:(y + 1) * WIDTH] = row
        return {
            "frame_id": fid, "width": WIDTH, "height": HEIGHT,
            "pixel_format": PFNC_MONO8, "timestamp_ns": time.monotonic_ns(),
            "data": bytes(data), "status": 0,
        }

    def stop(self):
        with self._lock:
            self._running = False

    def close(self):
        self.stop()
