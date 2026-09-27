/* wsl_bridge_client.c -- M5: TCP client for kineo_camera_bridge.py.
 * See wsl_bridge_client.h for the module's role and contract. Wire format
 * mirrors ~/kineo-bridge/wsl-camera/protocol.py byte-for-byte (manual LE
 * packing here rather than relying on struct layout/padding assumptions).
 */
#include "wsl_bridge_client.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

frame_source_mode_t g_frame_source_mode = FRAME_SOURCE_SYNTHETIC;

/* ---- wire protocol constants (must match protocol.py) ---- */
#define WIRE_MAGIC0 'K'
#define WIRE_MAGIC1 'C'
#define WIRE_MAGIC2 'B'
#define WIRE_MAGIC3 '1'
#define WIRE_VERSION 1
#define MSG_HELLO 1
#define MSG_OPEN 2
#define MSG_CONFIGURE 3
#define MSG_START 4
#define MSG_STOP 5
#define MSG_CLOSE 6
#define MSG_STATUS 7
#define MSG_FRAME 8
#define MSG_ERROR 9
#define WIRE_HDR_SIZE 10
#define FRAME_HDR_SIZE 33
#define WSL_FRAME_PAYLOAD_MAX 2304000
#define WSL_RECV_BUF_CAP (FRAME_HDR_SIZE + WSL_FRAME_PAYLOAD_MAX + 4096)

static wsl_log_fn_t g_log = NULL;
static void wlog(const char *fmt, ...) {
    if (!g_log) return;
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    g_log("[wsl-bridge] %s", buf);
}

static HANDLE g_thread = NULL;
static volatile int g_should_stop = 0;
static volatile int g_wsapi_ready = 0;

static char g_serial[64] = "";
static double g_cfg_exposure_us = 0, g_cfg_gain = 0, g_cfg_black_level = 0, g_cfg_frame_rate = 0;

/* latest-frame hand-off, single slot */
static CRITICAL_SECTION g_frame_lock;
static CONDITION_VARIABLE g_frame_cv;
static int g_frame_lock_ready = 0;
static uint8_t *g_frame_data = NULL;   /* WSL_FRAME_PAYLOAD_MAX bytes, malloc'd once */
static uint32_t g_frame_len = 0;
static uint64_t g_frame_ts_ns = 0;
static uint64_t g_frame_generation = 0;   /* incremented on every accepted frame */
static uint64_t g_consumed_generation = 0;

/* ---- little-endian pack/unpack helpers (no struct-layout assumptions) ---- */
static void put_u32le(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint64_t get_u64le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= ((uint64_t)p[i]) << (8*i);
    return v;
}

static int send_all(SOCKET s, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(s, (const char *)(buf + sent), (int)(len - sent), 0);
        if (n <= 0) return 0;
        sent += (size_t)n;
    }
    return 1;
}

static int recv_all(SOCKET s, uint8_t *buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        int n = recv(s, (char *)(buf + got), (int)(len - got), 0);
        if (n <= 0) return 0; /* 0 = orderly close, <0 = error */
        got += (size_t)n;
    }
    return 1;
}

static int send_msg(SOCKET s, uint8_t msg_type, const char *payload) {
    uint32_t plen = payload ? (uint32_t)strlen(payload) : 0;
    uint8_t hdr[WIRE_HDR_SIZE];
    hdr[0]=WIRE_MAGIC0; hdr[1]=WIRE_MAGIC1; hdr[2]=WIRE_MAGIC2; hdr[3]=WIRE_MAGIC3;
    hdr[4]=WIRE_VERSION; hdr[5]=msg_type;
    put_u32le(hdr+6, plen);
    if (!send_all(s, hdr, WIRE_HDR_SIZE)) return 0;
    if (plen && !send_all(s, (const uint8_t *)payload, plen)) return 0;
    return 1;
}

