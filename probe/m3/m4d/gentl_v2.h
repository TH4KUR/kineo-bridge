/* gentl.h — minimal, standard-accurate GenTL (GenICam GenTL 1.5) declarations.
 *
 * Hand-written for the Kineo bridge probe producer + harness (M0). No GenTL.h
 * ships anywhere on this machine, so this reproduces the subset of the EMVA
 * GenTL standard C interface that M0 needs. Signatures, enum values, and the
 * calling convention follow the published GenTL 1.5 spec so that the same
 * header is ABI-correct when a real GenTL consumer (ids_peak.dll) loads the
 * producer in M1.
 *
 * On Windows x86-64 there is a single calling convention; __cdecl is a no-op
 * and export names are undecorated (no leading underscore, no @N suffix).
 */
/* v2: extended for M1 iteration — adds Event, DS, and stacked-port
 * declarations that M0 deliberately omitted (streaming was out of scope).
 * Added
 * after M1a evidence showed ids_peak.dll rejects a producer library whose
 * export table is missing part of the common mandatory GenTL set (Phase 2's
 * "55 common exports" finding) before ever calling into it — see
 * investigation/progress.md M1 section. Still stub-only bodies; no real
 * streaming semantics yet (that remains M4).
 */
#ifndef KINEO_GENTL_H
#define KINEO_GENTL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GC_CALLTYPE
#  if defined(_WIN32)
#    define GC_CALLTYPE __cdecl
#  else
#    define GC_CALLTYPE
#  endif
#endif

typedef int32_t GC_ERROR;
typedef uint8_t bool8_t;

typedef void *TL_HANDLE;
typedef void *IF_HANDLE;
typedef void *DEV_HANDLE;
typedef void *DS_HANDLE;
typedef void *PORT_HANDLE;
typedef void *BUFFER_HANDLE;
typedef void *EVENT_HANDLE;
typedef void *EVENTSRC_HANDLE;

/* GC_ERROR_LIST */
enum GC_ERROR_LIST {
    GC_ERR_SUCCESS             = 0,
    GC_ERR_ERROR              = -1001,
    GC_ERR_NOT_INITIALIZED    = -1002,
    GC_ERR_NOT_IMPLEMENTED    = -1003,
    GC_ERR_RESOURCE_IN_USE    = -1004,
    GC_ERR_ACCESS_DENIED      = -1005,
    GC_ERR_INVALID_HANDLE     = -1006,
    GC_ERR_INVALID_ID         = -1007,
    GC_ERR_NO_DATA            = -1008,
    GC_ERR_INVALID_PARAMETER  = -1009,
    GC_ERR_IO                 = -1010,
    GC_ERR_TIMEOUT            = -1011,
    GC_ERR_ABORT              = -1012,
    GC_ERR_INVALID_BUFFER     = -1013,
    GC_ERR_NOT_AVAILABLE      = -1014,
    GC_ERR_INVALID_ADDRESS    = -1015,
    GC_ERR_BUFFER_TOO_SMALL   = -1016,
    GC_ERR_INVALID_INDEX      = -1017,
    GC_ERR_PARSING_ERROR      = -1018,
    GC_ERR_INVALID_VALUE      = -1019,
    GC_ERR_RESOURCE_EXHAUSTED = -1020,
    GC_ERR_OUT_OF_MEMORY      = -1021,
    GC_ERR_BUSY               = -1022,
    GC_ERR_AMBIGUOUS          = -1023,
    GC_ERR_CUSTOM_ID          = -10000
};

