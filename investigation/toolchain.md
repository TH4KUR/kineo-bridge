# Phase 5 — Build Toolchain: Producing a PE32+ x86-64 DLL from ARM64 WSL

Goal: determine the most reliable way to build a standard PE32+ x86-64 DLL (a future GenTL `.cti`
producer, exporting a small flat `extern "C"` C ABI) from this Surface Pro 11 / Windows 11 ARM64 +
WSL2 Ubuntu ARM64 host, without installing anything large yet. All findings below came from
read-only queries (`apt-cache policy`, `apt list`, `apt-get install --dry-run`) — nothing was
installed.

## What's available now

- WSL Ubuntu ("resolute") apt repos are reachable and populated.
- `objdump`, `llvm-objdump`, `nm`, `strings`, `python3` are already installed (confirmed earlier
  phases).
- No `clang`, no `x86_64-w64-mingw32-*` mingw-w64 cross-compiler currently installed.
- No Visual Studio, no Windows SDK, no `cl.exe` anywhere on the Windows side (confirmed earlier
  phases).
- Free disk on `/`: 953GB available — plenty of headroom for any of the options below.

## What would need installing

### Option A — GNU mingw-w64 cross-compiler (WSL-side) — RECOMMENDED PRIMARY

Available directly via apt, not yet installed:

| package | candidate version |
|---|---|
| `gcc-mingw-w64-x86-64` | 13.2.0-6ubuntu1+26.1 |
| `g++-mingw-w64-x86-64` | 13.2.0-6ubuntu1+26.1 |
| `binutils-mingw-w64-x86-64` | 2.45.90.20260125-1ubuntu1+13.3 |
| `mingw-w64-x86-64-dev` | 13.0.0-2ubuntu1 (headers + import libs) |

`apt-get install --dry-run gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 binutils-mingw-w64-x86-64`
resolves cleanly to 9 packages total (posix + win32-runtime variants get pulled in automatically;
posix threading is the sane default). Combined footprint is small — each meta-package reports
`Installed-Size: 255` (KB-scale wrapper) with the real weight in the runtime/binutils
sub-packages, on the order of a few hundred MB total. Trivial relative to 953GB free.

This gives a genuine `x86_64-w64-mingw32-gcc` / `x86_64-w64-mingw32-g++` cross toolchain with its
own `ld`, `windres`, `dlltool`, and mingw CRT/headers — everything needed to emit a real PE32+
x86-64 DLL directly on ARM64 Linux, with no emulation involved.

**Install command (NOT YET RUN — needs separate explicit approval):**
```
sudo apt-get install -y gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 binutils-mingw-w64-x86-64 mingw-w64-x86-64-dev
```

### Option B — clang + lld targeting mingw (WSL-side) — FALLBACK

Also available via apt, not yet installed:

| package | candidate version |
|---|---|
| `clang` | 1:21.1.6-71 |
| `lld` | 1:21.1.6-71 |
| `clang-17` | 1:17.0.6-23ubuntu7 (pinned alt.) |
| `clang-18` | 1:18.1.8-20ubuntu8 (pinned alt.) |

Clang can cross-compile with `--target=x86_64-w64-mingw32`, but it does **not** ship its own mingw
sysroot/CRT — it needs the exact same `mingw-w64-x86-64-dev` headers/import libs that Option A
installs, plus a linker (`-fuse-ld=lld` or GNU `ld` from `binutils-mingw-w64-x86-64`). So in this
environment clang/lld is really "an alternate frontend on top of the Option-A sysroot," not an
independent lighter-weight path. No self-contained `llvm-mingw` distribution (which bundles its
own headers/CRT/lld so it needs no separate mingw-w64 packages) is available in these apt repos —
that only exists as a GitHub-releases tarball, a heavier/less-verified step than installing
Option A.

**Install command (on top of Option A, NOT YET RUN):**
```
sudo apt-get install -y clang lld
```

### Option C — Windows-side ARM64-hosted MSVC Build Tools — DEFERRED/HEAVIER FALLBACK

Not tested or downloaded (per the read-only constraint of this phase). Based on known Visual
Studio Build Tools behavior: recent VS 2022 (17.x) Build Tools releases publish an ARM64-hosted
`cl.exe`/`link.exe` package (host tools for ARM64, targeting x64/x86/ARM/ARM64), installable from
the Windows side via the bootstrapper:

```
vs_buildtools.exe --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended
```

