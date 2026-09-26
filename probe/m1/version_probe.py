# version_probe.py — read-only diagnostic: what GenTL major/minor version
# does a REAL, working IDS CTI report via the exact same query path
# ids_peak.dll used to reject our probe? Opens the real CTI only as a
# producer library (no camera/device interaction) -- purely informational.
import os
import sys

cti_path = os.path.abspath(sys.argv[1])
script_dir = os.path.dirname(os.path.abspath(__file__))
pkg_dir = os.path.join(script_dir, "pip_ids_peak_190")
os.add_dll_directory(os.path.join(pkg_dir, "ids_peak"))
sys.path.insert(0, pkg_dir)
import ids_peak.ids_peak as ids_peak  # noqa: E402

ids_peak.Library.Initialize()
try:
    producer = ids_peak.ProducerLibrary.Open(cti_path)
    sys_desc = producer.System()
    system = sys_desc.OpenSystem()
    print(f"GenTLVersionMajor = {system.GenTLVersionMajor()}")
    print(f"GenTLVersionMinor = {system.GenTLVersionMinor()}")
    print(f"CharacterEncoding = {system.CharacterEncoding()}")
finally:
    ids_peak.Library.Close()