/* INFO_DATATYPE_LIST */
enum INFO_DATATYPE_LIST {
    INFO_DATATYPE_UNKNOWN    = 0,
    INFO_DATATYPE_STRING     = 1,
    INFO_DATATYPE_STRINGLIST = 2,
    INFO_DATATYPE_INT16      = 3,
    INFO_DATATYPE_UINT16     = 4,
    INFO_DATATYPE_INT32      = 5,
    INFO_DATATYPE_UINT32     = 6,
    INFO_DATATYPE_INT64      = 7,
    INFO_DATATYPE_UINT64     = 8,
    INFO_DATATYPE_FLOAT64    = 9,
    INFO_DATATYPE_PTR        = 10,
    INFO_DATATYPE_BOOL8      = 11,
    INFO_DATATYPE_SIZET      = 12,
    INFO_DATATYPE_BUFFER     = 13,
    INFO_DATATYPE_PTRDIFF    = 14
};
typedef int32_t INFO_DATATYPE;

/* TL_INFO_CMD_LIST — used by GCGetInfo (library) and TLGetInfo (system) */
enum TL_INFO_CMD_LIST {
    TL_INFO_ID              = 0,  /* STRING */
    TL_INFO_VENDOR          = 1,  /* STRING */
    TL_INFO_MODEL           = 2,  /* STRING */
    TL_INFO_VERSION         = 3,  /* STRING */
    TL_INFO_TLTYPE          = 4,  /* STRING */
    TL_INFO_NAME            = 5,  /* STRING */
    TL_INFO_PATHNAME        = 6,  /* STRING */
    TL_INFO_DISPLAYNAME     = 7,  /* STRING */
    TL_INFO_CHAR_ENCODING   = 8,  /* INT32  */
    TL_INFO_GENTL_VER_MAJOR = 9,  /* UINT32 */
    TL_INFO_GENTL_VER_MINOR = 10  /* UINT32 */
};
typedef int32_t TL_INFO_CMD;

/* TL_CHAR_ENCODING_LIST */
enum TL_CHAR_ENCODING_LIST {
    TL_CHAR_ENCODING_ASCII = 0,
    TL_CHAR_ENCODING_UTF8  = 1
};

/* INTERFACE_INFO_CMD_LIST */
enum INTERFACE_INFO_CMD_LIST {
    INTERFACE_INFO_ID          = 0, /* STRING */
    INTERFACE_INFO_DISPLAYNAME = 1, /* STRING */
    INTERFACE_INFO_TLTYPE      = 2  /* STRING */
};
typedef int32_t INTERFACE_INFO_CMD;

/* DEVICE_ACCESS_FLAGS_LIST — passed to IFOpenDevice */
enum DEVICE_ACCESS_FLAGS_LIST {
    DEVICE_ACCESS_UNKNOWN   = 0,
    DEVICE_ACCESS_NONE      = 1,
    DEVICE_ACCESS_READONLY  = 2,
    DEVICE_ACCESS_CONTROL   = 3,
    DEVICE_ACCESS_EXCLUSIVE = 4
};
typedef int32_t DEVICE_ACCESS_FLAGS;

/* DEVICE_ACCESS_STATUS_LIST */
enum DEVICE_ACCESS_STATUS_LIST {
    DEVICE_ACCESS_STATUS_UNKNOWN   = 0,
    DEVICE_ACCESS_STATUS_READWRITE = 1,
    DEVICE_ACCESS_STATUS_READONLY  = 2,
    DEVICE_ACCESS_STATUS_NOACCESS  = 3
};

/* DEVICE_INFO_CMD_LIST */
enum DEVICE_INFO_CMD_LIST {
    DEVICE_INFO_ID                  = 0,  /* STRING */
    DEVICE_INFO_VENDOR              = 1,  /* STRING */
    DEVICE_INFO_MODEL               = 2,  /* STRING */
    DEVICE_INFO_TLTYPE              = 3,  /* STRING */
    DEVICE_INFO_DISPLAYNAME         = 4,  /* STRING */
    DEVICE_INFO_ACCESS_STATUS       = 5,  /* INT32  */
    DEVICE_INFO_USER_DEFINED_NAME   = 6,  /* STRING */
    DEVICE_INFO_SERIAL_NUMBER       = 7,  /* STRING */
    DEVICE_INFO_VERSION             = 8,  /* STRING */
    DEVICE_INFO_TIMESTAMP_FREQUENCY = 9   /* UINT64 */
};
typedef int32_t DEVICE_INFO_CMD;

