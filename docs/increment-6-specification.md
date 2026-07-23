# Increment 6 Specification: Multi-Airframe Migration

## Status

Revision 2. Pre-drafting validation complete, then adversarially reviewed (`docs/increment-6-specification-review.md`, findings B1/M1/M2/M3/m1/m2 folded in); not yet implemented. Appendix B carries both the pre-drafting and the review measurements.

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

A small table, the one place aircraft-specific identity and trim IC live. It carries three **explicitly-distinct** fields per aircraft that must never be conflated or derived from one another (review finding B1):

| token | JSBSim `LoadModel` string | `aircraft_id` (wire) | canonical trim altitude | canonical trim VC | notes |
|---|---|---|---|---|---|
| `c172x` | `c172x` | 0 | 5000 ft | 100 kt | unchanged from increments 1-5 |
| `camel` | **`Camel`** | 1 | 5000 ft | 65 kt | LoadModel string is capitalised — the JSBSim directory is `Camel/`, and `LoadModel("camel")` **fails** on Linux's case-sensitive filesystem (measured, Appendix B) |
| `pa28` | `pa28` | 2 | 1000 ft | 100 kt | does **not** converge at 2000 ft or 5000 ft - see Appendix B |

- The **token** is the lowercase user-facing string: what `--aircraft` accepts, what `SERVER_HOST`-style env config uses, and what the wire `aircraft_id` decodes back to for display.
- The **`LoadModel` string** is whatever JSBSim's aircraft directory is *actually* named, case included — passed verbatim to `FGFDMExec::LoadModel`. It is **not** to be derived by lowercasing the token, uppercasing the first letter, or any other transform: c172x and pa28 happen to equal their tokens, Camel does not, and there is no rule connecting them beyond "look it up in this table."
- The **`aircraft_id`** is the numeric wire encoding (Appendix A).

pa28's canonical altitude (1000 ft ≈ 305 m) is also the altitude the shared-origin session (increment 5) trims at and every pa28 spawns at — comfortably clear of the flat ground plane at 0; chosen because it is pa28's only validated trim point, not for any origin-geometry reason (review finding m2).

`flightcore::FlightSession::initialize()` gains a parameter for the `LoadModel` string (currently hardcoded to `"c172x"`); every call site that currently hardcodes `setInitialCondition(5000.0, 100.0, ...)` (`src/server/main.cpp:221,276`, `src/godot_ext/predicted_aircraft.cpp:75`) looks both the `LoadModel` string and its IC up from this table by token instead.