/* Priority 2/3 (hardening): the receiver thread is the sole reader of the
 * session socket, but mid-session control sends (physical START/STOP,
 * live CONFIGURE) now come from OTHER threads (the GenTL calling thread,
 * and the config-forwarder thread below) -- g_send_lock serializes all
 * sends so two control messages can never interleave on the wire, and
 * g_active_sock is how those other threads find the live socket at all
 * (INVALID_SOCKET whenever no session is connected). */
static CRITICAL_SECTION g_send_lock;
static int g_send_lock_ready = 0;
static SOCKET g_active_sock = INVALID_SOCKET;

static void ensure_send_lock(void) {
    if (!g_send_lock_ready) {
        InitializeCriticalSection(&g_send_lock);
        g_send_lock_ready = 1;
    }
}

/* Sends on g_active_sock (the current live session, if any) while holding
 * g_send_lock -- safe to call from any thread. Reads g_active_sock itself,
 * under the same lock that clears it on disconnect, so there is no
 * check-then-use race with the socket being torn down concurrently. */
static int send_msg_locked(uint8_t msg_type, const char *payload) {
    ensure_send_lock();
    EnterCriticalSection(&g_send_lock);
    int ok = (g_active_sock != INVALID_SOCKET) && send_msg(g_active_sock, msg_type, payload);
    LeaveCriticalSection(&g_send_lock);
    return ok;
}

/* Reads one wire header. Returns 0 on disconnect/protocol error. */
static int recv_wire_header(SOCKET s, uint8_t *msg_type_out, uint32_t *plen_out) {
    uint8_t hdr[WIRE_HDR_SIZE];
    if (!recv_all(s, hdr, WIRE_HDR_SIZE)) return 0;
    if (hdr[0]!=WIRE_MAGIC0 || hdr[1]!=WIRE_MAGIC1 || hdr[2]!=WIRE_MAGIC2 || hdr[3]!=WIRE_MAGIC3) {
        wlog("bad magic in wire header, dropping connection");
        return 0;
    }
    if (hdr[4] != WIRE_VERSION) {
        wlog("unsupported protocol version %u, dropping connection", (unsigned)hdr[4]);
        return 0;
    }
    *msg_type_out = hdr[5];
    *plen_out = get_u32le(hdr+6);
    return 1;
}

/* Reads a small (<= bufcap-1) control-plane reply's raw JSON payload as a
 * NUL-terminated string. Used only during the handshake, one at a time --
 * no FRAME messages are in flight yet at that point. */
static int recv_small_payload(SOCKET s, uint8_t expected_type, char *buf, size_t bufcap) {
    uint8_t msg_type; uint32_t plen;
    if (!recv_wire_header(s, &msg_type, &plen)) return 0;
    if (plen >= bufcap) { wlog("control reply payload too large (%u bytes)", plen); return 0; }
    if (plen && !recv_all(s, (uint8_t *)buf, plen)) return 0;
    buf[plen] = '\0';
    if (msg_type != expected_type) {
        wlog("expected msg_type=%u, got %u: %s", expected_type, msg_type, buf);
        return 0;
    }
    return 1;
}

static int json_has_ok_true(const char *json) {
    return strstr(json, "\"ok\": true") != NULL || strstr(json, "\"ok\":true") != NULL;
}

static SOCKET connect_to_bridge(void) {
    char host[128]; DWORD hn = GetEnvironmentVariableA("KINEO_BRIDGE_HOST", host, sizeof host);
    if (hn == 0 || hn >= sizeof host) strcpy(host, "127.0.0.1");
    char portbuf[16]; DWORD pn = GetEnvironmentVariableA("KINEO_BRIDGE_PORT", portbuf, sizeof portbuf);
    if (pn == 0 || pn >= sizeof portbuf) strcpy(portbuf, "9494");

    struct addrinfo hints; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portbuf, &hints, &res) != 0 || !res) {
        wlog("getaddrinfo(%s:%s) failed", host, portbuf);
        return INVALID_SOCKET;
    }
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return INVALID_SOCKET; }
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        wlog("connect(%s:%s) failed, WSAGetLastError=%d", host, portbuf, WSAGetLastError());
        closesocket(s);
        freeaddrinfo(res);
        return INVALID_SOCKET;
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    wlog("connected to %s:%s", host, portbuf);
    return s;
}