/* PORT_INFO_CMD_LIST (subset — used by GCGetPortInfo stub) */
enum PORT_INFO_CMD_LIST {
    PORT_INFO_ID          = 0, /* STRING */
    PORT_INFO_VENDOR      = 1, /* STRING */
    PORT_INFO_MODEL       = 2, /* STRING */
    PORT_INFO_TLTYPE      = 3, /* STRING */
    PORT_INFO_MODULE      = 4, /* STRING */
    PORT_INFO_LITTLE_ENDIAN = 5, /* BOOL8 */
    PORT_INFO_BIG_ENDIAN  = 6, /* BOOL8 */
    PORT_INFO_ACCESS_READ = 7, /* BOOL8 */
    PORT_INFO_ACCESS_WRITE= 8, /* BOOL8 */
    PORT_INFO_ACCESS_NA   = 9, /* BOOL8 */
    PORT_INFO_ACCESS_NI   = 10,/* BOOL8 */
    PORT_INFO_VERSION     = 11,/* STRING */
    PORT_INFO_PORTNAME    = 12 /* STRING */
};
typedef int32_t PORT_INFO_CMD;

enum URL_INFO_CMD_LIST {
    URL_INFO_URL          = 0,
    URL_INFO_SCHEMA_VER_MAJOR = 1,
    URL_INFO_SCHEMA_VER_MINOR = 2,
    URL_INFO_FILE_VER_MAJOR = 3,
    URL_INFO_FILE_VER_MINOR = 4,
    URL_INFO_FILE_VER_SUBMINOR = 5,
    URL_INFO_FILE_SHA1_HASH = 6,
    URL_INFO_FILE_REGISTER_ADDRESS = 7,
    URL_INFO_FILE_SIZE    = 8,
    URL_INFO_SCHEME       = 9,
    URL_INFO_FILENAME     = 10
};
typedef int32_t URL_INFO_CMD;

enum URL_SCHEME_IDS {
    URL_SCHEME_LOCAL = 0,
    URL_SCHEME_HTTP  = 1,
    URL_SCHEME_FILE  = 2
};
typedef int32_t URL_SCHEME_ID;

typedef uint64_t EVENT_TYPE;

/* EVENT_TYPE_LIST (subset) */
#define EVENT_ERROR             0
#define EVENT_NEW_BUFFER        1000
#define EVENT_FEATURE_INVALIDATE 2000
#define EVENT_FEATURE_CHANGE    2001
#define EVENT_REMOTE_DEVICE     3000

/* BUFFER_INFO_CMD_LIST (subset) */
enum BUFFER_INFO_CMD_LIST {
    BUFFER_INFO_BASE          = 0,  /* PTR    */
    BUFFER_INFO_SIZE          = 1,  /* SIZET  */
    BUFFER_INFO_USER_PTR      = 2,  /* PTR    */
    BUFFER_INFO_TIMESTAMP     = 3,  /* UINT64 */
    BUFFER_INFO_NEW_DATA      = 4,  /* BOOL8  */
    BUFFER_INFO_IS_QUEUED     = 5,  /* BOOL8  */
    BUFFER_INFO_IS_ACQUIRING  = 6,  /* BOOL8  */
    BUFFER_INFO_IS_INCOMPLETE = 7,  /* BOOL8  */
    BUFFER_INFO_TLTYPE        = 8,  /* STRING */
    BUFFER_INFO_SIZE_FILLED   = 9,  /* SIZET  */
    BUFFER_INFO_WIDTH         = 10, /* SIZET  */
    BUFFER_INFO_HEIGHT        = 11, /* SIZET  */
    BUFFER_INFO_XOFFSET       = 12, /* SIZET  */
    BUFFER_INFO_YOFFSET       = 13, /* SIZET  */
    BUFFER_INFO_PIXELFORMAT   = 14, /* UINT64 */
    BUFFER_INFO_PIXEL_ENDIANNESS = 20, /* INT32 */
    BUFFER_INFO_DATA_SIZE     = 21, /* SIZET */
    BUFFER_INFO_FRAMEID       = 24, /* UINT64 */
    BUFFER_INFO_IMAGEPRESENT  = 25, /* BOOL8 */
    BUFFER_INFO_TIMESTAMP_NS  = 28, /* UINT64 */
    BUFFER_INFO_DATA_LARGER_THAN_BUFFER = 29, /* BOOL8 */
    BUFFER_INFO_CONTAINS_CHUNKDATA = 30, /* BOOL8 */
    BUFFER_INFO_PAYLOADTYPE   = 32 /* UINT32 */
};
typedef int32_t BUFFER_INFO_CMD;

