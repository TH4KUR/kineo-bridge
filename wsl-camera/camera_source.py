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
import os
import threading
import time

WIDTH = 1920
HEIGHT = 1200
PAYLOAD_SIZE = WIDTH * HEIGHT  # Mono8, 1 byte/px
PFNC_MONO8 = 0x01080001
EXPECTED_SERIAL = "4110010861"

# Lifecycle-hardening Step 2: temporary physical FPS cap. Kineo requests
# 60fps; sustained continuous streaming at that rate over usbipd/WSL2 was
# found to destabilize the USB3Vision link after several minutes (see
# investigation/hardening.md). Clamp the REAL camera's physical rate
# independently of whatever Kineo asked for -- the GenApi-facing node
# Kineo sees is untouched, only the real hardware setting is capped.
# Raise progressively (10 -> 15 -> 20 -> 30 -> 60) only after each rate is
# proven stable across repeated real analyses; never silently claim the
# physical camera is running at the requested rate when it isn't.
PHYSICAL_FPS_CAP = float(os.environ.get("KINEO_BRIDGE_MAX_FPS", "10.0"))


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
        # Every Aravis/libusb call below can raise its own native
        # GLib.GError (not our CameraError) on a real, sometimes
        # transient, USB-level failure (observed: "Failed to bootstrap
        # USB device"). That exception previously propagated all the way
        # up through the server's main loop and killed the entire bridge
        # PROCESS -- not just this one OPEN attempt -- silently taking
        # down every subsequent connection for the rest of the Kineo
        # session (confirmed: this is what actually caused two real
        # "GrabFrame failed" analyses, 42 minutes apart, with nothing
        # listening on the bridge port in between). Converting every
        # exception here into CameraError keeps the server process alive
        # no matter what the camera does -- a failed OPEN is reported
        # back to this one client/CTI connector thread, which already
        # retries with its own backoff.
        try:
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
        except CameraError:
            self._cam = None
            raise
        except Exception as e:
            self._cam = None
            raise CameraError(f"camera open failed: {e}") from e

    def configure(self, exposure_time_us=None, gain=None, black_level=None,
                  frame_rate=None, width=WIDTH, height=HEIGHT):
        # Every individual setting below is applied independently -- a
        # single failing write (observed: a one-off transient
        # "access-denied" on PixelFormat immediately after a fresh camera
        # open, root cause not yet identified) must never silently skip
        # every setting after it in program order. Each failure is caught,
        # logged into `applied` as its own `<name>_error` key, and
        # execution continues -- callers get a clear picture of exactly
        # what did and didn't apply, never a partial silent success.
        cam = self._cam
        applied = {}

        def attempt(name, fn):
            try:
                fn()
            except Exception as e:
                applied[f"{name}_error"] = str(e)

        # Skip a redundant PixelFormat write if it's already correct --
        # narrows the window for whatever caused the transient failure,
        # and avoids poking a register that doesn't need touching.
        if cam.get_pixel_format_as_string() != "Mono8":
            attempt("pixel_format", lambda: cam.set_pixel_format_from_string("Mono8"))
        attempt("region", lambda: cam.set_region(0, 0, width, height))

        if exposure_time_us is not None:
            def do_exposure():
                lo, hi = cam.get_exposure_time_bounds()
                v = min(max(exposure_time_us, lo), hi)
                cam.set_exposure_time(v)
                applied["exposure_time_us"] = v
                if v != exposure_time_us:
                    applied["exposure_time_clamped_from"] = exposure_time_us
            attempt("exposure_time", do_exposure)
        if gain is not None:
            def do_gain():
                lo, hi = cam.get_gain_bounds()
                v = min(max(gain, lo), hi)
                cam.set_gain(v)
                applied["gain"] = v
                if v != gain:
                    applied["gain_clamped_from"] = gain
            attempt("gain", do_gain)
        if black_level is not None and cam.is_black_level_available():
            def do_black_level():
                lo, hi = cam.get_black_level_bounds()
                v = min(max(black_level, lo), hi)
                cam.set_black_level(v)
                applied["black_level"] = v
                if v != black_level:
                    applied["black_level_clamped_from"] = black_level
            attempt("black_level", do_black_level)
        if frame_rate is not None and cam.is_frame_rate_available():
            def do_frame_rate():
                requested = frame_rate
                capped = min(frame_rate, PHYSICAL_FPS_CAP)
                lo, hi = cam.get_frame_rate_bounds()
                v = min(max(capped, lo), hi)
                cam.set_frame_rate(v)
                applied["frame_rate"] = v
                applied["frame_rate_requested"] = requested
                if v != requested:
                    applied["frame_rate_clamped_from"] = requested
                    if capped != requested:
                        applied["frame_rate_cap_applied"] = PHYSICAL_FPS_CAP
            attempt("frame_rate", do_frame_rate)
        attempt("acquisition_mode", lambda: cam.set_acquisition_mode(self._Aravis.AcquisitionMode.CONTINUOUS))
        applied["payload"] = cam.get_payload()

        # Verify what's ACTUALLY on the camera right now, regardless of
        # what we just attempted -- catches exactly the failure mode that
        # caused the blank-video bug (a write silently not taking effect).
        applied["readback_pixel_format"] = cam.get_pixel_format_as_string()
        applied["readback_exposure_time_us"] = cam.get_exposure_time()
        applied["readback_gain"] = cam.get_gain()
        if cam.is_black_level_available():
            applied["readback_black_level"] = cam.get_black_level()
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
        # Lifecycle-hardening Step 5: stop AND release the stream, not
        # just stop_acquisition -- a fresh stream is created by start()
        # next time regardless, so holding onto this one buys nothing and
        # keeping the camera's whole existing-state around between
        # analyses was the multi-minute continuous-streaming design that
        # proved unstable. The camera object itself (self._cam) stays
        # open -- only the stream/acquisition state is torn down here.
        if self._cam is not None and self._acquiring:
            self._cam.stop_acquisition()
        self._acquiring = False
        self._stream = None

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