/* One full session: connect, handshake, then receive frames until the
 * connection drops or we're told to stop. Returns normally either way --
 * the caller (thread proc) decides whether to reconnect. */
static void run_one_session(uint8_t *recv_buf) {
    SOCKET s = connect_to_bridge();
    if (s == INVALID_SOCKET) return;

    char small[512];
    if (!send_msg(s, MSG_HELLO, "{}") || !recv_small_payload(s, MSG_HELLO, small, sizeof small)) {
        wlog("HELLO handshake failed"); closesocket(s); return;
    }
    wlog("HELLO -> %s", small);

    char open_body[128];
    snprintf(open_body, sizeof open_body, "{\"serial\": \"%s\"}", g_serial);
    if (!send_msg(s, MSG_OPEN, open_body) || !recv_small_payload(s, MSG_OPEN, small, sizeof small)) {
        wlog("OPEN handshake failed"); closesocket(s); return;
    }
    wlog("OPEN -> %s", small);
    if (!json_has_ok_true(small)) { wlog("OPEN reply not ok, aborting session"); closesocket(s); return; }

    char cfg_body[256];
    snprintf(cfg_body, sizeof cfg_body,
             "{\"exposure_time_us\": %f, \"gain\": %f, \"black_level\": %f, \"frame_rate\": %f}",
             g_cfg_exposure_us, g_cfg_gain, g_cfg_black_level, g_cfg_frame_rate);
    if (!send_msg(s, MSG_CONFIGURE, cfg_body) || !recv_small_payload(s, MSG_CONFIGURE, small, sizeof small)) {
        wlog("CONFIGURE failed"); closesocket(s); return;
    }
    wlog("CONFIGURE -> %s", small);

    /* Lifecycle hardening (2026-09-27), FPS-investigation bugfix: this
     * handshake used to send an unconditional physical START here too --
     * a leftover from the original "stream continuously from connect"
     * architecture that survived every later redesign by accident. Since
     * the connection is established at DevOpenDataStream (which Kineo
     * reaches long before the user actually clicks Start Analysis), this
     * silently restarted the exact "long-running idle image stream"
     * problem the whole lifecycle redesign was meant to eliminate --
     * confirmed directly: a real session showed the physical camera
     * streaming continuously for 7.5 minutes between connect and the
     * first real AcquisitionStart, only stopping when the first genuine
     * AcquisitionStop finally arrived. Fixed by NOT starting here --
     * the connection now settles into the "ideal idle state" (camera
     * open, configured, zero image traffic) immediately after CONFIGURE,
     * and only wsl_bridge_notify_acquisition_start() (called from the
     * real GenApi AcquisitionStart command) ever starts physical
     * acquisition. Publish the live socket here, right after CONFIGURE,
     * so that notify call has something to send to. */
    ensure_send_lock();
    EnterCriticalSection(&g_send_lock);
    g_active_sock = s;
    LeaveCriticalSection(&g_send_lock);

    wlog("connection idle and ready (camera open+configured, not acquiring)");
    uint64_t frames_accepted = 0, frames_rejected = 0;
    while (!g_should_stop) {
        uint8_t msg_type; uint32_t plen;
        if (!recv_wire_header(s, &msg_type, &plen)) { wlog("disconnected while streaming"); break; }
        if (msg_type != MSG_FRAME) {
            /* Off-cycle control replies: STOP/START acks from
             * notify_acquisition_*() (Priority 2), CONFIGURE acks from the
             * live-config forwarder (Priority 3), or anything else -- log
             * which one, then drain its payload. Never a protocol error;
             * this is the expected shape of a mid-session STOP/START. */
            const char *what = (msg_type == MSG_STOP) ? "STOP ack" :
                                (msg_type == MSG_START) ? "START ack" :
                                (msg_type == MSG_CONFIGURE) ? "CONFIGURE ack" : "off-cycle reply";
            if (plen == 0) {
                wlog("%s (empty)", what);
            } else if (plen < sizeof(small)) {
                if (!recv_all(s, (uint8_t *)small, plen)) { wlog("disconnected reading %s", what); break; }
                small[plen] = '\0';
                wlog("%s: %s", what, small);
            } else {
                uint8_t discard[512];
                uint32_t remaining = plen;
                int ok = 1;
                while (remaining > 0) {
                    uint32_t chunk = remaining < sizeof discard ? remaining : (uint32_t)sizeof discard;
                    if (!recv_all(s, discard, chunk)) { ok = 0; break; }
                    remaining -= chunk;
                }
                if (!ok) { wlog("disconnected reading %s", what); break; }
                wlog("%s (%u bytes, not logged)", what, plen);
            }
            continue;
        }
        if (plen < FRAME_HDR_SIZE || plen > WSL_RECV_BUF_CAP) {
            wlog("FRAME payload size %u out of range, dropping connection", plen);
            break;
        }
        if (!recv_all(s, recv_buf, plen)) { wlog("disconnected mid-frame"); break; }

        uint32_t width = get_u32le(recv_buf+8);
        uint32_t height = get_u32le(recv_buf+12);
        uint32_t data_len = get_u32le(recv_buf+20);
        uint64_t ts_ns = get_u64le(recv_buf+24);
        uint8_t status = recv_buf[32];
        uint32_t actual_data = plen - FRAME_HDR_SIZE;
        int good = (status == 0 && data_len == actual_data && data_len <= WSL_FRAME_PAYLOAD_MAX);
        if (good) {
            EnterCriticalSection(&g_frame_lock);
            memcpy(g_frame_data, recv_buf + FRAME_HDR_SIZE, data_len);
            g_frame_len = data_len;
            g_frame_ts_ns = ts_ns;
            g_frame_generation++;
            WakeAllConditionVariable(&g_frame_cv);
            LeaveCriticalSection(&g_frame_lock);
            frames_accepted++;
            if (frames_accepted <= 10) {
                wlog("frame accepted: %ux%u len=%u (total accepted=%llu)", width, height, data_len,
                     (unsigned long long)frames_accepted);
            } else if (frames_accepted == 11) {
                wlog("further accepted frames logged only in the session summary");
            }
        } else {
            frames_rejected++;
            wlog("frame rejected: status=%u data_len=%u actual=%u", status, data_len, actual_data);
        }
    }
    wlog("session ending: frames_accepted=%llu frames_rejected=%llu",
         (unsigned long long)frames_accepted, (unsigned long long)frames_rejected);
    /* Unpublish first -- once g_active_sock is cleared, notify_* calls from
     * other threads correctly see "not connected" instead of racing the
     * closesocket() below. */
    EnterCriticalSection(&g_send_lock);
    g_active_sock = INVALID_SOCKET;
    LeaveCriticalSection(&g_send_lock);
    /* Best-effort STOP+CLOSE; the socket is being torn down regardless. */
    send_msg(s, MSG_STOP, "{}");
    send_msg(s, MSG_CLOSE, "{}");
    closesocket(s);
}