/* M4a: corrected to match the official GenTL v1.5 header exactly
 * (STREAM_INFO_CMD_LIST) -- the previous numbering here was a latent,
 * harmless ABI mismatch (never functionally exercised, since our code
 * always switched on the raw numeric iInfoCmd rather than these symbolic
 * names). Now load-bearing: real Kineo sends numeric cmd values that must
 * match this table for our logging to name them correctly. */
enum DS_INFO_CMD_LIST {
    STREAM_INFO_ID                   = 0,  /* STRING */
    STREAM_INFO_NUM_DELIVERED        = 1,
    STREAM_INFO_NUM_UNDERRUN         = 2,
    STREAM_INFO_NUM_ANNOUNCED        = 3,
    STREAM_INFO_NUM_QUEUED           = 4,
    STREAM_INFO_NUM_AWAIT_DELIVERY   = 5,
    STREAM_INFO_NUM_STARTED          = 6,
    STREAM_INFO_PAYLOAD_SIZE         = 7,  /* SIZET */
    STREAM_INFO_IS_GRABBING          = 8,  /* BOOL8 */
    STREAM_INFO_DEFINES_PAYLOADSIZE  = 9,  /* BOOL8 */
    STREAM_INFO_TLTYPE               = 10, /* STRING */
    STREAM_INFO_NUM_CHUNKS_MAX       = 11,
    STREAM_INFO_BUF_ANNOUNCE_MIN     = 12, /* SIZET, GenTL v1.3 */
    STREAM_INFO_BUF_ALIGNMENT        = 13,
    STREAM_INFO_CUSTOM_ID            = 1000
};
typedef int32_t DS_INFO_CMD;

typedef int32_t ACQ_START_FLAGS;
typedef int32_t ACQ_STOP_FLAGS;
typedef int32_t ACQ_QUEUE_TYPE;
#define ACQ_START_FLAGS_DEFAULT 0
#define ACQ_STOP_FLAGS_DEFAULT 0
#define ACQ_QUEUE_ALL_DISCARD 2
#define ACQ_QUEUE_ALL_TO_INPUT 3

/* Event/DS handles reuse EVENTSRC_HANDLE/DS_HANDLE/BUFFER_HANDLE/EVENT_HANDLE
 * already typedef'd above. */

/* ---- GenTL C interface function declarations (subset) ---- */

/* Library / system (GC*) */
GC_ERROR GC_CALLTYPE GCInitLib(void);
GC_ERROR GC_CALLTYPE GCCloseLib(void);
GC_ERROR GC_CALLTYPE GCGetInfo(TL_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize);
GC_ERROR GC_CALLTYPE GCGetLastError(GC_ERROR *piErrorCode, char *sErrorText, size_t *piSize);

