/* kineo_probe.c — M0 probe GenTL producer (.cti)
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
#include "gentl.h"

#define PROBE_VERSION "0.1-M0"

/* ---- identity constants presented by the fake producer ---- */
#define TL_ID_STR        "KineoBridge"
#define TL_VENDOR_STR    "IMV/KineoBridge"
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
#define H_TL   ((TL_HANDLE)&g_tl_obj)
#define H_IF   ((IF_HANDLE)&g_if_obj)
#define H_DEV  ((DEV_HANDLE)&g_dev_obj)
#define H_PORT ((PORT_HANDLE)&g_port_obj)

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
    probe_log("GCGetInfo cmd=%d", (int)iInfoCmd);
    return tl_info(iInfoCmd, piType, pBuffer, piSize);
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
GC_ERROR GC_CALLTYPE GCReadPort(PORT_HANDLE hPort, uint64_t iAddress, void *pBuffer, size_t *piSize) {
    (void)hPort; (void)pBuffer; (void)piSize;
    probe_log("GCReadPort addr=0x%llx  [STUB -> NOT_IMPLEMENTED]", (unsigned long long)iAddress);
    set_err(GC_ERR_NOT_IMPLEMENTED, "GCReadPort not implemented at M0");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCWritePort(PORT_HANDLE hPort, uint64_t iAddress, const void *pBuffer, size_t *piSize) {
    (void)hPort; (void)pBuffer; (void)piSize;
    probe_log("GCWritePort addr=0x%llx  [STUB -> NOT_IMPLEMENTED]", (unsigned long long)iAddress);
    set_err(GC_ERR_NOT_IMPLEMENTED, "GCWritePort not implemented at M0");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCGetPortURL(PORT_HANDLE hPort, char *sURL, size_t *piSize) {
    (void)hPort; (void)sURL; (void)piSize;
    probe_log("GCGetPortURL  [STUB -> NOT_IMPLEMENTED]");
    set_err(GC_ERR_NOT_IMPLEMENTED, "GCGetPortURL not implemented at M0");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCGetNumPortURLs(PORT_HANDLE hPort, uint32_t *piNumURLs) {
    (void)hPort;
    probe_log("GCGetNumPortURLs  [STUB -> 0]");
    if (piNumURLs) *piNumURLs = 0;
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE GCGetPortURLInfo(PORT_HANDLE hPort, uint32_t iURLIndex, URL_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    (void)hPort; (void)iURLIndex; (void)iInfoCmd; (void)piType; (void)pBuffer; (void)piSize;
    probe_log("GCGetPortURLInfo  [STUB -> NOT_IMPLEMENTED]");
    set_err(GC_ERR_NOT_IMPLEMENTED, "GCGetPortURLInfo not implemented at M0");
    return GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCGetPortInfo(PORT_HANDLE hPort, PORT_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    (void)hPort; (void)iInfoCmd; (void)piType; (void)pBuffer; (void)piSize;
    probe_log("GCGetPortInfo cmd=%d  [STUB -> NOT_IMPLEMENTED]", (int)iInfoCmd);
    set_err(GC_ERR_NOT_IMPLEMENTED, "GCGetPortInfo not implemented at M0");
    return GC_ERR_NOT_IMPLEMENTED;
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
GC_ERROR GC_CALLTYPE DevGetNumDataStreams(DEV_HANDLE hDevice, uint32_t *piNumDataStreams) {
    probe_log("DevGetNumDataStreams -> 0  [streaming is M4]");
    if (hDevice != H_DEV) return GC_ERR_INVALID_HANDLE;
    if (piNumDataStreams) *piNumDataStreams = 0;
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE DevGetDataStreamID(DEV_HANDLE hDevice, uint32_t iIndex, char *sDataStreamID, size_t *piSize) {
    (void)iIndex; (void)sDataStreamID; (void)piSize;
    probe_log("DevGetDataStreamID  [STUB -> INVALID_INDEX, streaming is M4]");
    if (hDevice != H_DEV) return GC_ERR_INVALID_HANDLE;
    set_err(GC_ERR_INVALID_INDEX, "no data streams at M0");
    return GC_ERR_INVALID_INDEX;
}
GC_ERROR GC_CALLTYPE DevOpenDataStream(DEV_HANDLE hDevice, const char *sDataStreamID, DS_HANDLE *phDataStream) {
    (void)sDataStreamID; (void)phDataStream;
    probe_log("DevOpenDataStream  [STUB -> NOT_IMPLEMENTED, streaming is M4]");
    if (hDevice != H_DEV) return GC_ERR_INVALID_HANDLE;
    set_err(GC_ERR_NOT_IMPLEMENTED, "data streams not implemented at M0");
    return GC_ERR_NOT_IMPLEMENTED;
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

/* ================= DllMain ================= */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst; (void)reserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            InitializeCriticalSection(&g_log_lock);
            log_resolve_path();
            g_log_ready = 1;
            probe_log("=== DllMain PROCESS_ATTACH  probe %s  log=%s ===", PROBE_VERSION, g_log_path);
            break;
        case DLL_PROCESS_DETACH:
            probe_log("=== DllMain PROCESS_DETACH ===");
            if (g_log_ready) { DeleteCriticalSection(&g_log_lock); g_log_ready = 0; }
            break;
        default:
            break;
    }
    return TRUE;
}
