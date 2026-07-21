# Increment 2 Specification: Godot Real-Time Integration

## Status

Draft, informed by empirical validation of the Godot/GDExtension integration performed ahead of drafting (see Appendix B for measured values and the specific findings that shaped the decisions below). This is increment 2 of the derisking sequence recorded in `docs/roadmap.md`. It builds directly on increment 1: the `FlightSession` abstraction, the c172x aircraft, and the four validated flight-dynamics scenarios are reused, not reimplemented.

## Goal

Validate that JSBSim can be driven inside Godot's real-time frame loop — at a fixed 120 Hz physics tick decoupled from Godot's variable render rate — with live keyboard input reaching the simulation and a minimal placeholder aircraft visibly responding, running interactively on modest hardware. Single aircraft, no networking, no art assets.

## Non-goals

Explicitly out of scope for this increment; must not be implemented:

- Any networking, client/server architecture, or multi-process design (increment 3 onwards)
- Any WWII aircraft configuration (still c172x; later increment)
- Multiple simultaneous aircraft instances (later increment)
- AI/bot-controlled aircraft (later increment)
- Weapons, hit detection, damage modelling (increment 8)
- Collision detection or crash physics of any kind
- Real art assets, textures, or modelled aircraft geometry (placeholder primitives only)
- Sound or music
- Any GUI, menu, or HUD required for a player (telemetry is exposed for the automated tests; a visible HUD is at the implementer's discretion and not part of acceptance)
- Save/load or persistence of any kind
- Joystick/HOTAS input (optional stretch; keyboard is the required baseline)
- Exporting or packaging the project for distribution (the deliverable runs via the Godot editor; export is a later, separate concern)

## Repository structure

Builds on increment 1's tree. New and changed paths:

```
.
├── CMakeLists.txt                      (changed: adds godot-cpp, flightcore, flight_gdext)
├── README.md                           (changed)
├── .gitignore                          (changed: see "Godot project cache")
├── .github/workflows/ci.yml            (changed)
├── src/
│   ├── main.cpp                        (unchanged)
│   ├── test_runner.h / test_runner.cpp (unchanged content; now built into flightcore)
│   ├── scenarios/                      (unchanged, increment 1's own four files)
│   ├── logging/                        (unchanged)
│   └── godot_ext/                      (new)
│       ├── register_types.h
│       ├── register_types.cpp
│       ├── flight_aircraft.h
│       └── flight_aircraft.cpp
├── godot/                              (new: the Godot project)
│   ├── project.godot
│   ├── .godot/                         (committed — see "Godot project cache")
│   ├── bin/
│   │   └── flight_gdext.gdextension
│   ├── scenes/
│   │   ├── main.tscn                   (the playable scene)
│   │   └── headless_test.tscn          (the automated-test scene)
│   └── scripts/
│       ├── flight_input.gd
│       └── headless_test_driver.gd
├── scripts/
│   └── run_tests.sh                    (changed: runs increment 1's suite unchanged, then the new Godot suite)
└── docs/
    ├── increment-1-specification.md
    ├── increment-1-specification-review.md
    ├── roadmap.md
    └── increment-2-specification.md    (this document)
```

Increment 1's own acceptance criteria continue to hold unchanged; `flightcore` is a build-structure change, not a behavioural one.

## Build environment

### Toolchain additions over increment 1

Same as increment 1 (CMake 3.20+, C++17 compiler, Git 2.30+, Bash 5.0+), plus:

- A Godot 4.5.x stable editor binary (Linux x86_64), used both to run the project interactively and to run the automated headless test suite.

### Godot editor binary acquisition

Fetched, not compiled — and fetched automatically by tooling, not installed by hand. The distinction from JSBSim/godot-cpp is deliberate: those two are libraries linked directly into our own binary, so building them ourselves (with matching flags/ABI) is a real requirement. Godot itself is a separate program that loads our compiled `flight_gdext.so` through GDExtension's stable, versioned interface — precisely designed so a precompiled Godot binary from anyone's toolchain can load a third-party extension built with anyone else's. There is no compatibility reason to compile Godot ourselves, and doing so anyway costs measured ~30 minutes and a dozen system libraries (Appendix B) against roughly 30 seconds for the official binary, for no benefit — that 30-minute path is what this specification's own validation work did, purely because the sandboxed environment used for validation could fetch git repositories but not download release binaries directly. That is a property of the validation sandbox, not of a normal developer machine or CI runner, and is not the approach this project takes.

`scripts/run_tests.sh` downloads the official precompiled Godot 4.5.x stable Linux editor build itself (a plain ~80–100 MB HTTPS download — e.g. from `https://github.com/godotengine/godot/releases` or `https://godotengine.org/download/archive/`), caching it under a git-ignored local directory (e.g. `.godot-tools/`) keyed by version, and skipping the download if a matching binary is already cached — the same "fetch once, reuse thereafter" property FetchContent already gives JSBSim and godot-cpp, just via a plain download rather than CMake, since Godot itself is a tool dependency, not a C++ build dependency. A developer or CI runner never installs Godot by hand; a fresh clone plus the test script is still the whole bootstrap, matching increment 1's "two commands" ethos.

This specific step — the binary download — was **not** empirically exercised while drafting this specification, for the reason above (the validation sandbox's restriction, not a real one). Verify the exact release asset URL and filename at implementation time, in the same spirit as increment 1's JSBSim tag-verification caution.

### godot-cpp acquisition

Fetched via CMake FetchContent, pinned to the tag matching the Godot editor version above:

```cmake
set(GODOTCPP_TARGET "editor" CACHE STRING "" FORCE)

FetchContent_Declare(
    godot-cpp
    GIT_REPOSITORY https://github.com/godotengine/godot-cpp.git
    GIT_TAG        godot-4.5-stable
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(godot-cpp)
```

This consumption path is validated: it configures and builds cleanly alongside the existing JSBSim FetchContent block, with no option or target conflicts. `GODOTCPP_TARGET=editor` is deliberate: it is the only godot-cpp/Godot target combination this specification's tests exercise, because the editor binary is the only one used, for both interactive play and headless CI (see "Godot editor binary acquisition" above — export templates are out of scope).

### Position-independent code

Both godot-cpp and JSBSim are linked into the shared `flight_gdext` library below, so their static libraries must be compiled as position-independent code:

```cmake
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
```

Set this before either `FetchContent_MakeAvailable` call. It has no adverse effect on the existing `increment1_tests` executable.

### Exceptions

godot-cpp marks `-fno-exceptions` as a `PUBLIC` compile option on its own target (`cmake/common_compiler_flags.cmake`), which propagates via CMake's usage-requirement mechanism to anything linking `godot::cpp` — this is not call-order-sensitive and cannot be overridden by a later `target_compile_options` call on the consuming target. Increment 1's C++ layer is already exception-free (see the `CsvLogger` fix predating this increment); **no new code in this increment may throw**, and no per-source-file exception carve-outs are needed or permitted. This matters beyond mere compilation: `FlightAircraft::_physics_process` (compiled without exceptions, as part of `flight_gdext`) calls directly into `flightcore` code, so an exception thrown anywhere in that call chain would need to unwind through frames with no unwind tables — undefined behaviour, not a guaranteed clean crash, regardless of which specific CMake target a given source file happens to belong to.

## flightcore: sharing increment 1's code

`test_runner.cpp` and `logging/csv_logger.cpp` move into a small static library, linked by both `increment1_tests` and the new `flight_gdext` target:

```cmake
add_library(flightcore STATIC
    src/test_runner.cpp
    src/logging/csv_logger.cpp
)
target_include_directories(flightcore PUBLIC src)
target_link_libraries(flightcore PUBLIC libJSBSim)
```

`PUBLIC` on both the include directory and the JSBSim link: consumers of `flightcore` (the GDExtension's `flight_aircraft.cpp`) need `test_runner.h`'s declarations and transitively need JSBSim's symbols, without restating either. `flightcore` itself has no dependency on Godot or godot-cpp and must stay that way — it is exactly increment 1's reusable core, unmodified in content.

`increment1_tests` links `flightcore` instead of compiling `test_runner.cpp`/`csv_logger.cpp` directly; its own sources (`main.cpp`, `scenarios/*.cpp`) and behaviour are otherwise unchanged.

## GDExtension registration and the Godot project cache

A hand-authored `.gdextension` file is **not discovered by a headless run** until Godot's editor/import machinery has scanned the project at least once — there is no error; the extension is silently never loaded, and any node type it defines fails with `Cannot get class '<Name>'`. This is not a build problem; it reproduces on a correctly-built extension referenced by a correctly-written `.gdextension` file.

The documented fix is `godot --headless --import --path <project>`, which does perform the registration (writing `.godot/extension_list.cfg`) — but **reliably crashes afterward** (SIGSEGV) attempting to load editor UI layout in headless mode, a Godot 4.5 limitation unrelated to this project. The crash happens strictly after the registration write completes, so the registration itself is not lost, but a script cannot treat this invocation's exit status as a pass/fail signal.

**Resolution**: `godot/.godot/` is committed to the repository, not gitignored. Inspecting a known-good copy of this directory shows it contains only `res://`-relative resource paths (e.g. `extension_list.cfg` contains the single line `res://bin/flight_gdext.gdextension`), so it is portable across clones and machines. A fresh clone therefore has a working extension registration from the moment of checkout, and no import step ever needs to run in CI. If `.godot/` is ever regenerated (e.g. a future increment adds a second `.gdextension`), regenerate it once with `--headless --import`, confirm `extension_list.cfg` contains the expected line regardless of that command's own exit status, and commit the refreshed directory.

## Simulation loop architecture

### Physics tick rate

Godot's own fixed-timestep physics loop is used directly — no custom accumulator or manual threading is implemented. Set in `godot/project.godot`:

```
[physics]
common/physics_ticks_per_second=120
```

This matches JSBSim's required 1/120 s timestep from increment 1 exactly. `FlightAircraft::_physics_process(double delta)` calls `FlightSession::step()` once per invocation; no other driving mechanism is used.

### Real-time pacing and its limit (informative, validated empirically)

Godot paces `_physics_process` to wall-clock time independent of the render (`_process`) rate — measured at ~121.8 Hz against the 120 Hz target with no artificial load (Appendix B). After a slow frame, Godot fires a burst of catch-up physics ticks rather than skipping — but that catch-up is capped by Godot's own internal smoothing algorithm (`main_timer_sync.cpp`), not something this project configures: a measured 100 ms hitch, needing ~12 ticks to fully catch up at 120 Hz, consistently received only 8, every time, across repeated trials. This means sustained slow frames cause simulated time to permanently drift behind wall-clock time rather than fully recovering, by design. It is not a practical risk for this increment specifically: real `FlightSession::step()` cost was measured at a mean of 57 μs and a worst observed 243 μs (Appendix B), under 1% and 3% respectively of the 8,333 μs per-tick budget at 120 Hz — nowhere near enough to itself cause the hitches this cap protects against. This is documented for awareness, not as something to engineer around in this increment; it becomes relevant again if a future increment adds a much heavier per-tick workload.

### Threading

`_process` and `_physics_process` are called synchronously on the same (main) thread — confirmed by thread-ID logging across two independent test runs, one with a no-op physics callback and one with real `FlightSession::step()` calls. `FGFDMExec::Run()` may be called directly from `_physics_process` with no cross-thread synchronization.

## FlightAircraft node

A `Node3D` subclass wrapping one `inc1::FlightSession`. Full member list in Appendix A. Summary:

- `initialize()`, `set_initial_condition(...)`, `trim()` — thin wrappers over the identically-named `FlightSession` methods, returning `bool` and printing failures via `UtilityFunctions::printerr` rather than propagating a C++ exception (see "Exceptions" above).
- `start_logging(path)` — opens a `CsvLogger` (increment 1's, unmodified) writing the same column schema as increment 1's CSVs; if never called, no file is written.
- Four read/write properties (`elevator_cmd`, `aileron_cmd`, `rudder_cmd`, `throttle_cmd`) mapped directly to the corresponding `fcs/*-cmd-norm` properties, using increment 1's established sign conventions unchanged.
- Five read-only telemetry properties (`altitude_m`, `airspeed_mps`, `true_airspeed_mps`, `pitch_deg`, `bank_deg`, `alpha_deg`) — exactly the fields the four reused scenarios' criteria need (see "Test scenarios").
- `_physics_process(double delta)`: calls `FlightSession::step()`, updates the node's own `Node3D` transform from the resulting position/attitude, writes a CSV row if logging is enabled. Nothing else. No scenario-specific or test-specific logic lives in this class.

`FlightAircraft` never calls `initialize()`/`set_initial_condition()`/`trim()` itself; the scene's own script sequences those three calls at `_ready()`, exactly mirroring increment 1's own three-call initialization idiom, just invoked from GDScript instead of C++.

## Scene design (`godot/scenes/main.tscn`)

```
Main (Node3D)
├── FlightAircraft (FlightAircraft)
│   ├── PlaceholderMesh (MeshInstance3D — an elongated BoxMesh or CapsuleMesh
│   │                     suggesting a fuselage, oriented nose along -Z to
│   │                     match Godot's forward-axis convention, with a
│   │                     smaller box/cone at the nose end so orientation is
│   │                     visually unambiguous)
│   └── ChaseCamera (Camera3D, current=true, offset behind and above in
│                     FlightAircraft's local space — a child of FlightAircraft
│                     so it follows automatically with no follow-cam script)
├── Ground (MeshInstance3D — a large flat PlaneMesh at 0 m MSL, purely a
│           visual motion reference; no collision shape, no physics body)
├── DirectionalLight3D (default settings; only so the placeholder mesh is
│                        visible)
└── FlightInput (Node, script: flight_input.gd)
```

`FlightInput` is placed before `FlightAircraft` in the tree so its `_physics_process` (which sets `FlightAircraft`'s control properties) runs before `FlightAircraft`'s own `_physics_process` (which consumes them) within the same tick, per Godot's tree-order processing guarantee. The practical latency difference either way is one tick (8.3 ms, imperceptible); the ordering is specified for determinism, not because it is critical.

## Input handling (`godot/scripts/flight_input.gd`)

GDScript, not C++ — this is exactly the kind of simple, frequently-retuned logic the project's language split (`docs/roadmap.md`) puts in the game layer. Polls `Input.is_action_pressed(...)` in `_physics_process`, at the same 120 Hz tick as the simulation:

| Control | Key(s) | Behaviour |
|---|---|---|
| Pitch | W (nose down) / S (nose up) | Self-centering: full deflection (±1.0) while held, relaxes toward 0.0 when released, matching a spring-loaded stick |
| Roll | A (roll left) / D (roll right) | Self-centering, same as pitch |
| Yaw (optional) | Q (left) / E (right) | Self-centering, same as pitch; not required to be bound if the implementer chooses to omit rudder control |
| Throttle | Page Up (increase) / Page Down (decrease) | Lever behaviour, not self-centering: held key ramps `throttle_cmd` by roughly 0.33/second (full range in ~3 s), released key holds the current value |

Sign conventions match increment 1's established mapping exactly (elevator −1.0 = full aft/nose-up, aileron +1.0 = roll right). `FlightAircraft`'s control properties are clamped to their valid ranges by the property setters themselves, not by this script.

At `_ready()`, `flight_input.gd` (or a sibling script) performs the same three-call sequence as every scenario: `initialize()`, `set_initial_condition(5000.0, 100.0, 0.0, 0.0, 0.0, 0.0)` — the same trimmed 5,000 ft / 100 kt starting point as increment 1's Test 1 — then `trim()`.

## Test scenarios

Five automated tests, run headlessly. The first four are increment 1's Test 1–4, unchanged in initial conditions, control-input schedule, and pass-criteria thresholds — only the driving mechanism changes, from this project's own `runLoop` to Godot's `_physics_process`. Identical criteria surviving a different execution path is itself the validation that going through Godot did not alter the physics; if any of these four fail here while increment 1's own suite still passes, the defect is in the Godot integration, not in JSBSim or the aircraft model.

Each runs as a **separate Godot process invocation** (fresh engine state, no risk of leakage between scenarios), driven by one parameterized scene and script, selecting behaviour via an environment variable:

```
TEST_SCENARIO=trim_stability|pitch_response|roll_response|power_response|realtime_pacing \
  godot --headless --path godot --quit-after <N> -- --script-args (or equivalent)
```

`godot/scripts/headless_test_driver.gd`, attached to `godot/scenes/headless_test.tscn`, reads `OS.get_environment("TEST_SCENARIO")` at `_ready()`, applies the matching initial condition and scripted control-input schedule below, tracks the running min/max/crossing values each `_physics_process` tick, and on completion prints a pass/fail line and calls `get_tree().quit(0)` or `get_tree().quit(1)`.

### Tests 1–4 (reused from increment 1, unchanged)

| Test | Initial condition | Control input | Duration | Criteria (all unchanged from increment 1) |
|---|---|---|---|---|
| `trim_stability` | 5,000 ft, 100 kt, trimmed level | none | 60 s | altitude drift ≤ 61 m; IAS drift ≤ 5 kt; pitch drift ≤ 5°; max\|bank\| ≤ 2° |
| `pitch_response` | same as above | t≥5s: elevator_cmd = −1.0 | 30 s | max pitch in [5,10]s ≥ 30°; max α ≥ 12°; min IAS ≤ 60 kt |
| `roll_response` | same as above | t≥5s: aileron_cmd = 1.0 | 15 s | max bank in [5,8]s ≥ 60°; min bank from t=5s to first +60° crossing ≥ −10° |
| `power_response` | 5,000 ft, 70 kt, trimmed level | t≥5s: throttle_cmd = 1.0 | 60 s | energy-height gain ≥ 100 m; altitude gain ≥ 0 m; IAS stays in [50, 140] kt |

All four also carry increment 1's universal criterion: no NaN/Inf in any read telemetry value at any tick (checked in GDScript with `is_nan()`/`is_inf()`, no new exposed property needed for this). Full derivations and rationale for every threshold are in `docs/increment-1-specification.md`; they are not re-litigated here.

Each of these four also calls `start_logging()` at the start, writing to `results/godot_<scenario_name>.csv` — same column schema as increment 1, for direct comparison against the original standalone CSVs if ever needed.

### Test 5: `realtime_pacing` (new)

**Purpose**: validate that the 120 Hz physics configuration in `project.godot` actually takes effect and that ticks are paced to wall-clock time, not run unboundedly fast.

**Procedure**: default initial condition (5,000 ft, 100 kt, trim); no control input. Record the wall-clock timestamp (`Time.get_ticks_usec()`) at every `_physics_process` call for 600 ticks (5 simulated seconds — long enough for a stable rate measurement; this session's own validation got a confident reading from far fewer samples, see Appendix B).

**Pass criteria**: measured rate (600 / elapsed wall-clock seconds) falls within [114, 126] Hz — a ±5% band around the nominal 120 Hz, with several times the margin this specification's own validation run measured (+1.5% deviation, Appendix B). No NaN/Inf.

### What is deliberately not automated

The artificial-slow-frame catch-up-cap behaviour documented above (informative) is Godot's own internal implementation detail, not something this project's code could break or is responsible for; it is not a gating test. Whether the aircraft is actually pleasant and correct to fly is a human-judgment matter — see "Acceptance criteria."

## Test runner and CI

`scripts/run_tests.sh` gains a second phase after increment 1's existing one (which is unchanged and must still pass): download or locate the Godot editor binary, then invoke it five times (once per `TEST_SCENARIO` value above), collecting each exit code. Overall script exit status is 0 only if increment 1's suite *and* all five Godot-driven tests pass.

### CI timing

Unlike increment 1's suite (which deliberately runs "as fast as the host CPU permits"), tests 1–4 here run through Godot's real, wall-clock-paced tick loop: their combined 165 simulated seconds take roughly 165 real seconds to execute, not the sub-second total increment 1 achieved standalone. Combined with the godot-cpp compile (measured ~5 minutes) and JSBSim (measured ~1 minute), a cold-cache CI run is estimated at 10–12 minutes — comfortable against a **20-minute** budget for this increment (raised from increment 1's 10, for this reason). **Build caching (e.g. `actions/cache` over the CMake build directory / FetchContent `_deps`) is required, not merely permitted as in increment 1**: without it, every push repays the full ~6-minute dependency compile, which is wasteful for iterative development at this increment's scale.

Do not use `Engine.time_scale` to accelerate the automated tests. It was considered and rejected: this specification's validation work did not test how time-scaling interacts with the catch-up-cap mechanism above, and getting that interaction wrong could silently corrupt the scripted control-input timing (a "t≥5s" input applying at the wrong simulated time). Running at normal pacing is simple, well-understood, and comfortably within budget; revisit only if CI time becomes a real constraint in a later increment.

## Documentation

New C++ (`src/godot_ext/*`) and GDScript (`godot/scripts/*.gd`) files carry the same GPL-3.0 header convention as increment 1 (`#` comment syntax for GDScript). Generated/cached files under `godot/.godot/` are not source and carry no header. README additions: how to obtain the Godot editor binary, how to open and run the project interactively, and that `scripts/run_tests.sh` now also drives the Godot-based suite.

## Licence

Unchanged from increment 1: GPL-3.0-or-later for this project. godot-cpp is MIT-licensed; Godot itself is MIT-licensed; both compatible with GPL-3.0 as consumed here (fetched/downloaded at build time, not redistributed in this repository).

## Acceptance criteria

Increment 2 is complete when all of the following hold simultaneously:

1. `scripts/run_tests.sh` on a fresh clone of a clean Debian 12 or Ubuntu 24.04 system exits 0 — increment 1's suite passes unchanged, and all five Godot-driven tests pass.
2. `godot/project.godot` opens in the Godot 4.5.x editor without error, and a human has manually pressed Play, controlled the placeholder aircraft with the keyboard, and confirms it visibly responds correctly (correct sign conventions, no obvious stutter) — this is a human-judgment criterion, documented as performed, not itself CI-gated.
3. The GitHub Actions workflow runs to completion successfully on push, within the 20-minute budget, with build caching in place.
4. `results/godot_*.csv` files are produced with the same column schema as increment 1's CSVs.
5. The README is sufficient for a competent C++/Godot-literate developer to build, test, and run the project interactively without additional explanation.

## Out of scope, explicitly deferred

Unchanged from increment 1's deferred list, plus this increment's own non-goals restated: Godot export/packaging, WWII aircraft, multiple aircraft, AI/bots, networking (increment 3 onwards), weapons/damage (increment 8), audio, joystick/HOTAS, collision/crash physics.

## Open questions for the implementer

At the implementer's discretion; document the choice in code or README:

- Whether to bind rudder control to keys at all (optional per "Input handling")
- Exact placeholder mesh dimensions/proportions, beyond "elongated, nose-marked"
- Whether to add a debug on-screen HUD using the exposed telemetry properties (not required for acceptance)
- Whether the five headless test invocations run sequentially in one script loop or are parallelized in CI (parallelizing is safe — each is an independent process with its own engine state)

These choices have low downstream impact and need not be specified in advance.

---

## Appendix A: FlightAircraft API reference (normative)

| Member | Kind | Type | Notes |
|---|---|---|---|
| `initialize()` | method | `bool` | Wraps `FlightSession::initialize`; prints failure via `UtilityFunctions::printerr`, returns `false` |
| `set_initial_condition(alt_ft, vc_kts, psi_true_deg, lat_deg, lon_deg, gamma_deg)` | method | `void` | Wraps `FlightSession::setInitialCondition`, same parameter order |
| `trim()` | method | `bool` | Wraps `FlightSession::trim`; same failure convention as `initialize()` |
| `start_logging(path)` | method | `bool` | Opens a `CsvLogger` at `path` (increment 1's schema, unmodified); wraps `CsvLogger::open`, same failure convention |
| `elevator_cmd` | property, read/write | `float`, [−1, 1] | → `fcs/elevator-cmd-norm` |
| `aileron_cmd` | property, read/write | `float`, [−1, 1] | → `fcs/aileron-cmd-norm` |
| `rudder_cmd` | property, read/write | `float`, [−1, 1] | → `fcs/rudder-cmd-norm` |
| `throttle_cmd` | property, read/write | `float`, [0, 1] | → `fcs/throttle-cmd-norm` |
| `altitude_m` | property, read-only | `float` | ← `FlightSample::alt_m` |
| `airspeed_mps` | property, read-only | `float` | ← `FlightSample::ias_mps` |
| `true_airspeed_mps` | property, read-only | `float` | ← `FlightSample::tas_mps` (needed for the power-response energy-height criterion) |
| `pitch_deg` | property, read-only | `float` | ← `FlightSample::pitch_deg` |
| `bank_deg` | property, read-only | `float` | ← `FlightSample::roll_deg` |
| `alpha_deg` | property, read-only | `float` | ← `FlightSample::alpha_deg` |

Godot's `real_t`/property Variant type is 32-bit single-precision float by default (unlike this project's internal double-precision C++ computation). This is a known, adequate precision reduction given every pass-criterion tolerance above carries orders-of-magnitude margin over single-precision rounding error (increment 1's Appendix B measured drifts of 0.1–0.15 against tolerances of 2–61); it is not expected to affect any test in this specification.

## Appendix B: measured values from this specification's validation work (informative)

Measured on a 4-core x86-64 container, Ubuntu 24.04, GCC 13.3.0, Godot 4.5.stable (custom-built from source for this validation only — see "Godot editor binary acquisition"), godot-cpp at `godot-4.5-stable`, JSBSim v1.3.1.

- Godot engine source build (this validation's own workaround only, not part of the real project's build): ~30 minutes wall-clock on 4 cores, `target=editor`.
- godot-cpp binding compile: ~5 minutes wall-clock on 4 cores (~2,000 generated files).
- JSBSim compile: ~1 minute (matches increment 1's own measurement).
- Physics tick rate at nominal 120 Hz, no artificial load: measured 121.8 Hz (+1.5%) over a 394 ms / 48-tick sample.
- Catch-up behaviour after a simulated 100 ms slow frame (needing ~12 ticks at 120 Hz to fully recover): consistently exactly 8 catch-up ticks fired, across 15 repeated trials — both with a no-op physics callback and with real `FlightSession::step()` calls. Traced to Godot's own adaptive step-smoothing in `main_timer_sync.cpp`, not a simple accumulator; not configured or controlled by this project.
- Real `FlightSession::step()` cost, measured inside `_physics_process`: mean 57.2 μs, median 41 μs, p95 154 μs, max observed 243 μs, against an 8,333 μs per-tick budget at 120 Hz (mean 0.69% of budget).
- Threading: single thread ID observed across all `_process`/`_physics_process` calls in every trial, with and without real JSBSim computation.
- GDExtension registration: confirmed that a fresh (never-imported) project's `.gdextension` file is not loaded by a headless run; confirmed `--headless --import` performs the registration (writes `.godot/extension_list.cfg`, containing only `res://`-relative paths) before crashing (SIGSEGV) in a later, unrelated startup phase (`loading_editor_layout`).
- Exceptions: confirmed `-fno-exceptions` is `PUBLIC` on godot-cpp's own target (`common_compiler_flags.cmake`), propagating via CMake usage requirements to any target linking `godot::cpp`, independent of `target_compile_options` call order on the consuming target.
