# Increment 1 Specification — Review Record

Date: 2026-07-20. Reviewed: draft 1 of `increment-1-specification.md`. Result: revision 2 of the specification (same file), which this document justifies finding by finding.

## Method

Two passes:

1. **Static review** of the draft against the actual JSBSim v1.3.x source tree (tags enumerated from the upstream repository; `c172x.xml`, `FGTrim`, `FGFDMExec` and the CMake build read directly).
2. **Empirical validation**: a ~200-line throwaway driver was built against JSBSim v1.3.1 exactly as the draft prescribes (FetchContent, static link, `libJSBSim` target, 120 Hz, `tFull` trim) and ran all four scenarios. Every numeric claim in the revised specification traces to this run. Environment: Ubuntu 24.04, GCC 13.3.0, CMake 3.28.3, Release, 4-core x86-64 container.

The draft's own open items — "verify the tag exists", "document if FetchContent conflicts" — were resolved as part of this review rather than left to the implementer.

## Findings

Severity: **blocking** = a correct implementation of the draft would fail its own acceptance criteria; **major** = would send the implementer down a wrong path or leave critical behaviour unspecified; **minor** = quality/precision issue.

### F1 (blocking) — Test 4's initial condition was aerodynamically impossible
Draft: "Aircraft trimmed for level flight at idle power". A C172 cannot maintain level flight at idle; measured, level flight at 70 kt requires **throttle 0.639**. JSBSim's trim treats throttle as a free variable and would never produce the drafted state. **Resolution**: Test 4 now trims to level flight at 70 kt (trim chooses throttle) and applies full throttle at t=5 s.

### F2 (blocking) — Test 4's airspeed criterion contradicted speed stability
Draft required IAS at t=60 s to exceed the initial value by ≥10 m/s. A statically stable aircraft with controls fixed converts excess power into climb while returning toward its trim speed; measured gain at t=60 s was **+4.5 m/s** — the criterion fails on correct physics. **Resolution**: replaced by an energy-height criterion (see F3) plus an airspeed envelope check.

### F3 (blocking) — Test 4's altitude criterion was phugoid-phase-sensitive
Even redesigned, altitude gain at exactly t=60 s measured **+136 m** against the draft's ≥150 m threshold — and the value depends on where in the (large, measured 70–112 kt) phugoid the clock stops, so it is not robust across JSBSim point releases. **Resolution**: primary criterion is now specific energy height h_e = h + V²/2g₀, which the phugoid does not affect: measured gain ≈ **+157 m**, threshold ≥100 m, plus a weak monotone altitude check (final > initial).

### F4 (blocking) — Test 2's stall-AoA criterion can never be met
Draft required α > 16°, "the documented stall AoA". The 16° figure is real (`<alphalimits>` max 0.28 rad = 16.05° in `c172x.xml`) but it is a **table-lookup clamp, not a dynamic bound**, and under sustained full aft stick at cruise power the dynamic α peaks at **14.29°**: the aircraft mushes, drops a wing and departs rather than breaking cleanly at 16°. The criterion fails on a correct implementation. **Resolution**: α > 12° and IAS < 60 kt (measured minimum 54.1 kt), with the test's purpose reworded to high-AoA departure robustness; the tumbling departure is documented as expected behaviour.

### F5 (blocking) — Test 3's left-bank criterion false-fails on Euler wrap
Draft: "Bank angle does not exceed 60 degrees left at any point during the run". With full aileron held for 10 s the aircraft rolls continuously through inverted; the Euler roll angle wraps between +180° and −180° (measured min **−179.8°**). The criterion as drafted fails every correct run. **Resolution**: the left-bank floor (−10°) now applies only from input onset until the first +60° crossing (measured crossing at t=6.01 s; measured floor in that window −0.15°), which still catches a sign inversion — the actual intent.

### F6 (blocking) — Engine state was never specified
JSBSim loads models with engines **off**. As drafted, Tests 1–3 would trim a powered glider (or fail to trim) and Test 4's throttle input would do nothing. **Resolution**: `FGPropulsion::InitRunning(-1)` is step 7 of a now-normative initialisation sequence, with the common initial state saying explicitly that the engine runs.