plus a Windows 11 SDK component. This is a *Windows-side* install, separate from WSL, requires
internet access from Windows, and is multi-GB (typically 2–6+ GB depending on selected
components) — much heavier than Option A's few-hundred-MB WSL footprint. Kept as a fallback only
if a future phase discovers an actual hard MSVC-ABI requirement (not expected for a GenTL
producer, which only needs a flat C export table).

### Option D — wine (verification only, not a build tool)

`wine` / `wine64` (10.0~repack-12ubuntu1) available via apt, not installed. Useful later to sanity
check that a produced DLL loads and its export table resolves, before ever touching the real
Kineo hardware/machine. Not part of the build toolchain itself.

## Gotchas / risks identified

- **Export name decoration**: mingw honors `__declspec(dllexport)` the same way MSVC does.
  For `__stdcall` exports, plain mingw defaults can leave decorated names (`_Name@N`) unless a
  `.def` file (`EXPORTS` section) or `-Wl,--kill-at` is used to force undecorated names. GenTL's
  C interface expects clean, exact export names (e.g. `GCGetInfo`), so plan to pair
  `extern "C" __declspec(dllexport)` in source with a `.def` file or `--kill-at` to guarantee
  undecorated names.
- **Runtime DLL dependencies**: a default dynamic mingw-w64 build links against
  `libgcc_s_seh-1.dll`, `libstdc++-6.dll` (if C++), and `libwinpthread-1.dll` at runtime. These are
  **not present** on a stock Windows machine (like Kineo's), so the target machine would fail to
  load the DLL unless the runtime is statically linked in. **Recommendation: always pass
  `-static -static-libgcc -static-libstdc++`** (and `-static -lwinpthread` if threads are used) on
  the final producer build so the `.cti` has zero extra runtime-DLL dependencies to ship.
- **C++ ABI boundary discipline**: mingw vs. MSVC C++ name-mangling/STL-ABI differences are
  irrelevant here as long as no C++ types/STL objects cross the DLL export boundary — only
  `extern "C"` flat functions are exported per the GenTL spec, which this toolchain fully supports.
- **Confirm real PE32+**: after building, explicitly verify the output is genuinely 64-bit PE32+
  (not accidentally 32-bit) with `file` and `objdump -f`; pass `-m64` explicitly if there is ever
  ambiguity.
- **Resource/version info**: if the producer eventually needs a Windows version resource or icon,
  `x86_64-w64-mingw32-windres` (part of `binutils-mingw-w64-x86-64`) compiles `.rc` → `.res`/`.o`.
  Not needed for the trivial phase-5 test DLL.

## Recommendation

**Primary: Option A — WSL-side `gcc-mingw-w64-x86-64` (GNU mingw-w64 cross toolchain).**

Justification: first-class Ubuntu apt package (no manual tarball download), small footprint
(~a few hundred MB against 953GB free), directly emits PE32+ x86-64 DLLs with a plain C export
table from the ARM64 WSL host with zero emulation, and fully satisfies the GenTL producer's
"flat C export, no COM/.NET" requirement.

**Fallback: Option B — clang + lld reusing the same mingw-w64 sysroot.**

Justification: if gcc-mingw's codegen/diagnostics ever become a limitation (e.g. wanting
sanitizers or LTO with lld), clang can cross-compile against the identical installed
`mingw-w64-x86-64-dev` headers/CRT with no separate sysroot download — it's additive on top of
Option A, not a replacement for it.

**Deferred: Option C — Windows-side ARM64-hosted MSVC Build Tools.** Only pursued if a future
phase discovers an actual MSVC-ABI requirement.

## Exact example build for a trivial test DLL (recommended approach)

Source:
```c
// foo.c
__declspec(dllexport) void __cdecl Foo(void) {}
```

Compile/link (once Option A is installed):
```
x86_64-w64-mingw32-gcc -shared -O2 -static -static-libgcc \
    -o foo.dll foo.c \
    -Wl,--out-implib,libfoo.dll.a
```

Verify:
```
file foo.dll
objdump -f foo.dll
objdump -p foo.dll | grep -A3 "Export Table"
```

Fallback (clang, Option B) equivalent:
```
clang --target=x86_64-w64-mingw32 -shared -O2 \
    -static -static-libgcc \
    -fuse-ld=lld \
    -o foo.dll foo.c
```

## Status

Nothing has been installed. The `sudo apt-get install` commands above are drafted and awaiting
separate explicit user approval before execution.
