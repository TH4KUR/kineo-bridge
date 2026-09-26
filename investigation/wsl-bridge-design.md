# Phase 6 — WSL Bridge Design

Windows-CTI <-> WSL-bridge IPC design and Aravis 0.8.36 API cheat-sheet, for the eventual
(not-yet-built) architecture:

```
Kineo UI -> real KineoDeviceService.exe -> real ids_peak.dll -> custom x64 GenTL .cti (Windows)
   -> local IPC -> WSL ARM64 bridge -> Aravis 0.8.36 -> real IDS USB camera
```

Design-phase output only — nothing here has been built or run against the live camera.

---

## 1. Aravis 0.8.36 API cheat-sheet (real function names, cited from source)

All confirmed by reading `~/aravis-0.8.36/src/{arvsystem,arvcamera,arvstream,arvbuffer,arvdevice,arvuvinterface,arvinterface}.{h,c}` directly — not from memory.

### 1.1 Discovery by serial number

Header: `arvsystem.h`. There is no `arv_camera_new_by_serial()`. The robust pattern is enumerate-and-match:

```c
void         arv_update_device_list (void);
unsigned int arv_get_n_devices (void);
const char * arv_get_device_id           (unsigned int index);
const char * arv_get_device_physical_id  (unsigned int index);
const char * arv_get_device_address      (unsigned int index);
const char * arv_get_device_vendor       (unsigned int index);
const char * arv_get_device_manufacturer_info (unsigned int index);
const char * arv_get_device_serial_nbr   (unsigned int index);
const char * arv_get_device_protocol     (unsigned int index);
```

Pattern:
```c
arv_update_device_list ();
for (i = 0; i < arv_get_n_devices (); i++) {
        if (g_strcmp0 (arv_get_device_serial_nbr (i), "4110010861") == 0) {
                device_id = arv_get_device_id (i);
                break;
        }
}
camera = arv_camera_new (device_id, &error);   /* device_id, NOT the bare serial */
```

Why not just pass the serial to `arv_camera_new()`: traced `arv_camera_new(name)` ->
`g_initable_new(ARV_TYPE_CAMERA, "name", name, …)` -> `arv_camera_constructed()` (arvcamera.c:4252) ->
`arv_open_device(priv->name, &error)` (arvcamera.c:4267) -> `arv_open_device()` (arvsystem.c:461) which
loops over interfaces calling `arv_interface_open_device(iface, device_id, error)`. For the USB3
Vision interface (`arvuvinterface.c`), the static `_open_device()` looks up a hash table
`uv_interface->priv->devices` keyed on **four aliases per device**, all inserted at discovery time
(`arvuvinterface.c:247-253`):
- `device_infos->id` — `"<manufacturer>-<guid>-<serial_nbr>"`
- `device_infos->name` — `"<vendor_alias>-<serial_nbr>"` (e.g. `IDS-4110010861`)
- `device_infos->full_name` — `"<manufacturer>-<serial_nbr>"`
- `device_infos->guid`

The bare serial number alone (`"4110010861"`) is **not** one of the keys, so `arv_camera_new("4110010861", &error)` will fail lookup and fall through to NOT_FOUND. `arv_camera_new("IDS-4110010861", &error)` (vendor-alias form) would work, but the enumerate-and-compare-`arv_get_device_serial_nbr()` loop above is the version-independent, documented-intent way to "find by serial" and is what should be used in the bridge.

### 1.2 Opening

```c
ArvCamera * arv_camera_new (const char *name, GError **error);
ArvCamera * arv_camera_new_with_device (ArvDevice *device, GError **error);
ArvDevice * arv_camera_get_device (ArvCamera *camera);
```
(`arvcamera.h:53-55`)

### 1.3 Pixel format Mono8 + resolution 1920x1200 (ROI)

```c
void arv_camera_set_pixel_format (ArvCamera *camera, ArvPixelFormat format, GError **error);
/* or, simpler for a bridge that just wants a known string: */
void arv_camera_set_pixel_format_from_string (ArvCamera *camera, const char *format, GError **error);
/* -> arv_camera_set_pixel_format_from_string(camera, "Mono8", &error); */

void arv_camera_set_region (ArvCamera *camera, gint x, gint y, gint width, gint height, GError **error);
void arv_camera_get_region (ArvCamera *camera, gint *x, gint *y, gint *width, gint *height, GError **error);
void arv_camera_get_width_bounds  (ArvCamera *camera, gint *min, gint *max, GError **error);
void arv_camera_get_height_bounds (ArvCamera *camera, gint *min, gint *max, GError **error);
```
(`arvcamera.h:72-81, 91-92`) — call `arv_camera_set_region(camera, 0, 0, 1920, 1200, &error)` for the native full frame.

