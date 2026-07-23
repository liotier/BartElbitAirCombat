# Increment 6 Specification: Multi-Airframe Migration

## Status

Draft. Pre-drafting validation complete (Appendix B); not yet adversarially reviewed or implemented.

## Goal

Prove the pipeline built around one hardcoded aircraft (c172x) generalises to others, by making the aircraft type a runtime configuration choice instead of a compiled-in constant (`LoadModel("c172x")`, `src/test_runner.cpp:43`), and validating a small fleet of off-the-shelf JSBSim configs — **c172x** (unchanged baseline), **Camel** (Sopwith Camel, WWI rotary biplane), and **pa28** (Piper Cherokee) — through the exact same initialisation sequence increment 1 validated for c172x alone, not merely checked for existing and correctly licensed.

## Non-goals

- **Multiple simultaneous aircraft types in one running server/session** — a server picks one type for its whole lifetime; players choosing between types in the same session is increment 8.
- **Any new player-facing engine-management controls** — mixture, propeller pitch, and startup/magneto sequencing stay abstracted behind the existing single throttle lever. Settled directly by this increment's own pre-drafting findings (Appendix B): p51d's custom autothrottle/mixture-control systems are exactly the kind of complexity that caused a real, time-consuming bug (its engine never starts), and exposing that class of system to players cuts against this project's own "pseudo-realistic, not simulator-grade" vision.
- **Fixing dr1, p51d, or L17's individual config defects.** Each has a distinct, real problem (Appendix B); none is fixed here. Revisit once there's a working proof of concept to demo.
- **Real WWII fighter models** (Spitfire, Bf-109, etc.). FGAddon's versions of both use YASim, not JSBSim, as their flight dynamics model — incompatible with `FlightSession`'s `FGFDMExec`-specific design. A real port is a separate, larger undertaking than this increment's scope.
- **Visual differentiation of aircraft types in the Godot client** (distinct meshes/liveries per type) beyond whatever is needed to demonstrate the pipeline. Placeholder geometry is unchanged.
- **Per-aircraft-type balance or gameplay tuning.**

## Architecture

### Aircraft catalog

A small table, the one place aircraft-specific identity and trim IC live:

| name | wire `aircraft_id` | canonical trim altitude | canonical trim VC | notes |
|---|---|---|---|---|
| `c172x` | 0 | 5000 ft | 100 kt | unchanged from increments 1-5 |
| `camel` | 1 | 5000 ft | 65 kt | |
| `pa28` | 2 | 1000 ft | 100 kt | does **not** converge at 2000 ft or 5000 ft - see Appendix B |