Scripted server mode (`flight_server --scenario ...`, increment 1's regression scenarios) stays c172x-only and untouched by any of this — those scenarios *are* the c172x ground truth, not something to re-run against other airframes. `--scenario` therefore **implies and requires** c172x: a non-c172x `--aircraft` combined with `--scenario` is rejected at startup with a clear message rather than silently run through c172x-tuned pass thresholds (review finding m1).

### Server-authoritative type selection

`flight_server` gains `--aircraft {c172x|camel|pa28}` (default `c172x`), fixed for the lifetime of one server process. Every aircraft that process manages — real clients, `--stress-aircraft` synthetic aircraft — uses the same configured type; this is a whole-session setting, not per-client (per-client choice is increment 8).

A connecting Godot client needs to trim the *same* type the server is actually running, or `PredictedSession`'s client-side prediction silently predicts with the wrong airframe entirely and every reconciliation fights it from the second correction onward. Rather than restructure `PredictedAircraft::_ready()` to defer trimming until a `ServerWelcome` arrives (a real behavioural change to an already-working sequence, more invasive than this needs), the fix is the same shape as `scripts/start_client.sh`'s existing `SERVER_HOST`/`SERVER_PORT` override: `PredictedAircraft` gains an `aircraft_type` exported property (default `"c172x"`), overridable via an `AIRCRAFT` environment variable read in `_ready()` before trimming, exactly like `SERVER_HOST`/`SERVER_PORT` already are (`src/godot_ext/predicted_aircraft.cpp`) — and `scripts/start_client.sh` gains a matching `--aircraft` flag alongside its existing `--server` one. This makes picking a type an explicit, coordinated choice between server operator and player, the same relationship `--port` already has today (a client pointed at the wrong port simply doesn't connect; a client configured for the wrong aircraft type needs to be *audible*, not silently wrong, which is what the wire field below is for).

`ServerWelcome` gains `aircraft_id` (see Appendix A). On receipt, `PredictedAircraft` compares it against whichever type it already loaded and trimmed locally. Because client and server run bit-identical JSBSim (determinism confirmed for all three airframes, Appendix B), a *matched* pair predicts and reconciles exactly as increment 4 established; a *mismatched* pair predicts a different airframe's physics from the server's authoritative state and is therefore corrected hard on essentially every snapshot — a continuously-fighting, unplayable session, not a subtle degradation (review finding M3). The behaviour on mismatch is therefore specified precisely, not left as "prints a warning":

- **Warn loudly and specifically**, naming both the client's configured token and the server's announced token.
- **By default, disconnect** rather than present an unplayable, continuously-corrected session — a wrong-aircraft client is a misconfiguration to surface, not a degraded-but-usable state to limp along in.
- **Continue flying only if a deliberate override flag is set** (for a tester who explicitly wants to observe the mismatch behaviour).

This keeps the same out-of-band coordination contract `--port` already uses (a client pointed at the wrong server simply doesn't connect; a client configured for the wrong aircraft is told so, loudly, and stops) rather than inventing a larger mechanism.

**Alternative, deliberately deferred**: the client could instead *adopt* the server's announced `aircraft_id` — trimming the server's type rather than a pre-configured one, eliminating the mismatch entirely. This is a real, defensible design, but it requires deferring the client's trim from `PredictedAircraft::_ready()` (where it happens today, before connecting) until `ServerWelcome` arrives — a genuine change to the already-validated increment-4 `_ready()`/trim sequence, and one that still fails if the server runs a type the client's build doesn't know. It is out of scope for this derisking increment, whose coordination story matches `--port`; recorded here so a future increment can adopt it deliberately rather than rediscover it.

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

**`kProtocolVersion` bumps from 1 to 2** (review finding M2). Adding a trailing field changes `ServerWelcome`'s wire length, and the version handshake exists precisely to reject a client and server built from different trees before they exchange misparsed messages: without the bump, a newer client reading an older server's shorter welcome fails to parse it with no clean "version mismatch" signal, while an older client silently ignores the new field. This matters newly at *this* increment because it is the first whose whole point is people connecting to a server from their own separately-built binaries (the `build_server.sh`/`build_client.sh` split, the `--aircraft` operator/player coordination), making build skew a realistic tester setup rather than a CI-only abstraction. The bump to 2 also covers the accumulated drift from increments 4 and 5, which changed `AircraftState`/`StateSnapshot`/`ServerWelcome` layout while leaving the version at 1 — a latent gap prior reviews missed. **Standing rule from here on: any change to a message's wire layout bumps `kProtocolVersion`.**

## Test plan

1. **Standalone load+trim regression test** (new, Godot-free, no server/client) for c172x, Camel, and pa28, each at its own catalog IC — pins down the exact validated combination as a regression guard, the same role `predictcore_tests`'/`interpcore_tests`' checks play for their own increments. A future JSBSim version bump or config edit that breaks Camel's or pa28's trim should fail this test immediately, not get discovered by a confused human tester.
2. **`flight_server --aircraft camel`, then `--aircraft pa28`**, each with a connected `flight_test_client`: confirm `ServerWelcome.aircraft_id` matches what was requested, and that the connection flies for a sustained period (comparable to increment 1's own test durations) with no NaN, no divergence, no crash — proportionate confirmation that generalisation actually holds end-to-end for both non-baseline types, not a re-litigation of increment 1's full four-scenario flight-dynamics correctness for each (c172x already has that; what's new here is "does the *pipeline* still work," not "is this airframe's aerodynamics good").
3. **The prediction/reconciliation stack itself, per airframe** (review finding M1 — the increment's central claim, and the one "flies without crashing" would silently pass while broken): `flight_test_client --mode prediction --aircraft camel --skip-eventual-agreement`, and the pa28 equivalent, asserting the airframe-*independent* prediction criteria — immediate response (client input moves the predicted state within a tick or two, before any server round-trip), bounded-envelope tracking (predicted-vs-authoritative error stays within increment 4's thresholds), and forced-desync recovery (an injected misprediction is reconciled away, with reconciliation-off as the negative control). These compare the predicting client against the *live server*, not a c172x ground-truth log, so they are valid for any airframe; only test 2's eventual-agreement check needs the c172x `server_pitch_response.csv` and is correctly skipped. This is the automated proof that a human flying Camel or pa28 gets increment 4's prediction quality, not merely an aircraft that doesn't explode — cheap and justified because cross-airframe determinism, prediction's load-bearing premise, is confirmed bit-identical (Appendix B).
4. **Mismatch behaviour** (review finding M3): connect a client configured for one type to a server configured for another; confirm it warns loudly (naming both types) and disconnects by default, and that the override flag lets it stay connected. Connect a correctly-matched pair; confirm it proceeds silently.
5. **Increment 5's multiclient test, re-run with `--aircraft camel`**: distinct player IDs, no cross-talk, live remote tracking, chunking — confirms the airframe choice is orthogonal to everything increment 5 built, not just individually compatible with it. (The cross-talk gate's ±5 m/4 s thresholds are confirmed to transfer to Camel and pa28, Appendix B, so this is a genuine re-run, not one needing per-airframe threshold retuning.)
6. **Scripted-mode guard** (review finding m1): `flight_server --aircraft camel --scenario pitch_response` is rejected at startup, not silently run.
7. **Full `scripts/run_tests.sh`**, increments 1 through 6 together.

## Documentation

`README.md`'s server-usage sections gain `--aircraft`/`--aircraft` mentions alongside the existing `--max-clients`/`--server` documentation. `docs/roadmap.md` entry 6 already reflects this increment's scope and findings.

## Licence

No change. c172x, Camel, and pa28 are all part of the same LGPL-2.1 JSBSim source tree already fetched via CMake `FetchContent` — no new dependency.

## Acceptance criteria

1. c172x, Camel, and pa28 each load (via their exact catalog `LoadModel` string, case included — `Camel`, not `camel`) and trim successfully through `FlightSession`'s exact initialisation sequence, at each one's own catalog IC, as a regression test — not merely a one-off manual confirmation.
2. `flight_server --aircraft {c172x|camel|pa28}` runs any of the three with no code branching beyond the catalog lookup — same worker pool, same wire protocol, same chunking, everything increment 5 built, unmodified.
3. `ServerWelcome.aircraft_id` correctly identifies the server's configured type end to end, and `kProtocolVersion` is 2.
4. **The prediction/reconciliation stack passes its airframe-independent criteria (immediate response, bounded-envelope tracking, forced-desync recovery with its negative control) for Camel and pa28**, not only c172x — the automated proof the pipeline generalises (review finding M1).
5. A deliberately forced client/server aircraft-type mismatch warns loudly (naming both types) and disconnects by default, staying connected only under the explicit override flag; a correctly-matched pair proceeds silently.
6. A full networked session flies for a sustained period with each of Camel and pa28 configured, with no NaN, divergence, or crash.
7. Increment 5's own multiclient test (capacity, chunking, no-cross-talk) passes unmodified when the server is configured for a non-c172x type.
8. `--aircraft {camel|pa28} --scenario ...` is rejected at startup rather than run through c172x-tuned thresholds.
9. The full `scripts/run_tests.sh` suite passes end-to-end across increments 1-6.
10. *(pending human verification, matching increment 4/5's own equivalent criteria)* A human confirms a Camel- or pa28-configured Godot session actually looks and feels sane to fly — qualitative, not something this project's automated tests can judge. (Camel specifically is the airframe most likely to feel marginal — its climb plateaus near stall under sustained full-aft elevator, Appendix B.)

## Out of scope, explicitly deferred

Everything in increments 1-5's deferred lists, plus: multiple simultaneous aircraft types in one session (increment 8), any engine-management controls beyond a single abstracted throttle (see "Non-goals" — this is a standing decision now, not merely deferred), fixing dr1/p51d/L17 (shelved, revisit post-demo), real Spitfire/Bf-109 models (YASim incompatibility, separate undertaking), per-type visual differentiation, weapons/damage/hit detection (increments 9-10).

## Open questions for the implementer

- **Resolved by review finding M1**: `--mode prediction` *does* need `--aircraft` and is exercised for Camel and pa28 (test plan item 3) — that check is the increment's central proof, not an optional extra. The purely transport-level modes (`fidelity_input`, `version_reject`, `resilience`) remain fine as c172x-only: they test ENet transport and protocol behaviour already shown aircraft-independent in increment 3, not anything the airframe touches.
- Whether pa28's narrow trim envelope (1000 ft works, 2000 ft and 5000 ft don't) is worth investigating further now, or accepted as-is the same way c172x and Camel each have exactly one validated canonical IC, not a claim that any altitude works. (Review: accepted as-is; a future need for pa28 at altitude re-validates its envelope then.)

## Appendix A: message and field reference (normative, updates to increment 5's)

`ServerWelcome`, current full layout:

| Field | Wire type | Notes |
|---|---|---|
| `protocol_version` | `uint8` | value bumped 1 → 2 (review finding M2); layout unchanged |
| `assigned_player_id` | `uint8` | unchanged |
| `origin_lat_deg` | `float32` | unchanged |
| `origin_lon_deg` | `float32` | unchanged |
| `snapshot_hz` | `uint16` | unchanged |
| `aircraft_id` | `uint8` | **new**: 0=c172x, 1=camel, 2=pa28 (see "Aircraft catalog") |

`ServerReject`, `ClientHello`, `ControlInput`, `StateSnapshot`, `PlayerLeft` — **unchanged** in layout from increment 5. `kProtocolVersion` becomes 2 (value only; the constant governs the `ClientHello`/`ServerReject` version handshake, whose layout does not change).

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

### Review measurements (added during adversarial review; `probe_inc6_review.cpp`)

A second probe, built for the adversarial review, checked five things the pre-drafting pass did not — same standalone harness, same fetched tree. Two disproved a review worry; one found a defect the draft had baked in.

- **LoadModel is case-sensitive, and the draft got Camel's name wrong** (finding B1, blocking): `LoadModel("camel")` **FAILS**, `LoadModel("Camel")` succeeds. The pre-drafting pass happened to pass `"Camel"`; the draft's catalog table then wrote it lowercase. This is why the catalog now carries a separate exact-`LoadModel`-string column.
- **A missing-property read returns 0, it does not throw**: `GetPropertyValue("fcs/this-does-not-exist")` returned `0.000` quietly. So `FlightSession::sample()`'s wholesale c172x-shaped property read is safe for an airframe whose FCS lacks one of those properties — the value is simply 0, no exception, no per-tick crash. (Contrast dr1's load *crash* above, which came from a `<switch>` `<test>` condition, a different JSBSim code path that *does* throw on an undeclared property.)
- **The full `sample()` property set reads cleanly for all three**: every property `sample()` reads, plus the three `velocities/[pqr]-rad_sec` body rates the networked server reads directly, resolved with no throw for c172x, Camel, and pa28. State sampling is airframe-independent — because absent properties read as 0, per the point above, not because all three define an identical set.
- **Cross-airframe determinism is bit-identical** (grounds finding M1): two independent instances each of Camel and of pa28, run through an identical 3-second mixed-input schedule (elevator/aileron/rudder/throttle steps), diverged by exactly `0.0` in latitude, altitude, roll, and pitch — matching c172x. The premise all of increment 4's prediction/reconciliation rests on generalises to the new airframes.
- **Increment 5's cross-talk gate transfers** (throttle 1.0, elevator ±0.8, ≥5 m altitude delta within 4 s): all six cases (three airframes × climb/descend) pass. Magnitudes vary widely — by 4 s, c172x climbs +80 m, pa28 +51 m, Camel only +23 m — and Camel's nose-up climb *plateaus and slightly reverses* between 3 s (+23.8 m) and 4 s (+23.1 m), energy bleed near stall at 65 kt under full-aft elevator. The 5 m gate clears with margin, so the automated re-run is safe, but Camel's plateau is the "feel" flag carried into acceptance criterion 10.
- **No stray output files for the new airframes**: c172x writes `JSBout172B.csv` during `LoadModel` (gitignored). Checked directly — Camel's file-output block is commented out and pa28 has none (their only `<output>` tags are FCS property assignments, not file writers), so `DisableOutput()` plus these configs produce no new stray files and need no new `.gitignore` entries.
