# Phase 7 — Route Comparison

Synthesis of Phases 1-6 evidence against the 8 candidate routes. No percentages
given unless a phase directly measured something; otherwise qualitative
complexity/risk only, per instructions.

## A. Custom Windows x64 GenTL producer -> WSL/Aravis bridge (the route under active investigation)

- **Implementation complexity:** Medium-high. Requires implementing the ~55
  common GenTL exports identified in `gentl-exports.md` (full GC/Event/TL/IF/Dev
  families, most of DS), a GenICam XML description, and a TCP-based IPC bridge
  to a WSL process that owns the camera via Aravis (`wsl-bridge-design.md`).
- **Reverse engineering already done:** Substantial — export surface,
  loading/validation mechanism, and camera node-name evidence are all in hand
  (Phases 2-4). Remaining RE is mostly *live verification* (does Kineo actually
  enumerate a third-party CTI end-to-end; exact GenApi node-map strictness),
  not further static analysis.
- **Likelihood of preserving Kineo's analysis pipeline:** High. This route never
  touches `KineoDeviceService.exe`, `ids_peak.dll`, or any existing `.cti` — it
  only adds a new producer alongside the existing ones. The proprietary
  OpenVINO/tracking pipeline is completely untouched.
- **Performance risk:** Low-medium. `wsl-bridge-design.md` shows the new
  localhost TCP hop (23-230 MB/s across 10-100 FPS) is nowhere near a WSL2
  loopback bottleneck; the actual ceiling is the pre-existing USB/IP link
  (already a known constraint today, not made worse by this route).
- **Maintenance risk:** Medium. A custom CTI is a real, spec-shaped artifact
  that must track GenTL semantics, but it's architecturally decoupled from
  Kineo/ids_peak internals (flat C ABI only) — no fragile coupling to
  IDS's C++ class layouts or Kineo's exact build.
- **Earliest feasibility test:** A zero-code test already available today —
  see "Step 0" in `PLAN.md` (drop the already-installed, already-working TIS
  USB3Vision `.cti` onto the GenTL path and see if Kineo surfaces anything
  from it) — before writing a single line of custom producer code.
- **Likely blocker:** Whether Kineo's own application layer silently restricts
  itself to only its two shipped IDS CTIs after `ids_peak.dll`'s generic
  enumeration (Phase 3's single biggest UNKNOWN — no evidence for or against
  in strings). Secondary risk: `kineo-camera-contract.md`'s finding that Kineo
  links IDS peak's C++ GenApi node-map wrapper, which may demand a more
  complete/well-formed GenICam XML than a bare-minimum synthetic one.

## B. Custom/modified GigE Vision bridge -> existing Windows GigE producer (TIS)

- **Implementation complexity:** Medium, but for a *different, harder* problem
  than Route A: the user's own prior testing (in the original background,
  not re-verified this pass) already got GVCP discovery ACKs working against
  a fake Aravis GigE camera, but saw **zero register reads/writes afterward**
  — meaning the actual GenICam register-access protocol layer was never
  exercised, which is where most of a GigE Vision device's real complexity
  lives (bootstrap register block, GVCP control-channel privilege handshake,
  XML-manifest-over-register-reads).
- **Reverse engineering needed:** Significant, and mostly *not done* by this
  investigation (Phases 1-6 focused on the GenTL/CTI layer, not GigE Vision
  wire protocol internals) — would need fresh RE into why TIS's producer
  stalls after discovery.
- **Likelihood of preserving analysis pipeline:** High (same as A — still real
  KineoDeviceService.exe).
- **Performance risk:** Medium — GVSP is packet-based and jitter-sensitive;
  no data collected on this project either way.
- **Maintenance risk:** Medium-high — GigE Vision protocol conformance is
  substantial to emulate correctly end-to-end.
- **Earliest feasibility test:** Build a minimal GVCP register-read/write
  responder for the existing fake camera and see if TIS's producer proceeds
  past discovery — this alone would resolve the open question from the user's
  prior experiment.
- **Likely blocker:** The camera is natively USB3 Vision, not GigE — this
  route requires emulating an entirely different vision standard as a
  detour, whereas Route A stays within the GenTL/CTI abstraction the camera
  already speaks (via Aravis). The already-observed "stuck after discovery"
  behavior is an unresolved deeper unknown, with no obvious next value
  compared to Route A's cleaner path.

## C. WinUSB on ARM64 + custom user-mode USB3 Vision implementation

- **Implementation complexity:** Very high — a full USB3 Vision protocol
  stack in user mode (control/event/streaming channels, bulk transfer
  framing) reimplementing what IDS's proprietary `\\.\ids_u3vcore` kernel
  driver currently does.
- **Reverse engineering needed:** Extensive — none of this investigation's
  phases touched the kernel driver or raw USB3 Vision wire protocol.
- **Likelihood of preserving pipeline:** High in theory, very fragile in
  practice given the scope.
- **Performance risk:** Potentially good (no USB/IP hop at all) but entirely
  unproven — the whole protocol stack would need to be built and tuned from
  scratch.
- **Maintenance risk:** Very high.
- **Earliest feasibility test:** Expensive — likely weeks just to get a basic
  control-channel handshake working, before any image data flows.
- **Likely blocker:** Scope. The real camera already streams successfully via
  the existing WSL/Aravis/USB-IP path — Route A gets the same end result
  (real frames into Kineo) for a small fraction of the engineering effort
  this route requires.