`flightcore::FlightSession::initialize()` gains a `name` parameter (currently hardcoded to `"c172x"`); every call site that currently hardcodes `setInitialCondition(5000.0, 100.0, ...)` (`src/server/main.cpp:221,276`, `src/godot_ext/predicted_aircraft.cpp:75`) looks its IC up from this table by name instead. Scripted server mode (`flight_server --scenario ...`, increment 1's regression scenarios) stays c172x-only and untouched by any of this — those scenarios *are* the c172x ground truth, not something to re-run against other airframes.

### Server-authoritative type selection

`flight_server` gains `--aircraft {c172x|camel|pa28}` (default `c172x`), fixed for the lifetime of one server process. Every aircraft that process manages — real clients, `--stress-aircraft` synthetic aircraft — uses the same configured type; this is a whole-session setting, not per-client (per-client choice is increment 8).

A connecting Godot client needs to trim the *same* type the server is actually running, or `PredictedSession`'s client-side prediction silently predicts with the wrong airframe entirely and every reconciliation fights it from the second correction onward. Rather than restructure `PredictedAircraft::_ready()` to defer trimming until a `ServerWelcome` arrives (a real behavioural change to an already-working sequence, more invasive than this needs), the fix is the same shape as `scripts/start_client.sh`'s existing `SERVER_HOST`/`SERVER_PORT` override: `PredictedAircraft` gains an `aircraft_type` exported property (default `"c172x"`), overridable via an `AIRCRAFT` environment variable read in `_ready()` before trimming, exactly like `SERVER_HOST`/`SERVER_PORT` already are (`src/godot_ext/predicted_aircraft.cpp`) — and `scripts/start_client.sh` gains a matching `--aircraft` flag alongside its existing `--server` one. This makes picking a type an explicit, coordinated choice between server operator and player, the same relationship `--port` already has today (a client pointed at the wrong port simply doesn't connect; a client configured for the wrong aircraft type needs to be *audible*, not silently wrong, which is what the wire field below is for).

`ServerWelcome` gains `aircraft_id` (see Appendix A). On receipt, `PredictedAircraft` compares it against whichever type it already loaded locally; a mismatch prints a loud, specific warning (naming both the client's configured type and the server's actual one) rather than leaving a tester to wonder why every reconciliation looks violent. This is a diagnostic, not a fix for the underlying coordination requirement — matching `--port`'s existing behaviour rather than inventing a new, larger mechanism (e.g. the client re-trimming mid-session against a type it didn't start with) this increment doesn't need.

`flight_test_client` gains `--aircraft` too, for its own scripted and multiclient modes.

## Wire protocol changes

`ServerWelcome` gains one field:

```
struct ServerWelcome {
    uint8_t protocol_version;
    uint8_t assigned_player_id;
    float origin_lat_deg;
    float origin_lon_deg;
    uint16_t snapshot_hz;
    uint8_t aircraft_id;   // new: 0=c172x, 1=camel, 2=pa28 (see "Aircraft catalog")
};
```

No other message changes. Every other participant's serialization code is unaffected — `AircraftState`/`StateSnapshot` carry the same physical-state fields regardless of which airframe produced them.

## Test plan

1. **Standalone load+trim regression test** (new, Godot-free, no server/client) for c172x, Camel, and pa28, each at its own catalog IC — pins down the exact validated combination as a regression guard, the same role `predictcore_tests`'/`interpcore_tests`' checks play for their own increments. A future JSBSim version bump or config edit that breaks Camel's or pa28's trim should fail this test immediately, not get discovered by a confused human tester.
2. **`flight_server --aircraft camel`, then `--aircraft pa28`**, each with a connected `flight_test_client`: confirm `ServerWelcome.aircraft_id` matches what was requested, and that the connection flies for a sustained period (comparable to increment 1's own test durations) with no NaN, no divergence, no crash — proportionate confirmation that generalisation actually holds end-to-end for both non-baseline types, not a re-litigation of increment 1's full four-scenario flight-dynamics correctness for each (c172x already has that; what's new here is "does the *pipeline* still work," not "is this airframe's aerodynamics good").
3. **Mismatch detection**: connect a client configured for one type to a server configured for another; confirm the warning fires. Connect a correctly-matched pair; confirm it stays silent.
4. **Increment 5's multiclient test, re-run with `--aircraft camel`**: distinct player IDs, no cross-talk, live remote tracking, chunking — confirms the airframe choice is orthogonal to everything increment 5 built, not just individually compatible with it.
5. **Full `scripts/run_tests.sh`**, increments 1 through 6 together.

## Documentation

`README.md`'s server-usage sections gain `--aircraft`/`--aircraft` mentions alongside the existing `--max-clients`/`--server` documentation. `docs/roadmap.md` entry 6 already reflects this increment's scope and findings.

## Licence

No change. c172x, Camel, and pa28 are all part of the same LGPL-2.1 JSBSim source tree already fetched via CMake `FetchContent` — no new dependency.

## Acceptance criteria

1. c172x, Camel, and pa28 each load and trim successfully through `FlightSession`'s exact initialisation sequence, at each one's own catalog IC, as a regression test — not merely a one-off manual confirmation.
2. `flight_server --aircraft {c172x|camel|pa28}` runs any of the three with no code branching beyond the catalog lookup — same worker pool, same wire protocol, same chunking, everything increment 5 built, unmodified.
3. `ServerWelcome.aircraft_id` correctly identifies the server's configured type end to end.
4. A deliberately forced client/server aircraft-type mismatch produces a clear, specific warning; a correctly-matched pair produces none.
5. A full networked session flies for a sustained period with each of Camel and pa28 configured, with no NaN, divergence, or crash.
6. Increment 5's own multiclient test (capacity, chunking, no-cross-talk) passes unmodified when the server is configured for a non-c172x type.
7. The full `scripts/run_tests.sh` suite passes end-to-end across increments 1-6.
8. *(pending human verification, matching increment 4/5's own equivalent criteria)* A human confirms a Camel- or pa28-configured Godot session actually looks and feels sane to fly — qualitative, not something this project's automated tests can judge.

## Out of scope, explicitly deferred

Everything in increments 1-5's deferred lists, plus: multiple simultaneous aircraft types in one session (increment 8), any engine-management controls beyond a single abstracted throttle (see "Non-goals" — this is a standing decision now, not merely deferred), fixing dr1/p51d/L17 (shelved, revisit post-demo), real Spitfire/Bf-109 models (YASim incompatibility, separate undertaking), per-type visual differentiation, weapons/damage/hit detection (increments 9-10).

## Open questions for the implementer

- Whether `flight_test_client`'s single-aircraft scripted modes (`fidelity_input`, `version_reject`, `resilience`, `prediction`) need their own `--aircraft` flag, or whether staying c172x-only is fine for those given they test transport/reconciliation correctness already shown to be aircraft-independent (increments 3-4) rather than flight-dynamics correctness. Leaning towards: out of scope, not required.
- Whether pa28's narrow trim envelope (1000 ft works, 2000 ft and 5000 ft don't) is worth investigating further now, or accepted as-is the same way c172x and Camel each have exactly one validated canonical IC, not a claim that any altitude works.

## Appendix A: message and field reference (normative, updates to increment 5's)

`ServerWelcome`, current full layout:

| Field | Wire type | Notes |
|---|---|---|
| `protocol_version` | `uint8` | unchanged |
| `assigned_player_id` | `uint8` | unchanged |
| `origin_lat_deg` | `float32` | unchanged |
| `origin_lon_deg` | `float32` | unchanged |
| `snapshot_hz` | `uint16` | unchanged |
| `aircraft_id` | `uint8` | **new**: 0=c172x, 1=camel, 2=pa28 (see "Aircraft catalog") |

`ServerReject`, `ClientHello`, `ControlInput`, `StateSnapshot`, `PlayerLeft` — **unchanged** in layout from increment 5.

## Appendix B: measured findings from pre-drafting validation (informative)

Measured via a standalone probe (Godot-free, no `flight_server`/`flight_test_client` involved) reusing `flightcore::FlightSession`'s exact initialisation sequence (`SetAircraftPath`/`SetEnginePath`/`SetSystemsPath`, `LoadModel`, `DisableOutput`, `ic/*` properties, `RunIC()`, `InitRunning(-1)`, `FGTrim(tFull)`), against the same fetched JSBSim source tree (`build/_deps/jsbsim-src`) this project's `CMakeLists.txt` already pulls via `FetchContent`.

**Confirmed working, first attempt, no patches:**

| aircraft | altitude | VC | result |
|---|---|---|---|
| c172x | 5000 ft | 100 kt | trimmed OK (throttle=0.792, aileron=−0.075, rudder=−0.004 — matches increment 4's own Appendix B figures exactly) |
| Camel | 5000 ft | 65 kt | trimmed OK (throttle=0.301, aileron=0.102, rudder=−0.064, theta=2.82°) |
| J3Cub (Piper Cub) | 3000 ft | 55 kt | trimmed OK, engine running=1 rpm=1555.8 power=33.0hp — not part of the chosen fleet, tested only to help distinguish a harness-wide bug from an aircraft-specific one (see below) |
| pa28 | 1000 ft | 100 kt | trimmed OK, engine running=1 rpm=1761.6 power=98.1hp |

**pa28's narrow trim envelope**: fails to converge at 2000 ft and 5000 ft (both otherwise-plausible cruise altitudes) despite the engine running correctly at both — trim converges only in the narrower band around 1000 ft tested. Not investigated further; recorded as this type's validated operating point, the same way c172x and Camel each have exactly one validated canonical IC rather than a broader envelope claim.

**Confirmed *not* working, three distinct defects, none a harness-wide problem:**

- **p51d (North American — actually Packard-built Merlin-engined — P-51D Mustang)**: the engine never starts. `InitRunning(-1)` — the identical call every working aircraft above uses — leaves `propulsion/engine[0]/set-running` at 0, RPM at 0, and power at −1.5 hp (pure windmilling drag) even after 2 full seconds of simulated `Run()` at full throttle and full mixture (`fcs/throttle-cmd-norm=1.0`, `fcs/mixture-cmd-norm=1.0`, `mixture/position=1.0` all explicitly forced). Swept 6 speeds (150-280 kt) × 4 altitudes (0-10,000 ft): every combination fails to trim, consistent with zero available thrust to balance drag at any operating point tried. p51d ships extra custom systems no other candidate has (`Systems/autothrottle.xml`, `Systems/mixture-control.xml`, `Systems/crash-detect.xml`, `Systems/alpha-buffet.xml`, `Systems/compressability.xml`); `autothrottle.xml`'s own comment ("on this engine it is on by default and is only over[r]idden...") suggests it may be actively overriding the throttle `InitRunning()` establishes. Root cause not fully isolated.
- **dr1 (Fokker Dr.I triplane)**: crashes on load with an uncaught `JSBSim::LogException`: `FGPropertyValue::GetValue() The property /sim/model/pushback/position-norm does not exist`. Traced to `aircraft/dr1/Systems/pushback.xml`, an optional ground-towing convenience system (irrelevant to flight) whose `<switch>` test conditions read `/sim/model/pushback/*` properties that FlightGear itself normally provides but bare standalone JSBSim never does — and, unlike `<input>`/`<output>` tags elsewhere, a `<switch>`'s `<test>` condition does not auto-vivify a never-declared property, it throws. A local patched copy with dr1's one `<system file="pushback"/>` line removed (dr1's *other* system, `chocks.xml`, was checked too — it only reads the always-standard `/controls/gear/brake-parking`, not a FlightGear-only path, so it's a non-issue) loads cleanly and its engine genuinely starts (running=1, rpm=1151.0, power=89.6 hp, confirming this is not an engine-start problem like p51d's) — but trim still fails to converge across 8 speeds (40-90 kt) × 4 altitudes (0-1000 ft) tried after that fix. A separate, unresolved issue from the load crash.
- **L17 (Navion)**: crashes on load, on a *different* missing property than dr1's: `FGPropertyValue::GetValue() The property fcs/flaps-pos-deg does not exist` — its aerodynamics reference a flap-position property its own FCS never actually defines. Not investigated further once found.

**Why this is not a harness-wide problem**: four aircraft beyond the plain c172x baseline (Camel, J3Cub, pa28, and dr1 *after* its load crash was patched) all trim successfully through the identical initialisation code, with zero special-casing per aircraft beyond the IC values themselves. The three that fail each fail for a different, specific, traceable reason rooted in that aircraft's own config — not a common pattern that would indict this project's own sequence.

**Spitfire and Bf-109 are not a near-term option**: both exist in FGAddon (FlightGear's aircraft hangar), but both use YASim, not JSBSim, as their flight dynamics model (confirmed via the Spitfire's own `-set.xml` and the Bf-109's FlightGear wiki page). YASim and JSBSim are incompatible engines with entirely different config formats; a real port is a from-scratch undertaking, not an off-the-shelf pull.
