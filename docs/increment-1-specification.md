# Increment 1 Specification: JSBSim Standalone Integration

## Status

Revision 2 — reviewed and empirically validated, 2026-07-20. Supersedes the initial draft.

This is the first of a sequence of derisking increments for an open-source multiplayer WWII air combat game project (working name: BartElbitAirCombat; repository: `github.com/liotier/BartElbitAirCombat`). This increment validates JSBSim integration in isolation. No networking, no rendering, no game logic.

Every numeric pass criterion in this revision was validated against an actual JSBSim v1.3.1 build (GCC 13.3, `-DCMAKE_BUILD_TYPE=Release`, x86-64 Linux) running the exact scenarios specified below. Measured reference values are given in Appendix B; the review that produced this revision is in `docs/increment-1-specification-review.md`.

## Goal

Validate that JSBSim can be built, integrated as a library, and driven through a single aircraft configuration to produce stable, correct flight dynamics output, in isolation from any rendering or networking concerns.

## Non-goals

The following are explicitly out of scope for this increment and must not be implemented:

- Any graphics, rendering, or visualisation
- Any networking, client/server architecture, or multi-process design
- Any real-time execution requirement (simulation runs as fast as the host CPU allows)
- Any user input handling beyond scripted control inputs defined in test scenarios
- Any aircraft selection mechanism (one aircraft is hardcoded)
- Any WWII-era aircraft configuration (deferred to a later increment)
- Any sound, music, or audio output
- Any GUI, menu, or interactive interface
- Any persistence, save/load, or session management

## Repository structure

The implementation lives in this repository (`liotier/BartElbitAirCombat`), which at the start of this increment contains only `LICENSE` (GPL-3.0) and `docs/`. On completion the layout must be:

```
.
├── CMakeLists.txt
├── README.md
├── LICENSE                    (already present)
├── .gitignore                 (must ignore at least build/ and results/)
├── .github/
│   └── workflows/
│       └── ci.yml
├── src/
│   ├── main.cpp
│   ├── test_runner.cpp
│   ├── test_runner.h
│   ├── scenarios/
│   │   ├── trim_stability.cpp
│   │   ├── pitch_response.cpp
│   │   ├── roll_response.cpp
│   │   └── power_response.cpp
│   └── logging/
│       ├── csv_logger.cpp
│       └── csv_logger.h
├── scripts/
│   └── run_tests.sh
└── docs/
    ├── increment-1-specification.md         (this document)
    └── increment-1-specification-review.md  (review record)
```

No other top-level directories or files. No vendored third-party code in the repository — JSBSim is fetched at configure time. `scripts/run_tests.sh` must carry the executable bit.

The exact split of code between `test_runner.*` and the scenario files is at the implementer's discretion (see Open questions); the file names above are the required skeleton.

## Build environment

### Target platform

Linux (Debian/Ubuntu primary target). The implementation must build and run on Debian 12 and Ubuntu 24.04 LTS without modification. Other platforms are not in scope for this increment.

### Required toolchain

- CMake 3.20 or later
- A C++17-capable compiler (GCC 11+ or Clang 14+)
- Git 2.30 or later (for FetchContent operations)
- Bash 5.0 or later (for the test runner script)

(Reference: the validation build used CMake 3.28.3 and GCC 13.3.0 on Ubuntu 24.04.)

### Dependencies

JSBSim only, fetched via CMake FetchContent as described below. No other external runtime dependencies. The build must not require any system-installed JSBSim package.

## JSBSim integration

### Version

JSBSim **v1.3.1** (tag `v1.3.1`, commit `3b25f25e49b42d0489c04ac805674fc1450ca579`), verified to exist and to build cleanly on 2026-07-20. This is the most recent v1.3.x release at the time of writing. Do not silently upgrade: if the tag has become unavailable, use the nearest v1.3.x tag, document the substitution in the README, and re-check the measured reference values in Appendix B.

### Acquisition

JSBSim is fetched at CMake configure time using FetchContent. JSBSim's optional components must be switched off **before** `FetchContent_MakeAvailable`, otherwise the configure stage will look for Python/Cython and Doxygen and the build grows substantially:

