# Increment 4 Specification: Client-Side Prediction and Server Reconciliation

## Status

Draft 1. This is increment 4 of the derisking sequence (`docs/roadmap.md`), following a split: the roadmap originally combined "multiplayer scaling and feel" into one increment 4. The two concerns are independent — prediction/reconciliation needs only the single client and aircraft increment 3 already has, and multi-core scaling needs no prediction logic to test — so mixing them would blur the review's focus across two very different failure-mode domains (netcode correctness vs. concurrency/performance). This document covers "feel" only; scaling becomes increment 5. Builds on increment 3's client-server architecture: `netcore`'s wire protocol, `flight_server`'s authoritative loop, and the Godot `NetworkClient`/`RemoteAircraft`/`FlightAircraft` classes.

Before this draft was written, the following was verified by building and running real code (details and measured numbers in Appendix B):

- JSBSim's `FGPropagate::GetVState()`/`SetVState()` correctly restores rigid-body state (position, velocity, attitude) with zero drift on an immediate snapshot/restore, and the cost of capturing a snapshot is negligible (well under 1 µs).
- `SetVState()` does **not** restore flight-control-system state: a rate-limited actuator (the c172x's aileron) is still at its pre-rewind position immediately after `SetVState()` returns — confirmed directly by inspecting the actuator's output property before and after, not inferred from reading JSBSim's source alone. Left unaddressed, this caused a large, immediate (single-tick) divergence between a "live" trajectory and a naively rewound-and-replayed one.
- A new GDExtension class can cleanly subclass an existing project-defined GDExtension class (`FlightAircraft`), inheriting its bound methods and properties; confirmed with a working headless probe that called an inherited method through the subclass and got correct behaviour.
- JSBSim exposes the building blocks (`FGPropagate::GetTl2i()`, `GetEarthPositionAngle()`) needed to convert a wire-format local-frame attitude into the ECI-frame quaternion `SetVState()` expects.

The draft's design choice, made explicitly in response to the actuator-state finding: **do not chase bit-perfect reconciliation**. Rewind and replay only the rigid-body state (position, velocity, attitude, body rates) that increment 3's wire protocol already carries (plus one small addition, below); leave flight-control-system internals unrewound. This is standard practice in networked games — correct the state that hit detection and rendering depend on precisely, and let secondary/filtered systems (actuator lag, control filters) settle over the next few ticks, since their error is bounded by how far they could have moved in the rewound window and decays as consistent input continues to arrive. It is also, deliberately, the boring choice: this project's economics favour mature, widely-used patterns over novel ones, and "rewind the physics state, tolerate transient actuator settle" is exactly what the Quake-lineage client-prediction pattern already does, not something invented for this project.

## Goal

Close the "feel" gap increment 3 explicitly deferred: give the local player's own aircraft an immediate, latency-independent response to input by simulating it predictively on the client, while remaining reconciled with the server's authoritative outcome once corrections arrive. Prove this holds under realistic latency and loss (reusing increment 3's `net_relay`), not just on a clean localhost link — that is the actual point of this increment, since prediction that only works with zero latency proves nothing increment 3 didn't already prove.

## Non-goals

Explicitly out of scope; must not be implemented:

- Multiple concurrent clients or aircraft, and therefore interpolation/extrapolation of *other* entities' snapshots (increment 5 — with one client and one aircraft there is nothing else to interpolate; this increment's own aircraft is always predicted, never interpolated)
- Multi-core server parallelism, or any server-side performance work (increment 5)
- Bit-perfect reconciliation of JSBSim flight-control-system internals (actuators, filters, PIDs) — accepted as a bounded, self-correcting divergence (see "Status" and Appendix B); only rigid-body state is rewound
- Patching or forking JSBSim to expose FCS component state — the alternative this increment deliberately does not take
- Hit detection, lag compensation for weapons, or "favor the shooter" — a related but distinct concern that arrives with weapons (increment 9)
- Jitter buffers, adaptive interpolation delay, or any smoothing beyond a straightforward fixed-duration blend on the *rendered* transform after a correction
- Cross-platform/cross-compiler determinism guarantees between client and server (both run the same binary/toolchain in this project's tests; real heterogeneous-hardware determinism is a production concern, not a derisking one)
- WWII aircraft, damage, weapons (later increments, unchanged)

## Architecture

Three new/changed pieces, layered the same way increment 3 layered `netcore`:

- **`predictcore`** (new, `src/predictcore/`) — a Godot-free C++ library depending on `flightcore` (for `FlightSession`, `geo::`) and `netcore` (for the wire types and `encodeAxis`/`decodeAxis`). Defines `PredictedSession`, the reusable predict/buffer/reconcile/replay algorithm, so the Godot client and the automated test harness run the *identical* reconciliation code — the same reasoning increment 3 used for the wire protocol itself: shared code is the only way to guarantee the automated tests actually exercise what the human-facing client does.
- **`PredictedAircraft`** (new, `src/godot_ext/predicted_aircraft.h`/`.cpp`) — a GDExtension `Node3D` that **subclasses `FlightAircraft`** (confirmed workable, Appendix B), inheriting its `FlightSession` ownership and all of its control-input setters and telemetry getters unchanged. It adds: a `PredictedSession`-driven input/state ring buffer, sending `ControlInput` over the network (reusing `net::NetClient`, the same wrapper `NetworkClient` uses), consuming `StateSnapshot`s to reconcile, and blending the *rendered* transform across a correction rather than snapping it. `networked.tscn` replaces its `RemoteAircraft` node with `PredictedAircraft` for the (still singular) local player's aircraft. `RemoteAircraft` itself is untouched and stays in the codebase unused by this increment's scene — it becomes relevant again the moment increment 5 adds other clients, whose aircraft the local player will see via undisturbed raw/interpolated snapshot display, exactly the job it already does.
- **`flight_test_client` gains `predictcore`** as a link dependency and a new `--mode prediction`, running a real `PredictedSession` headlessly so the automated suite exercises the exact same reconciliation code as the Godot client (see "Test plan"). This does not weaken increment 3's B1 finding (the client must not *re-derive physics criteria* from received snapshots) — it is a different thing: the client is now supposed to run a real local simulation, so testing that honestly requires the test harness to run one too.

`flight_server` is **unchanged in behaviour**. It keeps applying "the most recently received `ControlInput`" every tick, exactly as increment 3 built it — all of this increment's complexity lives client-side, which is both the classic shape of this pattern (dumb, simple, already-correct server; smart client) and the smallest possible diff to a component increment 3 already validated end-to-end. The only server changes are two additional fields it now includes in its outgoing `StateSnapshot` (see "Wire protocol changes").

```
 client, every physics tick (120 Hz):
   sample input -> apply to LOCAL FlightSession -> step -> buffer (input, resulting VState)
   -> send ControlInput{client_seq, input} (unreliable)     -> displayed transform updates immediately

 server, unchanged from increment 3:
   apply most-recently-received ControlInput each tick -> step authoritative FlightSession
   -> broadcast StateSnapshot{..., ack_client_seq} at 30 Hz (unreliable)

 client, on receiving a StateSnapshot:
   look up buffered state at ack_client_seq -> compare to the snapshot's authoritative state
   within tolerance -> discard old buffer entries, done
   beyond tolerance -> SetVState(reconstructed authoritative state)
                       -> replay every buffered input since ack_client_seq
                       -> new buffer tail becomes the corrected prediction
                       -> blend the RENDERED transform toward it over ~150ms
```

## Wire protocol changes

Two additive changes to increment 3's protocol (`docs/increment-3-specification.md`, "Wire protocol"). Neither changes a message's *tag*, and neither changes the *packetisation* shape that increment 3's m1 finding already flagged as the thing likely to change at scale — both are new fields within the existing messages.

1. **`StateSnapshot` gains a top-level `ack_client_seq` (`uint32`)**, placed after `server_tick` and before `aircraft_count`. This is the `client_seq` of the most recent `ControlInput` the server had applied as of this tick — connection-scoped metadata, not a property of any aircraft, so it does not belong in the per-aircraft record (unlike `server_tick`, which is genuinely global, `ack_client_seq` is only meaningful to whichever single client owns the connection this snapshot is sent on). The server already tracks this value internally (`flight_server`'s existing `lastSeq`); this only wires it into the outgoint snapshot.
2. **Each `AircraftState` gains `ang_vel_local_rps` (`float32[3]`)**, body-frame roll/pitch/yaw rate in rad/s, placed after `vel_local_mps`. Needed because reconstructing a JSBSim `VehicleState` for `SetVState()` requires `vPQR` (body angular rate), which increment 3's snapshot never carried (increment 3 had no reason to; nothing consumed it). Deriving it is a direct property read (`velocities/p-rad_sec` etc.) alongside the fields `flight_server` already samples per tick.

Field-width discipline carries over unchanged (little-endian, field-by-field, no struct `memcpy`). Per-aircraft record size grows from 42 to 54 bytes; `StateSnapshot`'s total grows by 4 (top-level) + 12 (per aircraft, ×1 aircraft this increment) = 16 bytes — immaterial at this increment's one-aircraft scale, and exactly the kind of per-aircraft-record growth increment 3's own m1 finding anticipated.

**`ControlInput` send rate changes from increment 3's 60 Hz default to 120 Hz**, matching the simulation tick exactly. This is a deliberate change, not an oversight: increment 3 chose 60 Hz somewhat arbitrarily (nothing consumed the exact tick correspondence yet). Reconciliation needs an exact one-to-one mapping between "the input predicted tick N used" and "the input the server acknowledges having applied at its tick N" — sending one `ControlInput` per physics tick, every tick, with a monotonically increasing `client_seq`, is what makes that mapping unambiguous. Bandwidth impact is immaterial: a `ControlInput` packet is ~13 bytes of payload (plus ENet/UDP overhead), so 120/s is on the order of 6–8 KB/s upstream per client — and unlike the downstream snapshot broadcast, this does not multiply with player count, since each client only ever sends its own input.

## Client-side prediction algorithm

Every physics tick (120 Hz, the same fixed-timestep pattern established in increment 2 and reused unchanged since):

1. Sample local input (same keyboard mapping as `networked_input.gd`, unchanged).
2. Increment `client_seq`.
3. Apply the input to the **local** `FlightSession` (the same one `FlightAircraft`'s inherited machinery already owns) and step it forward one tick. This is the prediction: the aircraft responds this tick, not after a round trip.
4. Push `(client_seq, input)` onto the input ring buffer and `(client_seq, session.GetVState())` onto the state ring buffer. Both are fixed-size circular buffers (Appendix B: negligible cost, ~1 second of ticks costs well under 500 KB, sized generously above any tested latency so it is not a tuning-sensitive parameter).
5. Send `ControlInput{client_seq, encoded input}` (unreliable channel).
6. Update the rendered transform from the local prediction (subject to the blend described below).

On receiving a `StateSnapshot` (server's ~30 Hz rate, decoupled from the client's 120 Hz prediction rate exactly as increment 3 decoupled snapshot rate from simulation rate):

1. Read `ack_client_seq` and this client's own `AircraftState` entry (matched by `player_id`, same lookup `NetworkClient` already does).
2. Find the state-ring-buffer entry at `ack_client_seq`. If it has already fallen out of the buffer (only possible if the round trip exceeded the buffer's generous window), treat it as an unconditional correction and log a warning — this must degrade safely, not undefined-behaviour, though it should not happen under any latency/loss this increment tests (Appendix B's buffer-sizing measurement is the reason it shouldn't).
3. Convert that buffered prediction into the same local-frame-position + quaternion + body-frame-velocity representation the wire uses, by calling the *same* `geo::computeLocalOffset()`/`geo::computeOrientationQuat()` functions `flight_server` already calls to build its own outgoing snapshots (increment 3's `src/geo/`, reused unchanged) — this guarantees the client compares like with like, not two different derivations that could disagree for reasons unrelated to an actual misprediction.
4. Compare against the received authoritative fields. **Threshold** (starting point, to be tuned empirically during implementation exactly as increment 3 tuned its own thresholds once real data existed): position mismatch beyond 0.5 m in the local frame, or attitude mismatch beyond 2°, triggers a correction; either check passing does not. (Increment 3's own lesson applies here too: compare via a numerically well-conditioned metric — component-wise or a chord-based angle, not `acos(dot)` naively, which is ill-conditioned near small angles.)
5. If within threshold: discard buffer entries older than `ack_client_seq` (no longer needed) and continue predicting from where it already was.
6. If beyond threshold — **reconciliation**:
   a. Reconstruct a JSBSim `VehicleState` from the snapshot's authoritative fields: position (local offset inverted against the known origin from `ServerWelcome`, giving lat/lon; altitude directly), attitude (`ang_vel_local_rps` gives `vPQR` directly; the wire's local quaternion must be converted to the ECI quaternion `SetVState()` requires via `Tl2i.GetQuaternion() * qLocal`, using the client's own `GetTl2i()`/`GetEarthPositionAngle()` at the target position — both computable independently of anything that diverged, since Earth Position Angle depends only on elapsed simulation time), velocity (rotate the wire's local-frame velocity into body frame using the now-known attitude).
   b. `SetVState()` the local `FlightSession` to this reconstructed state.
   c. Replay: for every buffered input from `ack_client_seq + 1` through the latest locally-generated `client_seq`, in order, apply it and step the session forward one tick, overwriting that tick's state-ring-buffer entry with the newly recomputed value.
   d. The final replayed tick becomes the new current prediction. Record that a correction occurred and its magnitude (for the smoothing step below, and for test/telemetry visibility — see "Test plan").

## Smooth error correction (visual only)

The *simulated* state corrects immediately (subsequent ticks must build on the right answer). The *rendered* transform does not snap: on a correction, blend the displayed `Transform3D` from wherever it was toward the newly-corrected prediction over a short, fixed window (starting point: ~150 ms, i.e. roughly 18 physics ticks at 120 Hz — tunable, not load-bearing; the exact constant is an implementer's-discretion detail, not an architectural one). This is display-layer only: it must not feed back into the simulated state, or it would reintroduce exactly the kind of state/display divergence this whole increment exists to close.

## Test plan

`flight_test_client --mode prediction` runs a real `PredictedSession` (via `predictcore`) against a live `flight_server`, optionally through `net_relay` for latency/loss (reusing increment 3's relay unchanged). Required assertions:

1. **Immediate response**: after commanding a step input change, the *local predicted* state (not the last snapshot received from the server) shows the expected directional change within 1–2 physics ticks — proving prediction is not silently waiting on a round trip. Uses the same kind of directly-observable, non-derived signal increment 3's input-path test used (e.g. altitude/attitude trend from the raw predicted `FlightSample`, not something requiring quaternion-derivation from the wire).
2. **Eventual agreement with the server's own ground truth**: once enough wall-clock time has passed for the server's authoritative response and any correction to settle (accounting for injected latency), the local prediction's trajectory matches what `flight_server`'s own scripted-mode evaluation of the identical input schedule produces (reusing increment 3's server-side ground truth machinery — this is the B1 lesson applied again: validate against the server's authoritative computation, not a self-consistency check the client could pass while being wrong).
3. **Reconciliation is actually exercised, not just possible**: the test must force at least one real misprediction (e.g. an artificial initial offset, or a dropped input via `net_relay`'s loss injection) and assert the correction-count telemetry is nonzero — mirroring increment 3's B2 finding that an optional code path is a code path CI will quietly never run.
4. **Bounded, convergent drift, not divergence**: after a forced correction, assert the aircraft's rigid-body state converges to within tolerance of ground truth within a bounded number of subsequent ticks (not that it is instantly perfect) — this is the direct test of the "bounded and self-correcting" claim the whole design rests on, rather than leaving it as an assertion in prose.
5. Repeat 1–4 through `net_relay` at the same latency/loss values increment 3 already validated (100 ms one-way delay; 20% drop), confirming the pattern holds under real impairment, not only on a clean localhost link.
6. Increment 1–3 suites continue to pass unchanged.

The Godot client (`PredictedAircraft` in `networked.tscn`) remains a manual verification criterion, consistent with increments 2 and 3: a human flies it under an artificially-latent connection (`net_relay` again) and confirms input feels immediate and corrections are not visually jarring — not CI-gated, documented as performed.

## Documentation

New C++/GDScript files carry the GPL-3.0 header convention, matching every prior increment. Design decisions with non-obvious rationale — the ECI-quaternion reconstruction, the accepted actuator-state gap, the ring-buffer sizing, the correction threshold — get inline comments pointing back to this spec, following the pattern already established in `flight_aircraft.cpp` and `flight_server`.

## Licence

Unchanged: GPL-3.0-or-later. No new third-party dependencies (this increment adds no new library, only new code using JSBSim and ENet, both already fetched).

## Acceptance criteria

Increment 4 is complete when all hold simultaneously:

1. `scripts/run_tests.sh` on a fresh clone exits 0 — increments 1–3 suites unchanged, and every increment-4 assertion in "Test plan" passes, including under injected latency and loss.
2. The immediate-response assertion demonstrates prediction responds within 1–2 ticks regardless of injected one-way latency up to 100 ms.
3. The forced-misprediction test demonstrates the rewind-and-replay path actually runs (nonzero correction count) and that the aircraft's rigid-body state converges afterward rather than diverging.
4. A human has flown `PredictedAircraft` through an artificially-latent connection and confirmed input feels immediate with no jarring visual snaps on correction — documented as performed, not CI-gated.
5. The GitHub Actions workflow runs to completion successfully within budget (20 minutes is expected to remain sufficient — this increment adds no new full-duration wall-clock scenario beyond what increment 3 already runs; confirm during implementation and raise the budget only if measurement says so).
6. The README documents the prediction/reconciliation architecture and how to fly the client through a simulated-latency connection.

## Out of scope, explicitly deferred

Everything in increments 1–3's deferred lists, plus: multiple clients/aircraft and remote-entity interpolation, multi-core server scaling (increment 5), bit-perfect FCS-state reconciliation or JSBSim patching, hit detection/lag compensation (increment 9), jitter buffers or adaptive smoothing beyond a fixed blend, cross-hardware determinism guarantees.

## Open questions for the implementer

At the implementer's discretion; document the choice:

- The exact correction thresholds (0.5 m / 2°, above) and blend duration (~150 ms) are starting points; tune once real client/server mismatch data exists under the tested latency/loss conditions.
- The exact ring buffer depth (suggested: enough ticks for ~1 second, generously above any tested round trip — Appendix B shows this costs nothing worth economising).
- Whether to log correction events (magnitude, frequency) to a CSV for the same kind of post-hoc inspection increment 1–3's CSVs already support; recommended, not required.
- Whether `flight_test_client --mode prediction` forces its misprediction via an artificial initial offset or via `net_relay` loss injection, or exposes both as sub-modes.

Not open: whether the input-path/reconciliation test is required (it is, per increment 3's B2 precedent) and whether the server changes behaviour (it must not — see "Architecture").

---

## Appendix A: message and field reference (normative, updates to increment 3's)

`StateSnapshot` (tag 5), current full layout:

| Field | Wire type | Notes |
|---|---|---|
| `server_tick` | `uint32` | unchanged from increment 3 |
| `ack_client_seq` | `uint32` | **new**: server's most-recently-applied `ControlInput.client_seq` as of this tick |
| `aircraft_count` | `uint8` | unchanged |
| *(per aircraft)* `player_id` | `uint8` | unchanged |
| *(per aircraft)* `pos_local_m` | `float32[3]` | unchanged (E, U, −N) |
| *(per aircraft)* `quat` | `float32[4]` | unchanged (x, y, z, w) |
| *(per aircraft)* `vel_local_mps` | `float32[3]` | unchanged, local frame |
| *(per aircraft)* `ang_vel_local_rps` | `float32[3]` | **new**: body-frame roll/pitch/yaw rate, rad/s |
| *(per aircraft)* `status_flags` | `uint8` | unchanged |

`ControlInput` (tag 4): unchanged fields; send rate changes from 60 Hz to 120 Hz (see "Wire protocol changes").

JSBSim API surface this increment relies on beyond increment 3's usage: `FGPropagate::GetVState()`/`SetVState()` (already named in increment 3's Appendix A as a flagged risk, now the confirmed mechanism), `FGPropagate::GetTl2i()`, `FGPropagate::GetEarthPositionAngle()`, and the property-tree reads `velocities/p-rad_sec`, `velocities/q-rad_sec`, `velocities/r-rad_sec`.

## Appendix B: measured findings from pre-drafting validation (informative)

Measured on the same 4-core x86-64 container, Ubuntu 24.04, GCC 13.3.0, JSBSim v1.3.1, Godot 4.5-stable (custom build) used for increment 3's validation.

- **`SetVState()` restores rigid-body state exactly on an immediate round trip**: capturing `GetVState()` and immediately calling `SetVState()` with it, with zero ticks in between, reproduced position/velocity/attitude with max-abs-diff `0.0` across position (m), attitude (deg), velocity (m/s), and body rates (rad/s).
- **`SetVState()` does not restore FCS actuator state — confirmed directly, and the effect is large and immediate**: a 6-tick (50 ms) window with a step aileron input, replayed after `SetVState()` restored position/attitude/velocity to the pre-step snapshot, diverged from the original run within the *first* replayed tick. Direct inspection showed why: immediately after `SetVState()`, the aileron actuator's output property (`fcs/left-aileron-pos-rad`) was `0.049844` — not the pre-disturbance value, but *exactly* the original run's final-tick value. The actuator (private state, no public accessor, not bound to the property tree) was never touched by the restore. Divergence grew roughly linearly with replay-window length (worst-case metric, mixed units: ~3.9 at 1 tick, ~93 at 24 ticks/200 ms, ~360 at 60 ticks/500 ms) — consistent with an immediate actuator-position mismatch propagating forward, not with a slowly-compounding numerical artifact.
- **`GetVState()`/`SetVState()` cost is negligible**: `GetVState()` into a proper O(1) ring-buffer slot measured 0.84 µs/call mean over 200,000 calls (an earlier attempt using `std::vector::erase(begin())` for the "ring" measured ~39 µs/call — an O(n) vector-shift artifact of the wrong data structure, not the actual copy cost; corrected by using a fixed-size circular buffer). `SetVState()` alone measured 0.58 µs/call. `sizeof(VehicleState)` is 1496 bytes; a 300-slot (2.5 s at 120 Hz) ring buffer is under 500 KB. Both are immaterial next to `fdm.Run()`'s own per-tick cost (increment 2 measured ~57 µs mean; this session's cross-check on the same hardware measured ~12 µs mean, both far above the sub-microsecond snapshot cost either way).
- **GDExtension supports subclassing a project-defined GDExtension class**: a probe class `ChildProbe : public FlightAircraft`, registered alongside `FlightAircraft` itself, successfully called an *inherited* method (`initialize()`, `setInitialCondition()`, `trim()`, all defined on `FlightAircraft`) through the subclass, in a real headless Godot process, and returned correct values (`initOk=1 trimOk=1 altitude_m=1524.0`, i.e. 5000 ft correctly converted) alongside a method defined only on the subclass. This grounds `PredictedAircraft : public FlightAircraft` as a proven-workable pattern rather than an assumption.
- **The ECI-attitude-reconstruction building blocks exist and are public**: `FGPropagate::GetTl2i()` (local-to-ECI rotation matrix) and `GetEarthPositionAngle()` are both public methods, confirming the reconstruction step in "Client-side prediction algorithm" is implementable against JSBSim's real API rather than requiring a guess or a JSBSim patch. The exact composition was identified (`Tl2i.GetQuaternion() * qLocal`) but not yet round-trip-tested; doing so (encode a known local attitude, convert, feed through `SetVState()`, read back and compare) is the first implementation task, the same kind of cross-check increment 3 did for its own quaternion transport.