### 1.4 Exposure / gain / black level

Dedicated convenience setters exist in 0.8.36 (no need to drop to raw GenICam feature names):
```c
void   arv_camera_set_exposure_time  (ArvCamera *camera, double exposure_time_us, GError **error);
double arv_camera_get_exposure_time  (ArvCamera *camera, GError **error);
void   arv_camera_get_exposure_time_bounds (ArvCamera *camera, double *min, double *max, GError **error);

void   arv_camera_set_gain  (ArvCamera *camera, double gain, GError **error);
double arv_camera_get_gain  (ArvCamera *camera, GError **error);
void   arv_camera_get_gain_bounds (ArvCamera *camera, double *min, double *max, GError **error);

void   arv_camera_set_black_level (ArvCamera *camera, double blacklevel, GError **error);
double arv_camera_get_black_level (ArvCamera *camera, GError **error);
void   arv_camera_get_black_level_bounds (ArvCamera *camera, double *min, double *max, GError **error);
```
(`arvcamera.h:133-165`) Each has a matching `arv_camera_is_*_available()` guard (`arv_camera_is_exposure_time_available`, `arv_camera_is_gain_available`, `arv_camera_is_black_level_available`) that should be checked before calling, since these map to optional GenICam features.

### 1.5 Asynchronous streaming (callback/queue based)

```c
typedef void (*ArvStreamCallback) (void *user_data, ArvStreamCallbackType type, ArvBuffer *buffer);
/* ARV_STREAM_CALLBACK_TYPE_{INIT,EXIT,START_BUFFER,BUFFER_DONE} */

ArvStream * arv_camera_create_stream (ArvCamera *camera, ArvStreamCallback callback, void *user_data, GError **error);
ArvStream * arv_camera_create_stream_full (ArvCamera *camera, ArvStreamCallback callback, void *user_data, GDestroyNotify destroy, GError **error);

void        arv_stream_push_buffer (ArvStream *stream, ArvBuffer *buffer);
ArvBuffer * arv_stream_pop_buffer (ArvStream *stream);
ArvBuffer * arv_stream_try_pop_buffer (ArvStream *stream);
ArvBuffer * arv_stream_timeout_pop_buffer (ArvStream *stream, guint64 timeout);
void        arv_stream_get_n_buffers (ArvStream *stream, gint *n_input_buffers, gint *n_output_buffers);
void        arv_stream_start_thread (ArvStream *stream);
unsigned int arv_stream_stop_thread (ArvStream *stream, gboolean delete_buffers);
void        arv_stream_get_statistics (ArvStream *stream, guint64 *n_completed_buffers, guint64 *n_failures, guint64 *n_underruns);

void   arv_camera_start_acquisition (ArvCamera *camera, GError **error);
void   arv_camera_stop_acquisition  (ArvCamera *camera, GError **error);
void   arv_camera_abort_acquisition (ArvCamera *camera, GError **error);
guint  arv_camera_get_payload (ArvCamera *camera, GError **error);   /* == 2,304,000 for this camera/mode */
```
(`arvstream.h`, `arvcamera.h:57-58, 101-105, 181`)

Buffer access, once popped:
```c
ArvBuffer *     arv_buffer_new_allocate (size_t size);
ArvBufferStatus arv_buffer_get_status (ArvBuffer *buffer);        /* ARV_BUFFER_STATUS_SUCCESS etc, arvbuffer.h:50-59 */
const void *    arv_buffer_get_data (ArvBuffer *buffer, size_t *size);
const void *    arv_buffer_get_image_data (ArvBuffer *buffer, size_t *size);
```

