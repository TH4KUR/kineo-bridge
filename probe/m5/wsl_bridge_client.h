/* wsl_bridge_client.h -- M5: TCP client for kineo_camera_bridge.py, the
 * WSL-side Aravis/camera bridge. Speaks the wire protocol defined in
 * ~/kineo-bridge/wsl-camera/protocol.py (KCB1 framing).
 *
 * Kept deliberately separate from m5_bridge.c (the GenTL producer) so the
 * proven M4d buffer/event/acquisition machinery in that file stays a
 * near-verbatim copy of the protected m4h_standalone.c baseline -- only the
 * worker thread's pixel-fill call site changes, gated by g_frame_source_mode.
 */
#ifndef WSL_BRIDGE_CLIENT_H
#define WSL_BRIDGE_CLIENT_H

#include <windows.h>
#include <stdint.h>

typedef enum { FRAME_SOURCE_SYNTHETIC = 0, FRAME_SOURCE_WSL = 1 } frame_source_mode_t;
extern frame_source_mode_t g_frame_source_mode;

typedef void (*wsl_log_fn_t)(const char *fmt, ...);

/* Reads KINEO_BRIDGE_SOURCE (default "synthetic") and sets g_frame_source_mode.
 * Cheap, no I/O -- call once, early (GCInitLib), before wsl_bridge_start(). */
void wsl_bridge_configure_mode(wsl_log_fn_t log_fn);

/* Spawns the connector/receiver thread: reads KINEO_BRIDGE_HOST (default
 * 127.0.0.1) / KINEO_BRIDGE_PORT (default 9494) from the environment,
 * connects, does HELLO/OPEN(serial)/CONFIGURE(exposure_time_us, gain,
 * black_level, frame_rate)/START, then loops receiving FRAME messages and
 * keeping only the latest complete one. Reconnects with a backoff on
 * disconnect rather than giving up. Idempotent: a second call is a no-op.
 * Only meaningful when g_frame_source_mode == FRAME_SOURCE_WSL. */
void wsl_bridge_start(const char *serial,
                       double exposure_time_us, double gain, double black_level,
                       double frame_rate);

/* Blocks up to timeout_ms for a frame this caller has not already consumed
 * (tracked internally via a monotonic generation counter), then copies its
 * pixel bytes into dst (capacity bytes must be >= the frame's data length).
 * Returns 1 and sets *out_ts_ns on success; 0 on timeout, no connection, or
 * a capacity/size mismatch (treated by the caller like an incomplete
 * buffer -- same as the existing capacity check in the synthetic path). */
int wsl_bridge_get_frame_blocking(uint8_t *dst, size_t capacity, uint64_t *out_ts_ns, DWORD timeout_ms);

/* Signals the connector/receiver thread to stop, best-effort sends
 * STOP+CLOSE, closes the socket to unblock any pending recv, and joins with
 * a timeout. Safe to call even if wsl_bridge_start() was never called.
 * Also stops the live-config forwarder thread (see below), if running. */
void wsl_bridge_stop(void);

/* Priority 2 (hardening): forward the GenApi "camera layer" AcquisitionStart/
 * AcquisitionStop commands to the physical camera over the *existing*,
 * already-open bridge connection -- no reconnect, no re-OPEN. A no-op (with
 * a log line) if not yet connected; the initial connect sequence in
 * wsl_bridge_start() already performs the first START on its own, so an
 * early or redundant call here is harmless (the bridge's own START/STOP
 * handling is idempotent). Only meaningful in FRAME_SOURCE_WSL mode. */
void wsl_bridge_notify_acquisition_start(void);
void wsl_bridge_notify_acquisition_stop(void);

/* Priority 3 (hardening): forward a live GenApi register write (Exposure/
 * Gain/BlackLevel/FrameRate) to the physical camera. Returns immediately --
 * never blocks the calling (GenTL) thread on network I/O. Values are handed
 * to a small dedicated forwarder thread that coalesces rapid successive
 * calls (only the latest set of values is ever in flight) and sends one
 * CONFIGURE message per wake-up. A no-op if not yet connected (logged, not
 * an error) or in synthetic mode. */
void wsl_bridge_notify_config_changed(double exposure_time_us, double gain,
                                       double black_level, double frame_rate);

#endif /* WSL_BRIDGE_CLIENT_H */
