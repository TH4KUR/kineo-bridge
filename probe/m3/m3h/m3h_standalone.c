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

#define PROBE_VERSION "0.4-M3h"

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
        fprintf(f, "[%02d:%02d:%02d.%03d] ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
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
    snprintf(xml_path, sizeof xml_path, "%skineo_bridge_m3h.xml", dll_path);

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
    snprintf(g_xml_file_url, sizeof g_xml_file_url, "local:kineo_bridge_m3h.xml;%llX;%zX",
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
        case URL_INFO_FILENAME:          rc = info_string(piType, pBuffer, piSize, "kineo_bridge_m3h.xml"); break;
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
GC_ERROR GC_CALLTYPE GCRegisterEvent(EVENTSRC_HANDLE hEventSrc, EVENT_TYPE iEventID, EVENT_HANDLE *phEvent) {
    (void)hEventSrc; (void)phEvent;
    probe_log("GCRegisterEvent id=%llu  [STUB -> NOT_IMPLEMENTED]", (unsigned long long)iEventID);
    set_err(GC_ERR_NOT_IMPLEMENTED, "GCRegisterEvent not implemented");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCUnregisterEvent(EVENTSRC_HANDLE hEventSrc, EVENT_TYPE iEventID) {
    (void)hEventSrc;
    probe_log("GCUnregisterEvent id=%llu  [STUB -> NOT_IMPLEMENTED]", (unsigned long long)iEventID);
    set_err(GC_ERR_NOT_IMPLEMENTED, "GCUnregisterEvent not implemented");
    return GC_ERR_NOT_IMPLEMENTED;
}

/* ================= DS* (M1 additions — stubs, real streaming is M4) ================= */

GC_ERROR GC_CALLTYPE DSAnnounceBuffer(DS_HANDLE hDataStream, void *pBuffer, size_t iSize, void *pPrivate, BUFFER_HANDLE *phBuffer) {
    (void)hDataStream; (void)pBuffer; (void)iSize; (void)pPrivate; (void)phBuffer;
    probe_log("DSAnnounceBuffer size=%zu  [STUB -> NOT_IMPLEMENTED]", iSize);
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSAnnounceBuffer not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSAllocAndAnnounceBuffer(DS_HANDLE hDataStream, size_t iBufferSize, void *pPrivate, BUFFER_HANDLE *phBuffer) {
    (void)hDataStream; (void)pPrivate; (void)phBuffer;
    probe_log("DSAllocAndAnnounceBuffer size=%zu  [STUB -> NOT_IMPLEMENTED]", iBufferSize);
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSAllocAndAnnounceBuffer not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSRevokeBuffer(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, void **pBuffer, void **pPrivate) {
    (void)hDataStream; (void)hBuffer; (void)pBuffer; (void)pPrivate;
    probe_log("DSRevokeBuffer  [STUB -> NOT_IMPLEMENTED]");
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSRevokeBuffer not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSQueueBuffer(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer) {
    (void)hDataStream; (void)hBuffer;
    probe_log("DSQueueBuffer  [STUB -> NOT_IMPLEMENTED]");
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSQueueBuffer not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
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
    probe_log("DSFlushQueue  [STUB -> NOT_IMPLEMENTED]");
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSFlushQueue not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSStartAcquisition(DS_HANDLE hDataStream, ACQ_START_FLAGS iStartFlags, uint64_t iNumToAcquire) {
    (void)hDataStream; (void)iStartFlags; (void)iNumToAcquire;
    probe_log("DSStartAcquisition  [STUB -> NOT_IMPLEMENTED]");
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSStartAcquisition not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSStopAcquisition(DS_HANDLE hDataStream, ACQ_STOP_FLAGS iStopFlags) {
    (void)hDataStream; (void)iStopFlags;
    probe_log("DSStopAcquisition  [STUB -> NOT_IMPLEMENTED]");
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSStopAcquisition not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSGetInfo(DS_HANDLE hDataStream, DS_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    (void)hDataStream; (void)piType; (void)pBuffer;
    probe_log("DSGetInfo cmd=%d  [STUB -> NOT_IMPLEMENTED]", (int)iInfoCmd);
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSGetInfo not implemented until M4");
    (void)piSize;
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSGetBufferID(DS_HANDLE hDataStream, uint32_t iIndex, BUFFER_HANDLE *phBuffer) {
    (void)hDataStream; (void)iIndex; (void)phBuffer;
    probe_log("DSGetBufferID  [STUB -> NOT_IMPLEMENTED]");
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSGetBufferID not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSGetBufferInfo(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, BUFFER_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    (void)hDataStream; (void)hBuffer; (void)piType; (void)pBuffer; (void)piSize;
    probe_log("DSGetBufferInfo cmd=%d  [STUB -> NOT_IMPLEMENTED]", (int)iInfoCmd);
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSGetBufferInfo not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSGetBufferChunkData(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, void *pChunkData, size_t *piNumChunks) {
    (void)hDataStream; (void)hBuffer; (void)pChunkData; (void)piNumChunks;
    probe_log("DSGetBufferChunkData  [STUB -> NOT_IMPLEMENTED]");
    set_err(GC_ERR_NOT_IMPLEMENTED, "DSGetBufferChunkData not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSClose(DS_HANDLE hDataStream) {
    probe_log("DSClose");
    (void)hDataStream;
    return GC_ERR_SUCCESS;
}

/* ================= Event* (M1 additions — stubs) ================= */

GC_ERROR GC_CALLTYPE EventGetData(EVENT_HANDLE hEvent, void *pBuffer, size_t *piSize, uint64_t iTimeout) {
    (void)hEvent; (void)pBuffer; (void)piSize; (void)iTimeout;
    probe_log("EventGetData  [STUB -> NOT_IMPLEMENTED]");
    set_err(GC_ERR_NOT_IMPLEMENTED, "EventGetData not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE EventGetInfo(EVENT_HANDLE hEvent, int32_t iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    (void)hEvent; (void)piType; (void)pBuffer; (void)piSize;
    probe_log("EventGetInfo cmd=%d  [STUB -> NOT_IMPLEMENTED]", (int)iInfoCmd);
    set_err(GC_ERR_NOT_IMPLEMENTED, "EventGetInfo not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE EventFlush(EVENT_HANDLE hEvent) {
    (void)hEvent;
    probe_log("EventFlush");
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE EventGetDataInfo(EVENT_HANDLE hEvent, const void *pInBuffer, size_t iInSize, int32_t iInfoCmd, INFO_DATATYPE *piType, void *pOutBuffer, size_t *piOutSize) {
    (void)hEvent; (void)pInBuffer; (void)iInSize; (void)piType; (void)pOutBuffer; (void)piOutSize;
    probe_log("EventGetDataInfo cmd=%d  [STUB -> NOT_IMPLEMENTED]", (int)iInfoCmd);
    set_err(GC_ERR_NOT_IMPLEMENTED, "EventGetDataInfo not implemented until M4");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE EventKill(EVENT_HANDLE hEvent) {
    (void)hEvent;
    probe_log("EventKill");
    return GC_ERR_SUCCESS;
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
            probe_log("=== DllMain PROCESS_ATTACH  probe %s  log=%s  xml_url=%s size=%llu ===",
                      PROBE_VERSION, g_log_path, g_xml_file_url, (unsigned long long)g_xml_file_size);
            break;
        case DLL_PROCESS_DETACH:
            for (size_t i = 0; i < NUM_REGS; i++) {
                if (g_regs[i].log_count > REG_LOG_LIMIT + 1) {
                    probe_log("=== final tally: %s accessed %d times total ===", g_regs[i].name, g_regs[i].log_count);
                }
            }
            probe_log("=== DllMain PROCESS_DETACH ===");
            if (g_log_ready) { DeleteCriticalSection(&g_log_lock); g_log_ready = 0; }
            break;
        default:
            break;
    }
    return TRUE;
}
