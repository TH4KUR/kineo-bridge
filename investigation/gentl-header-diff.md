# Official EMVA GenTL 1.5 header vs. our hand-written `probe/gentl.h`

Source of truth: official `GenTL_v1_5.h`, EMVA GenTL Subcommittee, 2015
(`https://www.emva.org/wp-content/uploads/GenTL_v1_5.h`), downloaded and
diffed against our hand-written header. M0 baseline files were **not**
modified for this comparison.

## ABI-relevant differences found

1. **Calling convention macro**: official `GC_CALLTYPE` = `__stdcall` on
   Windows; ours = `__cdecl`. On x86-64 Windows, both `__stdcall` and
   `__cdecl` are normalized to the single unified x64 calling convention by
   both MSVC and mingw — this keyword only has teeth on 32-bit x86 targets
   (affecting name decoration and callee/caller stack cleanup). Since this
   project targets x64 exclusively, **this difference has no observable
   effect** on any test run so far, and is not expected to matter going
   forward. Worth fixing for correctness/future-proofing regardless (e.g. if
   an x86 build is ever needed), but ruled out as a factor in the Gate 2
   investigation.
2. **`DEVICE_ACCESS_STATUS` enum incomplete**: official spec (GenTL v1.5)
   defines 7 values (`UNKNOWN`=0, `READWRITE`=1, `READONLY`=2, `NOACCESS`=3,
   `BUSY`=4, `OPEN_READWRITE`=5, `OPEN_READONLY`=6); ours only had the first
   4 (0-3), missing the 3 GenTL-1.5 additions. The 4 values we do have match
   exactly (correct values, not wrong ones) — this is a gap, not a bug.
   Irrelevant to Gate 2 (never queried in any test so far) but should be
   added before M3+ device-open work in case Kineo/ids_peak ever reports or
   expects one of the missing states.

## Confirmed exact matches (no differences)

- `GC_ERROR` values: `SUCCESS=0`, `ERROR=-1001`, `NOT_INITIALIZED=-1002`,
  `NOT_IMPLEMENTED=-1003`, `INVALID_HANDLE=-1006`, `INVALID_ID=-1007`,
  `INVALID_PARAMETER=-1009`, `NOT_AVAILABLE=-1014`, `BUFFER_TOO_SMALL=-1016`,
  `INVALID_INDEX=-1017` — all byte-for-byte identical to what we wrote.
- `INFO_DATATYPE` values 0-14 — exact match, including `UINT32=6` (the type
  our `GCGetInfo(TL_INFO_GENTL_VER_MAJOR/MINOR)` responses use, confirmed
  correct against the spec, not just against the real DLL's behavior).
- `TL_INFO_CMD` values 0-10 (`ID`, `VENDOR`, `MODEL`, `VERSION`, `TLTYPE`,
  `NAME`, `PATHNAME`, `DISPLAYNAME`, `CHAR_ENCODING`, `GENTL_VER_MAJOR`,
  `GENTL_VER_MINOR`) — exact match.
- `DEVICE_INFO_CMD` values 0-9 — exact match.
- `GCGetInfo`/`GCInitLib`/`GCCloseLib` function signatures — exact match,
  both the direct `GC_API` form and the function-pointer-typedef `GC_API_P`
  form.

## Conclusion

Our hand-written header is **ABI-equivalent for every function/enum
exercised in M0/M1 testing so far**. This rules out a header-transcription
bug as the explanation for Gate 2 — the rejection is a genuine behavioral
gate in `ids_peak.dll`, not an artifact of an incorrect struct layout or
enum value on our side. The two gaps found (calling-convention keyword,
incomplete `DEVICE_ACCESS_STATUS`) are both fix-before-M3 housekeeping
items, not Gate 2 candidates.

**Recommendation for future implementation**: switch to importing the
official `GenTL_v1_5.h` directly (license permits redistribution/use per its
header text) rather than maintaining a hand-written subset, once real M3+
work begins and the full function surface is needed.