### F7 (major) — The c172x pollutes the working directory unless outputs are disabled
`c172x.xml` defines its own output blocks: a CSV (`JSBout172B.csv` — observed appearing in the CWD during validation) and two socket outputs (ports 1138/1140). **Resolution**: `FGFDMExec::DisableOutput()` after `LoadModel()` is a required step.

**Amendment (found during implementation)**: `DisableOutput()` only suppresses the periodic data writes — `FGOutputFile::InitModel()` opens the file and writes its header synchronously as part of `LoadModel()`, before `DisableOutput()` can run. A header-only `JSBout172B.csv` therefore still appeared in the working directory even with the call in place. Since the filename is hardcoded in `c172x.xml` (not permitted to modify, per "Aircraft configuration"), the implementation instead has the test binary enter the already-git-ignored `results/` directory (creating it if absent) before constructing any `FlightSession`, so the stray file lands there instead of the repository root. This is an implementation detail with no effect on the CSV/JSON paths described elsewhere in the specification, which are already expressed relative to `results/`.

### F8 (major) — Where the trim solution actually lives
JSBSim's trim writes its pitch solution to `fcs/pitch-trim-cmd-norm` — `fcs/elevator-cmd-norm` stays 0.0 after trim — while the roll/yaw/power solutions go to the ordinary command properties (measured post-trim: aileron −0.075, throttle 0.792 at 100 kt). The draft's "control surfaces remain at their trimmed positions" and its elevator log column were both underspecified against this asymmetry. **Resolution**: convention documented ("held at trim" = write no `fcs/` property), and a `pitch_trim_norm` column added to the CSV so trimmed pitch state is visible in logs.

### F9 (major) — Runtime data requirements understated
The draft mentioned only "the aircraft data path". The c172x additionally requires `engine/` (`eng_io320` + propeller) and `systems/` (`GNCUtilities`), and pulls in the `c172ap` autopilot definition (inactive, all `ap/*` commands 0). **Resolution**: all three directories specified, autopilot noted.

### F10 (major) — Version pin resolved
`v1.3.0` exists, but so does **`v1.3.1`** (commit `3b25f25e49b42d0489c04ac805674fc1450ca579`), the most recent v1.3.x as of this review. The draft's "verify at implementation time" instruction is replaced by a hard pin to v1.3.1 with a documented substitution rule.

### F11 (major) — JSBSim CMake defaults are wrong for this consumer
`BUILD_PYTHON_MODULE` and `BUILD_DOCS` default **ON**; left alone they drag Python/Cython/Doxygen into configure. The revised spec sets five options OFF before `FetchContent_MakeAvailable`, names the link target (`libJSBSim`, static by default, propagates its include dirs), and warns not to apply `-Werror` to JSBSim (v1.3.1 emits a `-Wreturn-type` warning in `FGTable.cpp`). FetchContent consumption itself was validated clean — the draft's submodule fallback remains in place but is expected to stay unused.

### F12 (major) — Evaluation basis was ambiguous and self-contradictory
The draft evaluated criteria "against its CSV log" (10 Hz) in the script section while requiring evaluation "in C++ within the test binary" two paragraphs later; "at some point" criteria sampled at 10 Hz can also miss 120 Hz excursions. **Resolution**: all criteria are evaluated in-process at 120 Hz on every step; CSVs are artefacts; the script only orchestrates; a full exit-code map (binary 0/1/2, script 0–5) distinguishes "criterion failed" from "test errored" (e.g. trim non-convergence, which the draft never handled — F14).

### F13 (minor) — Initial conditions underspecified
No latitude/longitude (now 0°N 0°E over sea-level terrain), flight path angle, flap/gear/fuel state, or wind/turbulence statement (now: none). The NaN criterion appeared in only three of four tests (now universal). Yaw convention documented (JSBSim reports 360.0 for north at t=0).

### F14 (minor) — Trim failure handling unspecified
`FGTrim::DoTrim()` can fail to converge; the draft never checked it. Now: mandatory check, distinct execution-error exit path.

### F15 (minor) — "Indicated airspeed" defined
JSBSim has no instrument model; IAS is defined as calibrated airspeed from `velocities/vc-kts`. A `tas_mps` column was added (required by the energy criterion). All columns now have a normative property mapping (Appendix A of the spec) — every name exercised against v1.3.1 with zero lookup errors.

