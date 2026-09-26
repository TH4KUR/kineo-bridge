# Crash Baseline — Windows Application Event Log, Event ID 1000 (Application Error)

Captured via read-only `Get-WinEvent` query, before any XML/CTI change for
the `AcquisitionFrameRate` fix. Source: Windows Application log,
`KineoDeviceService.exe` faults only, most recent ~22 events (spans
2026-09-25 15:32 through 2026-09-26 15:53).

## Two distinct fault fingerprints found

### Fingerprint A — dominant (21 of 22 events)

```
Faulting application name: KineoDeviceService.exe, version: 1.2.0.0, time stamp: 0x69b9084d
Faulting module name:      ucrtbase.dll, version: 10.0.26100.9549, time stamp: 0x3a3504ca
Exception code:            0xc0000409  (STATUS_STACK_BUFFER_OVERRUN / __fastfail)
Fault offset:              0x0000000000161b7c
Faulting module path:      C:\WINDOWS\System32\ucrtbase.dll
```

**Identical fault offset (`0x161b7c`) in every single one of the 21
occurrences**, across many different process instances (different PIDs
each time, spanning two calendar days). This is a Microsoft system DLL,
not anything Kineo/IDS ships. `0xC0000409` at a fixed, deterministic
offset in `ucrtbase.dll` is the signature of the UCRT's `__fastfail`
path — most commonly reached via an **unhandled C++ exception
propagating to `std::terminate()`/`abort()`**, or a GS stack-cookie
failure. Given the perfect determinism (same offset every time, not
varying), an uncaught exception is far more likely than random memory
corruption.

**Correction to my own earlier report**: I had previously mischaracterized
Electron's logged exit code (`3221226505`) as `0xC0000005` (access
violation). It is actually `0xC0000409` — I made a hex-conversion error.
Every crash Electron observed via the child-process exit code was this
Fingerprint A (CRT fast-fail), not a raw access violation.

### Fingerprint B — rare (1 of 22 events)

```
TimeCreated:    2026-09-26 15:21:08 (local)
Faulting module name: KineoDeviceService.exe, version: 1.2.0.0, time stamp: 0x69b9084d
Exception code:       0xc0000005  (STATUS_ACCESS_VIOLATION)
Fault offset:         0x0000000000007ad5
Faulting module path: C:\IMVapps\Kineo Software\resources\chiron-cpp-module\x64\Release\KineoDeviceService.exe
```

A genuine memory access violation, directly inside `KineoDeviceService.exe`'s
own code (not a system DLL), at RVA `0x7ad5`. Occurred exactly once across
all captured events. Does not correlate with any explicitly-tracked test
session boundary I have precise markers for.

## Interpretation going in to the AcquisitionFrameRate fix

Per the pre-agreed CASE framework: Fingerprint A's perfect determinism and
correlation with the repeated failed-camera-init retry loop (every crash
timestamp falls within a session that was also logging repeated
`AcquisitionFrameRate`-not-found failures) makes it a strong candidate for
"downstream of the repeated failed camera-init path" (CASE 1) — to be
confirmed by whether it disappears once the node is added. Fingerprint B
is rare enough that its relationship to the same root cause is unclear;
tracked separately.