```cmake
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_PYTHON_MODULE OFF CACHE BOOL "" FORCE)
set(BUILD_JULIA_PACKAGE OFF CACHE BOOL "" FORCE)
set(BUILD_MATLAB_SFUNCTION OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    jsbsim
    GIT_REPOSITORY https://github.com/JSBSim-Team/jsbsim.git
    GIT_TAG        v1.3.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(jsbsim)
```

This exact consumption path has been validated: it configures in seconds and produces no option or target conflicts with a plain consumer project.

### Linking

Link against the CMake target `libJSBSim` (its output file is named `libJSBSim.a`; static is JSBSim's default). The target propagates its include directory, so `target_link_libraries(<exe> PRIVATE libJSBSim)` is sufficient — no manual include paths. Headers are included as `#include "FGFDMExec.h"`, `#include "initialization/FGTrim.h"`, `#include "models/FGPropulsion.h"`.

The resulting binary must run from a clean checkout with no `LD_LIBRARY_PATH` manipulation or runtime library installation.

Do not apply `-Werror` to the JSBSim target: v1.3.1 emits at least one `-Wreturn-type` warning (`src/math/FGTable.cpp`). Warnings-as-errors, if used at all, must be scoped to this project's own sources.

### Fallback

If JSBSim's CMake integration produces conflicts when consumed via FetchContent (option pollution, install rule conflicts, target name collisions), the implementer may switch to a git submodule with a separate build invocation. This decision must be documented in the README with the specific issue encountered. (Note: validation found no such conflict with v1.3.1; this fallback is expected to remain unused.)

## Aircraft configuration

This increment uses the **c172x** configuration distributed with JSBSim. This is the canonical Cessna 172 model, exercised by JSBSim's own test suite, with well-understood behaviour.

The model requires three data directories from the JSBSim source tree at runtime, not just the aircraft folder:

- `aircraft/c172x/` — the aircraft definition (also pulls in the `c172ap` autopilot definition, which loads but stays inactive: all `ap/*` command properties default to 0)
- `engine/` — the `eng_io320` engine and its propeller definition
- `systems/` — support systems referenced by the model (e.g. `GNCUtilities`)

The build system must make these paths discoverable at runtime, either by copying the three directories into the build tree or by passing the absolute path of the fetched JSBSim source tree (`${jsbsim_SOURCE_DIR}`, e.g. via a compile definition) to the executable. Either approach is acceptable; document the choice.

No modifications to the c172x configuration are permitted in this increment.

Note: `c172x.xml` defines its own output blocks (a CSV file `JSBout172B.csv` and two sockets on ports 1138/1140). These must be suppressed by calling `FGFDMExec::DisableOutput()` after `LoadModel(...)`, otherwise every run pollutes the working directory. This is part of the required initialisation sequence below.

## Definitions, units and conventions

JSBSim's internal state and properties are in imperial units (feet, feet per second, knots). All CSV output and all pass criteria in this specification are metric. Use exactly these conversion constants:

- 1 ft = 0.3048 m (exact)
- 1 knot = 1852.0/3600.0 m/s ≈ 0.514444 m/s (exact as a fraction)
- g₀ = 9.80665 m/s² (standard gravity, for the energy-height criterion)

Frames and angles:

- Velocities are logged in the local NED (north-east-down) frame.
- `roll_deg` (φ) is in (−180, 180], positive right wing down. `pitch_deg` (θ) is in [−90, 90], positive nose up. `yaw_deg` (ψ) is in [0, 360], measured from true north; note that JSBSim reports exactly 360.0 (not 0.0) for north at initialisation.
- Euler angles wrap: an aircraft rolling continuously through inverted passes from φ ≈ +180 to φ ≈ −180. Pass criteria are written to be immune to this; do not "fix" it.
- JSBSim has no instrument-error model, so indicated airspeed is taken as calibrated airspeed, read from `velocities/vc-kts`.

Control conventions (verified against `c172x.xml`):

- `fcs/elevator-cmd-norm` ∈ [−1, 1]; **−1.0 is full aft stick** (trailing edge up, −28°, nose-up); +1.0 is full forward (+23°).
- `fcs/aileron-cmd-norm` ∈ [−1, 1]; **+1.0 commands roll right**.
- `fcs/rudder-cmd-norm` ∈ [−1, 1].
- `fcs/throttle-cmd-norm` ∈ [0, 1].
- The effective pitch input is `clip(elevator-cmd + pitch-trim-cmd + ap-elevator-cmd, −1, 1)`. JSBSim's trim routine writes its pitch solution to `fcs/pitch-trim-cmd-norm` (NOT to `fcs/elevator-cmd-norm`, which stays 0 after trim), while its roll and yaw solutions go to `fcs/aileron-cmd-norm` and `fcs/rudder-cmd-norm`, and its power solution to `fcs/throttle-cmd-norm`. "Held at trim" therefore means: do not write any `fcs/` property after trim completes.

## Simulation parameters

### Timestep

JSBSim runs at a fixed timestep of 1/120 s (120 Hz), set with `FGFDMExec::Setdt()` before initialisation and never varied during a run.

### Execution mode

The simulation runs as fast as the host CPU permits, not in real time. A test specifying 60 simulated seconds advances JSBSim 7200 steps in a tight loop, with no sleeping or rate limiting. (Reference: all four tests together complete in well under one second of wall time.)

### Common initial state

Unless a test says otherwise:

- Position: latitude 0.0°, longitude 0.0° (over ocean; JSBSim's default terrain elevation is sea level)
- Altitude: 1524 m (5000 ft) above mean sea level
- Heading: due north; flight path angle 0 (level)
- Flaps up, landing gear as modelled (fixed), default fuel load from the model file
- Wind and turbulence: none (JSBSim defaults; do not enable any atmospheric disturbance)
- Engine: running (see initialisation sequence — JSBSim loads models with engines OFF)

### Initialisation sequence

Each test uses a **fresh `FGFDMExec` instance** (no state may leak between tests) and must perform exactly this sequence. Deviating from this order is the most likely source of silent failure (an aircraft trimmed with a dead engine, for example, still "flies" — as a glider):

1. Construct `FGFDMExec`; call `Setdt(1.0/120.0)`.
2. Set data paths: `SetRootDir(<jsbsim source dir>)`, `SetAircraftPath(SGPath("aircraft"))`, `SetEnginePath(SGPath("engine"))`, `SetSystemsPath(SGPath("systems"))`.
3. `LoadModel("c172x")` — abort the test run with an execution error if it returns false.
4. `DisableOutput()` — suppress the model's own CSV/socket output blocks.
5. Set initial conditions through the property tree: `ic/h-sl-ft`, `ic/vc-kts`, `ic/psi-true-deg`, `ic/lat-gc-deg`, `ic/long-gc-deg`, `ic/gamma-deg` (values per test).
6. `RunIC()` — applies the initial conditions.
7. `GetPropulsion()->InitRunning(-1)` — starts all engines.
8. Trim: construct `JSBSim::FGTrim trim(&fdm, JSBSim::tFull)` and call `trim.DoTrim()`. **Check the return value**: if trim fails to converge, print a clear message and exit with the execution-error status (see Exit codes). Do not proceed with an untrimmed aircraft.
9. Log the t=0 state (before the first `Run()`), then loop: apply the test's scripted inputs for the current time, call `Run()`, evaluate criteria, log.

## Test scenarios

Four test scenarios are required. Each produces a CSV log file and a pass/fail determination.

**Evaluation basis**: all pass criteria are evaluated inside the test binary at the full 120 Hz rate, on every integration step, using the same double-precision values that are logged. The 10 Hz CSV files are a by-product for humans and CI artefacts, not the evaluation input. "At some point" means at any integration step; "throughout" means at every integration step.

**Universal criterion**: in every test, every logged quantity must remain finite (no NaN, no ±Inf) at every integration step. A non-finite value fails the test at the step where it appears.

Each criterion below is followed by the value measured during validation (Appendix B has the full table); thresholds are set with comfortable margin so that minor upstream changes do not cause flakiness.

### Test 1: Trim stability

**Purpose**: Verify that the simulation is numerically stable in a trimmed condition.

**Initial conditions**: common state; calibrated airspeed 51.4 m/s (100 kt); trimmed to level flight before t=0.

**Control inputs**: none. All `fcs/` properties remain untouched after trim.

**Duration**: 60 simulated seconds.

**Pass criteria**:
- Altitude at t=60 s within ±61 m (200 ft) of the t=0 altitude. *(measured drift: 0.13 m)*
- Indicated airspeed at t=60 s within ±2.6 m/s (5 kt) of the t=0 value. *(measured drift: 0.01 m/s)*
- Pitch attitude at t=60 s within ±5° of the t=0 (trimmed) pitch attitude. *(measured drift: 0.002°)*
- Bank angle within ±2° throughout. *(measured max |φ|: 0.15°)*

### Test 2: Pitch response

**Purpose**: Verify pitch control authority, sign convention, and robustness of the model through high-AoA departure.

**Initial conditions**: same as Test 1.

**Control inputs**:
- t=0 to t=5 s: held at trim
- t≥5 s: full aft elevator (`fcs/elevator-cmd-norm` = −1.0), all other properties untouched

**Duration**: 30 simulated seconds.

**Pass criteria**:
- Pitch attitude exceeds +30° at some point in t ∈ [5, 10] s. *(measured: 58.2°, first crossing at t=5.58 s)*
- Angle of attack exceeds 12° at some point during the run. *(measured peak: 14.3°)*
- Indicated airspeed falls below 31 m/s (60 kt) at some point during the run. *(measured minimum: 27.8 m/s / 54.1 kt)*

**Behavioural note (informative)**: the c172x does not exhibit a clean 16° stall break — its aerodynamic tables are clamped at α = 0.28 rad (16.05°) and the dynamic α peaks around 14°. Under sustained full aft stick at cruise power the aircraft zooms, decelerates, drops a wing and enters a tumbling rolling descent with large attitude excursions and roughly 300 m of altitude loss over the run. This is expected, is precisely why this scenario is a good numerical robustness test, and is why the criteria above are written against α and airspeed rather than a "stall at 16°" event. Do not add criteria constraining the post-departure trajectory.

### Test 3: Roll response

**Purpose**: Verify roll control authority and lateral axis sign convention.

**Initial conditions**: same as Test 1.

**Control inputs**:
- t=0 to t=5 s: held at trim
- t≥5 s: full right aileron (`fcs/aileron-cmd-norm` = +1.0), all other properties untouched (elevator stays at its trim state)

**Duration**: 15 simulated seconds.

**Pass criteria**:
- Bank angle exceeds +60° (right) at some point in t ∈ [5, 8] s. *(measured: first crossing at t=6.01 s, i.e. ~1 s after input)*
- From t=5 s until the first sample where bank exceeds +60°, bank angle never falls below −10°. This is the sign-convention check: a control inversion would roll left instead. *(measured minimum in that window: −0.15°)*

**Behavioural note (informative)**: after the +60° crossing the aircraft keeps rolling, passes inverted, and the Euler roll angle wraps between +180° and −180° repeatedly. No bank-angle criterion may be applied after the first +60° crossing — a naive "never banks left" check over the whole run false-fails on the wrap, not on physics. Only the universal finiteness criterion applies for the remainder of the run.

### Test 4: Power response

**Purpose**: Verify propulsion integration: adding power must add total energy.

**Initial conditions**: common state; calibrated airspeed 36.0 m/s (70 kt); trimmed to level flight before t=0. The trim routine sets the throttle required for level flight at 70 kt — expect roughly 0.55–0.75 *(measured: 0.639)*. (A trim to level flight *at idle* is aerodynamically impossible and must not be attempted.)

**Control inputs**:
- t=0 to t=5 s: held at trim
- t≥5 s: full throttle (`fcs/throttle-cmd-norm` = 1.0), all other properties untouched

**Duration**: 60 simulated seconds.

**Pass criteria**, using energy height h_e(t) = alt_m(t) + tas_mps(t)²/(2·g₀):
- h_e(60) − h_e(0) ≥ 100 m. *(measured: ≈ +157 m)*
- Altitude at t=60 s is above the t=0 altitude. *(measured: +136 m)*
- Indicated airspeed stays within [25.7, 72.0] m/s ([50, 140] kt) throughout. *(measured range: 69.4–112.4 kt)*

**Behavioural note (informative)**: with all controls frozen, the power increase does not produce a tidy wings-level climb: propeller torque and slipstream roll the aircraft into a climbing right spiral (bank oscillating roughly 35–70°) with a pronounced phugoid (airspeed swinging ~70–112 kt). Altitude at any single instant is therefore phugoid-phase-sensitive — which is why the primary criterion is energy height, which the phugoid does not affect. A stick-fixed aircraft converts excess power into climb while returning toward its trim speed; do not add a "final airspeed must increase by N" criterion (the draft's +10 m/s version was physically wrong and measured at only +4.5 m/s).

## Logging

### Output format

Each test produces a CSV file named `<test_name>.csv` (`trim_stability.csv`, `pitch_response.csv`, `roll_response.csv`, `power_response.csv`) in a `results/` directory relative to the working directory of the test binary (the test runner script invokes the binary from the repository root). UTF-8, comma-separated, one header row, Unix line endings.

### Logging frequency

State is logged at 10 Hz of simulated time — every 12th integration step. The first row is at t=0.0 (after trim, before the first `Run()`), the last at the test's end time. A 60 s test therefore has exactly 601 data rows; 30 s → 301; 15 s → 151.

### Required columns

The header and column order must be exactly:

```
time_s,lat_deg,lon_deg,alt_m,vel_north_mps,vel_east_mps,vel_down_mps,roll_deg,pitch_deg,yaw_deg,alpha_deg,beta_deg,ias_mps,tas_mps,elevator_norm,aileron_norm,rudder_norm,pitch_trim_norm,throttle_norm
```

Field definitions (the JSBSim property behind each column is normative — see Appendix A):

- `time_s`: simulated time in seconds since test start
- `lat_deg`, `lon_deg`: geodetic latitude and longitude, decimal degrees
- `alt_m`: altitude above mean sea level, metres
- `vel_north_mps`, `vel_east_mps`, `vel_down_mps`: velocity in the local NED frame, m/s
- `roll_deg`, `pitch_deg`, `yaw_deg`: Euler angles, degrees, per the conventions section
- `alpha_deg`, `beta_deg`: angle of attack and sideslip, degrees
- `ias_mps`: indicated (= calibrated) airspeed, m/s
- `tas_mps`: true airspeed, m/s (needed by the Test 4 energy criterion)
- `elevator_norm`, `aileron_norm`, `rudder_norm`: **commanded** control values as scripted, ∈ [−1, 1]
- `pitch_trim_norm`: pitch trim command ∈ [−1, 1] — this is where the trim solution for the pitch axis lives; without this column the logged elevator command reads 0.0 in trimmed flight and the pitch state would be unexplained
- `throttle_norm`: commanded throttle ∈ [0, 1]

All numeric fields are written in fixed-point decimal (decimal point, no exponent, no thousands separators) with at least six digits after the decimal point. (Six decimal places of latitude ≈ 0.11 m of position resolution; the draft's "four significant figures" would have been ~111 m.)

### Determinism

With a fixed timestep, fixed initial conditions and no atmospheric disturbances, repeated runs of the same binary must produce byte-identical CSV files (verified during validation). Bit-identity across different compilers or platforms is *not* required.

## Test runner

### Division of responsibility

All pass/fail evaluation is implemented in C++ inside the test binary — never in shell. The binary runs all four tests in sequence (continuing through failures), writes the CSV logs and `results/summary.json`, prints a human-readable summary table to stdout, and communicates the overall outcome through its exit status. The shell script only orchestrates: environment check, configure, build, invoke, relay.

### Exit codes

Test binary:
- `0` — all tests passed
- `1` — at least one criterion failed
- `2` — execution error (model load failure, trim non-convergence, I/O failure); a test that errors is reported as errored, not merely failed

`scripts/run_tests.sh`:
- `0` — all tests passed
- `1` — at least one test failed
- `2` — required tool missing (`cmake`, `git`, C++ compiler)
- `3` — CMake configure failed
- `4` — build failed
- `5` — test binary reported an execution error (binary exit 2)

### Script behaviour: `scripts/run_tests.sh`

From a clean repository checkout the script must:

1. Verify required tools are available (`cmake`, `git`, a C++ compiler); exit 2 with a clear message if not.
2. Run `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`; exit 3 on failure.
3. Run `cmake --build build --parallel "$(nproc)"`; exit 4 on failure.
4. Recreate `results/` empty (removing any prior contents).
5. From the repository root, execute the test binary (all four tests, CSVs and `summary.json` into `results/`).
6. Exit 0, 1 or 5 according to the binary's exit status.

The summary table (printed by the binary) lists each test name, pass/fail/error status, and for every failed criterion its name, the measured value and the limit.

### Summary JSON format

`results/summary.json`:

```json
{
  "increment": 1,
  "jsbsim_tag": "v1.3.1",
  "tests": [
    {
      "name": "trim_stability",
      "status": "passed",
      "criteria": [
        {"name": "altitude_drift_abs", "passed": true, "actual": 0.13, "limit": 61.0, "comparison": "<=", "unit": "m"}
      ]
    }
  ],
  "all_passed": true
}
```

- `status` is `"passed"`, `"failed"` or `"error"` (with an additional `"message"` field when `"error"`).
- Every criterion appears with `actual`, `limit`, a `comparison` of `"<="` or `">="` such that `passed == (actual comparison limit)`, and a `unit`. Band criteria (e.g. Test 4's airspeed envelope) are decomposed into a `>=` entry for the floor and a `<=` entry for the ceiling.
- `jsbsim_tag` is the tag actually fetched.
- `all_passed` is true only if every test has status `"passed"`.

## Continuous integration

A GitHub Actions workflow at `.github/workflows/ci.yml` must:

- Trigger on push to any branch and on pull request to the default branch
- Run on `ubuntu-24.04`
- Install build dependencies (`cmake`, `build-essential`, `git` — mostly pre-installed on the runner; the step is for self-documentation)
- Execute `scripts/run_tests.sh`
- Upload the contents of `results/` as a workflow artefact **regardless of outcome** (`if: always()`)
- Report success or failure from the script's exit status

The workflow must complete within 10 minutes on a standard GitHub-hosted runner. This is comfortable: the validation build measured 8 s configure + 56 s compile on 4 cores (≈123 translation units for JSBSim with the optional components off) and under 1 s to run all four tests; a 2-core runner roughly doubles the compile. No cache is required to meet the budget; adding one is permitted but not required.

## Documentation

### README.md

The README must contain, in this order:

1. Project name and one-line description
2. Status note indicating this is increment 1 of a derisking sequence
3. Build instructions: exactly two commands to go from nothing to test results (`git clone …` then `./scripts/run_tests.sh`)
4. Expected output: a short description of the summary table
5. Link to this specification and to the review document in `docs/`
6. Licence statement

### Inline documentation

C++ source files contain header comments stating their purpose. Non-obvious choices (the initialisation order, the energy-height criterion, the windowed roll criterion) are commented with reference to the specification section that motivates them. No documentation generation tooling is required.

## Licence

The project is released under GPL version 3 or later. The `LICENSE` file (already present in the repository) contains the standard GPL-3.0 text. Source files include the standard GPL-3.0 header.

JSBSim is LGPL 2.1-licensed; this is compatible with a GPL-3.0 project as consumed here. JSBSim is fetched at build time and not redistributed in this repository.

## Acceptance criteria

Increment 1 is complete when all of the following hold simultaneously:

1. A fresh clone of the repository on a clean Debian 12 or Ubuntu 24.04 system, followed by `./scripts/run_tests.sh`, completes with exit status 0.
2. The four CSV log files exist in `results/` with the exact specified header, the specified row counts, and finite numeric data throughout.
3. `results/summary.json` exists and reports `"all_passed": true`.
4. The GitHub Actions workflow runs to completion successfully on a push to the repository, within the time budget, and the `results/` artefact is downloadable.
5. The README is sufficient for a competent C++ developer to build and run the tests without additional explanation.

## Out of scope, explicitly deferred

Considered and deferred to later increments; must not be implemented in increment 1:

- Godot integration (increment 2)
- Real-time execution and frame pacing (increment 2)
- WWII aircraft configurations (later increment)
- Multiple aircraft instances in a single simulation (later increment)
- Network protocol implementation (increment 3 onwards)
- Hit detection, weapons, damage modelling (increments 9-10)
- Visual rendering of any kind (increment 2)
- Audio (later, possibly never)
- User input handling (increment 2)

## Open questions for the implementer

At the implementer's discretion; document the choice in code or README:

- The internal structure of the test scenario classes (inheritance hierarchy, function signatures)
- Whether to write CSV/JSON manually or via a header-only helper written in-repo (no new external dependencies)
- Whether JSBSim data directories are copied into the build tree or referenced in the source tree (both acceptable, see Aircraft configuration)
- Compiler warning flags for the project's own sources

These choices have low downstream impact and need not be specified in advance.

---

## Appendix A: JSBSim property map (normative)

Every quantity this specification refers to, with its exact JSBSim property and conversion. All property names below were exercised against JSBSim v1.3.1 during validation; none produced a lookup error.

| Quantity / column | JSBSim property | Convert |
|---|---|---|
| `time_s` | `simulation/sim-time-sec` | — |
| `lat_deg` | `position/lat-geod-deg` | — |
| `lon_deg` | `position/long-gc-deg` | — |
| `alt_m` | `position/h-sl-ft` | × 0.3048 |
| `vel_north_mps` | `velocities/v-north-fps` | × 0.3048 |
| `vel_east_mps` | `velocities/v-east-fps` | × 0.3048 |
| `vel_down_mps` | `velocities/v-down-fps` | × 0.3048 |
| `roll_deg` | `attitude/phi-deg` | — |
| `pitch_deg` | `attitude/theta-deg` | — |
| `yaw_deg` | `attitude/psi-deg` | — |
| `alpha_deg` | `aero/alpha-deg` | — |
| `beta_deg` | `aero/beta-deg` | — |
| `ias_mps` | `velocities/vc-kts` | × 1852/3600 |
| `tas_mps` | `velocities/vt-fps` | × 0.3048 |
| `elevator_norm` | `fcs/elevator-cmd-norm` | — |
| `aileron_norm` | `fcs/aileron-cmd-norm` | — |
| `rudder_norm` | `fcs/rudder-cmd-norm` | — |
| `pitch_trim_norm` | `fcs/pitch-trim-cmd-norm` | — |
| `throttle_norm` | `fcs/throttle-cmd-norm` | — |

Initial-condition properties (set before `RunIC()`): `ic/h-sl-ft`, `ic/vc-kts`, `ic/psi-true-deg`, `ic/lat-gc-deg`, `ic/long-gc-deg`, `ic/gamma-deg`.

C++ entry points used: `FGFDMExec::{Setdt, SetRootDir, SetAircraftPath, SetEnginePath, SetSystemsPath, LoadModel, DisableOutput, SetPropertyValue, GetPropertyValue, RunIC, Run, GetPropulsion}`, `FGPropulsion::InitRunning(-1)`, `FGTrim(&fdm, JSBSim::tFull)::DoTrim()`.

## Appendix B: measured reference values (informative)

Measured with JSBSim v1.3.1, GCC 13.3.0 `-O2` (Release), Ubuntu 24.04 x86-64, 2026-07-20, running exactly the scenarios above. These are sanity rails for the implementer — if a correct implementation produces values wildly different from these, suspect the initialisation sequence (dead engine, missing trim, wrong paths) before suspecting JSBSim. They are not pass criteria and small deviations are normal.

Trim solutions (`tFull`):

| Condition | throttle | aileron cmd | pitch attitude |
|---|---|---|---|
| 100 kt CAS, 5000 ft, level | 0.792 | −0.075 | 0.80° |
| 70 kt CAS, 5000 ft, level | 0.639 | −0.144 | 3.43° |

Scenario outcomes:

- **Test 1**: altitude drift 0.13 m over 60 s; IAS drift 0.02 kt; pitch drift < 0.01°; max |bank| 0.15°.
- **Test 2**: pitch crosses +30° at t=5.58 s, peaks at 58.2° within the window; α peaks at 14.29°; IAS minimum 54.1 kt; the aircraft departs into a tumbling descent after t≈7 s, losing ≈300 m by t=30 s; all values finite throughout.
- **Test 3**: bank crosses +60° at t=6.01 s (≈60°/s roll rate onset); minimum bank before the crossing −0.15°; bank wraps ±180° repeatedly from t≈8 s onward; all values finite.
- **Test 4**: energy-height gain ≈ +157 m at t=60 s; altitude gain +136 m; IAS range 69.4–112.4 kt; climbing right spiral, bank 35–70°; all values finite.

Build and run timings (4-core container): configure incl. shallow fetch 8.2 s; JSBSim + driver compile 56 s; all four scenarios < 1 s. Two consecutive runs of the binary produced byte-identical CSV output.
