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

    if (!send_msg(s, MSG_START, "{}") || !recv_small_payload(s, MSG_START, small, sizeof small)) {
        wlog("START failed"); closesocket(s); return;
    }
    wlog("START -> %s", small);
    if (!json_has_ok_true(small)) { wlog("START reply not ok, aborting session"); closesocket(s); return; }

    wlog("streaming loop entered");
    uint64_t frames_accepted = 0, frames_rejected = 0;
    while (!g_should_stop) {
        uint8_t msg_type; uint32_t plen;
        if (!recv_wire_header(s, &msg_type, &plen)) { wlog("disconnected while streaming"); break; }
        if (msg_type != MSG_FRAME) {
            /* Unexpected off-cycle control message (e.g. a STATUS reply to
             * nothing we sent) -- drain its payload and ignore. */
            if (plen > 0) {
                uint8_t discard[512];
                uint32_t remaining = plen;
                while (remaining > 0) {
                    uint32_t chunk = remaining < sizeof discard ? remaining : (uint32_t)sizeof discard;
                    if (!recv_all(s, discard, chunk)) { plen = 0; break; }
                    remaining -= chunk;
                }
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

void wsl_bridge_stop(void) {
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