static DWORD WINAPI wsl_bridge_thread_proc(LPVOID param) {
    (void)param;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        wlog("WSAStartup failed, WSL bridge disabled for this process");
        return 0;
    }
    g_wsapi_ready = 1;

    uint8_t *recv_buf = (uint8_t *)malloc(WSL_RECV_BUF_CAP);
    if (!recv_buf) { wlog("out of memory allocating receive buffer"); WSACleanup(); return 0; }

    int backoff_ms = 500;
    while (!g_should_stop) {
        run_one_session(recv_buf);
        if (g_should_stop) break;
        wlog("reconnecting in %d ms", backoff_ms);
        Sleep((DWORD)backoff_ms);
        if (backoff_ms < 5000) backoff_ms *= 2;
    }

    free(recv_buf);
    WSACleanup();
    wlog("thread exiting");
    return 0;
}

void wsl_bridge_configure_mode(wsl_log_fn_t log_fn) {
    g_log = log_fn;
    char buf[32];
    DWORD n = GetEnvironmentVariableA("KINEO_BRIDGE_SOURCE", buf, sizeof buf);
    if (n > 0 && n < sizeof buf && (_stricmp(buf, "wsl") == 0 || _stricmp(buf, "camera") == 0)) {
        g_frame_source_mode = FRAME_SOURCE_WSL;
    } else {
        g_frame_source_mode = FRAME_SOURCE_SYNTHETIC;
    }
    wlog("mode=%s (KINEO_BRIDGE_SOURCE=%s)",
         g_frame_source_mode == FRAME_SOURCE_WSL ? "WSL" : "synthetic",
         n > 0 ? buf : "(unset)");
}

