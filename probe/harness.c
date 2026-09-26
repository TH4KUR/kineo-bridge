/* harness.c — M0 standalone test harness.
 *
 * LoadLibrary's the probe CTI by absolute path (argv[1]) and exercises it
 * exactly like a minimal GenTL consumer would for enumerate+open: GCInitLib,
 * TLOpen, enumerate 1 interface, TLOpenInterface, enumerate 1 device, read
 * its DEVICE_INFO fields, IFOpenDevice, DevGetInfo, then close everything in
 * reverse order. No Kineo, no ids_peak.dll, no environment variable is
 * touched at all — this only proves the probe itself is a correct, loadable,
 * enumerable, openable GenTL producer (PLAN.md M0).
 *
 * Usage: harness.exe <path-to-kineo_probe.cti>
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "gentl.h"

typedef GC_ERROR (GC_CALLTYPE *pfn_GCInitLib)(void);
typedef GC_ERROR (GC_CALLTYPE *pfn_GCCloseLib)(void);
typedef GC_ERROR (GC_CALLTYPE *pfn_GCGetInfo)(TL_INFO_CMD, INFO_DATATYPE*, void*, size_t*);
typedef GC_ERROR (GC_CALLTYPE *pfn_GCGetLastError)(GC_ERROR*, char*, size_t*);
typedef GC_ERROR (GC_CALLTYPE *pfn_TLOpen)(TL_HANDLE*);
typedef GC_ERROR (GC_CALLTYPE *pfn_TLClose)(TL_HANDLE);
typedef GC_ERROR (GC_CALLTYPE *pfn_TLGetNumInterfaces)(TL_HANDLE, uint32_t*);
typedef GC_ERROR (GC_CALLTYPE *pfn_TLGetInterfaceID)(TL_HANDLE, uint32_t, char*, size_t*);
typedef GC_ERROR (GC_CALLTYPE *pfn_TLOpenInterface)(TL_HANDLE, const char*, IF_HANDLE*);
typedef GC_ERROR (GC_CALLTYPE *pfn_TLUpdateInterfaceList)(TL_HANDLE, bool8_t*, uint64_t);
typedef GC_ERROR (GC_CALLTYPE *pfn_IFClose)(IF_HANDLE);
typedef GC_ERROR (GC_CALLTYPE *pfn_IFGetNumDevices)(IF_HANDLE, uint32_t*);
typedef GC_ERROR (GC_CALLTYPE *pfn_IFGetDeviceID)(IF_HANDLE, uint32_t, char*, size_t*);
typedef GC_ERROR (GC_CALLTYPE *pfn_IFUpdateDeviceList)(IF_HANDLE, bool8_t*, uint64_t);
typedef GC_ERROR (GC_CALLTYPE *pfn_IFGetDeviceInfo)(IF_HANDLE, const char*, DEVICE_INFO_CMD, INFO_DATATYPE*, void*, size_t*);
typedef GC_ERROR (GC_CALLTYPE *pfn_IFOpenDevice)(IF_HANDLE, const char*, DEVICE_ACCESS_FLAGS, DEV_HANDLE*);
typedef GC_ERROR (GC_CALLTYPE *pfn_DevGetInfo)(DEV_HANDLE, DEVICE_INFO_CMD, INFO_DATATYPE*, void*, size_t*);
typedef GC_ERROR (GC_CALLTYPE *pfn_DevClose)(DEV_HANDLE);

#define GETPROC(name) \
    pfn_##name name = (pfn_##name)GetProcAddress(mod, #name); \
    if (!name) { printf("FAIL: GetProcAddress(%s) failed, err=%lu\n", #name, GetLastError()); ok = 0; goto done; }

int main(int argc, char **argv) {
    int ok = 1;
    if (argc < 2) {
        printf("usage: %s <path-to-kineo_probe.cti>\n", argv[0]);
        return 2;
    }

    printf("=== M0 harness: loading %s ===\n", argv[1]);
    HMODULE mod = LoadLibraryA(argv[1]);
    if (!mod) {
        printf("FAIL: LoadLibraryA failed, err=%lu\n", GetLastError());
        return 1;
    }
    printf("OK: LoadLibraryA succeeded\n");

    GETPROC(GCInitLib)
    GETPROC(GCCloseLib)
    GETPROC(GCGetInfo)
    GETPROC(GCGetLastError)
    GETPROC(TLOpen)
    GETPROC(TLClose)
    GETPROC(TLGetNumInterfaces)
    GETPROC(TLGetInterfaceID)
    GETPROC(TLOpenInterface)
    GETPROC(TLUpdateInterfaceList)
    GETPROC(IFClose)
    GETPROC(IFGetNumDevices)
    GETPROC(IFGetDeviceID)
    GETPROC(IFUpdateDeviceList)
    GETPROC(IFGetDeviceInfo)
    GETPROC(IFOpenDevice)
    GETPROC(DevGetInfo)
    GETPROC(DevClose)
    printf("OK: all required GetProcAddress lookups resolved\n");

    GC_ERROR rc;

    rc = GCInitLib();
    printf("GCInitLib -> %d %s\n", rc, rc == GC_ERR_SUCCESS ? "OK" : "FAIL");
    if (rc != GC_ERR_SUCCESS) { ok = 0; goto done; }

    char idbuf[256];
    size_t sz = sizeof idbuf;
    INFO_DATATYPE dt;
    rc = GCGetInfo(TL_INFO_VENDOR, &dt, idbuf, &sz);
    printf("GCGetInfo(TL_INFO_VENDOR) -> %d, value=\"%s\"\n", rc, rc == GC_ERR_SUCCESS ? idbuf : "(n/a)");

    TL_HANDLE hTL = NULL;
    rc = TLOpen(&hTL);
    printf("TLOpen -> %d %s\n", rc, rc == GC_ERR_SUCCESS ? "OK" : "FAIL");
    if (rc != GC_ERR_SUCCESS) { ok = 0; goto done; }

    bool8_t changed = 0;
    rc = TLUpdateInterfaceList(hTL, &changed, 1000);
    printf("TLUpdateInterfaceList -> %d\n", rc);

    uint32_t nIfaces = 0;
    rc = TLGetNumInterfaces(hTL, &nIfaces);
    printf("TLGetNumInterfaces -> %d, count=%u %s\n", rc, nIfaces, (rc == GC_ERR_SUCCESS && nIfaces == 1) ? "OK" : "FAIL(expected 1)");
    if (nIfaces != 1) ok = 0;

    char ifaceId[256];
    sz = sizeof ifaceId;
    rc = TLGetInterfaceID(hTL, 0, ifaceId, &sz);
    printf("TLGetInterfaceID(0) -> %d, id=\"%s\"\n", rc, rc == GC_ERR_SUCCESS ? ifaceId : "(n/a)");
    if (rc != GC_ERR_SUCCESS) { ok = 0; goto done; }

    IF_HANDLE hIF = NULL;
    rc = TLOpenInterface(hTL, ifaceId, &hIF);
    printf("TLOpenInterface(\"%s\") -> %d %s\n", ifaceId, rc, rc == GC_ERR_SUCCESS ? "OK" : "FAIL");
    if (rc != GC_ERR_SUCCESS) { ok = 0; goto done; }

    rc = IFUpdateDeviceList(hIF, &changed, 1000);
    printf("IFUpdateDeviceList -> %d\n", rc);

    uint32_t nDevs = 0;
    rc = IFGetNumDevices(hIF, &nDevs);
    printf("IFGetNumDevices -> %d, count=%u %s\n", rc, nDevs, (rc == GC_ERR_SUCCESS && nDevs == 1) ? "OK" : "FAIL(expected 1)");
    if (nDevs != 1) ok = 0;

    char devId[256];
    sz = sizeof devId;
    rc = IFGetDeviceID(hIF, 0, devId, &sz);
    printf("IFGetDeviceID(0) -> %d, id=\"%s\"\n", rc, rc == GC_ERR_SUCCESS ? devId : "(n/a)");
    if (rc != GC_ERR_SUCCESS) { ok = 0; goto done; }

    /* Read the DEVICE_INFO fields a real GenTL consumer/device-picker would want */
    struct { DEVICE_INFO_CMD cmd; const char *name; } fields[] = {
        { DEVICE_INFO_VENDOR,        "VENDOR" },
        { DEVICE_INFO_MODEL,         "MODEL" },
        { DEVICE_INFO_SERIAL_NUMBER, "SERIAL_NUMBER" },
        { DEVICE_INFO_DISPLAYNAME,   "DISPLAYNAME" },
        { DEVICE_INFO_ACCESS_STATUS, "ACCESS_STATUS" },
    };
    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); i++) {
        char buf[256];
        size_t bs = sizeof buf;
        INFO_DATATYPE t;
        rc = IFGetDeviceInfo(hIF, devId, fields[i].cmd, &t, buf, &bs);
        if (t == INFO_DATATYPE_STRING)
            printf("IFGetDeviceInfo(%s) -> %d, value=\"%s\"\n", fields[i].name, rc, rc == GC_ERR_SUCCESS ? buf : "(n/a)");
        else
            printf("IFGetDeviceInfo(%s) -> %d, (non-string type=%d)\n", fields[i].name, rc, (int)t);
    }

    DEV_HANDLE hDev = NULL;
    rc = IFOpenDevice(hIF, devId, DEVICE_ACCESS_CONTROL, &hDev);
    printf("IFOpenDevice(\"%s\", CONTROL) -> %d %s\n", devId, rc, rc == GC_ERR_SUCCESS ? "OK - DEVICE OPENED" : "FAIL");
    if (rc != GC_ERR_SUCCESS) { ok = 0; goto done; }

    char model[256];
    sz = sizeof model;
    rc = DevGetInfo(hDev, DEVICE_INFO_MODEL, &dt, model, &sz);
    printf("DevGetInfo(MODEL) -> %d, value=\"%s\"\n", rc, rc == GC_ERR_SUCCESS ? model : "(n/a)");

    rc = DevClose(hDev);
    printf("DevClose -> %d\n", rc);
    rc = IFClose(hIF);
    printf("IFClose -> %d\n", rc);
    rc = TLClose(hTL);
    printf("TLClose -> %d\n", rc);
    rc = GCCloseLib();
    printf("GCCloseLib -> %d\n", rc);

done:
    FreeLibrary(mod);
    printf("=== M0 harness result: %s ===\n", ok ? "PASS (device enumerated and opened)" : "FAIL");
    return ok ? 0 : 1;
}
