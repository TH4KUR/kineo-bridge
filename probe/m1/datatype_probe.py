# datatype_probe.py — check the exact RawInformation.DataType tag a real,
# working CTI reports for TL_INFO_GENTL_VER_MAJOR(9)/MINOR(10), via
# SystemDescriptor.Info() -- called BEFORE OpenSystem(), so this queries the
# same library-level GCGetInfo path ids_peak's construct-time validation uses.
import os
import sys

cti_path = os.path.abspath(sys.argv[1])
script_dir = os.path.dirname(os.path.abspath(__file__))
pkg_dir = os.path.join(script_dir, "pip_ids_peak_190")
os.add_dll_directory(os.path.join(pkg_dir, "ids_peak"))
sys.path.insert(0, pkg_dir)
import ids_peak.ids_peak as ids_peak  # noqa: E402

DATATYPE_NAMES = {
    0: "UNKNOWN", 1: "STRING", 2: "STRINGLIST", 3: "INT16", 4: "UINT16",
    5: "INT32", 6: "UINT32", 7: "INT64", 8: "UINT64", 9: "FLOAT64",
    10: "PTR", 11: "BOOL8", 12: "SIZET", 13: "BUFFER", 14: "PTRDIFF",
}

ids_peak.Library.Initialize()
try:
    producer = ids_peak.ProducerLibrary.Open(cti_path)
    sys_desc = producer.System()
    for cmd, name in [(0, "TL_INFO_ID"), (1, "TL_INFO_VENDOR"), (4, "TL_INFO_TLTYPE"),
                       (8, "TL_INFO_CHAR_ENCODING"), (9, "TL_INFO_GENTL_VER_MAJOR"), (10, "TL_INFO_GENTL_VER_MINOR")]:
        try:
            info = sys_desc.Info(cmd)
            dt = info.DataType
            data = bytes(info.Data)
            print(f"cmd={cmd:2d} {name:28s} DataType={dt} ({DATATYPE_NAMES.get(dt,'?')})  raw_bytes={data.hex()}  len={len(data)}")
        except Exception as e:
            print(f"cmd={cmd:2d} {name:28s} EXCEPTION: {e}")
finally:
    ids_peak.Library.Close()