void wsl_bridge_start(const char *serial, double exposure_time_us, double gain,
                       double black_level, double frame_rate) {
    if (g_frame_source_mode != FRAME_SOURCE_WSL) return;
    if (g_thread) return; /* idempotent */

    strncpy(g_serial, serial, sizeof(g_serial) - 1);
    g_serial[sizeof(g_serial)-1] = '\0';
    g_cfg_exposure_us = exposure_time_us;
    g_cfg_gain = gain;
    g_cfg_black_level = black_level;
    g_cfg_frame_rate = frame_rate;

    if (!g_frame_lock_ready) {
        InitializeCriticalSection(&g_frame_lock);
        InitializeConditionVariable(&g_frame_cv);
        g_frame_data = (uint8_t *)malloc(WSL_FRAME_PAYLOAD_MAX);
        g_frame_lock_ready = 1;
    }
    g_should_stop = 0;
    g_thread = CreateThread(NULL, 0, wsl_bridge_thread_proc, NULL, 0, NULL);
    wlog("connector/receiver thread launched (serial=%s exposure_us=%.3f gain=%.3f black_level=%.3f frame_rate=%.3f)",
         g_serial, exposure_time_us, gain, black_level, frame_rate);
}

int wsl_bridge_get_frame_blocking(uint8_t *dst, size_t capacity, uint64_t *out_ts_ns, DWORD timeout_ms) {
    if (!g_frame_lock_ready) return 0;
    EnterCriticalSection(&g_frame_lock);
    while (g_frame_generation == g_consumed_generation && !g_should_stop) {
        if (!SleepConditionVariableCS(&g_frame_cv, &g_frame_lock, timeout_ms)) {
            /* timeout (GetLastError()==ERROR_TIMEOUT) or the stop flag flipped */
            break;
        }
    }
    if (g_frame_generation == g_consumed_generation || g_should_stop) {
        LeaveCriticalSection(&g_frame_lock);
        return 0;
    }
    int ok = (g_frame_len > 0 && g_frame_len <= capacity);
    if (ok) {
        memcpy(dst, g_frame_data, g_frame_len);
        if (out_ts_ns) *out_ts_ns = g_frame_ts_ns;
    }
    g_consumed_generation = g_frame_generation;
    LeaveCriticalSection(&g_frame_lock);
    return ok;
}

/* ===== Priority 2 (hardening): physical START/STOP on the live session ===== */

void wsl_bridge_notify_acquisition_start(void) {
    if (g_frame_source_mode != FRAME_SOURCE_WSL) return;
    if (send_msg_locked(MSG_START, "{}")) {
        wlog("sent physical START request (AcquisitionStart)");
    } else {
        wlog("AcquisitionStart: not connected yet, request skipped (initial connect will start on its own)");
    }
}

void wsl_bridge_notify_acquisition_stop(void) {
    if (g_frame_source_mode != FRAME_SOURCE_WSL) return;
    if (send_msg_locked(MSG_STOP, "{}")) {
        wlog("sent physical STOP request (AcquisitionStop)");
    } else {
        wlog("AcquisitionStop: not connected, nothing to stop");
    }
}

/* ===== Priority 3 (hardening): live control forwarding, non-blocking ===== */

