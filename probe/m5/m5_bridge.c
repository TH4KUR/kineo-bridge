/* kineo_probe_v2.c — M1 iteration of the probe GenTL producer (.cti)
 *
 * Superset of the M0-validated kineo_probe.c (M0 baseline is preserved
 * unchanged at ../kineo_probe.c). Adds stub exports for the Event, DS, and
 * stacked-port families that M0 deliberately omitted, after M1a evidence
 * showed ids_peak.dll rejects a producer whose export table is incomplete
 * relative to the full common GenTL set (Phase 2 finding), before ever
 * calling into the probe's logged functions. See
 * investigation/progress.md M1 section for the trace that led here.
 *
 * Purpose (per PLAN.md M0): a minimal but ABI-correct GenTL producer that
 * reports exactly one transport layer, one interface, and one fake device,
 * logs every entry point call with a timestamp, and lets a consumer enumerate
 * and OPEN the fake device. No data streaming yet (that is M4). Safe to load
 * without any camera present.
 *
 * Handles are fixed sentinel pointers into static storage; there is exactly one
 * of each module instance, which is all M0 needs.
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "gentl_v2.h"
#include "wsl_bridge_client.h"

#define PROBE_VERSION "0.14-M5"

/* ---- identity constants presented by the fake producer ---- */
#define TL_ID_STR        "KineoBridge"
#define TL_VENDOR_STR    "IDS Imaging Development Systems GmbH"
#define TL_MODEL_STR     "KineoBridgeProducer"
#define TL_DISPLAY_STR   "Kineo Bridge Producer"
#define TL_FILENAME_STR  "kineo_bridge_probe.cti"
#define TLTYPE_U3V       "U3V"

#define IFACE_ID_STR     "KineoBridgeU3V-IF0"
#define IFACE_DISPLAY    "Kineo Bridge USB3 Interface"

#define DEV_ID_STR       "KineoBridgeDevice0"
#define DEV_VENDOR_STR   "IDS Imaging Development Systems GmbH"
#define DEV_MODEL_STR    "U3-3560XCP-M"
#define DEV_DISPLAY_STR  "IDS U3-3560XCP-M (KineoBridge)"
#define DEV_SERIAL_STR   "4110010861"
#define DEV_VERSION_STR  "1.0"

/* ---- sentinel handle storage ---- */
static char g_tl_obj;
static char g_if_obj;
static char g_dev_obj;
static char g_port_obj;
static char g_ds_obj;
#define H_TL   ((TL_HANDLE)&g_tl_obj)
#define H_IF   ((IF_HANDLE)&g_if_obj)
#define H_DEV  ((DEV_HANDLE)&g_dev_obj)
#define H_PORT ((PORT_HANDLE)&g_port_obj)
#define H_DS   ((DS_HANDLE)&g_ds_obj)
#define DS_STREAM_ID "KineoStream0"

static int g_lib_init = 0;
static GC_ERROR g_last_err = GC_ERR_SUCCESS;
static char g_last_err_txt[256] = "no error";

/* ---- logging ---- */
static CRITICAL_SECTION g_log_lock;
static int g_log_ready = 0;
static char g_log_path[MAX_PATH];

static void log_resolve_path(void) {
    DWORD n = GetEnvironmentVariableA("KINEO_BRIDGE_LOG", g_log_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        strcpy(g_log_path, "kineo_probe_cti.log");
    }
}