/* Port access (GC*) — stubbed at M0, real at M3 */
GC_ERROR GC_CALLTYPE GCReadPort(PORT_HANDLE hPort, uint64_t iAddress, void *pBuffer, size_t *piSize);
GC_ERROR GC_CALLTYPE GCWritePort(PORT_HANDLE hPort, uint64_t iAddress, const void *pBuffer, size_t *piSize);
GC_ERROR GC_CALLTYPE GCGetPortURL(PORT_HANDLE hPort, char *sURL, size_t *piSize);
GC_ERROR GC_CALLTYPE GCGetNumPortURLs(PORT_HANDLE hPort, uint32_t *piNumURLs);
GC_ERROR GC_CALLTYPE GCGetPortURLInfo(PORT_HANDLE hPort, uint32_t iURLIndex, URL_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize);
GC_ERROR GC_CALLTYPE GCGetPortInfo(PORT_HANDLE hPort, PORT_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize);

/* Transport layer (TL*) */
GC_ERROR GC_CALLTYPE TLOpen(TL_HANDLE *phTL);
GC_ERROR GC_CALLTYPE TLClose(TL_HANDLE hTL);
GC_ERROR GC_CALLTYPE TLGetInfo(TL_HANDLE hTL, TL_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize);
GC_ERROR GC_CALLTYPE TLGetNumInterfaces(TL_HANDLE hTL, uint32_t *piNumIfaces);
GC_ERROR GC_CALLTYPE TLGetInterfaceID(TL_HANDLE hTL, uint32_t iIndex, char *sID, size_t *piSize);
GC_ERROR GC_CALLTYPE TLGetInterfaceInfo(TL_HANDLE hTL, const char *sIfaceID, INTERFACE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize);
GC_ERROR GC_CALLTYPE TLOpenInterface(TL_HANDLE hTL, const char *sIfaceID, IF_HANDLE *phIface);
GC_ERROR GC_CALLTYPE TLUpdateInterfaceList(TL_HANDLE hTL, bool8_t *pbChanged, uint64_t iTimeout);

/* Interface (IF*) */
GC_ERROR GC_CALLTYPE IFClose(IF_HANDLE hIface);
GC_ERROR GC_CALLTYPE IFGetInfo(IF_HANDLE hIface, INTERFACE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize);
GC_ERROR GC_CALLTYPE IFGetNumDevices(IF_HANDLE hIface, uint32_t *piNumDevices);
GC_ERROR GC_CALLTYPE IFGetDeviceID(IF_HANDLE hIface, uint32_t iIndex, char *sID, size_t *piSize);
GC_ERROR GC_CALLTYPE IFUpdateDeviceList(IF_HANDLE hIface, bool8_t *pbChanged, uint64_t iTimeout);
GC_ERROR GC_CALLTYPE IFGetDeviceInfo(IF_HANDLE hIface, const char *sDeviceID, DEVICE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize);
GC_ERROR GC_CALLTYPE IFOpenDevice(IF_HANDLE hIface, const char *sDeviceID, DEVICE_ACCESS_FLAGS iOpenFlags, DEV_HANDLE *phDevice);
GC_ERROR GC_CALLTYPE IFGetParentTL(IF_HANDLE hIface, TL_HANDLE *phSystem);

/* Device (Dev*) */
GC_ERROR GC_CALLTYPE DevGetPort(DEV_HANDLE hDevice, PORT_HANDLE *phRemoteDevice);
GC_ERROR GC_CALLTYPE DevGetNumDataStreams(DEV_HANDLE hDevice, uint32_t *piNumDataStreams);
GC_ERROR GC_CALLTYPE DevGetDataStreamID(DEV_HANDLE hDevice, uint32_t iIndex, char *sDataStreamID, size_t *piSize);
GC_ERROR GC_CALLTYPE DevOpenDataStream(DEV_HANDLE hDevice, const char *sDataStreamID, DS_HANDLE *phDataStream);
GC_ERROR GC_CALLTYPE DevGetInfo(DEV_HANDLE hDevice, DEVICE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize);
GC_ERROR GC_CALLTYPE DevClose(DEV_HANDLE hDevice);
GC_ERROR GC_CALLTYPE DevGetParentIF(DEV_HANDLE hDevice, IF_HANDLE *phIface);