Recommended pattern for the WSL bridge process: create the stream with `arv_camera_create_stream()`, pre-fill it with N `arv_buffer_new_allocate(payload)` buffers via `arv_stream_push_buffer()`, call `arv_camera_start_acquisition()`, then loop on `arv_stream_timeout_pop_buffer()` (bounded wait, so the bridge stays responsive to control-channel commands and shutdown) — check `arv_buffer_get_status() == ARV_BUFFER_STATUS_SUCCESS`, forward `arv_buffer_get_data()` bytes over the data channel, then `arv_stream_push_buffer()` the same buffer back into the queue immediately (standard GenTL-style buffer recycling — this is also exactly the shape a GenTL `.cti`'s own buffer queue expects, so it maps cleanly onto the eventual Windows-side data-stream object).

### 1.6 Reconnect / error handling

`ArvDevice` (base class every transport layer derives from, `arvdevice.c:1430`) defines a GObject signal:
```c
"control-lost"   /* g_signal_new("control-lost", ..., G_STRUCT_OFFSET(ArvDeviceClass, control_lost), ...) */
```
emitted via `arv_device_emit_control_lost_signal(ArvDevice *device)`. Confirmed call sites in both transport backends: `arvuvdevice.c:198` and `arvuvdevice.c:1010` (USB3 Vision — the one relevant here) and `arvgvdevice.c:487` (GigE Vision). Connect to it with `g_signal_connect(device, "control-lost", G_CALLBACK(on_control_lost), user_data)` right after `arv_camera_get_device(camera)`. This is the correct hook for detecting the camera being physically lost/USB-reset/usbip-detached out from under Aravis — the bridge should treat it as "device gone", stop acquisition, and push a `DEVICE_LOST` event up the control channel to the Windows CTI side so it can surface a GenTL `EVENT_ERROR`/module-detached style event to Kineo rather than hanging.

There is no separate Aravis-level "auto reconnect" API — reconnect is an application-level policy: on `control-lost`, tear down the `ArvStream`/`ArvCamera`, loop `arv_update_device_list()` + serial-match (§1.1) until the device reappears, then re-open and re-apply configuration (pixel format/region/exposure/gain/black level) before restarting acquisition. All error returns elsewhere in the API use the standard GLib `GError **error` out-parameter with domain `ARV_DEVICE_ERROR` (`arvdevice.h:38`), enum values include `ARV_DEVICE_ERROR_NOT_CONNECTED`, `ARV_DEVICE_ERROR_TIMEOUT`, `ARV_DEVICE_ERROR_NOT_FOUND`, `ARV_DEVICE_ERROR_NOT_CONTROLLER` (arvdevice.h:59-77) — the bridge should map these onto its own control-channel error codes rather than inventing new categories.

---

## 2. Windows-CTI <-> WSL-bridge IPC design

### 2.1 Transport decision: TCP over localhost (WSL2 loopback forwarding)

**Recommended: plain TCP, both sides bound to 127.0.0.1, using WSL2's built-in localhost forwarding.**

Rejected/deferred alternatives:
- **Windows named pipes** — NOT directly reachable from WSL2; a named pipe created on the Windows side lives in the Windows kernel object namespace and there is no built-in path for a Linux process inside the WSL2 VM to open `\\.\pipe\...` directly. This would require its own relay process (e.g. a small helper translating pipe<->socket), which reintroduces exactly the extra moving part this option was supposed to avoid, for zero benefit over TCP. Flagged as a likely blocker — do not pursue for v1.
- **Unix domain socket + `socat`** — works (a `socat TCP-LISTEN:PORT,fork UNIX-CONNECT:/path.sock` relay), but adds a third process to keep alive, monitor, and restart, and buys nothing bandwidth- or latency-wise over talking TCP directly, since the bandwidth ceiling here (§3) is nowhere near where UDS-vs-TCP framing overhead would matter. Not recommended for v1; worth revisiting only if a future phase needs strict filesystem-permission-scoped IPC instead of a TCP port.
- **TCP over localhost** — WSL2 has transparently forwarded `localhost`/`127.0.0.1` between Windows and the WSL2 VM by default since ~build 18945 (both directions, in the default NAT networking mode this setup already uses). No extra relay process, ordinary Winsock `connect("127.0.0.1", port)` on the Windows CTI side, ordinary POSIX `listen()` on the WSL bridge side. Simplest possible option and the one to build first.

### 2.2 Message framing

Two independent TCP connections, both to 127.0.0.1 on fixed ports, both initiated by the Windows CTI (bridge listens, CTI connects — bridge process outlives any one CTI/Kineo session):

```
Windows CTI (client)                          WSL bridge (server, owns Aravis/camera)
   |-- TCP connect :50100 (control) -------------->|
   |-- TCP connect :50101 (data)    -------------->|
   |                                                |
   |-- OPEN(serial) --------------------------->    |  arv_update_device_list() + serial match
   |<-- OPEN_ACK(status, w,h,payload_size) -----    |  arv_camera_new(device_id)
   |-- CONFIGURE(pixfmt,x,y,w,h,exp,gain,bl) -->    |  arv_camera_set_* calls
   |<-- CONFIGURE_ACK(status) ------------------    |
   |-- START_ACQUISITION ----------------------->    |  arv_camera_create_stream + start_acquisition
   |                                                |------ frame ------>|  (data channel, one-way)
   |                                                |------ frame ------>|
   |<== DEVICE_LOST (async, control-lost signal) ===|  (only if it happens)
   |-- STOP_ACQUISITION ------------------------->    |
```

**Control channel** — small, latency-sensitive, request/response. Each frame:
`[u32 length LE][u8 msg_type][payload]`, payload per type:
- `OPEN` — serial string (UTF-8, length-prefixed)
- `OPEN_ACK` — `u8 status, u32 sensor_w, u32 sensor_h, u32 payload_size`
- `CONFIGURE` — `u8 pixel_format_enum, i32 x, i32 y, i32 w, i32 h, f64 exposure_us, f64 gain, f64 black_level`
- `CONFIGURE_ACK` — `u8 status`
- `START_ACQUISITION` / `STOP_ACQUISITION` — no payload
- `GET_BUFFER_INFO` / reply — buffer queue depth, completed/failed/underrun counters (mirrors `arv_stream_get_statistics`)
- `DEVICE_LOST` — pushed asynchronously by the bridge (not a response to any request) when the `control-lost` signal fires
- `ERROR` — `u32 error_code, string message` (error_code maps from `ARV_DEVICE_ERROR_*`)

**Data channel** — one-way, bridge -> CTI, bulk frame payloads, kept separate from control so a large frame in flight never head-of-line-blocks a control ack (e.g. STOP_ACQUISITION needs to land promptly even mid-frame). Each frame:
`[u64 frame_id][u64 timestamp_ns][u32 payload_length][u8 buffer_status][raw pixel bytes...]`
`payload_length` is `2,304,000` for the confirmed Mono8 1920x1200 mode; `buffer_status` mirrors `ArvBufferStatus` so a `SUCCESS`-vs-`TIMEOUT`/`MISSING_PACKETS` frame can be distinguished without a second round trip. No per-frame ack is needed for v1 — ordinary TCP backpressure (send buffer fills, `arv_stream_pop_buffer` loop naturally stalls the bridge's forward loop) is an adequate first-pass flow-control mechanism, matching how GenTL's own buffer-queue depth already provides slack.

This is intentionally the simplest correct design, not the fastest — no shared memory, no multiplexed single-socket framing, no per-frame acks. Given the bandwidth numbers below, there is no performance case for anything more complex in v1.

---

## 3. Bandwidth estimate

Frame size fixed: 2,304,000 bytes/frame (1920x1200 Mono8, confirmed payload).

| FPS | Bytes/s | MB/s (decimal) | Mbps | Gbps |
|---|---|---|---|---|
| 10  | 23,040,000  | 23.04  | 184.32  | 0.18 |
| 20  | 46,080,000  | 46.08  | 368.64  | 0.37 |
| 30  | 69,120,000  | 69.12  | 552.96  | 0.55 |
| 60  | 138,240,000 | 138.24 | 1,105.92 | 1.11 |
| 100 | 230,400,000 | 230.40 | 1,843.20 | 1.84 |

**Sanity check against localhost TCP throughput:** WSL2's virtualized loopback path (Hyper-V networking between the Windows host and the WSL2 VM) commonly measures multi-Gbps in practice (widely observed in the 5-20+ Gbps range depending on host CPU/generation), i.e. comfortably above even the 100 FPS figure (1.84 Gbps) with headroom to spare. **The new Windows-CTI <-> WSL-bridge TCP hop is not expected to be the bottleneck at any frame rate in this table.**

**The real, already-established bottleneck is one hop further back: USB/IP carrying the actual camera's USB3 traffic from the physical Windows host into WSL2, ahead of Aravis.** This is the same link the project has already characterized as unreliable for *synchronous* USB acquisition, and USB/IP-over-network is well known to fall well short of native USB3 SuperSpeed throughput/latency even for working (async) transfers — practical usbip throughput is commonly a small fraction of USB3's nominal 5 Gbps, and it adds scheduling jitter that raw local USB doesn't have. Concretely:
- At 10-30 FPS (23-69 MB/s), well within what the already-working async pipeline handles today — low risk.
- At 60 FPS (138 MB/s) and especially 100 FPS (230 MB/s), sustained throughput starts approaching the range where usbip-over-network overhead, and USB3 Vision's own isochronous/bulk transfer scheduling through that virtualized link, becomes the limiting factor — **not** the localhost TCP hop this phase is designing.
- This means the new IPC design does not need extra engineering effort to "keep up" — whatever frame rate Aravis can already sustain end-to-end through USB/IP today is what the bridge will forward; the TCP link adds negligible additional ceiling.

Conclusion: **design for simplicity, not throughput** — confirmed the guidance in the task. The bottleneck to actually go measure is USB/IP-to-Aravis async throughput at increasing target frame rates (see next experiment in progress.md), not the IPC transport being designed here.