static void probe_log(const char *fmt, ...) {
    if (!g_log_ready) return;
    EnterCriticalSection(&g_log_lock);
    FILE *f = fopen(g_log_path, "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        /* M4c-3: thread ID on every line, from this build onward -- per
         * instructions, needed to see whether ids_peak spins up a
         * separate waiting thread around DSStartAcquisition/EventGetData. */
        fprintf(f, "[%02d:%02d:%02d.%03d] [tid=%lu] ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                (unsigned long)GetCurrentThreadId());
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_log_lock);
}

static void set_err(GC_ERROR e, const char *txt) {
    g_last_err = e;
    if (txt) {
        strncpy(g_last_err_txt, txt, sizeof(g_last_err_txt) - 1);
        g_last_err_txt[sizeof(g_last_err_txt) - 1] = '\0';
    }
}

/* ---- INFO getters (GenTL string/int size-negotiation contract) ---- */
static GC_ERROR info_string(INFO_DATATYPE *piType, void *pBuffer, size_t *piSize, const char *s) {
    size_t need = strlen(s) + 1;
    if (piType) *piType = INFO_DATATYPE_STRING;
    if (!pBuffer) { if (piSize) *piSize = need; return GC_ERR_SUCCESS; }
    if (!piSize) return GC_ERR_INVALID_PARAMETER;
    if (*piSize < need) { *piSize = need; return GC_ERR_BUFFER_TOO_SMALL; }
    memcpy(pBuffer, s, need);
    *piSize = need;
    return GC_ERR_SUCCESS;
}
static GC_ERROR info_i32(INFO_DATATYPE *piType, void *pBuffer, size_t *piSize, int32_t v) {
    if (piType) *piType = INFO_DATATYPE_INT32;
    if (!pBuffer) { if (piSize) *piSize = sizeof(int32_t); return GC_ERR_SUCCESS; }
    if (!piSize || *piSize < sizeof(int32_t)) { if (piSize) *piSize = sizeof(int32_t); return GC_ERR_BUFFER_TOO_SMALL; }
    memcpy(pBuffer, &v, sizeof v); *piSize = sizeof v; return GC_ERR_SUCCESS;
}
static GC_ERROR info_bool8(INFO_DATATYPE *piType, void *pBuffer, size_t *piSize, bool8_t v) {
    if (piType) *piType = INFO_DATATYPE_BOOL8;
    if (!pBuffer) { if (piSize) *piSize = sizeof(bool8_t); return GC_ERR_SUCCESS; }
    if (!piSize || *piSize < sizeof(bool8_t)) { if (piSize) *piSize = sizeof(bool8_t); return GC_ERR_BUFFER_TOO_SMALL; }
    memcpy(pBuffer, &v, sizeof v); *piSize = sizeof v; return GC_ERR_SUCCESS;
}
static GC_ERROR info_u32(INFO_DATATYPE *piType, void *pBuffer, size_t *piSize, uint32_t v) {
    if (piType) *piType = INFO_DATATYPE_UINT32;
    if (!pBuffer) { if (piSize) *piSize = sizeof(uint32_t); return GC_ERR_SUCCESS; }
    if (!piSize || *piSize < sizeof(uint32_t)) { if (piSize) *piSize = sizeof(uint32_t); return GC_ERR_BUFFER_TOO_SMALL; }
    memcpy(pBuffer, &v, sizeof v); *piSize = sizeof v; return GC_ERR_SUCCESS;
}
static GC_ERROR info_u64(INFO_DATATYPE *piType, void *pBuffer, size_t *piSize, uint64_t v) {
    if (piType) *piType = INFO_DATATYPE_UINT64;
    if (!pBuffer) { if (piSize) *piSize = sizeof(uint64_t); return GC_ERR_SUCCESS; }
    if (!piSize || *piSize < sizeof(uint64_t)) { if (piSize) *piSize = sizeof(uint64_t); return GC_ERR_BUFFER_TOO_SMALL; }
    memcpy(pBuffer, &v, sizeof v); *piSize = sizeof v; return GC_ERR_SUCCESS;
}
static GC_ERROR info_sizet(INFO_DATATYPE *piType, void *pBuffer, size_t *piSize, size_t v) {
    if (piType) *piType = INFO_DATATYPE_SIZET;
    if (!pBuffer) { if (piSize) *piSize = sizeof(size_t); return GC_ERR_SUCCESS; }
    if (!piSize || *piSize < sizeof(size_t)) { if (piSize) *piSize = sizeof(size_t); return GC_ERR_BUFFER_TOO_SMALL; }
    memcpy(pBuffer, &v, sizeof v); *piSize = sizeof v; return GC_ERR_SUCCESS;
}

/* shared TL_INFO responder for GCGetInfo (library) and TLGetInfo (system) */
static GC_ERROR tl_info(TL_INFO_CMD cmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    switch (cmd) {
        case TL_INFO_ID:            return info_string(piType, pBuffer, piSize, TL_ID_STR);
        case TL_INFO_VENDOR:        return info_string(piType, pBuffer, piSize, TL_VENDOR_STR);
        case TL_INFO_MODEL:         return info_string(piType, pBuffer, piSize, TL_MODEL_STR);
        case TL_INFO_VERSION:       return info_string(piType, pBuffer, piSize, PROBE_VERSION);
        case TL_INFO_TLTYPE:        return info_string(piType, pBuffer, piSize, TLTYPE_U3V);
        case TL_INFO_NAME:          return info_string(piType, pBuffer, piSize, TL_FILENAME_STR);
        case TL_INFO_PATHNAME:      return info_string(piType, pBuffer, piSize, TL_FILENAME_STR);
        case TL_INFO_DISPLAYNAME:   return info_string(piType, pBuffer, piSize, TL_DISPLAY_STR);
        case TL_INFO_CHAR_ENCODING: return info_i32(piType, pBuffer, piSize, TL_CHAR_ENCODING_ASCII);
        case TL_INFO_GENTL_VER_MAJOR: return info_u32(piType, pBuffer, piSize, 1);
        case TL_INFO_GENTL_VER_MINOR: return info_u32(piType, pBuffer, piSize, 5);
        default:
            set_err(GC_ERR_NOT_AVAILABLE, "TL info command not available");
            return GC_ERR_NOT_AVAILABLE;
    }
}

/* ================= GC* ================= */

GC_ERROR GC_CALLTYPE GCInitLib(void) {
    probe_log("GCInitLib");
    g_lib_init = 1;
    /* M5: decide synthetic vs. WSL-bridge frame source now (env var read,
     * no I/O) -- actually connecting is deferred to the first
     * DSStartAcquisition, same lazy point the worker thread itself uses. */
    wsl_bridge_configure_mode(probe_log);
    set_err(GC_ERR_SUCCESS, "no error");
    return GC_ERR_SUCCESS;
}

GC_ERROR GC_CALLTYPE GCCloseLib(void) {
    probe_log("GCCloseLib");
    g_lib_init = 0;
    return GC_ERR_SUCCESS;
}

GC_ERROR GC_CALLTYPE GCGetInfo(TL_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    probe_log("GCGetInfo(cmd=%d, pBuffer=%p, *piSize=%zu)", (int)iInfoCmd, pBuffer, piSize ? *piSize : 0);
    GC_ERROR rc = tl_info(iInfoCmd, piType, pBuffer, piSize);
    probe_log("  -> rc=%d piType=%d *piSize=%zu", rc, piType ? *piType : -1, piSize ? *piSize : 0);
    return rc;
}

GC_ERROR GC_CALLTYPE GCGetLastError(GC_ERROR *piErrorCode, char *sErrorText, size_t *piSize) {
    probe_log("GCGetLastError");
    if (piErrorCode) *piErrorCode = g_last_err;
    size_t need = strlen(g_last_err_txt) + 1;
    if (!sErrorText) { if (piSize) *piSize = need; return GC_ERR_SUCCESS; }
    if (!piSize) return GC_ERR_INVALID_PARAMETER;
    if (*piSize < need) { *piSize = need; return GC_ERR_BUFFER_TOO_SMALL; }
    memcpy(sErrorText, g_last_err_txt, need);
    *piSize = need;
    return GC_ERR_SUCCESS;
}

/* Port access — not implemented until M3 (GenICam XML). Logged so M1 reveals
 * whether ids_peak calls them before we implement them. */
/* M3b (revised): URL_SCHEME_LOCAL -- ids_peak queried URL_SCHEME_FILE info
 * successfully but never actually opened the file (silently abandoned node
 * map construction, no error) -- empirically, this consumer does not act on
 * the file: scheme in practice despite it being in the GenTL 1.5 header.
 * Switched to the original, universally-implemented local: register-mapped
 * scheme instead: XML bytes loaded into memory at DllMain, served via
 * GCReadPort against a fixed virtual base address. */
#define XML_VIRTUAL_BASE 0x10000ULL
static uint8_t *g_xml_data = NULL;
static size_t g_xml_data_len = 0;

/* ===== M3f: register-backed GenApi feature map =====
 * Symbolic address -> named virtual register, so real feature reads/writes
 * are directly observable in the log (unlike the earlier literal-<Value>
 * nodes, which are invisible once the XML is parsed once). Initial values
 * per the known Kineo camera_settings.json schema and native sensor mode. */
typedef enum { REG_U32, REG_I64_RO, REG_F64, REG_CMD } reg_kind_t;

typedef struct {
    uint64_t addr;
    size_t   size;
    const char *name;
    reg_kind_t kind;
    void *storage;
    int access_ro;
    int log_count;
} reg_desc_t;

#define REG_LOG_LIMIT 5

static int32_t g_width = 1920;
static int32_t g_height = 1200;
static int32_t g_pixelformat = 0x01080001; /* Mono8 (PFNC) */
static int64_t g_payloadsize = 2304000;
static double  g_exposure_time = 1422.267;
static double  g_gain = 1.0;
static double  g_black_level = 1.75;
static int32_t g_brightness_target = 170;
static int32_t g_brightness_percentile = 30;
static int32_t g_brightness_tolerance = 20;
static int32_t g_offset_x = 0;
static int32_t g_offset_y = 0;
static int32_t g_trigger_mode = 0;
static int32_t g_tlparams_locked = 0;
static int32_t g_acq_start_cmd = 0;
static int32_t g_acq_stop_cmd = 0;
static int32_t g_exposure_start_cmd = 0;
/* M3g: AcquisitionFrameRate — SYNTHETIC value/range. Step 1 (query the real
 * camera via Aravis/WSL) could not complete this pass: the camera's USB
 * device node under WSL (dynamically attached via usbipd) is owned root:root
 * with no group/world write access, and no udev rule grants it to a
 * non-root group; `sudo arv-tool-0.8` failed non-interactively ("a terminal
 * is required to authenticate"). Per the pre-agreed fallback, using a
 * conservative writable Float: default 30.0 fps, range 1.0-60.0 fps. These
 * are NOT read from the real device -- revisit once Step 1 can be completed
 * (e.g. a one-time interactive sudo command or udev rule, out of scope for
 * this pass). */
static double  g_acq_frame_rate = 30.0;

/* M3h: AcquisitionMode -- SFNC-style writable Enumeration, same
 * register-backed pattern as TriggerMode. Values are our own stable
 * assignment (Continuous=0, SingleFrame=1, MultiFrame=2) -- consumers
 * select by symbolic entry name via GenApi, not raw integer, so these
 * don't need to match any external convention, only be internally
 * consistent. No acquisition-behavior side effects yet; this only stores
 * the selected mode. */
static int32_t g_acquisition_mode = 0; /* 0 = Continuous (default) */

/* M3i: TriggerSelector -- SFNC-style writable Enumeration, same
 * register-backed pattern as TriggerMode/AcquisitionMode. Values match
 * the standard SFNC ordering confirmed against a real working reference
 * (Aravis's arv-fake-camera.xml, already on this machine): FrameStart=0,
 * AcquisitionStart=1. ExposureStart=2 added as a straightforward third
 * entry (we already expose an ExposureStart command node). No trigger
 * behavior side effects yet -- storage only. */
static int32_t g_trigger_selector = 0; /* 0 = FrameStart (default) */

static reg_desc_t g_regs[] = {
    { 0x20000, 4, "Width",                         REG_U32,    &g_width,                0, 0 },
    { 0x20010, 4, "Height",                        REG_U32,    &g_height,               0, 0 },
    { 0x20020, 4, "PixelFormat",                   REG_U32,    &g_pixelformat,          0, 0 },
    { 0x20030, 8, "PayloadSize",                   REG_I64_RO, &g_payloadsize,          1, 0 },
    { 0x20040, 8, "ExposureTime",                  REG_F64,    &g_exposure_time,        0, 0 },
    { 0x20050, 8, "Gain",                          REG_F64,    &g_gain,                 0, 0 },
    { 0x20060, 8, "BlackLevel",                    REG_F64,    &g_black_level,          0, 0 },
    { 0x20070, 4, "BrightnessAutoTarget",          REG_U32,    &g_brightness_target,    0, 0 },
    { 0x20080, 4, "BrightnessAutoPercentile",      REG_U32,    &g_brightness_percentile,0, 0 },
    { 0x20090, 4, "BrightnessAutoTargetTolerance", REG_U32,    &g_brightness_tolerance, 0, 0 },
    { 0x200A0, 4, "OffsetX",                       REG_U32,    &g_offset_x,             0, 0 },
    { 0x200B0, 4, "OffsetY",                       REG_U32,    &g_offset_y,             0, 0 },
    { 0x200C0, 4, "TriggerMode",                   REG_U32,    &g_trigger_mode,         0, 0 },
    { 0x200D0, 4, "TLParamsLocked",                REG_U32,    &g_tlparams_locked,      0, 0 },
    { 0x200E0, 4, "AcquisitionStart",               REG_CMD,    &g_acq_start_cmd,        0, 0 },
    { 0x200F0, 4, "AcquisitionStop",                REG_CMD,    &g_acq_stop_cmd,         0, 0 },
    { 0x20100, 4, "ExposureStart",                  REG_CMD,    &g_exposure_start_cmd,   0, 0 },
    { 0x20110, 8, "AcquisitionFrameRate",           REG_F64,    &g_acq_frame_rate,       0, 0 },
    { 0x20120, 4, "AcquisitionMode",                REG_U32,    &g_acquisition_mode,     0, 0 },
    { 0x20130, 4, "TriggerSelector",                REG_U32,    &g_trigger_selector,     0, 0 },
};
#define NUM_REGS (sizeof(g_regs)/sizeof(g_regs[0]))

static reg_desc_t *find_reg(uint64_t addr) {
    for (size_t i = 0; i < NUM_REGS; i++) if (g_regs[i].addr == addr) return &g_regs[i];
    return NULL;
}

static void recompute_payload_size(void) {
    int64_t new_size = (int64_t)g_width * (int64_t)g_height; /* Mono8 = 1 byte/pixel */
    if (new_size != g_payloadsize) {
        probe_log("  [internal] PayloadSize recomputed -> %lld", (long long)new_size);
        g_payloadsize = new_size;
    }
}

static void reg_log_value(reg_desc_t *r, const char *verb) {
    const char *arrow = (verb[0] == 'W') ? "<-" : "->"; /* WRITE <- value ; READ -> value */
    switch (r->kind) {
        case REG_U32:    probe_log("%s %-28s addr=0x%llx %s %d", verb, r->name, (unsigned long long)r->addr, arrow, *(int32_t*)r->storage); break;
        case REG_I64_RO: probe_log("%s %-28s addr=0x%llx %s %lld", verb, r->name, (unsigned long long)r->addr, arrow, (long long)*(int64_t*)r->storage); break;
        case REG_F64:    probe_log("%s %-28s addr=0x%llx %s %f", verb, r->name, (unsigned long long)r->addr, arrow, *(double*)r->storage); break;
        case REG_CMD:    probe_log("COMMAND %-28s addr=0x%llx", r->name, (unsigned long long)r->addr); break;
    }
}

GC_ERROR GC_CALLTYPE GCReadPort(PORT_HANDLE hPort, uint64_t iAddress, void *pBuffer, size_t *piSize) {
    if (hPort != H_PORT) {
        probe_log("GCReadPort hPort=%p addr=0x%llx -> INVALID_HANDLE", hPort, (unsigned long long)iAddress);
        return GC_ERR_INVALID_HANDLE;
    }
    if (g_xml_data && iAddress >= XML_VIRTUAL_BASE && iAddress < XML_VIRTUAL_BASE + g_xml_data_len) {
        probe_log("GCReadPort hPort=%p addr=0x%llx *piSize=%zu [XML]", hPort, (unsigned long long)iAddress, piSize ? *piSize : 0);
        size_t offset = (size_t)(iAddress - XML_VIRTUAL_BASE);
        size_t avail = g_xml_data_len - offset;
        size_t want = piSize ? *piSize : 0;
        size_t n = (want < avail) ? want : avail;
        if (pBuffer && n > 0) memcpy(pBuffer, g_xml_data + offset, n);
        if (piSize) *piSize = n;
        probe_log("  -> rc=0 copied=%zu", n);
        return GC_ERR_SUCCESS;
    }
    reg_desc_t *r = find_reg(iAddress);
    if (!r) {
        probe_log("GCReadPort hPort=%p addr=0x%llx -> INVALID_ADDRESS (unmapped)", hPort, (unsigned long long)iAddress);
        set_err(GC_ERR_INVALID_ADDRESS, "unmapped register address");
        return GC_ERR_INVALID_ADDRESS;
    }
    size_t want = piSize ? *piSize : 0;
    size_t n = (want < r->size) ? want : r->size;
    if (pBuffer && n > 0) memcpy(pBuffer, r->storage, n);
    if (piSize) *piSize = n;
    r->log_count++;
    if (r->log_count <= REG_LOG_LIMIT) reg_log_value(r, "READ ");
    else if (r->log_count == REG_LOG_LIMIT + 1) probe_log("READ  %-28s further reads suppressed (counting silently)", r->name);
    return GC_ERR_SUCCESS;
}

/* M4c-1b: defined further below (with the buffer-table state it reports
 * on); forward-declared here since GCWritePort's COMMAND branch needs to
 * detect AcquisitionStart/AcquisitionStop as a possible burst boundary. */
static void note_buffer_burst_boundary(const char *next_fn_name);
/* M4d: forward declarations -- real definitions are with the rest of the
 * buffer/acquisition state further below; GCWritePort's COMMAND branch
 * needs to flip g_camera_running (and wake the worker/any EventGetData
 * wait) the moment AcquisitionStart/AcquisitionStop is written. */
static CRITICAL_SECTION g_buf_lock;
static int g_buf_lock_ready;
static CONDITION_VARIABLE g_buf_cv;
static volatile int g_camera_running;
GC_ERROR GC_CALLTYPE GCWritePort(PORT_HANDLE hPort, uint64_t iAddress, const void *pBuffer, size_t *piSize) {
    if (hPort != H_PORT) {
        probe_log("GCWritePort hPort=%p addr=0x%llx -> INVALID_HANDLE", hPort, (unsigned long long)iAddress);
        return GC_ERR_INVALID_HANDLE;
    }
    reg_desc_t *r = find_reg(iAddress);
    if (!r) {
        probe_log("GCWritePort hPort=%p addr=0x%llx -> INVALID_ADDRESS (unmapped)", hPort, (unsigned long long)iAddress);
        set_err(GC_ERR_INVALID_ADDRESS, "unmapped register address");
        return GC_ERR_INVALID_ADDRESS;
    }
    if (r->access_ro) {
        probe_log("WRITE %-28s addr=0x%llx -> ACCESS_DENIED (read-only)", r->name, (unsigned long long)r->addr);
        set_err(GC_ERR_ACCESS_DENIED, "register is read-only");
        return GC_ERR_ACCESS_DENIED;
    }
    size_t want = piSize ? *piSize : 0;
    size_t n = (want < r->size) ? want : r->size;
    if (pBuffer && n > 0) memcpy(r->storage, pBuffer, n);
    if (piSize) *piSize = n;

    r->log_count++;
    if (r->kind == REG_CMD) {
        note_buffer_burst_boundary(r->name); /* e.g. AcquisitionStart/AcquisitionStop */
        /* M4d: the GenApi AcquisitionStart/AcquisitionStop commands arm/
         * disarm the "camera" layer of the two-layer acquisition model
         * (DSStartAcquisition arms the stream layer separately). The
         * duplicate AcquisitionStart writes Kineo has been observed to
         * send are harmless/idempotent by construction -- setting the
         * same flag twice changes nothing. */
        if (r->storage == &g_acq_start_cmd) {
            EnterCriticalSection(&g_buf_lock);
            g_camera_running = 1;
            WakeAllConditionVariable(&g_buf_cv);
            LeaveCriticalSection(&g_buf_lock);
        } else if (r->storage == &g_acq_stop_cmd) {
            EnterCriticalSection(&g_buf_lock);
            g_camera_running = 0;
            WakeAllConditionVariable(&g_buf_cv);
            LeaveCriticalSection(&g_buf_lock);
        }
        if (r->log_count <= REG_LOG_LIMIT) probe_log("COMMAND %-28s addr=0x%llx", r->name, (unsigned long long)r->addr);
        else if (r->log_count == REG_LOG_LIMIT + 1) probe_log("COMMAND %-28s further invocations suppressed (counting silently)", r->name);
        /* auto-clear: GenApi's IsDone check (register != CommandValue) sees completion immediately */
        *(int32_t*)r->storage = 0;
    } else {
        if (r->log_count <= REG_LOG_LIMIT) reg_log_value(r, "WRITE");
        else if (r->log_count == REG_LOG_LIMIT + 1) probe_log("WRITE %-28s further writes suppressed (counting silently)", r->name);
        if (r->storage == &g_width || r->storage == &g_height) recompute_payload_size();
    }
    return GC_ERR_SUCCESS;
}
/* M3b: serve the GenICam XML via URL_SCHEME_FILE (GenTL v1.5, officially
 * spec-sanctioned: "The XML can be read from the local hard drive") --
 * much simpler than URL_SCHEME_LOCAL's register-mapped chunked reads via
 * GCReadPort, and requires no GCReadPort/GCWritePort work for the XML
 * itself. Only exposed on H_PORT (the RemoteDevice port) -- real producers
 * conventionally put the camera's feature XML on the remote port, not the
 * local Device port, and the M2 real-Kineo trace showed both are queried
 * separately, so H_DEV correctly reports 0 URLs. */
static char g_xml_file_url[MAX_PATH + 16];
static uint64_t g_xml_file_size = 0;

static void resolve_xml_url(HINSTANCE hinst) {
    char dll_path[MAX_PATH];
    DWORD n = GetModuleFileNameA(hinst, dll_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { g_xml_file_url[0] = '\0'; return; }
    char *last_slash = strrchr(dll_path, '\\');
    if (last_slash) *(last_slash + 1) = '\0';
    char xml_path[MAX_PATH];
    snprintf(xml_path, sizeof xml_path, "%skineo_bridge_m5.xml", dll_path);

    HANDLE f = CreateFileA(xml_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { g_xml_file_url[0] = '\0'; return; }
    LARGE_INTEGER sz;
    if (GetFileSizeEx(f, &sz)) {
        g_xml_file_size = (uint64_t)sz.QuadPart;
        g_xml_data = (uint8_t *)malloc((size_t)sz.QuadPart);
        if (g_xml_data) {
            DWORD read_bytes = 0;
            if (ReadFile(f, g_xml_data, (DWORD)sz.QuadPart, &read_bytes, NULL) && read_bytes == (DWORD)sz.QuadPart) {
                g_xml_data_len = read_bytes;
            } else {
                free(g_xml_data);
                g_xml_data = NULL;
                g_xml_data_len = 0;
            }
        }
    }
    CloseHandle(f);

    /* local:<filename>;<hex address>;<hex length> -- standard GenTL local-URL
     * convention (URL_SCHEME_LOCAL), hex without a 0x prefix. */
    snprintf(g_xml_file_url, sizeof g_xml_file_url, "local:kineo_bridge_m5.xml;%llX;%zX",
              (unsigned long long)XML_VIRTUAL_BASE, g_xml_data_len);
}

GC_ERROR GC_CALLTYPE GCGetPortURL(PORT_HANDLE hPort, char *sURL, size_t *piSize) {
    probe_log("GCGetPortURL hPort=%p", hPort);
    if (hPort != H_PORT) {
        probe_log("  -> NOT_AVAILABLE (no URL on this port)");
        set_err(GC_ERR_NOT_AVAILABLE, "no URL on this port");
        return GC_ERR_NOT_AVAILABLE;
    }
    GC_ERROR rc = info_string(NULL, sURL, piSize, g_xml_file_url);
    probe_log("  -> rc=%d url=%s", rc, g_xml_file_url);
    return rc;
}
GC_ERROR GC_CALLTYPE GCGetNumPortURLs(PORT_HANDLE hPort, uint32_t *piNumURLs) {
    probe_log("GCGetNumPortURLs hPort=%p", hPort);
    if (piNumURLs) *piNumURLs = (hPort == H_PORT && g_xml_file_url[0]) ? 1 : 0;
    probe_log("  -> count=%u", piNumURLs ? *piNumURLs : 0);
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE GCGetPortURLInfo(PORT_HANDLE hPort, uint32_t iURLIndex, URL_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    probe_log("GCGetPortURLInfo hPort=%p idx=%u cmd=%d", hPort, iURLIndex, (int)iInfoCmd);
    if (hPort != H_PORT || iURLIndex != 0) {
        set_err(GC_ERR_INVALID_INDEX, "no such URL");
        return GC_ERR_INVALID_INDEX;
    }
    GC_ERROR rc;
    switch (iInfoCmd) {
        case URL_INFO_URL:              rc = info_string(piType, pBuffer, piSize, g_xml_file_url); break;
        case URL_INFO_SCHEMA_VER_MAJOR:  rc = info_i32(piType, pBuffer, piSize, 1); break;
        case URL_INFO_SCHEMA_VER_MINOR:  rc = info_i32(piType, pBuffer, piSize, 1); break;
        case URL_INFO_FILE_VER_MAJOR:    rc = info_i32(piType, pBuffer, piSize, 1); break;
        case URL_INFO_FILE_VER_MINOR:    rc = info_i32(piType, pBuffer, piSize, 0); break;
        case URL_INFO_FILE_VER_SUBMINOR: rc = info_i32(piType, pBuffer, piSize, 0); break;
        case URL_INFO_FILE_SIZE:         rc = info_u64(piType, pBuffer, piSize, g_xml_file_size); break;
        case URL_INFO_SCHEME:            rc = info_i32(piType, pBuffer, piSize, URL_SCHEME_LOCAL); break;
        case URL_INFO_FILENAME:          rc = info_string(piType, pBuffer, piSize, "kineo_bridge_m5.xml"); break;
        case URL_INFO_FILE_REGISTER_ADDRESS: rc = info_u64(piType, pBuffer, piSize, XML_VIRTUAL_BASE); break;
        default:
            probe_log("  -> NOT_AVAILABLE (unrecognized URL_INFO cmd)");
            set_err(GC_ERR_NOT_AVAILABLE, "URL_INFO command not available");
            return GC_ERR_NOT_AVAILABLE;
    }
    probe_log("  -> rc=%d", rc);
    return rc;
}
/* M3a: GCGetPortInfo is called by real ids_peak/Kineo directly on the raw
 * DEV_HANDLE (confirmed via M2 smoke-test trace -- DevGetPort is never
 * called at all), matching GenTL's "local port" concept: every module
 * handle (TL/IF/Dev) can itself serve as a PORT_HANDLE for that module's
 * own local port/description, distinct from the *remote device* port
 * DevGetPort() would return. We support both: H_DEV as the local "Device"
 * port, H_PORT (from DevGetPort) as the "RemoteDevice" port. Values below
 * follow GenTL_v1_5.h's PORT_INFO_CMD_LIST semantics exactly. */
GC_ERROR GC_CALLTYPE GCGetPortInfo(PORT_HANDLE hPort, PORT_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    probe_log("GCGetPortInfo hPort=%p cmd=%d", hPort, (int)iInfoCmd);
    const char *module_str;
    const char *port_name;
    const char *port_id;
    if (hPort == H_DEV) {
        module_str = "Device";
        port_name = "Device";
        port_id = "KineoBridgeDevice0_LocalPort";
    } else if (hPort == H_PORT) {
        module_str = "RemoteDevice";
        port_name = "Remote";
        port_id = "KineoBridgeDevice0_RemotePort";
    } else {
        probe_log("  -> INVALID_HANDLE (unrecognized port handle)");
        return GC_ERR_INVALID_HANDLE;
    }
    GC_ERROR rc;
    switch (iInfoCmd) {
        case PORT_INFO_ID:            rc = info_string(piType, pBuffer, piSize, port_id); break;
        case PORT_INFO_VENDOR:        rc = info_string(piType, pBuffer, piSize, DEV_VENDOR_STR); break;
        case PORT_INFO_MODEL:         rc = info_string(piType, pBuffer, piSize, DEV_MODEL_STR); break;
        case PORT_INFO_TLTYPE:        rc = info_string(piType, pBuffer, piSize, TLTYPE_U3V); break;
        case PORT_INFO_MODULE:        rc = info_string(piType, pBuffer, piSize, module_str); break;
        case PORT_INFO_LITTLE_ENDIAN: rc = info_bool8(piType, pBuffer, piSize, 1); break; /* x64 is LE */
        case PORT_INFO_BIG_ENDIAN:    rc = info_bool8(piType, pBuffer, piSize, 0); break;
        case PORT_INFO_ACCESS_READ:   rc = info_bool8(piType, pBuffer, piSize, 1); break;
        case PORT_INFO_ACCESS_WRITE:  rc = info_bool8(piType, pBuffer, piSize, 1); break;
        case PORT_INFO_ACCESS_NA:     rc = info_bool8(piType, pBuffer, piSize, 0); break;
        case PORT_INFO_ACCESS_NI:     rc = info_bool8(piType, pBuffer, piSize, 0); break;
        case PORT_INFO_VERSION:       rc = info_string(piType, pBuffer, piSize, "1.0"); break;
        case PORT_INFO_PORTNAME:      rc = info_string(piType, pBuffer, piSize, port_name); break;
        default:
            probe_log("  -> NOT_AVAILABLE (unrecognized PORT_INFO cmd)");
            set_err(GC_ERR_NOT_AVAILABLE, "PORT_INFO command not available");
            return GC_ERR_NOT_AVAILABLE;
    }
    probe_log("  -> rc=%d piType=%d *piSize=%zu", rc, piType?*piType:-1, piSize?*piSize:0);
    return rc;
}

/* ================= TL* ================= */

GC_ERROR GC_CALLTYPE TLOpen(TL_HANDLE *phTL) {
    probe_log("TLOpen");
    if (!g_lib_init) { set_err(GC_ERR_NOT_INITIALIZED, "GCInitLib not called"); return GC_ERR_NOT_INITIALIZED; }
    if (!phTL) return GC_ERR_INVALID_PARAMETER;
    *phTL = H_TL;
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE TLClose(TL_HANDLE hTL) {
    probe_log("TLClose");
    if (hTL != H_TL) return GC_ERR_INVALID_HANDLE;
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE TLGetInfo(TL_HANDLE hTL, TL_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    probe_log("TLGetInfo cmd=%d", (int)iInfoCmd);
    if (hTL != H_TL) return GC_ERR_INVALID_HANDLE;
    return tl_info(iInfoCmd, piType, pBuffer, piSize);
}
GC_ERROR GC_CALLTYPE TLGetNumInterfaces(TL_HANDLE hTL, uint32_t *piNumIfaces) {
    probe_log("TLGetNumInterfaces -> 1");
    if (hTL != H_TL) return GC_ERR_INVALID_HANDLE;
    if (piNumIfaces) *piNumIfaces = 1;
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE TLGetInterfaceID(TL_HANDLE hTL, uint32_t iIndex, char *sID, size_t *piSize) {
    probe_log("TLGetInterfaceID index=%u", iIndex);
    if (hTL != H_TL) return GC_ERR_INVALID_HANDLE;
    if (iIndex != 0) { set_err(GC_ERR_INVALID_INDEX, "only interface 0 exists"); return GC_ERR_INVALID_INDEX; }
    return info_string(NULL, sID, piSize, IFACE_ID_STR);
}
GC_ERROR GC_CALLTYPE TLGetInterfaceInfo(TL_HANDLE hTL, const char *sIfaceID, INTERFACE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    probe_log("TLGetInterfaceInfo id=%s cmd=%d", sIfaceID ? sIfaceID : "(null)", (int)iInfoCmd);
    if (hTL != H_TL) return GC_ERR_INVALID_HANDLE;
    if (!sIfaceID || strcmp(sIfaceID, IFACE_ID_STR) != 0) { set_err(GC_ERR_INVALID_ID, "unknown interface id"); return GC_ERR_INVALID_ID; }
    switch (iInfoCmd) {
        case INTERFACE_INFO_ID:          return info_string(piType, pBuffer, piSize, IFACE_ID_STR);
        case INTERFACE_INFO_DISPLAYNAME: return info_string(piType, pBuffer, piSize, IFACE_DISPLAY);
        case INTERFACE_INFO_TLTYPE:      return info_string(piType, pBuffer, piSize, TLTYPE_U3V);
        default: set_err(GC_ERR_NOT_AVAILABLE, "iface info not available"); return GC_ERR_NOT_AVAILABLE;
    }
}
GC_ERROR GC_CALLTYPE TLOpenInterface(TL_HANDLE hTL, const char *sIfaceID, IF_HANDLE *phIface) {
    probe_log("TLOpenInterface id=%s", sIfaceID ? sIfaceID : "(null)");
    if (hTL != H_TL) return GC_ERR_INVALID_HANDLE;
    if (!sIfaceID || strcmp(sIfaceID, IFACE_ID_STR) != 0) { set_err(GC_ERR_INVALID_ID, "unknown interface id"); return GC_ERR_INVALID_ID; }
    if (!phIface) return GC_ERR_INVALID_PARAMETER;
    *phIface = H_IF;
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE TLUpdateInterfaceList(TL_HANDLE hTL, bool8_t *pbChanged, uint64_t iTimeout) {
    probe_log("TLUpdateInterfaceList timeout=%llu", (unsigned long long)iTimeout);
    if (hTL != H_TL) return GC_ERR_INVALID_HANDLE;
    if (pbChanged) *pbChanged = 0;
    return GC_ERR_SUCCESS;
}

/* ================= IF* ================= */

GC_ERROR GC_CALLTYPE IFClose(IF_HANDLE hIface) {
    probe_log("IFClose");
    if (hIface != H_IF) return GC_ERR_INVALID_HANDLE;
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE IFGetInfo(IF_HANDLE hIface, INTERFACE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    probe_log("IFGetInfo cmd=%d", (int)iInfoCmd);
    if (hIface != H_IF) return GC_ERR_INVALID_HANDLE;
    switch (iInfoCmd) {
        case INTERFACE_INFO_ID:          return info_string(piType, pBuffer, piSize, IFACE_ID_STR);
        case INTERFACE_INFO_DISPLAYNAME: return info_string(piType, pBuffer, piSize, IFACE_DISPLAY);
        case INTERFACE_INFO_TLTYPE:      return info_string(piType, pBuffer, piSize, TLTYPE_U3V);
        default: set_err(GC_ERR_NOT_AVAILABLE, "iface info not available"); return GC_ERR_NOT_AVAILABLE;
    }
}
GC_ERROR GC_CALLTYPE IFGetNumDevices(IF_HANDLE hIface, uint32_t *piNumDevices) {
    probe_log("IFGetNumDevices -> 1");
    if (hIface != H_IF) return GC_ERR_INVALID_HANDLE;
    if (piNumDevices) *piNumDevices = 1;
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE IFGetDeviceID(IF_HANDLE hIface, uint32_t iIndex, char *sID, size_t *piSize) {
    probe_log("IFGetDeviceID index=%u", iIndex);
    if (hIface != H_IF) return GC_ERR_INVALID_HANDLE;
    if (iIndex != 0) { set_err(GC_ERR_INVALID_INDEX, "only device 0 exists"); return GC_ERR_INVALID_INDEX; }
    return info_string(NULL, sID, piSize, DEV_ID_STR);
}
GC_ERROR GC_CALLTYPE IFUpdateDeviceList(IF_HANDLE hIface, bool8_t *pbChanged, uint64_t iTimeout) {
    probe_log("IFUpdateDeviceList timeout=%llu", (unsigned long long)iTimeout);
    if (hIface != H_IF) return GC_ERR_INVALID_HANDLE;
    if (pbChanged) *pbChanged = 0;
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE IFGetDeviceInfo(IF_HANDLE hIface, const char *sDeviceID, DEVICE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    probe_log("IFGetDeviceInfo id=%s cmd=%d", sDeviceID ? sDeviceID : "(null)", (int)iInfoCmd);
    if (hIface != H_IF) return GC_ERR_INVALID_HANDLE;
    if (!sDeviceID || strcmp(sDeviceID, DEV_ID_STR) != 0) { set_err(GC_ERR_INVALID_ID, "unknown device id"); return GC_ERR_INVALID_ID; }
    switch (iInfoCmd) {
        case DEVICE_INFO_ID:                  return info_string(piType, pBuffer, piSize, DEV_ID_STR);
        case DEVICE_INFO_VENDOR:              return info_string(piType, pBuffer, piSize, DEV_VENDOR_STR);
        case DEVICE_INFO_MODEL:               return info_string(piType, pBuffer, piSize, DEV_MODEL_STR);
        case DEVICE_INFO_TLTYPE:              return info_string(piType, pBuffer, piSize, TLTYPE_U3V);
        case DEVICE_INFO_DISPLAYNAME:         return info_string(piType, pBuffer, piSize, DEV_DISPLAY_STR);
        case DEVICE_INFO_ACCESS_STATUS:       return info_i32(piType, pBuffer, piSize, DEVICE_ACCESS_STATUS_READWRITE);
        case DEVICE_INFO_USER_DEFINED_NAME:   return info_string(piType, pBuffer, piSize, "KineoBridge");
        case DEVICE_INFO_SERIAL_NUMBER:       return info_string(piType, pBuffer, piSize, DEV_SERIAL_STR);
        case DEVICE_INFO_VERSION:             return info_string(piType, pBuffer, piSize, DEV_VERSION_STR);
        case DEVICE_INFO_TIMESTAMP_FREQUENCY: return info_u64(piType, pBuffer, piSize, 1000000000ULL);
        default: set_err(GC_ERR_NOT_AVAILABLE, "device info not available"); return GC_ERR_NOT_AVAILABLE;
    }
}
GC_ERROR GC_CALLTYPE IFOpenDevice(IF_HANDLE hIface, const char *sDeviceID, DEVICE_ACCESS_FLAGS iOpenFlags, DEV_HANDLE *phDevice) {
    probe_log("IFOpenDevice id=%s flags=%d", sDeviceID ? sDeviceID : "(null)", (int)iOpenFlags);
    if (hIface != H_IF) return GC_ERR_INVALID_HANDLE;
    if (!sDeviceID || strcmp(sDeviceID, DEV_ID_STR) != 0) { set_err(GC_ERR_INVALID_ID, "unknown device id"); return GC_ERR_INVALID_ID; }
    if (!phDevice) return GC_ERR_INVALID_PARAMETER;
    *phDevice = H_DEV;
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE IFGetParentTL(IF_HANDLE hIface, TL_HANDLE *phSystem) {
    probe_log("IFGetParentTL");
    if (hIface != H_IF) return GC_ERR_INVALID_HANDLE;
    if (!phSystem) return GC_ERR_INVALID_PARAMETER;
    *phSystem = H_TL;
    return GC_ERR_SUCCESS;
}

/* ================= Dev* ================= */

GC_ERROR GC_CALLTYPE DevGetPort(DEV_HANDLE hDevice, PORT_HANDLE *phRemoteDevice) {
    probe_log("DevGetPort");
    if (hDevice != H_DEV) return GC_ERR_INVALID_HANDLE;
    if (!phRemoteDevice) return GC_ERR_INVALID_PARAMETER;
    /* Return the remote-device port handle; port register/XML access is stubbed
     * until M3, but the handle itself is valid so consumers can proceed. */
    *phRemoteDevice = H_PORT;
    return GC_ERR_SUCCESS;
}
/* Pass 2 (workflow trace): DevGetNumDataStreams/DevGetDataStreamID/
 * DevOpenDataStream now report exactly one, real, stable data stream --
 * per investigation/kineo-workflow-trace.md's exact-minimum-implementation
 * finding. No buffers/events/acquisition/frames/IPC yet -- purpose is only
 * to observe what real Kineo does next once a stream genuinely exists. */
GC_ERROR GC_CALLTYPE DevGetNumDataStreams(DEV_HANDLE hDevice, uint32_t *piNumDataStreams) {
    probe_log("DevGetNumDataStreams -> 1  [Pass 2]");
    if (hDevice != H_DEV) return GC_ERR_INVALID_HANDLE;
    if (piNumDataStreams) *piNumDataStreams = 1;
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE DevGetDataStreamID(DEV_HANDLE hDevice, uint32_t iIndex, char *sDataStreamID, size_t *piSize) {
    probe_log("DevGetDataStreamID index=%u  [Pass 2]", iIndex);
    if (hDevice != H_DEV) return GC_ERR_INVALID_HANDLE;
    if (iIndex != 0) { set_err(GC_ERR_INVALID_INDEX, "only stream 0 exists"); return GC_ERR_INVALID_INDEX; }
    return info_string(NULL, sDataStreamID, piSize, DS_STREAM_ID);
}
GC_ERROR GC_CALLTYPE DevOpenDataStream(DEV_HANDLE hDevice, const char *sDataStreamID, DS_HANDLE *phDataStream) {
    probe_log("DevOpenDataStream id=%s  [Pass 2]", sDataStreamID ? sDataStreamID : "(null)");
    if (hDevice != H_DEV) return GC_ERR_INVALID_HANDLE;
    if (!sDataStreamID || strcmp(sDataStreamID, DS_STREAM_ID) != 0) {
        set_err(GC_ERR_INVALID_ID, "unknown data stream id");
        return GC_ERR_INVALID_ID;
    }
    if (!phDataStream) return GC_ERR_INVALID_PARAMETER;
    *phDataStream = H_DS;
    /* M5: start the WSL bridge connection here, not lazily at
     * DSStartAcquisition. Real Kineo has been observed opening the data
     * stream tens of seconds before ever starting acquisition, but its
     * *first* EventGetData after AcquisitionStart uses a short (~150ms)
     * timeout -- far shorter than the real bridge's one-time handshake
     * (TCP connect + open the real camera via Aravis/usbipd + configure +
     * start, ~1-1.5s). Starting here instead gives that handshake the
     * whole open-to-acquisition window to finish, so the stream is
     * already flowing well before Kineo's first tight-timeout poll.
     * Idempotent (a no-op on the 2nd/3rd DevOpenDataStream pass observed
     * in practice) and a no-op entirely in synthetic mode. By this point
     * ExposureTime/Gain/BlackLevel/AcquisitionFrameRate have already been
     * written by Kineo (observed immediately before this call each pass),
     * so the forwarded CONFIGURE values are Kineo's real ones, not
     * defaults. */
    wsl_bridge_start(DEV_SERIAL_STR, g_exposure_time, g_gain, g_black_level, g_acq_frame_rate);
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE DevGetInfo(DEV_HANDLE hDevice, DEVICE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    probe_log("DevGetInfo cmd=%d", (int)iInfoCmd);
    if (hDevice != H_DEV) return GC_ERR_INVALID_HANDLE;
    switch (iInfoCmd) {
        case DEVICE_INFO_ID:                  return info_string(piType, pBuffer, piSize, DEV_ID_STR);
        case DEVICE_INFO_VENDOR:              return info_string(piType, pBuffer, piSize, DEV_VENDOR_STR);
        case DEVICE_INFO_MODEL:               return info_string(piType, pBuffer, piSize, DEV_MODEL_STR);
        case DEVICE_INFO_TLTYPE:              return info_string(piType, pBuffer, piSize, TLTYPE_U3V);
        case DEVICE_INFO_DISPLAYNAME:         return info_string(piType, pBuffer, piSize, DEV_DISPLAY_STR);
        case DEVICE_INFO_ACCESS_STATUS:       return info_i32(piType, pBuffer, piSize, DEVICE_ACCESS_STATUS_READWRITE);
        case DEVICE_INFO_USER_DEFINED_NAME:   return info_string(piType, pBuffer, piSize, "KineoBridge");
        case DEVICE_INFO_SERIAL_NUMBER:       return info_string(piType, pBuffer, piSize, DEV_SERIAL_STR);
        case DEVICE_INFO_VERSION:             return info_string(piType, pBuffer, piSize, DEV_VERSION_STR);
        case DEVICE_INFO_TIMESTAMP_FREQUENCY: return info_u64(piType, pBuffer, piSize, 1000000000ULL);
        default: set_err(GC_ERR_NOT_AVAILABLE, "device info not available"); return GC_ERR_NOT_AVAILABLE;
    }
}
GC_ERROR GC_CALLTYPE DevClose(DEV_HANDLE hDevice) {
    note_buffer_burst_boundary("DevClose");
    probe_log("DevClose");
    if (hDevice != H_DEV) return GC_ERR_INVALID_HANDLE;
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE DevGetParentIF(DEV_HANDLE hDevice, IF_HANDLE *phIface) {
    probe_log("DevGetParentIF");
    if (hDevice != H_DEV) return GC_ERR_INVALID_HANDLE;
    if (!phIface) return GC_ERR_INVALID_PARAMETER;
    *phIface = H_IF;
    return GC_ERR_SUCCESS;
}

/* ================= GC* stacked/event (M1 additions — stubs) ================= */

GC_ERROR GC_CALLTYPE GCReadPortStacked(PORT_HANDLE hPort, void *pEntries, size_t iNumEntries) {
    (void)hPort; (void)pEntries; (void)iNumEntries;
    probe_log("GCReadPortStacked  [STUB -> NOT_IMPLEMENTED]");
    set_err(GC_ERR_NOT_IMPLEMENTED, "GCReadPortStacked not implemented");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCWritePortStacked(PORT_HANDLE hPort, void *pEntries, size_t iNumEntries) {
    (void)hPort; (void)pEntries; (void)iNumEntries;
    probe_log("GCWritePortStacked  [STUB -> NOT_IMPLEMENTED]");
    set_err(GC_ERR_NOT_IMPLEMENTED, "GCWritePortStacked not implemented");
    return GC_ERR_NOT_IMPLEMENTED;
}
/* ================= M4c-2: standard EVENT_NEW_BUFFER registration =================
 * Confirmed via direct empirical testing (calling the official
 * ids_peak Python bindings' DataStream.StartAcquisition() with no Kineo
 * involved at all reproduces the identical GCRegisterEvent(iEventID=1)
 * call before ever reaching the real DSStartAcquisition GenTL function)
 * and cross-checked against a careful, verbatim re-fetch of the actual
 * EMVA GenTL_v1_5.h text: iEventID=1 is the standard, public
 * EVENT_NEW_BUFFER (see gentl_v2.h's corrected EVENT_TYPE_LIST values --
 * an earlier bad web-fetch had these wrong and was never independently
 * re-verified before being trusted). This is not IDS-specific or
 * private; it's exactly what every correctly-behaving GenTL producer is
 * supposed to support. Only one DataStream and one event type
 * (EVENT_NEW_BUFFER) are supported this pass -- a single global slot is
 * sufficient; duplicate registration is rejected. */
#define EVENT_MAGIC 0x4B455646u /* "KEVF" */

typedef struct {
    uint32_t magic;
    EVENT_TYPE type;        /* always EVENT_NEW_BUFFER for now */
    DS_HANDLE owner_ds;
    int killed;             /* set by EventKill/GCUnregisterEvent/teardown; wakes blocked EventGetData */
    int refcount;           /* number of threads currently inside EventGetData referencing this object */
    int pending_free;       /* set when GCUnregisterEvent/teardown wants to free but a waiter still holds a ref */
    uint64_t num_fired;     /* cumulative EVENT_NEW_BUFFER deliveries since this event was registered */
} event_t;

/* M4d: unified under g_buf_lock/g_buf_cv (not a separate lock) -- the
 * event's "is there a completed item" predicate and its "killed" state
 * must be checked atomically together by EventGetData, and both the
 * worker and GCUnregisterEvent already need to touch buffer/queue state
 * under the same lock, so splitting them across two locks would only
 * invite ordering bugs for no benefit (only one event exists at a time
 * anyway). g_buf_lock/g_buf_lock_ready are declared with the rest of the
 * buffer-table state above. */
static event_t *g_ds_new_buffer_event = NULL; /* only one DS + one event type supported so far */
/* Tentative (uninitialized) declarations -- real definitions with the
 * rest of the final-tally counters further below; needed here since
 * GCRegisterEvent/GCUnregisterEvent (defined next) increment them. */
static uint64_t g_event_register_count;
static uint64_t g_event_unregister_count;

/* Defensive cleanup, mirroring free_all_buffers_defensive: free any
 * still-registered event at DSClose/DllMain teardown, since Kineo's own
 * shutdown has been observed to hard-kill the process without reaching
 * either path (per the M4b/M4c "process termination finding"). Safe
 * against a concurrently-blocked EventGetData: marks killed + broadcasts
 * first, only frees immediately if nobody is currently referencing it. */
static void free_event_defensive(const char *reason) {
    if (!g_buf_lock_ready) return;
    EnterCriticalSection(&g_buf_lock);
    event_t *ev = g_ds_new_buffer_event;
    if (ev) {
        g_ds_new_buffer_event = NULL;
        ev->killed = 1;
        WakeAllConditionVariable(&g_buf_cv);
        if (ev->refcount == 0) {
            probe_log("  [defensive cleanup: %s] freeing still-registered EVENT_NEW_BUFFER event (never unregistered)", reason);
            ev->magic = 0;
            free(ev);
        } else {
            ev->pending_free = 1;
        }
    }
    LeaveCriticalSection(&g_buf_lock);
}

GC_ERROR GC_CALLTYPE GCRegisterEvent(EVENTSRC_HANDLE hEventSrc, EVENT_TYPE iEventID, EVENT_HANDLE *phEvent) {
    {
        char fn_desc[48];
        snprintf(fn_desc, sizeof fn_desc, "GCRegisterEvent(id=%llu)", (unsigned long long)iEventID);
        note_buffer_burst_boundary(fn_desc);
    }
    if (hEventSrc == (EVENTSRC_HANDLE)H_DS && iEventID == EVENT_NEW_BUFFER) {
        if (!phEvent) {
            probe_log("GCRegisterEvent(DS, EVENT_NEW_BUFFER) -> INVALID_PARAMETER (NULL phEvent)");
            set_err(GC_ERR_INVALID_PARAMETER, "GCRegisterEvent: NULL phEvent");
            return GC_ERR_INVALID_PARAMETER;
        }
        EnterCriticalSection(&g_buf_lock);
        if (g_ds_new_buffer_event != NULL) {
            LeaveCriticalSection(&g_buf_lock);
            /* GenTL doesn't define a dedicated "already registered" error
             * code; GC_ERR_RESOURCE_IN_USE ("needed resource is already in
             * use") is the best-fit standard code, same one already used
             * for the analogous "buffer already queued" duplicate case. */
            probe_log("GCRegisterEvent(DS, EVENT_NEW_BUFFER) -> RESOURCE_IN_USE (already registered)");
            set_err(GC_ERR_RESOURCE_IN_USE, "GCRegisterEvent: EVENT_NEW_BUFFER already registered on this DataStream");
            return GC_ERR_RESOURCE_IN_USE;
        }
        event_t *ev = (event_t *)malloc(sizeof(event_t));
        if (!ev) {
            LeaveCriticalSection(&g_buf_lock);
            probe_log("GCRegisterEvent(DS, EVENT_NEW_BUFFER) -> OUT_OF_MEMORY");
            set_err(GC_ERR_OUT_OF_MEMORY, "GCRegisterEvent: malloc failed");
            return GC_ERR_OUT_OF_MEMORY;
        }
        ev->magic = EVENT_MAGIC;
        ev->type = EVENT_NEW_BUFFER;
        ev->owner_ds = H_DS;
        ev->killed = 0;
        ev->refcount = 0;
        ev->pending_free = 0;
        ev->num_fired = 0;
        g_ds_new_buffer_event = ev;
        g_event_register_count++;
        LeaveCriticalSection(&g_buf_lock);
        *phEvent = (EVENT_HANDLE)ev;
        probe_log("GCRegisterEvent(hEventSrc=DS, EVENT_NEW_BUFFER) -> SUCCESS, handle=%p", (void*)ev);
        set_err(GC_ERR_SUCCESS, "no error");
        return GC_ERR_SUCCESS;
    }
    probe_log("GCRegisterEvent hEventSrc=%p id=%llu  [STUB -> NOT_IMPLEMENTED] (unsupported module/event combination)",
              hEventSrc, (unsigned long long)iEventID);
    set_err(GC_ERR_NOT_IMPLEMENTED, "GCRegisterEvent not implemented for this module/event combination");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCUnregisterEvent(EVENTSRC_HANDLE hEventSrc, EVENT_TYPE iEventID) {
    if (hEventSrc == (EVENTSRC_HANDLE)H_DS && iEventID == EVENT_NEW_BUFFER) {
        EnterCriticalSection(&g_buf_lock);
        event_t *ev = g_ds_new_buffer_event;
        if (!ev) {
            LeaveCriticalSection(&g_buf_lock);
            probe_log("GCUnregisterEvent(DS, EVENT_NEW_BUFFER) -> INVALID_HANDLE (not currently registered)");
            set_err(GC_ERR_INVALID_HANDLE, "GCUnregisterEvent: EVENT_NEW_BUFFER not registered");
            return GC_ERR_INVALID_HANDLE;
        }
        /* Remove from the registry before freeing, so no concurrent
         * GCRegisterEvent/EventKill call can observe or touch it mid-free
         * -- prevents duplicate-free/use-after-free. Kineo has proven
         * this can be called from a different thread than the one that
         * registered/started acquisition -- fully safe here since
         * everything is under g_buf_lock, and the actual free is
         * deferred (pending_free) if an EventGetData is still blocked
         * referencing this exact object (refcount>0). */
        g_ds_new_buffer_event = NULL;
        ev->killed = 1;
        WakeAllConditionVariable(&g_buf_cv); /* wake any blocked EventGetData so it observes killed and returns */
        g_event_unregister_count++;
        int refs = ev->refcount;
        if (refs == 0) {
            ev->magic = 0;
            free(ev);
        } else {
            ev->pending_free = 1;
        }
        LeaveCriticalSection(&g_buf_lock);
        probe_log("GCUnregisterEvent(DS, EVENT_NEW_BUFFER) -> SUCCESS, %s",
                  (refs == 0) ? "freed" : "killed (free deferred: a waiter still holds it)");
        return GC_ERR_SUCCESS;
    }
    probe_log("GCUnregisterEvent hEventSrc=%p id=%llu  [STUB -> NOT_IMPLEMENTED]", hEventSrc, (unsigned long long)iEventID);
    set_err(GC_ERR_NOT_IMPLEMENTED, "GCUnregisterEvent not implemented for this module/event combination");
    return GC_ERR_NOT_IMPLEMENTED;
}

/* ================= M4b/M4c: producer-owned buffer objects =================
 * Kineo (per M4a's trace) calls DSAllocAndAnnounceBuffer only, never
 * DSAnnounceBuffer -- so our producer allocates and owns every buffer's
 * memory. Per GenTL semantics for producer-allocated buffers (the header
 * itself has no inline documentation on this point; this follows the
 * convention used by working GenTL producers): the producer must free
 * the memory itself, at the latest at DSRevokeBuffer/stream-close/
 * teardown -- the consumer never frees it.
 *
 * M4c-1/1b tried a fixed-size table (16, then 64) to discover Kineo's
 * "natural" buffer count -- it kept climbing linearly through both
 * ceilings with no sign of converging, meaning the fixed cap was only
 * ever measuring our own limit, not Kineo's real behavior. Direction
 * changed: this build removes the artificial count ceiling entirely.
 * We are optimizing for interoperability (let Kineo have as many real,
 * genuinely usable buffers as it wants), not for discovering its exact
 * chosen pool size -- the practical limit is now simply "can we still
 * malloc," which is the same limit any real GenTL producer has.
 *
 * Handle design: each buffer_t is individually malloc'd (never moved),
 * so its address is a permanently stable BUFFER_HANDLE regardless of how
 * many buffers exist or in what order they're freed. A separate,
 * growable array of *pointers* to these objects (g_buffers) is the
 * ownership/lookup registry -- growing/shrinking that outer array only
 * ever copies pointer values via realloc, never the buffer_t objects
 * themselves, so it can safely resize without invalidating any handle
 * Kineo is currently holding. A magic marker on each object still lets a
 * malformed/garbage handle fail validation cleanly (checked by pointer
 * *identity* against the registry, not by address-range arithmetic,
 * since objects can now be anywhere in the heap). */
#define BUFFER_MAGIC 0x4B425546u /* "KBUF" */

/* M4d: explicit lifecycle states, not inferred from pointer presence or
 * ad-hoc int flags. ANNOUNCED = allocated, not yet queued. QUEUED = in
 * the producer's input FIFO, waiting to be filled. FILLING = popped by
 * the worker, being written (briefly, lock-free). COMPLETED = filled and
 * awaiting delivery/already delivered, not yet requeued. */
typedef enum {
    BUF_STATE_ANNOUNCED = 0,
    BUF_STATE_QUEUED,
    BUF_STATE_FILLING,
    BUF_STATE_COMPLETED
} buf_state_t;

static const char *buf_state_name(buf_state_t s) {
    switch (s) {
        case BUF_STATE_ANNOUNCED: return "ANNOUNCED";
        case BUF_STATE_QUEUED:    return "QUEUED";
        case BUF_STATE_FILLING:   return "FILLING";
        case BUF_STATE_COMPLETED: return "COMPLETED";
        default:                  return "UNKNOWN";
    }
}

typedef struct {
    uint32_t magic;      /* BUFFER_MAGIC while live; object is freed (not reused) on revoke */
    uint32_t generation; /* monotonic id across the whole session, for logging */
    uint8_t *base;       /* producer-allocated memory (real, committed) */
    size_t capacity;     /* exact size requested at announce time */
    void *private_ptr;   /* pPrivate supplied by the consumer at announce */
    buf_state_t state;
    /* M4d: real per-buffer delivery metadata, reset on each requeue
     * (transition back to QUEUED) per BUFFER_INFO_SIZE_FILLED's official
     * "reset to 0 when placed into the Input Buffer Pool" semantics. */
    size_t filled_size;
    int is_incomplete;
    int new_data;
    uint64_t frame_id;
    uint64_t timestamp_ns;
} buffer_t;

/* Growable registry of live buffers -- pointers only, so realloc here
 * never moves a buffer_t object or invalidates a handle. */
static buffer_t **g_buffers = NULL;
static size_t g_buffers_count = 0;
static size_t g_buffers_capacity = 0;

static CRITICAL_SECTION g_buf_lock;
static int g_buf_lock_ready = 0;
/* M4d: single condition variable, broadcast on any change the worker
 * thread or a blocked EventGetData might care about (buffer queued,
 * armed/running state changed, frame completed, event killed/
 * unregistered, worker told to stop). A native Win32 primitive -- no
 * external runtime dependency. */
static CONDITION_VARIABLE g_buf_cv;

static uint32_t g_buf_generation_ctr = 0;
static uint64_t g_buf_alloc_count = 0;
static uint64_t g_buf_alloc_bytes = 0;
static uint64_t g_buf_revoke_count = 0;
static uint64_t g_buf_leaked_count = 0; /* freed defensively at teardown */
static uint64_t g_buf_queue_count = 0;  /* total successful DSQueueBuffer calls (initial + requeues) */
static uint64_t g_buf_requeue_count = 0; /* subset of the above that were COMPLETED->QUEUED requeues */
static int g_buf_max_queue_depth = 0;   /* highest queue depth observed */
static uint64_t g_frames_produced = 0;
static uint64_t g_events_delivered = 0;
static uint64_t g_eventgetdata_calls = 0;
static uint64_t g_queue_starvation_count = 0; /* worker wanted a buffer, none was queued */
static uint64_t g_acq_start_count = 0;
static uint64_t g_acq_stop_count = 0;
static uint64_t g_event_register_count = 0;
static uint64_t g_event_unregister_count = 0;

/* M4d: two-layer acquisition state, matching the real ordering Kineo has
 * demonstrated (DSStartAcquisition arms the stream; the separate GenApi
 * AcquisitionStart command arms the "camera"). Synthetic frame
 * production requires both. */
static volatile int g_ds_grabbing = 0;    /* stream_armed; also STREAM_INFO_IS_GRABBING truth */
static volatile int g_camera_running = 0; /* set by the AcquisitionStart/AcquisitionStop GenApi commands */
static volatile int g_worker_should_stop = 0;
static HANDLE g_worker_thread = NULL;
static ACQ_START_FLAGS g_acq_start_flags = 0;
static uint64_t g_acq_num_to_acquire = 0;

/* M4c-1b: log the first BUF_LOG_LIMIT allocations/queues in full, then
 * checkpoint at each power of two >= 16 (16, 32, 64, ... indefinitely --
 * no upper bound now that the count ceiling is gone), and log one clear
 * line at the exact moment Kineo moves on to a different function after
 * a burst. */
#define BUF_LOG_LIMIT 5
static int is_buf_checkpoint(uint64_t n) {
    if (n <= BUF_LOG_LIMIT) return 0; /* handled by the individual-log path instead */
    return n >= 16 && (n & (n - 1)) == 0; /* power of two */
}
static int g_buf_burst_active = 0;
static uint64_t g_buf_burst_start_seq = 0;   /* alloc count before this burst began */
static uint64_t g_buf_burst_start_bytes = 0; /* alloc bytes before this burst began */

static void note_buffer_burst_boundary(const char *next_fn_name) {
    if (!g_buf_burst_active) return;
    g_buf_burst_active = 0;
    probe_log("=== buffer alloc/queue burst ended: %llu buffer(s) this burst (alloc #%llu-#%llu), "
              "%llu bytes, max queue depth %d -- next call: %s ===",
              (unsigned long long)(g_buf_alloc_count - g_buf_burst_start_seq),
              (unsigned long long)(g_buf_burst_start_seq + 1), (unsigned long long)g_buf_alloc_count,
              (unsigned long long)(g_buf_alloc_bytes - g_buf_burst_start_bytes),
              g_buf_max_queue_depth, next_fn_name);
}

/* Explicit input-pool FIFO -- also growable via realloc. It holds plain
 * BUFFER_HANDLE values (not objects), so moving it on growth is always
 * safe. Caller must hold g_buf_lock for all access. */
static BUFFER_HANDLE *g_input_queue = NULL;
static size_t g_input_queue_count = 0;
static size_t g_input_queue_capacity = 0;

static void input_queue_push_locked(BUFFER_HANDLE h) {
    if (g_input_queue_count == g_input_queue_capacity) {
        size_t new_cap = g_input_queue_capacity ? g_input_queue_capacity * 2 : 64;
        BUFFER_HANDLE *grown = (BUFFER_HANDLE *)realloc(g_input_queue, new_cap * sizeof(BUFFER_HANDLE));
        if (!grown) return; /* extremely unlikely (tiny allocation); drop rather than crash */
        g_input_queue = grown;
        g_input_queue_capacity = new_cap;
    }
    g_input_queue[g_input_queue_count++] = h;
}

/* M4d: pop the head of the input FIFO (true queue order), for the
 * worker thread to consume. Returns NULL if empty. Caller must hold
 * g_buf_lock. */
static BUFFER_HANDLE input_queue_pop_locked(void) {
    if (g_input_queue_count == 0) return NULL;
    BUFFER_HANDLE h = g_input_queue[0];
    memmove(&g_input_queue[0], &g_input_queue[1], (g_input_queue_count - 1) * sizeof(BUFFER_HANDLE));
    g_input_queue_count--;
    return h;
}

/* M4d: completed-buffer FIFO -- what EventGetData delivers, in
 * completion order. Holds the exact fields EVENT_NEW_BUFFER_DATA needs,
 * captured at completion time (not re-derived from the buffer object
 * later, so it's correct even if the buffer gets revoked/requeued
 * before the consumer retrieves the event). Caller must hold
 * g_buf_lock. */
typedef struct {
    BUFFER_HANDLE handle;
    void *user_ptr;
} completed_item_t;

static completed_item_t *g_completed_queue = NULL;
static size_t g_completed_queue_count = 0;
static size_t g_completed_queue_capacity = 0;

static void completed_queue_push_locked(BUFFER_HANDLE h, void *user_ptr) {
    if (g_completed_queue_count == g_completed_queue_capacity) {
        size_t new_cap = g_completed_queue_capacity ? g_completed_queue_capacity * 2 : 64;
        completed_item_t *grown = (completed_item_t *)realloc(g_completed_queue, new_cap * sizeof(completed_item_t));
        if (!grown) return;
        g_completed_queue = grown;
        g_completed_queue_capacity = new_cap;
    }
    g_completed_queue[g_completed_queue_count].handle = h;
    g_completed_queue[g_completed_queue_count].user_ptr = user_ptr;
    g_completed_queue_count++;
}

static int completed_queue_pop_locked(completed_item_t *out) {
    if (g_completed_queue_count == 0) return 0;
    *out = g_completed_queue[0];
    memmove(&g_completed_queue[0], &g_completed_queue[1], (g_completed_queue_count - 1) * sizeof(completed_item_t));
    g_completed_queue_count--;
    return 1;
}

/* validate + return the object pointer, or NULL if the handle is bogus,
 * stale, or already revoked. Checked by identity against the live
 * registry (not address-range math, since objects can be anywhere in
 * the heap now) -- a malformed/garbage handle simply won't match
 * anything and is never dereferenced. Caller must hold g_buf_lock. */
static buffer_t *find_buffer_locked(BUFFER_HANDLE h) {
    for (size_t i = 0; i < g_buffers_count; i++) {
        if ((BUFFER_HANDLE)g_buffers[i] == h) {
            return (g_buffers[i]->magic == BUFFER_MAGIC) ? g_buffers[i] : NULL;
        }
    }
    return NULL;
}

/* Remove from the registry by identity (swap-with-last -- order doesn't
 * matter here, the separate g_input_queue tracks FIFO order for
 * queuing). Caller must hold g_buf_lock. Does not free the object. */
static void registry_remove_locked(buffer_t *b) {
    for (size_t i = 0; i < g_buffers_count; i++) {
        if (g_buffers[i] == b) {
            g_buffers[i] = g_buffers[--g_buffers_count];
            return;
        }
    }
}

/* Defensive cleanup: free any still-announced producer-owned buffers.
 * Called from DSClose and from DllMain's PROCESS_DETACH, so a test that
 * ends without a clean DSRevokeBuffer sequence (aborted run, crash-free
 * but sloppy teardown, etc.) cannot leak large amounts of memory
 * indefinitely. Idempotent -- safe to call more than once. */
static void free_all_buffers_defensive(const char *reason) {
    if (!g_buf_lock_ready) return;
    EnterCriticalSection(&g_buf_lock);
    size_t freed_here = 0;
    while (g_buffers_count > 0) {
        buffer_t *b = g_buffers[--g_buffers_count];
        if (freed_here < 5) {
            probe_log("  [defensive cleanup: %s] freeing buffer base=%p capacity=%zu (never revoked)",
                      reason, (void*)b->base, b->capacity);
        }
        free(b->base);
        free(b);
        g_buf_leaked_count++;
        freed_here++;
    }
    LeaveCriticalSection(&g_buf_lock);
    if (freed_here > 0) probe_log("  [defensive cleanup: %s] freed %llu still-announced buffer(s)", reason, (unsigned long long)freed_here);
}

/* ================= DS* ================= */

GC_ERROR GC_CALLTYPE DSAnnounceBuffer(DS_HANDLE hDataStream, void *pBuffer, size_t iSize, void *pPrivate, BUFFER_HANDLE *phBuffer) {
    (void)hDataStream; (void)pBuffer; (void)iSize; (void)pPrivate; (void)phBuffer;
    probe_log("DSAnnounceBuffer size=%zu  [STUB -> NOT_IMPLEMENTED]", iSize);
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSAnnounceBuffer not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSAllocAndAnnounceBuffer(DS_HANDLE hDataStream, size_t iBufferSize, void *pPrivate, BUFFER_HANDLE *phBuffer) {
    if (hDataStream != H_DS) {
        probe_log("DSAllocAndAnnounceBuffer hDataStream=%p -> INVALID_HANDLE (unknown stream)", hDataStream);
        return GC_ERR_INVALID_HANDLE;
    }
    if (iBufferSize == 0 || !phBuffer) {
        probe_log("DSAllocAndAnnounceBuffer size=%zu phBuffer=%p -> INVALID_PARAMETER", iBufferSize, (void*)phBuffer);
        set_err(GC_ERR_INVALID_PARAMETER, "DSAllocAndAnnounceBuffer: zero size or NULL phBuffer");
        return GC_ERR_INVALID_PARAMETER;
    }
    /* Not hard-coding rejection of unexpected sizes (per instructions) --
     * just logging clearly if it differs from the known PayloadSize. */
    if (iBufferSize != (size_t)g_payloadsize) {
        probe_log("  [note] requested size %zu differs from current PayloadSize %lld",
                  iBufferSize, (long long)g_payloadsize);
    }

    /* Real physical-memory safety valve (per instructions: "do not
     * deliberately drive the machine into instability"). Not an
     * arbitrary buffer-count ceiling -- a genuine system-memory check,
     * consulted before every allocation since it's a cheap single
     * syscall. Threshold is conservative (512MB free) precisely to stop
     * *before* the host becomes unstable, distinct from an ordinary
     * malloc failure. */
    MEMORYSTATUSEX mem_status;
    mem_status.dwLength = sizeof(mem_status);
    GlobalMemoryStatusEx(&mem_status);
    if (mem_status.ullAvailPhys < 512ULL * 1024 * 1024) {
        probe_log("DSAllocAndAnnounceBuffer size=%zu -> RESOURCE_EXHAUSTED "
                  "(SAFETY STOP: only %llu MB physical memory free, refusing to risk system instability)",
                  iBufferSize, (unsigned long long)(mem_status.ullAvailPhys / (1024 * 1024)));
        set_err(GC_ERR_RESOURCE_EXHAUSTED, "DSAllocAndAnnounceBuffer: refused, system memory pressure too high");
        return GC_ERR_RESOURCE_EXHAUSTED;
    }

    uint8_t *mem = (uint8_t *)malloc(iBufferSize);
    if (!mem) {
        int was_burst = g_buf_burst_active;
        probe_log("DSAllocAndAnnounceBuffer size=%zu -> OUT_OF_MEMORY (malloc failed -- this is the real, natural "
                  "limit, not an artificial ceiling)", iBufferSize);
        if (was_burst) {
            EnterCriticalSection(&g_buf_lock);
            g_buf_burst_active = 0;
            uint64_t count_this_burst = g_buf_alloc_count - g_buf_burst_start_seq;
            uint64_t bytes_this_burst = g_buf_alloc_bytes - g_buf_burst_start_bytes;
            LeaveCriticalSection(&g_buf_lock);
            probe_log("=== NATURAL MEMORY LIMIT REACHED: %llu buffer(s) successfully allocated this burst "
                      "(%llu bytes), buffer #%llu failed -- this is Kineo's real allocation attempt hitting "
                      "genuine system memory exhaustion, not a synthetic cap ===",
                      (unsigned long long)count_this_burst, (unsigned long long)bytes_this_burst,
                      (unsigned long long)(g_buf_alloc_count + 1));
        }
        set_err(GC_ERR_OUT_OF_MEMORY, "DSAllocAndAnnounceBuffer: malloc failed");
        return GC_ERR_OUT_OF_MEMORY;
    }
    buffer_t *b = (buffer_t *)malloc(sizeof(buffer_t));
    if (!b) {
        free(mem);
        probe_log("DSAllocAndAnnounceBuffer size=%zu -> OUT_OF_MEMORY (metadata alloc failed)", iBufferSize);
        set_err(GC_ERR_OUT_OF_MEMORY, "DSAllocAndAnnounceBuffer: metadata malloc failed");
        return GC_ERR_OUT_OF_MEMORY;
    }
    b->magic = BUFFER_MAGIC;
    b->base = mem;
    b->capacity = iBufferSize;
    b->private_ptr = pPrivate;
    b->state = BUF_STATE_ANNOUNCED;
    b->filled_size = 0;
    b->is_incomplete = 0;
    b->new_data = 0;
    b->frame_id = 0;
    b->timestamp_ns = 0;

    EnterCriticalSection(&g_buf_lock);
    b->generation = ++g_buf_generation_ctr;
    if (g_buffers_count == g_buffers_capacity) {
        size_t new_cap = g_buffers_capacity ? g_buffers_capacity * 2 : 64;
        buffer_t **grown = (buffer_t **)realloc(g_buffers, new_cap * sizeof(buffer_t *));
        if (!grown) {
            LeaveCriticalSection(&g_buf_lock);
            free(mem);
            free(b);
            probe_log("DSAllocAndAnnounceBuffer size=%zu -> OUT_OF_MEMORY (registry growth failed)", iBufferSize);
            set_err(GC_ERR_OUT_OF_MEMORY, "DSAllocAndAnnounceBuffer: registry realloc failed");
            return GC_ERR_OUT_OF_MEMORY;
        }
        g_buffers = grown;
        g_buffers_capacity = new_cap;
    }
    g_buffers[g_buffers_count++] = b;
    BUFFER_HANDLE h = (BUFFER_HANDLE)b;
    if (!g_buf_burst_active) {
        g_buf_burst_active = 1;
        g_buf_burst_start_seq = g_buf_alloc_count;
        g_buf_burst_start_bytes = g_buf_alloc_bytes;
    }
    g_buf_alloc_count++;
    g_buf_alloc_bytes += iBufferSize;
    uint64_t seq = g_buf_alloc_count;
    uint32_t gen = b->generation;
    LeaveCriticalSection(&g_buf_lock);

    *phBuffer = h;
    if (seq <= BUF_LOG_LIMIT) {
        probe_log("DSAllocAndAnnounceBuffer #%llu size=%zu pPrivate=%p -> handle=%p base=%p (gen=%u)",
                  (unsigned long long)seq, iBufferSize, pPrivate, h, (void*)mem, gen);
    } else if (seq == BUF_LOG_LIMIT + 1) {
        probe_log("DSAllocAndAnnounceBuffer #%llu ... further allocations logged only at checkpoints (16, 32, 64, ...)",
                  (unsigned long long)seq);
    } else if (is_buf_checkpoint(seq)) {
        probe_log("DSAllocAndAnnounceBuffer #%llu (checkpoint) total_bytes=%llu avail_phys=%llu MB",
                  (unsigned long long)seq, (unsigned long long)g_buf_alloc_bytes,
                  (unsigned long long)(mem_status.ullAvailPhys / (1024 * 1024)));
    }
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE DSRevokeBuffer(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, void **pBuffer, void **pPrivate) {
    if (hDataStream != H_DS) {
        probe_log("DSRevokeBuffer hDataStream=%p -> INVALID_HANDLE (unknown stream)", hDataStream);
        return GC_ERR_INVALID_HANDLE;
    }
    EnterCriticalSection(&g_buf_lock);
    buffer_t *b = find_buffer_locked(hBuffer);
    if (!b) {
        LeaveCriticalSection(&g_buf_lock);
        probe_log("DSRevokeBuffer handle=%p -> INVALID_HANDLE (malformed or already-revoked handle)", hBuffer);
        set_err(GC_ERR_INVALID_HANDLE, "DSRevokeBuffer: invalid or stale buffer handle");
        return GC_ERR_INVALID_HANDLE;
    }
    /* Per GenTL semantics, a currently-queued buffer belongs to the
     * stream's input pool and must not be freed out from under it -- the
     * consumer must flush the queue (returning it to ANNOUNCED) before
     * revoking. (DSFlushQueue itself stays a logged stub until Kineo's
     * trace shows it's actually needed on the live path -- this guard
     * only prevents an unsafe free, it doesn't implement flushing.) */
    if (b->state == BUF_STATE_QUEUED || b->state == BUF_STATE_FILLING) {
        buf_state_t st = b->state;
        LeaveCriticalSection(&g_buf_lock);
        probe_log("DSRevokeBuffer handle=%p -> RESOURCE_IN_USE (buffer is %s, must be flushed/dequeued first)",
                  hBuffer, buf_state_name(st));
        set_err(GC_ERR_RESOURCE_IN_USE, "DSRevokeBuffer: cannot revoke a queued or filling buffer");
        return GC_ERR_RESOURCE_IN_USE;
    }
    /* Captured as plain integers, not pointers, before free() -- we only
     * ever hand this address value back for the caller's own bookkeeping
     * (per convention, see comment above buffer_t) or log it; we never
     * dereference it again after free(). */
    uintptr_t base_addr = (uintptr_t)b->base;
    void *priv = b->private_ptr;
    size_t capacity = b->capacity;
    buf_state_t was_state = b->state;
    /* Producer owns this memory (allocated via DSAllocAndAnnounceBuffer) --
     * free it now, ourselves. pBuffer/pPrivate are returned to the caller
     * for its own bookkeeping only, per common producer convention (the
     * official header has no inline doc on this point -- see comment
     * above the buffer_t definition). GCC's -Wuse-after-free still flags
     * the address value being handed back below even though it is never
     * dereferenced (only compared/logged by the caller) -- intentional,
     * silenced locally. The buffer_t object itself is also freed (it was
     * individually malloc'd, not part of a reusable static table) --
     * removed from the registry first so no other thread's lookup can
     * find it mid-free. */
    registry_remove_locked(b);
    free(b->base);
    free(b);
    g_buf_revoke_count++;
    LeaveCriticalSection(&g_buf_lock);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuse-after-free"
    if (pBuffer) *pBuffer = (void *)base_addr;
    probe_log("DSRevokeBuffer handle=%p base=%p capacity=%zu (was %s) -> freed, OK",
              hBuffer, (void *)base_addr, capacity, buf_state_name(was_state));
#pragma GCC diagnostic pop
    if (pPrivate) *pPrivate = priv;
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE DSQueueBuffer(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer) {
    if (hDataStream != H_DS) {
        probe_log("DSQueueBuffer hDataStream=%p -> INVALID_HANDLE (unknown stream)", hDataStream);
        return GC_ERR_INVALID_HANDLE;
    }
    EnterCriticalSection(&g_buf_lock);
    buffer_t *b = find_buffer_locked(hBuffer);
    if (!b) {
        LeaveCriticalSection(&g_buf_lock);
        probe_log("DSQueueBuffer handle=%p -> INVALID_HANDLE (malformed or stale buffer handle)", hBuffer);
        set_err(GC_ERR_INVALID_HANDLE, "DSQueueBuffer: invalid or stale buffer handle");
        return GC_ERR_INVALID_HANDLE;
    }
    if (b->state == BUF_STATE_QUEUED || b->state == BUF_STATE_FILLING) {
        /* Illegal duplicate queueing: already in the input pool or
         * actively owned by the acquisition engine. */
        LeaveCriticalSection(&g_buf_lock);
        probe_log("DSQueueBuffer handle=%p -> RESOURCE_IN_USE (already %s, illegal duplicate)",
                  hBuffer, buf_state_name(b->state));
        set_err(GC_ERR_RESOURCE_IN_USE, "DSQueueBuffer: buffer is already queued or filling");
        return GC_ERR_RESOURCE_IN_USE;
    }
    buf_state_t old_state = b->state;
    void *base = b->base;
    size_t capacity = b->capacity;
    int is_requeue = (old_state == BUF_STATE_COMPLETED);
    /* M4d: a requeued (previously COMPLETED) buffer gets its delivery
     * metadata cleared -- BUFFER_INFO_SIZE_FILLED/NEW_DATA are reset to
     * 0 "when the buffer is placed into the Input Buffer Pool" per the
     * official GenTL semantics; frame_id/timestamp are stale until the
     * worker fills it again. */
    b->filled_size = 0;
    b->new_data = 0;
    b->is_incomplete = 0;
    b->state = BUF_STATE_QUEUED;
    input_queue_push_locked(hBuffer);
    size_t depth = g_input_queue_count;
    if ((int)depth > g_buf_max_queue_depth) g_buf_max_queue_depth = (int)depth;
    g_buf_queue_count++;
    if (is_requeue) g_buf_requeue_count++;
    uint64_t qseq = g_buf_queue_count;
    WakeAllConditionVariable(&g_buf_cv); /* wake the worker if it was waiting for input */
    LeaveCriticalSection(&g_buf_lock);

    if (qseq <= BUF_LOG_LIMIT || is_requeue) {
        probe_log("DSQueueBuffer #%llu handle=%p base=%p capacity=%zu old_state=%s new_state=%s queue_depth=%zu%s",
                  (unsigned long long)qseq, hBuffer, base, capacity,
                  buf_state_name(old_state), buf_state_name(BUF_STATE_QUEUED), depth,
                  is_requeue ? " (REQUEUE)" : "");
    } else if (qseq == BUF_LOG_LIMIT + 1) {
        probe_log("DSQueueBuffer #%llu ... further queues logged only at checkpoints (16, 32, 64, ...)",
                  (unsigned long long)qseq);
    } else if (is_buf_checkpoint(qseq)) {
        probe_log("DSQueueBuffer #%llu (checkpoint) queue_depth=%zu", (unsigned long long)qseq, depth);
    }
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE DSGetParentDev(DS_HANDLE hDataStream, DEV_HANDLE *phDevice) {
    probe_log("DSGetParentDev  [STUB -> NOT_IMPLEMENTED]");
    (void)hDataStream;
    if (phDevice) *phDevice = H_DEV;
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSGetParentDev not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSFlushQueue(DS_HANDLE hDataStream, ACQ_QUEUE_TYPE iOperation) {
    (void)hDataStream; (void)iOperation;
    note_buffer_burst_boundary("DSFlushQueue");
    probe_log("DSFlushQueue  [STUB -> NOT_IMPLEMENTED]");
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSFlushQueue not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
/* M4d: synthetic frame parameters -- fixed for this build, matching the
 * real camera's native mode already confirmed throughout this project
 * (Width=1920, Height=1200, Mono8). */
#define FRAME_WIDTH 1920
#define FRAME_HEIGHT 1200
#define FRAME_PAYLOAD_SIZE 2304000
#define WORKER_FRAME_INTERVAL_MS 100 /* 10 FPS, per instructions -- not optimizing rate yet */

/* Deterministic, changing pattern: a moving vertical bright bar. Chosen
 * specifically (per instructions) so consecutive frames are visibly
 * different -- a constant/gray image wouldn't prove anything is
 * actually being produced per-frame. */
static void fill_synthetic_frame(uint8_t *dst, uint64_t frame_id) {
    const int bar_width = 60;
    const uint8_t bg = 32;
    const uint8_t fg = 255;
    int pos = (int)((frame_id * 12) % FRAME_WIDTH);
    for (int row = 0; row < FRAME_HEIGHT; row++) {
        uint8_t *row_ptr = dst + (size_t)row * FRAME_WIDTH;
        for (int col = 0; col < FRAME_WIDTH; col++) {
            int rel = (col - pos + FRAME_WIDTH) % FRAME_WIDTH;
            row_ptr[col] = (rel < bar_width) ? fg : bg;
        }
    }
}

static uint64_t query_timestamp_ns(void) {
    static LARGE_INTEGER freq;
    static int have_freq = 0;
    if (!have_freq) { QueryPerformanceFrequency(&freq); have_freq = 1; }
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return (uint64_t)((double)count.QuadPart / (double)freq.QuadPart * 1e9);
}

/* M4d: single producer worker. Created once (lazily, on the first
 * DSStartAcquisition) and lives until real teardown (DSClose/DllMain
 * PROCESS_DETACH) -- it just idles (blocked on g_buf_cv) whenever not
 * both stream_armed (g_ds_grabbing) and camera_running. Never holds
 * g_buf_lock while actually filling a buffer (per instructions). */
static DWORD WINAPI worker_thread_proc(LPVOID param) {
    (void)param;
    probe_log("[worker] thread started");
    uint64_t local_frame_ctr = 0;
    for (;;) {
        EnterCriticalSection(&g_buf_lock);
        for (;;) {
            if (g_worker_should_stop) { LeaveCriticalSection(&g_buf_lock); goto done; }
            if (g_ds_grabbing && g_camera_running && g_input_queue_count > 0) break;
            if (g_ds_grabbing && g_camera_running && g_input_queue_count == 0) g_queue_starvation_count++;
            SleepConditionVariableCS(&g_buf_cv, &g_buf_lock, INFINITE);
        }
        BUFFER_HANDLE h = input_queue_pop_locked();
        buffer_t *b = find_buffer_locked(h);
        if (!b) { LeaveCriticalSection(&g_buf_lock); continue; } /* shouldn't happen: FILLING/QUEUED blocks revoke */
        b->state = BUF_STATE_FILLING;
        uint8_t *base = b->base;
        size_t capacity = b->capacity;
        void *user_ptr = b->private_ptr;
        LeaveCriticalSection(&g_buf_lock);

        local_frame_ctr++;
        int ok;
        uint64_t ts_ns;
        if (g_frame_source_mode == FRAME_SOURCE_WSL) {
            /* M5: real frame from the WSL/Aravis bridge. Blocks (bounded)
             * for one not yet consumed -- the bridge's own ~10fps pacing
             * governs delivery rate here, not WORKER_FRAME_INTERVAL_MS. On
             * timeout (bridge not yet connected/streaming, or a stall) this
             * buffer completes as incomplete, same as the too-small-
             * capacity case below already did in M4d. */
            ok = (capacity >= FRAME_PAYLOAD_SIZE) &&
                 wsl_bridge_get_frame_blocking(base, capacity, &ts_ns, 2000);
            if (!ok) ts_ns = query_timestamp_ns();
        } else {
            ok = (capacity >= FRAME_PAYLOAD_SIZE);
            if (ok) fill_synthetic_frame(base, local_frame_ctr);
            ts_ns = query_timestamp_ns();
        }

        EnterCriticalSection(&g_buf_lock);
        b = find_buffer_locked(h);
        if (b && b->magic == BUFFER_MAGIC) {
            b->filled_size = ok ? FRAME_PAYLOAD_SIZE : 0;
            b->is_incomplete = ok ? 0 : 1;
            b->new_data = 1;
            b->frame_id = local_frame_ctr;
            b->timestamp_ns = ts_ns;
            b->state = BUF_STATE_COMPLETED;
            completed_queue_push_locked(h, user_ptr);
            g_frames_produced++;
            WakeAllConditionVariable(&g_buf_cv);
        }
        LeaveCriticalSection(&g_buf_lock);

        if (local_frame_ctr <= 10) {
            probe_log("[worker] frame #%llu completed handle=%p filled=%s bytes=%d",
                      (unsigned long long)local_frame_ctr, h, ok ? "OK" : "INCOMPLETE", ok ? FRAME_PAYLOAD_SIZE : 0);
        } else if (local_frame_ctr == 11) {
            probe_log("[worker] further frame completions logged only in the final tally");
        }
        if (g_frame_source_mode != FRAME_SOURCE_WSL) {
            /* Synthetic-source pacing only -- the WSL path's own blocking
             * receive already paces to the bridge's real frame rate. */
            Sleep(WORKER_FRAME_INTERVAL_MS);
        }
    }
done:
    probe_log("[worker] thread exiting, total frames produced this session=%llu", (unsigned long long)local_frame_ctr);
    return 0;
}

/* Stop and join the worker cleanly -- called only from real teardown
 * paths (DSClose / DllMain PROCESS_DETACH), never relied upon for
 * correctness during a hard-kill (per the established process-
 * termination finding). */
static void stop_worker_defensive(const char *reason) {
    if (!g_worker_thread) return;
    EnterCriticalSection(&g_buf_lock);
    g_worker_should_stop = 1;
    WakeAllConditionVariable(&g_buf_cv);
    LeaveCriticalSection(&g_buf_lock);
    probe_log("  [%s] waiting for worker thread to exit...", reason);
    WaitForSingleObject(g_worker_thread, 2000);
    CloseHandle(g_worker_thread);
    g_worker_thread = NULL;
}

GC_ERROR GC_CALLTYPE DSStartAcquisition(DS_HANDLE hDataStream, ACQ_START_FLAGS iStartFlags, uint64_t iNumToAcquire) {
    note_buffer_burst_boundary("DSStartAcquisition");
    const char *count_desc =
        (iNumToAcquire == 0) ? "0 = unspecified/default" :
        (iNumToAcquire == (uint64_t)-1 || iNumToAcquire > 0xFFFFFFFFULL) ? "looks like INFINITE_NUMBER/continuous" :
        "finite count";
    if (hDataStream != H_DS) {
        probe_log("DSStartAcquisition hDataStream=%p -> INVALID_HANDLE (unknown stream)", hDataStream);
        return GC_ERR_INVALID_HANDLE;
    }
    EnterCriticalSection(&g_buf_lock);
    size_t queued = g_input_queue_count;
    LeaveCriticalSection(&g_buf_lock);
    if (queued == 0) {
        probe_log("DSStartAcquisition iStartFlags=%d iNumToAcquire=%llu (%s) -> RESOURCE_EXHAUSTED (no queued buffers)",
                  (int)iStartFlags, (unsigned long long)iNumToAcquire, count_desc);
        set_err(GC_ERR_RESOURCE_EXHAUSTED, "DSStartAcquisition: no buffers in the input queue");
        return GC_ERR_RESOURCE_EXHAUSTED;
    }
    /* M4d: DSStartAcquisition arms the STREAM layer only (stream_armed).
     * Synthetic frame production also requires the separate GenApi
     * AcquisitionStart command to have armed the "camera" layer
     * (g_camera_running) -- see the GCWritePort COMMAND branch. This
     * matches the real ordering Kineo has demonstrated. */
    EnterCriticalSection(&g_buf_lock);
    g_acq_start_flags = iStartFlags;
    g_acq_num_to_acquire = iNumToAcquire;
    g_ds_grabbing = 1;
    g_acq_start_count++;
    if (!g_worker_thread) {
        g_worker_should_stop = 0;
        g_worker_thread = CreateThread(NULL, 0, worker_thread_proc, NULL, 0, NULL);
        /* M5: defensive fallback only -- the primary trigger is now in
         * DevOpenDataStream (see its comment), reached far earlier so the
         * bridge handshake has time to finish before Kineo's first tight-
         * timeout EventGetData. wsl_bridge_start() is idempotent, so this
         * is a no-op in the normal case; it only matters if some caller
         * reaches DSStartAcquisition without DevOpenDataStream first. */
        wsl_bridge_start(DEV_SERIAL_STR, g_exposure_time, g_gain, g_black_level, g_acq_frame_rate);
    }
    WakeAllConditionVariable(&g_buf_cv);
    LeaveCriticalSection(&g_buf_lock);
    probe_log("DSStartAcquisition iStartFlags=%d iNumToAcquire=%llu (%s) queued_buffers=%zu -> SUCCESS, grabbing=TRUE",
              (int)iStartFlags, (unsigned long long)iNumToAcquire, count_desc, queued);
    set_err(GC_ERR_SUCCESS, "no error");
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE DSStopAcquisition(DS_HANDLE hDataStream, ACQ_STOP_FLAGS iStopFlags) {
    (void)iStopFlags;
    note_buffer_burst_boundary("DSStopAcquisition");
    if (hDataStream != H_DS) {
        probe_log("DSStopAcquisition hDataStream=%p -> INVALID_HANDLE (unknown stream)", hDataStream);
        return GC_ERR_INVALID_HANDLE;
    }
    /* Does not join the worker thread here -- it stays alive, idling,
     * for a possible future re-start (matches the observed re-open/
     * retry pattern). Only real teardown (DSClose/DllMain DETACH) joins
     * it. Broadcasting wakes both the worker (so it stops producing) and
     * any blocked EventGetData (so a pending wait re-checks promptly
     * rather than riding out its full timeout). */
    EnterCriticalSection(&g_buf_lock);
    g_ds_grabbing = 0;
    g_camera_running = 0; /* safe-teardown default, per instructions */
    g_acq_stop_count++;
    WakeAllConditionVariable(&g_buf_cv);
    LeaveCriticalSection(&g_buf_lock);
    probe_log("DSStopAcquisition -> SUCCESS, grabbing=FALSE");
    set_err(GC_ERR_SUCCESS, "no error");
    return GC_ERR_SUCCESS;
}
/* M4a: minimum number of buffers to announce before acquisition can
 * start (GenTL v1.3, STREAM_INFO_BUF_ANNOUNCE_MIN). 1 is a
 * standards-valid conservative minimum -- nothing in the GenTL spec
 * requires more than one announced buffer to be able to start. */
#define STREAM_BUF_ANNOUNCE_MIN 1

static const char *ds_info_cmd_name(DS_INFO_CMD cmd) {
    switch (cmd) {
        case STREAM_INFO_ID:                  return "STREAM_INFO_ID";
        case STREAM_INFO_NUM_DELIVERED:        return "STREAM_INFO_NUM_DELIVERED";
        case STREAM_INFO_NUM_UNDERRUN:         return "STREAM_INFO_NUM_UNDERRUN";
        case STREAM_INFO_NUM_ANNOUNCED:        return "STREAM_INFO_NUM_ANNOUNCED";
        case STREAM_INFO_NUM_QUEUED:           return "STREAM_INFO_NUM_QUEUED";
        case STREAM_INFO_NUM_AWAIT_DELIVERY:   return "STREAM_INFO_NUM_AWAIT_DELIVERY";
        case STREAM_INFO_NUM_STARTED:          return "STREAM_INFO_NUM_STARTED";
        case STREAM_INFO_PAYLOAD_SIZE:         return "STREAM_INFO_PAYLOAD_SIZE";
        case STREAM_INFO_IS_GRABBING:          return "STREAM_INFO_IS_GRABBING";
        case STREAM_INFO_DEFINES_PAYLOADSIZE:  return "STREAM_INFO_DEFINES_PAYLOADSIZE";
        case STREAM_INFO_TLTYPE:               return "STREAM_INFO_TLTYPE";
        case STREAM_INFO_NUM_CHUNKS_MAX:        return "STREAM_INFO_NUM_CHUNKS_MAX";
        case STREAM_INFO_BUF_ANNOUNCE_MIN:      return "STREAM_INFO_BUF_ANNOUNCE_MIN";
        case STREAM_INFO_BUF_ALIGNMENT:         return "STREAM_INFO_BUF_ALIGNMENT";
        case STREAM_INFO_CUSTOM_ID:             return "STREAM_INFO_CUSTOM_ID";
        default:                                return "UNKNOWN";
    }
}

GC_ERROR GC_CALLTYPE DSGetInfo(DS_HANDLE hDataStream, DS_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    (void)hDataStream;
    {
        char fn_desc[64];
        snprintf(fn_desc, sizeof fn_desc, "DSGetInfo(%s)", ds_info_cmd_name(iInfoCmd));
        note_buffer_burst_boundary(fn_desc);
    }
    probe_log("DSGetInfo cmd=%d (%s)", (int)iInfoCmd, ds_info_cmd_name(iInfoCmd));
    GC_ERROR rc;
    switch (iInfoCmd) {
        case STREAM_INFO_BUF_ANNOUNCE_MIN:
            rc = info_sizet(piType, pBuffer, piSize, STREAM_BUF_ANNOUNCE_MIN);
            break;
        case STREAM_INFO_IS_GRABBING:
            /* M4c-3: reflects real acquisition state now -- FALSE before
             * DSStartAcquisition, TRUE while grabbing, FALSE again after
             * DSStopAcquisition. */
            rc = info_bool8(piType, pBuffer, piSize, g_ds_grabbing ? 1 : 0);
            break;
        default:
            probe_log("  -> NOT_IMPLEMENTED (new DS_INFO_CMD, not yet implemented this pass)");
            set_err(GC_ERR_NOT_IMPLEMENTED, "DSGetInfo: this specific info command not implemented yet");
            return GC_ERR_NOT_IMPLEMENTED;
    }
    probe_log("  -> rc=%d piType=%d *piSize=%zu", rc, piType ? *piType : -1, piSize ? *piSize : 0);
    return rc;
}
GC_ERROR GC_CALLTYPE DSGetBufferID(DS_HANDLE hDataStream, uint32_t iIndex, BUFFER_HANDLE *phBuffer) {
    note_buffer_burst_boundary("DSGetBufferID");
    if (hDataStream != H_DS) {
        probe_log("DSGetBufferID hDataStream=%p -> INVALID_HANDLE", hDataStream);
        return GC_ERR_INVALID_HANDLE;
    }
    EnterCriticalSection(&g_buf_lock);
    if (iIndex >= g_buffers_count) {
        LeaveCriticalSection(&g_buf_lock);
        probe_log("DSGetBufferID index=%u -> INVALID_INDEX (only %zu buffer(s) registered)", iIndex, g_buffers_count);
        set_err(GC_ERR_INVALID_INDEX, "DSGetBufferID: index out of range");
        return GC_ERR_INVALID_INDEX;
    }
    BUFFER_HANDLE h = (BUFFER_HANDLE)g_buffers[iIndex];
    LeaveCriticalSection(&g_buf_lock);
    if (phBuffer) *phBuffer = h;
    probe_log("DSGetBufferID index=%u -> handle=%p", iIndex, h);
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE DSGetBufferInfo(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, BUFFER_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    {
        char fn_desc[48];
        snprintf(fn_desc, sizeof fn_desc, "DSGetBufferInfo(cmd=%d)", (int)iInfoCmd);
        note_buffer_burst_boundary(fn_desc);
    }
    if (hDataStream != H_DS) {
        probe_log("DSGetBufferInfo hDataStream=%p -> INVALID_HANDLE", hDataStream);
        return GC_ERR_INVALID_HANDLE;
    }
    EnterCriticalSection(&g_buf_lock);
    buffer_t *b = find_buffer_locked(hBuffer);
    if (!b) {
        LeaveCriticalSection(&g_buf_lock);
        probe_log("DSGetBufferInfo handle=%p cmd=%d -> INVALID_HANDLE", hBuffer, (int)iInfoCmd);
        set_err(GC_ERR_INVALID_HANDLE, "DSGetBufferInfo: invalid or stale buffer handle");
        return GC_ERR_INVALID_HANDLE;
    }
    /* Snapshot under the lock, answer the query after releasing it. */
    uint8_t *base = b->base; size_t capacity = b->capacity, filled = b->filled_size;
    int is_incomplete = b->is_incomplete, new_data = b->new_data;
    int is_queued = (b->state == BUF_STATE_QUEUED), is_acquiring = (b->state == BUF_STATE_FILLING);
    uint64_t frame_id = b->frame_id, ts_ns = b->timestamp_ns;
    void *user_ptr = b->private_ptr;
    LeaveCriticalSection(&g_buf_lock);

    GC_ERROR rc;
    switch (iInfoCmd) {
        case BUFFER_INFO_BASE:
            if (piType) *piType = INFO_DATATYPE_PTR;
            if (!pBuffer) { if (piSize) *piSize = sizeof(void*); rc = GC_ERR_SUCCESS; }
            else if (!piSize || *piSize < sizeof(void*)) { if (piSize) *piSize = sizeof(void*); rc = GC_ERR_BUFFER_TOO_SMALL; }
            else { *(void**)pBuffer = base; *piSize = sizeof(void*); rc = GC_ERR_SUCCESS; }
            break;
        case BUFFER_INFO_SIZE:      rc = info_sizet(piType, pBuffer, piSize, capacity); break;
        case BUFFER_INFO_USER_PTR:  if (piType) *piType = INFO_DATATYPE_PTR;
                                     if (!pBuffer) { if (piSize) *piSize = sizeof(void*); rc = GC_ERR_SUCCESS; }
                                     else if (!piSize || *piSize < sizeof(void*)) { if (piSize) *piSize = sizeof(void*); rc = GC_ERR_BUFFER_TOO_SMALL; }
                                     else { *(void**)pBuffer = user_ptr; *piSize = sizeof(void*); rc = GC_ERR_SUCCESS; }
                                     break;
        case BUFFER_INFO_TIMESTAMP:
        case BUFFER_INFO_TIMESTAMP_NS: rc = info_u64(piType, pBuffer, piSize, ts_ns); break;
        case BUFFER_INFO_NEW_DATA:  rc = info_bool8(piType, pBuffer, piSize, new_data ? 1 : 0); break;
        case BUFFER_INFO_IS_QUEUED: rc = info_bool8(piType, pBuffer, piSize, is_queued ? 1 : 0); break;
        case BUFFER_INFO_IS_ACQUIRING: rc = info_bool8(piType, pBuffer, piSize, is_acquiring ? 1 : 0); break;
        case BUFFER_INFO_IS_INCOMPLETE: rc = info_bool8(piType, pBuffer, piSize, is_incomplete ? 1 : 0); break;
        case BUFFER_INFO_TLTYPE:    rc = info_string(piType, pBuffer, piSize, TLTYPE_U3V); break;
        case BUFFER_INFO_SIZE_FILLED: rc = info_sizet(piType, pBuffer, piSize, filled); break;
        case BUFFER_INFO_WIDTH:     rc = info_sizet(piType, pBuffer, piSize, (size_t)FRAME_WIDTH); break;
        case BUFFER_INFO_HEIGHT:    rc = info_sizet(piType, pBuffer, piSize, (size_t)FRAME_HEIGHT); break;
        case BUFFER_INFO_XOFFSET:
        case BUFFER_INFO_YOFFSET:
        case BUFFER_INFO_XPADDING:
        case BUFFER_INFO_YPADDING:  rc = info_sizet(piType, pBuffer, piSize, 0); break;
        case BUFFER_INFO_FRAMEID:   rc = info_u64(piType, pBuffer, piSize, frame_id); break;
        case BUFFER_INFO_IMAGEPRESENT: rc = info_bool8(piType, pBuffer, piSize, filled > 0 ? 1 : 0); break;
        case BUFFER_INFO_IMAGEOFFSET: rc = info_sizet(piType, pBuffer, piSize, 0); break; /* single-part image, no offset */
        case BUFFER_INFO_PAYLOADTYPE: rc = info_sizet(piType, pBuffer, piSize, (size_t)PAYLOAD_TYPE_IMAGE); break;
        case BUFFER_INFO_PIXELFORMAT: rc = info_u64(piType, pBuffer, piSize, (uint64_t)(uint32_t)g_pixelformat); break;
        case BUFFER_INFO_PIXELFORMAT_NAMESPACE: rc = info_u64(piType, pBuffer, piSize, (uint64_t)PIXELFORMAT_NAMESPACE_PFNC_32BIT); break;
        case BUFFER_INFO_DATA_SIZE: rc = info_sizet(piType, pBuffer, piSize, filled); break;
        case BUFFER_INFO_DATA_LARGER_THAN_BUFFER: rc = info_bool8(piType, pBuffer, piSize, 0); break;
        case BUFFER_INFO_CONTAINS_CHUNKDATA: rc = info_bool8(piType, pBuffer, piSize, 0); break;
        default:
            probe_log("DSGetBufferInfo handle=%p cmd=%d -> NOT_IMPLEMENTED (unrecognized command)", hBuffer, (int)iInfoCmd);
            set_err(GC_ERR_NOT_IMPLEMENTED, "DSGetBufferInfo: this specific info command not implemented");
            return GC_ERR_NOT_IMPLEMENTED;
    }
    probe_log("DSGetBufferInfo handle=%p cmd=%d -> rc=%d", hBuffer, (int)iInfoCmd, rc);
    return rc;
}
GC_ERROR GC_CALLTYPE DSGetBufferChunkData(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, void *pChunkData, size_t *piNumChunks) {
    (void)hDataStream; (void)hBuffer; (void)pChunkData; (void)piNumChunks;
    note_buffer_burst_boundary("DSGetBufferChunkData");
    probe_log("DSGetBufferChunkData  [STUB -> NOT_IMPLEMENTED]");
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSGetBufferChunkData not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSClose(DS_HANDLE hDataStream) {
    note_buffer_burst_boundary("DSClose");
    probe_log("DSClose");
    (void)hDataStream;
    /* Order matters: stop+join the worker first (so nothing is still
     * touching a buffer/event when we free them), then event, then
     * buffers. */
    stop_worker_defensive("DSClose");
    wsl_bridge_stop();
    free_event_defensive("DSClose");
    free_all_buffers_defensive("DSClose");
    return GC_ERR_SUCCESS;
}

/* ================= Event* (M1 additions — stubs) ================= */

/* Validate a handle against the live event object by identity (not
 * range math -- individually malloc'd, same approach as buffer_t).
 * Caller must hold g_buf_lock. */
static event_t *find_event_locked(EVENT_HANDLE h) {
    return (g_ds_new_buffer_event && (EVENT_HANDLE)g_ds_new_buffer_event == h) ? g_ds_new_buffer_event : NULL;
}

GC_ERROR GC_CALLTYPE EventGetData(EVENT_HANDLE hEvent, void *pBuffer, size_t *piSize, uint64_t iTimeout) {
    note_buffer_burst_boundary("EventGetData");
    g_eventgetdata_calls++;
    DWORD tid = GetCurrentThreadId();
    /* DWORD wait milliseconds: a very large iTimeout (the SDK's
     * INFINITE_NUMBER sentinel, or anything that would overflow a
     * DWORD) maps to Windows' own INFINITE wait. */
    int is_infinite = (iTimeout > 0xFFFFFFF0ULL);
    DWORD timeout_ms = is_infinite ? INFINITE : (DWORD)iTimeout;
    if (g_eventgetdata_calls <= 10) {
        probe_log("EventGetData handle=%p timeout=%llu%s tid=%lu", hEvent,
                  (unsigned long long)iTimeout, is_infinite ? " (INFINITE)" : "", (unsigned long)tid);
    }

    EnterCriticalSection(&g_buf_lock);
    event_t *ev = find_event_locked(hEvent);
    if (!ev) {
        LeaveCriticalSection(&g_buf_lock);
        probe_log("EventGetData handle=%p -> INVALID_HANDLE (unknown, stale, or unregistered event) tid=%lu", hEvent, (unsigned long)tid);
        set_err(GC_ERR_INVALID_HANDLE, "EventGetData: invalid or stale event handle");
        return GC_ERR_INVALID_HANDLE;
    }
    ev->refcount++;

    ULONGLONG deadline = is_infinite ? 0 : (GetTickCount64() + timeout_ms);
    completed_item_t item;
    int have_item = 0;
    int timed_out = 0;
    while (1) {
        have_item = completed_queue_pop_locked(&item);
        if (have_item || ev->killed) break;
        DWORD wait_ms;
        if (is_infinite) {
            wait_ms = INFINITE;
        } else {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) { timed_out = 1; break; }
            wait_ms = (DWORD)(deadline - now);
        }
        BOOL woke = SleepConditionVariableCS(&g_buf_cv, &g_buf_lock, wait_ms);
        if (!woke && !is_infinite && GetTickCount64() >= deadline) { timed_out = 1; break; }
    }

    GC_ERROR rc;
    if (have_item) {
        /* Size-query / buffer-too-small semantics, same convention as
         * every info_* helper elsewhere in this producer. */
        size_t need = sizeof(EVENT_NEW_BUFFER_DATA);
        if (!pBuffer) {
            if (piSize) *piSize = need;
            rc = GC_ERR_SUCCESS;
        } else if (!piSize || *piSize < need) {
            if (piSize) *piSize = need;
            rc = GC_ERR_BUFFER_TOO_SMALL;
        } else {
            EVENT_NEW_BUFFER_DATA data;
            data.BufferHandle = item.handle;
            data.pUserPointer = item.user_ptr;
            memcpy(pBuffer, &data, need);
            *piSize = need;
            ev->num_fired++;
            g_events_delivered++;
            rc = GC_ERR_SUCCESS;
        }
    } else if (ev->killed) {
        /* Matches the official bindings' own documented mapping
         * (WaitForFinishedBuffer's docstring: "raises AbortedException
         * The wait was aborted"), confirmed earlier in this project. */
        rc = GC_ERR_ABORT;
    } else {
        /* Loop only exits without have_item/killed when timed_out was
         * set; asserting that explicitly here (rather than assuming it
         * via bare else) both documents the invariant and uses the
         * variable. */
        (void)timed_out;
        rc = GC_ERR_TIMEOUT;
    }

    ev->refcount--;
    if (ev->killed && ev->refcount == 0 && ev->pending_free) {
        ev->magic = 0;
        free(ev);
    }
    LeaveCriticalSection(&g_buf_lock);

    if (g_eventgetdata_calls <= 10) {
        probe_log("EventGetData handle=%p -> rc=%d%s tid=%lu", hEvent, rc,
                  (rc == GC_ERR_SUCCESS && have_item) ? " (NEW_BUFFER delivered)" :
                  (rc == GC_ERR_ABORT) ? " (ABORT: killed/unregistered)" :
                  (rc == GC_ERR_TIMEOUT) ? " (TIMEOUT)" : "", (unsigned long)tid);
    } else if (g_eventgetdata_calls == 11) {
        probe_log("EventGetData ... further calls logged only in the final tally");
    }
    if (rc != GC_ERR_SUCCESS) set_err(rc, "EventGetData: see log");
    return rc;
}
GC_ERROR GC_CALLTYPE EventGetInfo(EVENT_HANDLE hEvent, int32_t iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    note_buffer_burst_boundary("EventGetInfo");
    EnterCriticalSection(&g_buf_lock);
    event_t *ev = find_event_locked(hEvent);
    if (!ev) {
        LeaveCriticalSection(&g_buf_lock);
        probe_log("EventGetInfo handle=%p cmd=%d -> INVALID_HANDLE", hEvent, (int)iInfoCmd);
        set_err(GC_ERR_INVALID_HANDLE, "EventGetInfo: invalid or stale event handle");
        return GC_ERR_INVALID_HANDLE;
    }
    GC_ERROR rc;
    switch (iInfoCmd) {
        case EVENT_EVENT_TYPE:
            rc = info_i32(piType, pBuffer, piSize, (int32_t)ev->type);
            break;
        case EVENT_NUM_IN_QUEUE:
            rc = info_sizet(piType, pBuffer, piSize, g_completed_queue_count);
            break;
        case EVENT_NUM_FIRED:
            rc = info_u64(piType, pBuffer, piSize, ev->num_fired);
            break;
        case EVENT_SIZE_MAX:
        case EVENT_INFO_DATA_SIZE_MAX:
            /* Real size from the official struct, not a hard-coded
             * number, per instructions. */
            rc = info_sizet(piType, pBuffer, piSize, sizeof(EVENT_NEW_BUFFER_DATA));
            break;
        default:
            LeaveCriticalSection(&g_buf_lock);
            probe_log("EventGetInfo handle=%p cmd=%d -> NOT_IMPLEMENTED (unrecognized command)", hEvent, (int)iInfoCmd);
            set_err(GC_ERR_NOT_IMPLEMENTED, "EventGetInfo: this specific info command not implemented");
            return GC_ERR_NOT_IMPLEMENTED;
    }
    LeaveCriticalSection(&g_buf_lock);
    probe_log("EventGetInfo handle=%p cmd=%d -> rc=%d", hEvent, (int)iInfoCmd, rc);
    return rc;
}
GC_ERROR GC_CALLTYPE EventFlush(EVENT_HANDLE hEvent) {
    note_buffer_burst_boundary("EventFlush");
    EnterCriticalSection(&g_buf_lock);
    event_t *ev = find_event_locked(hEvent);
    int flushed = 0;
    if (ev) {
        flushed = (int)g_completed_queue_count;
        g_completed_queue_count = 0; /* discard pending notifications; buffers themselves are untouched */
    }
    LeaveCriticalSection(&g_buf_lock);
    probe_log("EventFlush handle=%p -> discarded %d pending event(s)", hEvent, flushed);
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE EventGetDataInfo(EVENT_HANDLE hEvent, const void *pInBuffer, size_t iInSize, int32_t iInfoCmd, INFO_DATATYPE *piType, void *pOutBuffer, size_t *piOutSize) {
    /* Not implemented: EVENT_NEW_BUFFER's data is the fixed
     * EVENT_NEW_BUFFER_DATA struct retrieved directly via EventGetData;
     * no concrete evidence yet (official bindings or real Kineo) that
     * this function is ever called for it -- deferring rather than
     * guessing a semantics for it. */
    (void)hEvent; (void)pInBuffer; (void)iInSize; (void)piType; (void)pOutBuffer; (void)piOutSize;
    note_buffer_burst_boundary("EventGetDataInfo");
    probe_log("EventGetDataInfo cmd=%d  [STUB -> NOT_IMPLEMENTED]", (int)iInfoCmd);
    set_err(GC_ERR_NOT_IMPLEMENTED, "EventGetDataInfo not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE EventKill(EVENT_HANDLE hEvent) {
    note_buffer_burst_boundary("EventKill");
    EnterCriticalSection(&g_buf_lock);
    event_t *ev = find_event_locked(hEvent);
    if (ev) {
        ev->killed = 1; /* interrupts any pending EventGetData wait -- does not free the object (GCUnregisterEvent/teardown does that) */
        WakeAllConditionVariable(&g_buf_cv);
        LeaveCriticalSection(&g_buf_lock);
        probe_log("EventKill handle=%p -> killed", hEvent);
        return GC_ERR_SUCCESS;
    }
    LeaveCriticalSection(&g_buf_lock);
    probe_log("EventKill handle=%p -> INVALID_HANDLE (unknown or already-unregistered event)", hEvent);
    set_err(GC_ERR_INVALID_HANDLE, "EventKill: invalid or stale event handle");
    return GC_ERR_INVALID_HANDLE;
}

/* ================= IDS-only multipart DS extensions (M1c addition) =================
 * Real GenTL functions (not GC-lifecycle noise), included in the real IDS
 * CTI's 81-symbol export table beyond the 55 common ones. Own stub
 * implementation (not forwarded) -- signatures known precisely from
 * gentl_v2.h, safe to implement directly. */
GC_ERROR GC_CALLTYPE DSGetNumBufferParts(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, uint32_t *piNumParts) {
    (void)hDataStream; (void)hBuffer;
    probe_log("DSGetNumBufferParts  [STUB -> NOT_IMPLEMENTED]");
    if (piNumParts) *piNumParts = 0;
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSGetNumBufferParts not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSGetBufferPartInfo(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, uint32_t iPartIndex, int32_t iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    (void)hDataStream; (void)hBuffer; (void)iPartIndex; (void)piType; (void)pBuffer; (void)piSize;
    probe_log("DSGetBufferPartInfo cmd=%d  [STUB -> NOT_IMPLEMENTED]", iInfoCmd);
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSGetBufferPartInfo not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}

/* ================= DllMain ================= */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            InitializeCriticalSection(&g_log_lock);
            log_resolve_path();
            g_log_ready = 1;
            resolve_xml_url(hinst);
            InitializeCriticalSection(&g_buf_lock);
            g_buf_lock_ready = 1;
            InitializeConditionVariable(&g_buf_cv);
            probe_log("=== DllMain PROCESS_ATTACH  probe %s  log=%s  xml_url=%s size=%llu ===",
                      PROBE_VERSION, g_log_path, g_xml_file_url, (unsigned long long)g_xml_file_size);
            break;
        case DLL_PROCESS_DETACH:
            for (size_t i = 0; i < NUM_REGS; i++) {
                if (g_regs[i].log_count > REG_LOG_LIMIT + 1) {
                    probe_log("=== final tally: %s accessed %d times total ===", g_regs[i].name, g_regs[i].log_count);
                }
            }
            /* Same order as DSClose: stop+join the worker before freeing
             * anything it might touch. */
            stop_worker_defensive("DllMain PROCESS_DETACH");
            wsl_bridge_stop();
            free_event_defensive("DllMain PROCESS_DETACH");
            free_all_buffers_defensive("DllMain PROCESS_DETACH");
            probe_log("=== final buffer tally: allocated=%llu (%llu bytes) queued=%llu (of which requeues=%llu) "
                      "max_queue_depth=%d revoked=%llu leaked-and-freed-defensively=%llu ===",
                      (unsigned long long)g_buf_alloc_count, (unsigned long long)g_buf_alloc_bytes,
                      (unsigned long long)g_buf_queue_count, (unsigned long long)g_buf_requeue_count, g_buf_max_queue_depth,
                      (unsigned long long)g_buf_revoke_count, (unsigned long long)g_buf_leaked_count);
            probe_log("=== final acquisition/event tally: frames_produced=%llu events_delivered=%llu "
                      "eventgetdata_calls=%llu queue_starvation_events=%llu acq_starts=%llu acq_stops=%llu "
                      "event_registrations=%llu event_unregistrations=%llu ===",
                      (unsigned long long)g_frames_produced, (unsigned long long)g_events_delivered,
                      (unsigned long long)g_eventgetdata_calls, (unsigned long long)g_queue_starvation_count,
                      (unsigned long long)g_acq_start_count, (unsigned long long)g_acq_stop_count,
                      (unsigned long long)g_event_register_count, (unsigned long long)g_event_unregister_count);
            probe_log("=== DllMain PROCESS_DETACH ===");
            if (g_log_ready) { DeleteCriticalSection(&g_log_lock); g_log_ready = 0; }
            if (g_buf_lock_ready) { DeleteCriticalSection(&g_buf_lock); g_buf_lock_ready = 0; }
            break;
        default:
            break;
    }
    return TRUE;
}