/* Stacked port access + event registration (GC*) — added for M1 */
GC_ERROR GC_CALLTYPE GCReadPortStacked(PORT_HANDLE hPort, void *pEntries, size_t iNumEntries);
GC_ERROR GC_CALLTYPE GCWritePortStacked(PORT_HANDLE hPort, void *pEntries, size_t iNumEntries);
GC_ERROR GC_CALLTYPE GCRegisterEvent(EVENTSRC_HANDLE hEventSrc, EVENT_TYPE iEventID, EVENT_HANDLE *phEvent);
GC_ERROR GC_CALLTYPE GCUnregisterEvent(EVENTSRC_HANDLE hEventSrc, EVENT_TYPE iEventID);

/* Data stream / buffer (DS*) — stub bodies only; real streaming is M4 */
GC_ERROR GC_CALLTYPE DSAnnounceBuffer(DS_HANDLE hDataStream, void *pBuffer, size_t iSize, void *pPrivate, BUFFER_HANDLE *phBuffer);
GC_ERROR GC_CALLTYPE DSAllocAndAnnounceBuffer(DS_HANDLE hDataStream, size_t iBufferSize, void *pPrivate, BUFFER_HANDLE *phBuffer);
GC_ERROR GC_CALLTYPE DSRevokeBuffer(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, void **pBuffer, void **pPrivate);
GC_ERROR GC_CALLTYPE DSQueueBuffer(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer);
GC_ERROR GC_CALLTYPE DSGetParentDev(DS_HANDLE hDataStream, DEV_HANDLE *phDevice);
GC_ERROR GC_CALLTYPE DSFlushQueue(DS_HANDLE hDataStream, ACQ_QUEUE_TYPE iOperation);
GC_ERROR GC_CALLTYPE DSStartAcquisition(DS_HANDLE hDataStream, ACQ_START_FLAGS iStartFlags, uint64_t iNumToAcquire);
GC_ERROR GC_CALLTYPE DSStopAcquisition(DS_HANDLE hDataStream, ACQ_STOP_FLAGS iStopFlags);
GC_ERROR GC_CALLTYPE DSGetInfo(DS_HANDLE hDataStream, DS_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize);
GC_ERROR GC_CALLTYPE DSGetBufferID(DS_HANDLE hDataStream, uint32_t iIndex, BUFFER_HANDLE *phBuffer);
GC_ERROR GC_CALLTYPE DSGetBufferInfo(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, BUFFER_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize);
GC_ERROR GC_CALLTYPE DSGetBufferChunkData(DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, void *pChunkData, size_t *piNumChunks);
GC_ERROR GC_CALLTYPE DSClose(DS_HANDLE hDataStream);

/* Events (Event*) — stub bodies only */
GC_ERROR GC_CALLTYPE EventGetData(EVENT_HANDLE hEvent, void *pBuffer, size_t *piSize, uint64_t iTimeout);
GC_ERROR GC_CALLTYPE EventGetInfo(EVENT_HANDLE hEvent, int32_t iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize);
GC_ERROR GC_CALLTYPE EventFlush(EVENT_HANDLE hEvent);
GC_ERROR GC_CALLTYPE EventGetDataInfo(EVENT_HANDLE hEvent, const void *pInBuffer, size_t iInSize, int32_t iInfoCmd, INFO_DATATYPE *piType, void *pOutBuffer, size_t *piOutSize);
GC_ERROR GC_CALLTYPE EventKill(EVENT_HANDLE hEvent);

#ifdef __cplusplus
}
#endif

#endif /* KINEO_GENTL_H */
