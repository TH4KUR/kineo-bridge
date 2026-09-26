/* kineo_proxy.c — Test 2: forwarding IDS CTI proxy.
 *
 * Loads a scratch copy of the REAL ids_u3vgentlk.cti (never the installed
 * one) by absolute path from KINEO_PROXY_TARGET, resolves each of the 55
 * real GenTL functions via GetProcAddress, and forwards every call
 * (arguments + return value unchanged) to the real implementation, logging
 * each call. The remaining 22 OS_ and 1 CRT noise exports (Phase 2: not part of
 * the GenTL API, irrelevant internal helpers) are handled via PE export
 * forwarding at the linker level instead (see kineo_proxy.def) -- a true
 * zero-code, argument-perfect redirect, since their signatures are unknown
 * and they're not expected to matter to producer validation.
 *
 * Purpose: PLAN.md M1 Test 2 -- does ProducerLibrary.Open() accept a CTI
 * whose behavior is externally indistinguishable from the real one (same
 * exports, same forwarded behavior), or does it reject something not
 * observable through the ordinary GenTL API surface?
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include "gentl_v2.h"

static HMODULE g_target = NULL;
static CRITICAL_SECTION g_log_lock;
static int g_log_ready = 0;
static char g_log_path[MAX_PATH];

static void log_resolve_path(void) {
    DWORD n = GetEnvironmentVariableA("KINEO_BRIDGE_LOG", g_log_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) strcpy(g_log_path, "kineo_proxy.log");
}
static void plog(const char *fmt, ...) {
    if (!g_log_ready) return;
    EnterCriticalSection(&g_log_lock);
    FILE *f = fopen(g_log_path, "a");
    if (f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f); fclose(f);
    }
    LeaveCriticalSection(&g_log_lock);
}

/* Resolve one real function pointer, logging failure loudly (a missing
 * target export would be a serious, load-bearing fact, not something to
 * silently swallow). */
static FARPROC real(const char *name) {
    FARPROC p = g_target ? GetProcAddress(g_target, name) : NULL;
    if (!p) plog("!!! GetProcAddress(%s) FAILED on real target, err=%lu", name, GetLastError());
    return p;
}

/* ===== typed forwarding wrappers for the 55 real GenTL functions ===== */

