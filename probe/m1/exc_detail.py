import os, sys
cti_path = os.path.abspath(sys.argv[1])
script_dir = os.path.dirname(os.path.abspath(__file__))
pkg_dir = os.path.join(script_dir, "pip_ids_peak_190")
os.add_dll_directory(os.path.join(pkg_dir, "ids_peak"))
sys.path.insert(0, pkg_dir)
import ids_peak.ids_peak as ids_peak  # noqa: E402

ids_peak.Library.Initialize()
try:
    producer = ids_peak.ProducerLibrary.Open(cti_path)
    print("opened OK")
except Exception as e:
    print("type:", type(e))
    print("mro:", [c.__name__ for c in type(e).__mro__])
    print("args:", e.args)
    print("dir:", [a for a in dir(e) if not a.startswith('_')])
    for a in dir(e):
        if not a.startswith('_') and a not in ('args', 'with_traceback'):
            try:
                print(f"  .{a} = {getattr(e, a)!r}")
            except Exception as e2:
                print(f"  .{a} raised {e2!r}")
finally:
    ids_peak.Library.Close()