## D. Proxy ids_peak.dll

- **Implementation complexity:** High — `ids_peak.dll` exposes a rich C++ API
  (`peak::core::*` classes, confirmed via Phase 3/4 symbol evidence) that
  Kineo links directly against, not just the flat GenTL C ABI. A proxy would
  need to replicate exact MSVC class layouts/name-mangling, a much bigger and
  more fragile surface than Route A's plain C export table.
- **Reverse engineering needed:** Very extensive — full C++ ABI-compatible
  reimplementation or a binary-compatible hook layer.
- **Likelihood of preserving pipeline:** Risky — could break unrelated
  ids_peak-dependent behavior (e.g. IPL processing) if any C++ object
  layout/vtable assumption is wrong.
- **Performance risk:** Medium.
- **Maintenance risk:** Very high — tied to the exact `ids_peak.dll` version's
  C++ ABI (v1.9.0.0 confirmed this pass); breaks on every IDS SDK update.
- **Earliest feasibility test:** Expensive — would need to reverse the C++
  class layouts before any test is even possible.
- **Likely blocker:** C++ ABI fragility. Route A avoids this entirely by
  operating one layer down, at the flat-C GenTL boundary `ids_peak.dll`
  itself already generically supports (Phase 3 evidence).

## E. Proxy/replace the IDS CTI (`ids_u3vgentlk.cti`) directly

- **Implementation complexity:** Similar to Route A for the CTI code itself,
  but this route **replaces** a file the user's own safety rules explicitly
  protect ("do NOT ... modify any installed .cti"). It would also sidestep
  the one real open question in Route A (whether Kineo accepts a *third-party*
  CTI) only by risking breaking the *existing, working* IDS producer, and by
  requiring re-application after every Kineo/IDS update overwrites it back.
- **Likelihood of preserving pipeline:** High if it works, but higher blast
  radius than Route A (modifies an in-place vendor file vs. adding a new one).
- **Maintenance risk:** High — silently reverted by any Kineo/IDS update.
- **Likely blocker:** Conflicts with the user's explicit standing constraint;
  and Phase 3 evidence (no vendor allowlist found in `ids_peak.dll`) suggests
  this risk isn't even necessary — a new, additively-placed CTI (Route A)
  should be sufficient without ever touching the existing file.

## F. Replace KineoDeviceService via WebSocket emulation

- Already explicitly deprioritized by the user ("Replacing the whole service
  is NOT currently preferred because it contains proprietary analysis
  logic") and is the exact shape of the legacy, do-not-extend experiment
  already sitting in `~/kineo-bridge/legacy/websocket-service-probe/`.
- **Likelihood of preserving analysis pipeline: none by definition** — the
  entire point of this route is replacing the component that contains the
  proprietary OpenVINO/tracking logic, which is precisely what every other
  route is designed to avoid having to reimplement.
- Kept only as historical reference per the project's own instructions; not
  re-evaluated further here.

## G. Native ARM64 driver

- **Implementation complexity:** Extremely high, and largely outside the
  user's control — either IDS ships an ARM64-compatible kernel driver (an
  external dependency with no evidence either way from this investigation),
  or the user writes a full WDF USB3 Vision kernel-mode driver from scratch.
- **Reverse engineering needed:** Extensive kernel-mode work; plus Windows
  driver-signing requirements for ARM64 kernel drivers are a substantial
  practical barrier independent of the engineering effort.
- **Likelihood of preserving pipeline:** Perfect if achieved — but this is
  gated on factors (IDS's roadmap, driver signing) the user cannot directly
  control on a useful timeline.
- **Likely blocker:** Driver signing + IDS dependency, not really an
  engineering problem this investigation can resolve.

## H. Use another x86-64 PC as acquisition host

- **Implementation complexity:** Lowest of all options — no bridge code at
  all, since it's just running the existing, fully-working stack on
  compatible hardware.
- **Reverse engineering needed:** None.
- **Likelihood of preserving pipeline:** Perfect (100%) — literally the
  unmodified real stack.
- **Performance risk:** None — native.
- **Maintenance risk:** Very low technically; but this is a logistics
  tradeoff (a second machine/desk footprint), not a fix for the actual goal
  of getting the camera working on this single ARM64 Surface Pro 11.
- **Earliest feasibility test:** Trivial — try it with any spare x86-64
  machine.
- **Likely blocker:** Not a technical blocker at all — it's a fallback safety
  net worth keeping in mind (e.g. while Route A is being built) rather than a
  long-term answer to the stated goal.

## Conclusion

**Route A (custom Windows x64 GenTL producer -> WSL/Aravis bridge) remains the
recommended route.** Nothing in Phases 1-6 overturned the user's original
preferred architecture — if anything, Phase 3's finding that `ids_peak.dll`
has no vendor allowlist and Kineo is built on IDS's generic
`peak::core::ProducerLibrary` wrapper is direct, positive evidence *for* this
route's core assumption. The main remaining risks (Kineo-level filtering
after enumeration; GenICam XML completeness given the C++ node-map linkage)
are both cheap to test early and are exactly what the milestone sequence in
`PLAN.md` front-loads. Route H (spare x86-64 PC) is worth keeping as a
zero-effort fallback safety net during development, not as the target
architecture.