#define FWD0(NAME, RET) \
    RET GC_CALLTYPE NAME(void) { \
        plog(#NAME "()"); \
        typedef RET (GC_CALLTYPE *pfn)(void); \
        pfn f = (pfn)real(#NAME); \
        return f ? f() : GC_ERR_NOT_IMPLEMENTED; \
    }

/* ===== HYBRID1: GCInitLib/GCCloseLib/GCGetInfo are OUR OWN implementation
 * (byte-for-byte proven-identical behavior to the real target for cmd=9/10,
 * confirmed via side-by-side detailed logging), NOT forwarded. Everything
 * else in this file still forwards to the real target. Purpose: bisect
 * whether the "not supported" gate is triggered by something about these 3
 * specific functions beyond their observable GCGetInfo(9)/(10) semantics. */
static int g_lib_init_count = 0;
static GC_ERROR g_last_err = GC_ERR_SUCCESS;

GC_ERROR GC_CALLTYPE GCInitLib(void) {
    g_lib_init_count++;
    plog("GCInitLib() [OWN IMPL, refcount now %d]", g_lib_init_count);
    g_last_err = GC_ERR_SUCCESS;
    return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE GCCloseLib(void) {
    plog("GCCloseLib() [OWN IMPL, refcount before=%d]", g_lib_init_count);
    if (g_lib_init_count <= 0) {
        plog("  -> rc=NOT_INITIALIZED (refcount already 0)");
        return GC_ERR_NOT_INITIALIZED;
    }
    g_lib_init_count--;
    return GC_ERR_SUCCESS;
}
static GC_ERROR own_info_string(INFO_DATATYPE *piType, void *pBuffer, size_t *piSize, const char *s) {
    size_t need = strlen(s) + 1;
    if (piType) *piType = INFO_DATATYPE_STRING;
    if (!pBuffer) { if (piSize) *piSize = need; return GC_ERR_SUCCESS; }
    if (!piSize) return GC_ERR_INVALID_PARAMETER;
    if (*piSize < need) { *piSize = need; return GC_ERR_BUFFER_TOO_SMALL; }
    memcpy(pBuffer, s, need); *piSize = need; return GC_ERR_SUCCESS;
}
static GC_ERROR own_info_u32(INFO_DATATYPE *piType, void *pBuffer, size_t *piSize, uint32_t v) {
    if (piType) *piType = INFO_DATATYPE_UINT32;
    if (!pBuffer) { if (piSize) *piSize = sizeof(uint32_t); return GC_ERR_SUCCESS; }
    if (!piSize || *piSize < sizeof(uint32_t)) { if (piSize) *piSize = sizeof(uint32_t); return GC_ERR_BUFFER_TOO_SMALL; }
    memcpy(pBuffer, &v, sizeof v); *piSize = sizeof v; return GC_ERR_SUCCESS;
}
static GC_ERROR own_info_i32(INFO_DATATYPE *piType, void *pBuffer, size_t *piSize, int32_t v) {
    if (piType) *piType = INFO_DATATYPE_INT32;
    if (!pBuffer) { if (piSize) *piSize = sizeof(int32_t); return GC_ERR_SUCCESS; }
    if (!piSize || *piSize < sizeof(int32_t)) { if (piSize) *piSize = sizeof(int32_t); return GC_ERR_BUFFER_TOO_SMALL; }
    memcpy(pBuffer, &v, sizeof v); *piSize = sizeof v; return GC_ERR_SUCCESS;
}
GC_ERROR GC_CALLTYPE GCGetInfo(TL_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    plog("GCGetInfo(cmd=%d, pBuffer=%p, *piSize=%zu) [OWN IMPL]", (int)iInfoCmd, pBuffer, piSize ? *piSize : 0);
    GC_ERROR rc;
    switch (iInfoCmd) {
        case TL_INFO_ID:            rc = own_info_string(piType, pBuffer, piSize, "KineoBridge"); break;
        case TL_INFO_VENDOR:        rc = own_info_string(piType, pBuffer, piSize, "IDS Imaging Development Systems GmbH"); break;
        case TL_INFO_MODEL:         rc = own_info_string(piType, pBuffer, piSize, "KineoBridgeProducer"); break;
        case TL_INFO_VERSION:       rc = own_info_string(piType, pBuffer, piSize, "0.3-hybrid1"); break;
        case TL_INFO_TLTYPE:        rc = own_info_string(piType, pBuffer, piSize, "U3V"); break;
        case TL_INFO_NAME:          rc = own_info_string(piType, pBuffer, piSize, "hybrid1.cti"); break;
        case TL_INFO_PATHNAME:      rc = own_info_string(piType, pBuffer, piSize, "hybrid1.cti"); break;
        case TL_INFO_DISPLAYNAME:   rc = own_info_string(piType, pBuffer, piSize, "Kineo Bridge Hybrid1"); break;
        case TL_INFO_CHAR_ENCODING: rc = own_info_i32(piType, pBuffer, piSize, TL_CHAR_ENCODING_ASCII); break;
        case TL_INFO_GENTL_VER_MAJOR: rc = own_info_u32(piType, pBuffer, piSize, 1); break;
        case TL_INFO_GENTL_VER_MINOR: rc = own_info_u32(piType, pBuffer, piSize, 5); break;
        default: rc = GC_ERR_NOT_AVAILABLE; break;
    }
    plog("  -> rc=%d piType=%d *piSize=%zu [OWN IMPL]", rc, piType?*piType:-1, piSize?*piSize:0);
    return rc;
}
GC_ERROR GC_CALLTYPE GCGetLastError(GC_ERROR *piErrorCode, char *sErrorText, size_t *piSize) {
    plog("GCGetLastError()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(GC_ERROR*, char*, size_t*);
    pfn f = (pfn)real("GCGetLastError");
    return f ? f(piErrorCode, sErrorText, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCReadPort(PORT_HANDLE hPort, uint64_t iAddress, void *pBuffer, size_t *piSize) {
    plog("GCReadPort(addr=0x%llx)", (unsigned long long)iAddress);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(PORT_HANDLE, uint64_t, void*, size_t*);
    pfn f = (pfn)real("GCReadPort");
    return f ? f(hPort, iAddress, pBuffer, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCWritePort(PORT_HANDLE hPort, uint64_t iAddress, const void *pBuffer, size_t *piSize) {
    plog("GCWritePort(addr=0x%llx)", (unsigned long long)iAddress);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(PORT_HANDLE, uint64_t, const void*, size_t*);
    pfn f = (pfn)real("GCWritePort");
    return f ? f(hPort, iAddress, pBuffer, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCGetPortURL(PORT_HANDLE hPort, char *sURL, size_t *piSize) {
    plog("GCGetPortURL()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(PORT_HANDLE, char*, size_t*);
    pfn f = (pfn)real("GCGetPortURL");
    return f ? f(hPort, sURL, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCGetNumPortURLs(PORT_HANDLE hPort, uint32_t *piNumURLs) {
    plog("GCGetNumPortURLs()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(PORT_HANDLE, uint32_t*);
    pfn f = (pfn)real("GCGetNumPortURLs");
    return f ? f(hPort, piNumURLs) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCGetPortURLInfo(PORT_HANDLE hPort, uint32_t iURLIndex, URL_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    plog("GCGetPortURLInfo()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(PORT_HANDLE, uint32_t, URL_INFO_CMD, INFO_DATATYPE*, void*, size_t*);
    pfn f = (pfn)real("GCGetPortURLInfo");
    return f ? f(hPort, iURLIndex, iInfoCmd, piType, pBuffer, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCGetPortInfo(PORT_HANDLE hPort, PORT_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    plog("GCGetPortInfo(cmd=%d)", (int)iInfoCmd);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(PORT_HANDLE, PORT_INFO_CMD, INFO_DATATYPE*, void*, size_t*);
    pfn f = (pfn)real("GCGetPortInfo");
    return f ? f(hPort, iInfoCmd, piType, pBuffer, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCReadPortStacked(PORT_HANDLE hPort, void *pEntries, size_t iNumEntries) {
    plog("GCReadPortStacked()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(PORT_HANDLE, void*, size_t);
    pfn f = (pfn)real("GCReadPortStacked");
    return f ? f(hPort, pEntries, iNumEntries) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCWritePortStacked(PORT_HANDLE hPort, void *pEntries, size_t iNumEntries) {
    plog("GCWritePortStacked()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(PORT_HANDLE, void*, size_t);
    pfn f = (pfn)real("GCWritePortStacked");
    return f ? f(hPort, pEntries, iNumEntries) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCRegisterEvent(EVENTSRC_HANDLE hEventSrc, EVENT_TYPE iEventID, EVENT_HANDLE *phEvent) {
    plog("GCRegisterEvent(id=%llu)", (unsigned long long)iEventID);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(EVENTSRC_HANDLE, EVENT_TYPE, EVENT_HANDLE*);
    pfn f = (pfn)real("GCRegisterEvent");
    return f ? f(hEventSrc, iEventID, phEvent) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE GCUnregisterEvent(EVENTSRC_HANDLE hEventSrc, EVENT_TYPE iEventID) {
    plog("GCUnregisterEvent(id=%llu)", (unsigned long long)iEventID);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(EVENTSRC_HANDLE, EVENT_TYPE);
    pfn f = (pfn)real("GCUnregisterEvent");
    return f ? f(hEventSrc, iEventID) : GC_ERR_NOT_IMPLEMENTED;
}

GC_ERROR GC_CALLTYPE EventGetData(EVENT_HANDLE hEvent, void *pBuffer, size_t *piSize, uint64_t iTimeout) {
    plog("EventGetData()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(EVENT_HANDLE, void*, size_t*, uint64_t);
    pfn f = (pfn)real("EventGetData");
    return f ? f(hEvent, pBuffer, piSize, iTimeout) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE EventGetInfo(EVENT_HANDLE hEvent, int32_t iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    plog("EventGetInfo(cmd=%d)", iInfoCmd);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(EVENT_HANDLE, int32_t, INFO_DATATYPE*, void*, size_t*);
    pfn f = (pfn)real("EventGetInfo");
    return f ? f(hEvent, iInfoCmd, piType, pBuffer, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE EventFlush(EVENT_HANDLE hEvent) {
    plog("EventFlush()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(EVENT_HANDLE);
    pfn f = (pfn)real("EventFlush");
    return f ? f(hEvent) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE EventGetDataInfo(EVENT_HANDLE hEvent, const void *pInBuffer, size_t iInSize, int32_t iInfoCmd, INFO_DATATYPE *piType, void *pOutBuffer, size_t *piOutSize) {
    plog("EventGetDataInfo(cmd=%d)", iInfoCmd);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(EVENT_HANDLE, const void*, size_t, int32_t, INFO_DATATYPE*, void*, size_t*);
    pfn f = (pfn)real("EventGetDataInfo");
    return f ? f(hEvent, pInBuffer, iInSize, iInfoCmd, piType, pOutBuffer, piOutSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE EventKill(EVENT_HANDLE hEvent) {
    plog("EventKill()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(EVENT_HANDLE);
    pfn f = (pfn)real("EventKill");
    return f ? f(hEvent) : GC_ERR_NOT_IMPLEMENTED;
}

GC_ERROR GC_CALLTYPE TLOpen(TL_HANDLE *phTL) {
    plog("TLOpen()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(TL_HANDLE*);
    pfn f = (pfn)real("TLOpen");
    return f ? f(phTL) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE TLClose(TL_HANDLE hTL) {
    plog("TLClose()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(TL_HANDLE);
    pfn f = (pfn)real("TLClose");
    return f ? f(hTL) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE TLGetInfo(TL_HANDLE hTL, TL_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    plog("TLGetInfo(cmd=%d)", (int)iInfoCmd);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(TL_HANDLE, TL_INFO_CMD, INFO_DATATYPE*, void*, size_t*);
    pfn f = (pfn)real("TLGetInfo");
    return f ? f(hTL, iInfoCmd, piType, pBuffer, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE TLGetNumInterfaces(TL_HANDLE hTL, uint32_t *piNumIfaces) {
    plog("TLGetNumInterfaces()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(TL_HANDLE, uint32_t*);
    pfn f = (pfn)real("TLGetNumInterfaces");
    return f ? f(hTL, piNumIfaces) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE TLGetInterfaceID(TL_HANDLE hTL, uint32_t iIndex, char *sID, size_t *piSize) {
    plog("TLGetInterfaceID(idx=%u)", iIndex);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(TL_HANDLE, uint32_t, char*, size_t*);
    pfn f = (pfn)real("TLGetInterfaceID");
    return f ? f(hTL, iIndex, sID, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE TLGetInterfaceInfo(TL_HANDLE hTL, const char *sIfaceID, INTERFACE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    plog("TLGetInterfaceInfo(id=%s cmd=%d)", sIfaceID?sIfaceID:"(null)", (int)iInfoCmd);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(TL_HANDLE, const char*, INTERFACE_INFO_CMD, INFO_DATATYPE*, void*, size_t*);
    pfn f = (pfn)real("TLGetInterfaceInfo");
    return f ? f(hTL, sIfaceID, iInfoCmd, piType, pBuffer, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE TLOpenInterface(TL_HANDLE hTL, const char *sIfaceID, IF_HANDLE *phIface) {
    plog("TLOpenInterface(id=%s)", sIfaceID?sIfaceID:"(null)");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(TL_HANDLE, const char*, IF_HANDLE*);
    pfn f = (pfn)real("TLOpenInterface");
    return f ? f(hTL, sIfaceID, phIface) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE TLUpdateInterfaceList(TL_HANDLE hTL, bool8_t *pbChanged, uint64_t iTimeout) {
    plog("TLUpdateInterfaceList()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(TL_HANDLE, bool8_t*, uint64_t);
    pfn f = (pfn)real("TLUpdateInterfaceList");
    return f ? f(hTL, pbChanged, iTimeout) : GC_ERR_NOT_IMPLEMENTED;
}

GC_ERROR GC_CALLTYPE IFClose(IF_HANDLE hIface) {
    plog("IFClose()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(IF_HANDLE);
    pfn f = (pfn)real("IFClose");
    return f ? f(hIface) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE IFGetInfo(IF_HANDLE hIface, INTERFACE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    plog("IFGetInfo(cmd=%d)", (int)iInfoCmd);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(IF_HANDLE, INTERFACE_INFO_CMD, INFO_DATATYPE*, void*, size_t*);
    pfn f = (pfn)real("IFGetInfo");
    return f ? f(hIface, iInfoCmd, piType, pBuffer, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE IFGetNumDevices(IF_HANDLE hIface, uint32_t *piNumDevices) {
    plog("IFGetNumDevices()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(IF_HANDLE, uint32_t*);
    pfn f = (pfn)real("IFGetNumDevices");
    return f ? f(hIface, piNumDevices) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE IFGetDeviceID(IF_HANDLE hIface, uint32_t iIndex, char *sID, size_t *piSize) {
    plog("IFGetDeviceID(idx=%u)", iIndex);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(IF_HANDLE, uint32_t, char*, size_t*);
    pfn f = (pfn)real("IFGetDeviceID");
    return f ? f(hIface, iIndex, sID, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE IFUpdateDeviceList(IF_HANDLE hIface, bool8_t *pbChanged, uint64_t iTimeout) {
    plog("IFUpdateDeviceList()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(IF_HANDLE, bool8_t*, uint64_t);
    pfn f = (pfn)real("IFUpdateDeviceList");
    return f ? f(hIface, pbChanged, iTimeout) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE IFGetDeviceInfo(IF_HANDLE hIface, const char *sDeviceID, DEVICE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    plog("IFGetDeviceInfo(id=%s cmd=%d)", sDeviceID?sDeviceID:"(null)", (int)iInfoCmd);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(IF_HANDLE, const char*, DEVICE_INFO_CMD, INFO_DATATYPE*, void*, size_t*);
    pfn f = (pfn)real("IFGetDeviceInfo");
    return f ? f(hIface, sDeviceID, iInfoCmd, piType, pBuffer, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE IFOpenDevice(IF_HANDLE hIface, const char *sDeviceID, DEVICE_ACCESS_FLAGS iOpenFlags, DEV_HANDLE *phDevice) {
    plog("IFOpenDevice(id=%s flags=%d)", sDeviceID?sDeviceID:"(null)", (int)iOpenFlags);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(IF_HANDLE, const char*, DEVICE_ACCESS_FLAGS, DEV_HANDLE*);
    pfn f = (pfn)real("IFOpenDevice");
    return f ? f(hIface, sDeviceID, iOpenFlags, phDevice) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE IFGetParentTL(IF_HANDLE hIface, TL_HANDLE *phSystem) {
    plog("IFGetParentTL()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(IF_HANDLE, TL_HANDLE*);
    pfn f = (pfn)real("IFGetParentTL");
    return f ? f(hIface, phSystem) : GC_ERR_NOT_IMPLEMENTED;
}

GC_ERROR GC_CALLTYPE DevGetPort(DEV_HANDLE hDevice, PORT_HANDLE *phRemoteDevice) {
    plog("DevGetPort()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DEV_HANDLE, PORT_HANDLE*);
    pfn f = (pfn)real("DevGetPort");
    return f ? f(hDevice, phRemoteDevice) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DevGetNumDataStreams(DEV_HANDLE hDevice, uint32_t *piNumDataStreams) {
    plog("DevGetNumDataStreams()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DEV_HANDLE, uint32_t*);
    pfn f = (pfn)real("DevGetNumDataStreams");
    return f ? f(hDevice, piNumDataStreams) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DevGetDataStreamID(DEV_HANDLE hDevice, uint32_t iIndex, char *sDataStreamID, size_t *piSize) {
    plog("DevGetDataStreamID(idx=%u)", iIndex);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DEV_HANDLE, uint32_t, char*, size_t*);
    pfn f = (pfn)real("DevGetDataStreamID");
    return f ? f(hDevice, iIndex, sDataStreamID, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DevOpenDataStream(DEV_HANDLE hDevice, const char *sDataStreamID, DS_HANDLE *phDataStream) {
    plog("DevOpenDataStream(id=%s)", sDataStreamID?sDataStreamID:"(null)");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DEV_HANDLE, const char*, DS_HANDLE*);
    pfn f = (pfn)real("DevOpenDataStream");
    return f ? f(hDevice, sDataStreamID, phDataStream) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DevGetInfo(DEV_HANDLE hDevice, DEVICE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    plog("DevGetInfo(cmd=%d)", (int)iInfoCmd);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DEV_HANDLE, DEVICE_INFO_CMD, INFO_DATATYPE*, void*, size_t*);
    pfn f = (pfn)real("DevGetInfo");
    return f ? f(hDevice, iInfoCmd, piType, pBuffer, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DevClose(DEV_HANDLE hDevice) {
    plog("DevClose()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DEV_HANDLE);
    pfn f = (pfn)real("DevClose");
    return f ? f(hDevice) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DevGetParentIF(DEV_HANDLE hDevice, IF_HANDLE *phIface) {
    plog("DevGetParentIF()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DEV_HANDLE, IF_HANDLE*);
    pfn f = (pfn)real("DevGetParentIF");
    return f ? f(hDevice, phIface) : GC_ERR_NOT_IMPLEMENTED;
}

GC_ERROR GC_CALLTYPE DSAnnounceBuffer(DS_HANDLE hDataStream, void *pBuffer, size_t iSize, void *pPrivate, BUFFER_HANDLE *phBuffer) {
    plog("DSAnnounceBuffer(size=%zu)", iSize);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DS_HANDLE, void*, size_t, void*, BUFFER_HANDLE*);
    pfn f = (pfn)real("DSAnnounceBuffer");
    return f ? f(hDataStream, pBuffer, iSize, pPrivate, phBuffer) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSAllocAndAnnounceBuffer(DS_HANDLE hDataStream, size_t iBufferSize, void *pPrivate, BUFFER_HANDLE *phBuffer) {
    plog("DSAllocAndAnnounceBuffer(size=%zu)", iBufferSize);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DS_HANDLE, size_t, void*, BUFFER_HANDLE*);
    pfn f = (pfn)real("DSAllocAndAnnounceBuffer");
    return f ? f(hDataStream, iBufferSize, pPrivate, phBuffer) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSRevokeBuffer(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, void **pBuffer, void **pPrivate) {
    plog("DSRevokeBuffer()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DS_HANDLE, BUFFER_HANDLE, void**, void**);
    pfn f = (pfn)real("DSRevokeBuffer");
    return f ? f(hDataStream, hBuffer, pBuffer, pPrivate) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSQueueBuffer(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer) {
    plog("DSQueueBuffer()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DS_HANDLE, BUFFER_HANDLE);
    pfn f = (pfn)real("DSQueueBuffer");
    return f ? f(hDataStream, hBuffer) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSGetParentDev(DS_HANDLE hDataStream, DEV_HANDLE *phDevice) {
    plog("DSGetParentDev()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DS_HANDLE, DEV_HANDLE*);
    pfn f = (pfn)real("DSGetParentDev");
    return f ? f(hDataStream, phDevice) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSFlushQueue(DS_HANDLE hDataStream, ACQ_QUEUE_TYPE iOperation) {
    plog("DSFlushQueue()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DS_HANDLE, ACQ_QUEUE_TYPE);
    pfn f = (pfn)real("DSFlushQueue");
    return f ? f(hDataStream, iOperation) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSStartAcquisition(DS_HANDLE hDataStream, ACQ_START_FLAGS iStartFlags, uint64_t iNumToAcquire) {
    plog("DSStartAcquisition()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DS_HANDLE, ACQ_START_FLAGS, uint64_t);
    pfn f = (pfn)real("DSStartAcquisition");
    return f ? f(hDataStream, iStartFlags, iNumToAcquire) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSStopAcquisition(DS_HANDLE hDataStream, ACQ_STOP_FLAGS iStopFlags) {
    plog("DSStopAcquisition()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DS_HANDLE, ACQ_STOP_FLAGS);
    pfn f = (pfn)real("DSStopAcquisition");
    return f ? f(hDataStream, iStopFlags) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSGetInfo(DS_HANDLE hDataStream, DS_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    plog("DSGetInfo(cmd=%d)", (int)iInfoCmd);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DS_HANDLE, DS_INFO_CMD, INFO_DATATYPE*, void*, size_t*);
    pfn f = (pfn)real("DSGetInfo");
    return f ? f(hDataStream, iInfoCmd, piType, pBuffer, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSGetBufferID(DS_HANDLE hDataStream, uint32_t iIndex, BUFFER_HANDLE *phBuffer) {
    plog("DSGetBufferID(idx=%u)", iIndex);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DS_HANDLE, uint32_t, BUFFER_HANDLE*);
    pfn f = (pfn)real("DSGetBufferID");
    return f ? f(hDataStream, iIndex, phBuffer) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSGetBufferInfo(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, BUFFER_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    plog("DSGetBufferInfo(cmd=%d)", (int)iInfoCmd);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DS_HANDLE, BUFFER_HANDLE, BUFFER_INFO_CMD, INFO_DATATYPE*, void*, size_t*);
    pfn f = (pfn)real("DSGetBufferInfo");
    return f ? f(hDataStream, hBuffer, iInfoCmd, piType, pBuffer, piSize) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSGetBufferChunkData(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, void *pChunkData, size_t *piNumChunks) {
    plog("DSGetBufferChunkData()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DS_HANDLE, BUFFER_HANDLE, void*, size_t*);
    pfn f = (pfn)real("DSGetBufferChunkData");
    return f ? f(hDataStream, hBuffer, pChunkData, piNumChunks) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSClose(DS_HANDLE hDataStream) {
    plog("DSClose()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DS_HANDLE);
    pfn f = (pfn)real("DSClose");
    return f ? f(hDataStream) : GC_ERR_NOT_IMPLEMENTED;
}
/* IDS-only multi-part buffer extensions (part of the real 81, not in our
 * gentl_v2.h's 55-common set -- declare inline here since they're only
 * needed by this proxy). */
GC_ERROR GC_CALLTYPE DSGetNumBufferParts(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, uint32_t *piNumParts) {
    plog("DSGetNumBufferParts()");
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DS_HANDLE, BUFFER_HANDLE, uint32_t*);
    pfn f = (pfn)real("DSGetNumBufferParts");
    return f ? f(hDataStream, hBuffer, piNumParts) : GC_ERR_NOT_IMPLEMENTED;
}
GC_ERROR GC_CALLTYPE DSGetBufferPartInfo(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, uint32_t iPartIndex, int32_t iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize) {
    plog("DSGetBufferPartInfo(part=%u cmd=%d)", iPartIndex, iInfoCmd);
    typedef GC_ERROR (GC_CALLTYPE *pfn)(DS_HANDLE, BUFFER_HANDLE, uint32_t, int32_t, INFO_DATATYPE*, void*, size_t*);
    pfn f = (pfn)real("DSGetBufferPartInfo");
    return f ? f(hDataStream, hBuffer, iPartIndex, iInfoCmd, piType, pBuffer, piSize) : GC_ERR_NOT_IMPLEMENTED;
}

/* ================= DllMain ================= */
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst; (void)reserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            InitializeCriticalSection(&g_log_lock);
            log_resolve_path();
            g_log_ready = 1;
            char target_path[MAX_PATH];
            DWORD n = GetEnvironmentVariableA("KINEO_PROXY_TARGET", target_path, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) {
                plog("!!! KINEO_PROXY_TARGET env var not set -- proxy cannot load real target!");
            } else {
                g_target = LoadLibraryA(target_path);
                plog("=== DllMain PROCESS_ATTACH  target=%s  loaded=%p ===", target_path, (void*)g_target);
                if (!g_target) plog("!!! LoadLibraryA(%s) FAILED, err=%lu", target_path, GetLastError());
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            plog("=== DllMain PROCESS_DETACH ===");
            if (g_target) FreeLibrary(g_target);
            if (g_log_ready) { DeleteCriticalSection(&g_log_lock); g_log_ready = 0; }
            break;
        default:
            break;
    }
    return TRUE;
}