static CRITICAL_SECTION g_pending_cfg_lock;
static CONDITION_VARIABLE g_pending_cfg_cv;
static int g_pending_cfg_lock_ready = 0;
static int g_pending_cfg_dirty = 0;
static int g_pending_cfg_should_stop = 0;
static double g_pending_exposure_us = 0, g_pending_gain = 0, g_pending_black = 0, g_pending_rate = 0;
static HANDLE g_cfg_thread = NULL;

static DWORD WINAPI cfg_forwarder_thread_proc(LPVOID param) {
    (void)param;
    wlog("[cfg-forwarder] thread started");
    for (;;) {
        EnterCriticalSection(&g_pending_cfg_lock);
        while (!g_pending_cfg_dirty && !g_pending_cfg_should_stop) {
            SleepConditionVariableCS(&g_pending_cfg_cv, &g_pending_cfg_lock, INFINITE);
        }
        if (g_pending_cfg_should_stop) { LeaveCriticalSection(&g_pending_cfg_lock); break; }
        double e = g_pending_exposure_us, g = g_pending_gain, b = g_pending_black, r = g_pending_rate;
        g_pending_cfg_dirty = 0;
        LeaveCriticalSection(&g_pending_cfg_lock);

        char body[256];
        snprintf(body, sizeof body,
                 "{\"exposure_time_us\": %f, \"gain\": %f, \"black_level\": %f, \"frame_rate\": %f}",
                 e, g, b, r);
        int ok = send_msg_locked(MSG_CONFIGURE, body);
        if (ok) {
            wlog("[cfg-forwarder] live CONFIGURE sent: exposure_us=%.3f gain=%.3f black_level=%.3f frame_rate=%.3f",
                 e, g, b, r);
        } else {
            wlog("[cfg-forwarder] not connected, live config change dropped (will apply from scratch on next connect)");
        }
    }
    wlog("[cfg-forwarder] thread exiting");
    return 0;
}

void wsl_bridge_notify_config_changed(double exposure_time_us, double gain,
                                       double black_level, double frame_rate) {
    if (g_frame_source_mode != FRAME_SOURCE_WSL) return;
    if (!g_pending_cfg_lock_ready) {
        InitializeCriticalSection(&g_pending_cfg_lock);
        InitializeConditionVariable(&g_pending_cfg_cv);
        g_pending_cfg_lock_ready = 1;
        g_cfg_thread = CreateThread(NULL, 0, cfg_forwarder_thread_proc, NULL, 0, NULL);
    }
    /* Just publish the latest values and wake the forwarder -- returns in
     * microseconds, never touches the network from this (GenTL) thread. */
    EnterCriticalSection(&g_pending_cfg_lock);
    g_pending_exposure_us = exposure_time_us;
    g_pending_gain = gain;
    g_pending_black = black_level;
    g_pending_rate = frame_rate;
    g_pending_cfg_dirty = 1;
    WakeAllConditionVariable(&g_pending_cfg_cv);
    LeaveCriticalSection(&g_pending_cfg_lock);
}

void wsl_bridge_stop(void) {
    if (g_cfg_thread) {
        EnterCriticalSection(&g_pending_cfg_lock);
        g_pending_cfg_should_stop = 1;
        WakeAllConditionVariable(&g_pending_cfg_cv);
        LeaveCriticalSection(&g_pending_cfg_lock);
        WaitForSingleObject(g_cfg_thread, 2000);
        CloseHandle(g_cfg_thread);
        g_cfg_thread = NULL;
    }
    if (!g_thread) return;
    g_should_stop = 1;
    if (g_frame_lock_ready) {
        EnterCriticalSection(&g_frame_lock);
        WakeAllConditionVariable(&g_frame_cv);
        LeaveCriticalSection(&g_frame_lock);
    }
    wlog("waiting for connector/receiver thread to exit...");
    WaitForSingleObject(g_thread, 3000);
    CloseHandle(g_thread);
    g_thread = NULL;
}