### F16 (minor) — CSV precision
"At least four significant figures" gives ~111 m latitude resolution. Now fixed-point with ≥6 decimal places.

### F17 (minor) — Repository framing and project name
The draft said "a fresh git repository"; the implementation target is this existing repository (`liotier/BartElbitAirCombat`, currently LICENSE-only), and the spec now says so. The draft's working name "Ailbit Air Combat" did not match the repository name; resolved by normalising the working name to "BartElbitAirCombat" throughout.

### Verified as-is (no change)
- Control sign conventions: elevator −1.0 = full aft (−28°, trailing edge up); aileron +1.0 = roll right. Draft was correct on both.
- Test 1 criteria: comfortably robust — measured drifts (0.13 m altitude, 0.02 kt, <0.01° pitch, 0.15° bank over 60 s) are 2–3 orders of magnitude inside the tolerances.
- Test 2 pitch criterion (>30° in [5,10] s): measured 58.2°, first crossing t=5.58 s.
- Test 3 timing window ([5,8] s to reach +60°): measured crossing t=6.01 s.
- 120 Hz timestep, 10 Hz logging, GPL-3.0/LGPL-2.1 licensing stance, CI shape, README shape, deferred-scope list.
- CI 10-minute budget: measured 8 s configure + 56 s compile (4 cores, 123 TUs) + <1 s test run; comfortable even at 2 cores.
- Determinism: two consecutive runs produced byte-identical CSVs.

## Measured data summary

| Scenario | Key measurements (v1.3.1, Release, x86-64) |
|---|---|
| Trim @100 kt/5000 ft | throttle 0.792, aileron −0.075, θ 0.80° |
| Trim @70 kt/5000 ft | throttle 0.639, aileron −0.144, θ 3.43° |
| T1 (60 s hands-off) | Δalt 0.13 m; ΔIAS 0.02 kt; Δθ <0.01°; max|φ| 0.15° |
| T2 (full aft elevator) | θmax[5,10] 58.2° (crossing 5.58 s); αmax 14.29°; IASmin 54.1 kt; departs, −300 m by t=30; no NaN |
| T3 (full right aileron) | +60° at t=6.01 s; pre-crossing φmin −0.15°; wraps ±180° after t≈8 s; no NaN |
| T4 (full throttle at t=5) | Δh_e ≈ +157 m; Δalt +136 m @60 s; IAS 69.4–112.4 kt; climbing right spiral; no NaN |

## Implementation model recommendation

**Recommendation: Sonnet (claude-sonnet-5), run as a Claude Code session that can build and execute** (network access for FetchContent, ability to run `scripts/run_tests.sh` iteratively). Rationale:

- Every external unknown that would have demanded judgment is now pinned in the spec: exact tag + commit, CMake options, link target, all property names (validated), the canonical initialisation order, and pass thresholds with measured margins of 20 %–3 orders of magnitude.
- The task is a single-executable C++17 transcription with a self-checking definition of done (`run_tests.sh` exit 0, CI green). The model can verify its own work end-to-end, which compensates for most capability gaps.
- Appendix B gives sanity rails, so the classic failure mode — plausible-looking wrong numbers — is detectable without aeronautical judgment: if trim throttle isn't ≈0.79 at 100 kt, the engine isn't running.

Escalate to Opus (or another frontier-tier model) only if: the implementation session cannot execute builds (then the verification loop is gone and margin of judgment matters again); the JSBSim tag must be substituted and measured values shift; or trim fails to converge on the target platform, which requires debugging beyond the spec's script. Draft-1 as it stood was *not* safely implementable by any model — five of its criteria failed on correct physics; the fixes above, not model choice, were the risk reduction that mattered.

## Residual risks

1. **Upstream drift**: a future forced move off v1.3.1 could shift measured values; criteria margins (α: 19 %, energy: 36 %, roll timing: 2× window) were chosen to absorb point-release drift, but a re-validation run is cheap and recommended after any version change.
2. **Runner variance**: GitHub-hosted runners are slower and occasionally flaky on network fetch; FetchContent hits GitHub once per clean build. If CI flakes, add a fetch retry or cache — permitted by the spec.
