# Increment 4 Specification: Client-Side Prediction and Server Reconciliation

## Status

Revision 2 — reviewed. Supersedes draft 1; the adversarial review that produced this revision is in `docs/increment-4-specification-review.md`, which justifies the changes below finding by finding. This is increment 4 of the derisking sequence (`docs/roadmap.md`), following a split: the roadmap originally combined "multiplayer scaling and feel" into one increment 4. The two concerns are independent — prediction/reconciliation needs only the single client and aircraft increment 3 already has, and multi-core scaling needs no prediction logic to test — so mixing them would blur the review's focus across two very different failure-mode domains (netcode correctness vs. concurrency/performance). This document covers "feel" only; scaling becomes increment 5. Builds on increment 3's client-server architecture: `netcore`'s wire protocol, `flight_server`'s authoritative loop, and the Godot `NetworkClient`/`RemoteAircraft`/`FlightAircraft` classes.

The following was verified by building and running real code before and during review (details and measured numbers in Appendix B):

- **JSBSim replay is bit-deterministic**: two independent `FGFDMExec` instances given identical initial condition, trim, and per-tick input produced max divergence **0.0** over 540 ticks. This is the premise the entire increment rests on — client and server, same binary, stay in lockstep, so any divergence the tests observe is a real misprediction or input-model artifact, never simulation noise.
- `FGPropagate::GetVState()`/`SetVState()` restores rigid-body state (position, velocity, attitude) with zero drift on an immediate snapshot/restore, and the cost of capturing a snapshot is negligible (well under 1 µs; a generous ring buffer is under 500 KB).
- `SetVState()` does **not** restore flight-control-system state: a rate-limited actuator (the c172x's aileron) is still at its pre-rewind position immediately after `SetVState()` returns — confirmed directly by inspecting the actuator's output property before and after.
- **The un-restored actuator divergence is NOT self-correcting** (review finding B1, correcting a false claim in draft 1): after a single mid-maneuver reconciliation, rigid-body position divergence *grows* monotonically (0.014 m at 0.5 s → 0.31 m at 1.5 s), because a transient bank difference leaves a permanent heading offset and heading is neutrally stable. What keeps it bounded in practice is the **continuous 30 Hz correction stream**, not any settling: the inter-snapshot (~33 ms) drift is only ~0.1° / sub-millimetre, re-corrected before it can grow.
- **The increment-3 "apply most recent input" server model desyncs under aggressive analog input + loss** (review finding B2): fine for keyboard and gentle analog (<0.01 m / 0.11° at 20% loss), but a full-amplitude fast stick waggle at 20% loss trips the reconciliation threshold on ~19% of snapshots from the input model alone — and a USB joystick, not the keyboard, is the expected controller. The fix (verified: zero divergence at 40% loss) is the actual Quake/Source mechanism — redundant recent commands per packet + an ordered server-side command buffer — so `flight_server` **does** change this increment.
- A new GDExtension class can cleanly subclass an existing project-defined GDExtension class (`FlightAircraft`), inheriting its bound methods and properties.
- The full wire-field → `VehicleState` → `SetVState()` reconstruction **round-trips correctly** (attitude to 6×10⁻⁶°, position to 0.1 mm, velocity to 0, through inverted flight and 90° pitch) using JSBSim's own frame composition — the review's reconstruction gate, passed during review; the exact verified recipe (and two non-obvious gotchas it exposed) is in "Reconstruction gate".

The design choice, made explicitly in response to the actuator-state finding: **do not chase bit-perfect reconciliation**. Rewind and replay only the rigid-body state (position, velocity, attitude, body rates) that the wire protocol carries; leave flight-control-system internals unrewound. This is standard practice in networked games — correct the state that hit detection and rendering depend on precisely, and let the continuous correction stream keep the residual (from unrewound actuators/filters) bounded. The residual is *not* self-cancelling — left uncorrected it drifts without bound (B1) — but each 30 Hz snapshot re-corrects it while it is still sub-degree, so it never accumulates. It is, deliberately, the boring choice: this project's economics favour mature, widely-used patterns over novel ones, and "rewind the rigid-body state, re-correct every snapshot, tolerate a bounded actuator-settle residual between snapshots" is exactly what the Quake-lineage client-prediction pattern already does.

## Goal

Close the "feel" gap increment 3 explicitly deferred: give the local player's own aircraft an immediate, latency-independent response to input by simulating it predictively on the client, while remaining reconciled with the server's authoritative outcome once corrections arrive. Prove this holds under realistic latency and loss (reusing increment 3's `net_relay`), not just on a clean localhost link — that is the actual point of this increment, since prediction that only works with zero latency proves nothing increment 3 didn't already prove.

## Non-goals

Explicitly out of scope; must not be implemented:

- Multiple concurrent clients or aircraft, and therefore interpolation/extrapolation of *other* entities' snapshots (increment 5 — with one client and one aircraft there is nothing else to interpolate; this increment's own aircraft is always predicted, never interpolated)
- Multi-core server parallelism, or any server-side performance work (increment 5)
- Bit-perfect reconciliation of JSBSim flight-control-system internals (actuators, filters, PIDs) — accepted as a residual divergence kept bounded by the continuous correction stream (see "Status" and Appendix B); only rigid-body state is rewound
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

`flight_server`'s input handling **changes** this increment — draft 1 kept increment 3's "apply the most recently received `ControlInput` each tick" model and called that a virtue; review finding B2 showed (by measurement) that model desyncs from the client's prediction under aggressive analog input plus loss, tripping reconciliation on ~1 snapshot in 5 at 20% loss *regardless of prediction quality*, because the server simulates a different input-per-tick sequence than the client predicted. Since a USB joystick is the expected controller and the increment's own resilience tests inject 20% loss, that is a real feel regression, not a corner case. The fix is the actual Quake/Source mechanism: the client sends **redundant** recent commands in every `ControlInput` packet, and the server consumes them as an **ordered command buffer**, applying each `client_seq` exactly once in order and holding the last command only across a genuine multi-packet gap. Verified to hold zero divergence from the client's prediction even at 40% loss (Appendix B). This keeps the server simple and still authoritative — it is not client-trusting, it just replays the client's committed input stream in order — and it makes the reconciliation comparison *exact* (server-state-at-`ack_seq` becomes bit-identical to client-buffered-state-at-`ack_seq`, since both apply each command for exactly one tick). Otherwise the server is as increment 3 built it, plus the two new outgoing `StateSnapshot` fields below.

```
 client, every physics tick (120 Hz):
   sample input -> apply to LOCAL FlightSession -> step -> buffer (client_seq, input, resulting VState)
   -> send ControlInput{ newest client_seq, last R commands } (unreliable)  -> display updates immediately

 server, ordered command buffer:
   for each tick: apply the buffered command for the next expected client_seq (hold last across a gap)
   -> step authoritative FlightSession
   -> broadcast StateSnapshot{..., ack_client_seq = highest seq applied} at 30 Hz (unreliable)

 client, on receiving a StateSnapshot:
   look up buffered state at ack_client_seq -> compare to the snapshot's authoritative state
   within tolerance -> discard old buffer entries, done
   beyond tolerance -> SetVState(reconstructed authoritative state)
                       -> replay every buffered input since ack_client_seq
                       -> new buffer tail becomes the corrected prediction
                       -> blend the RENDERED transform toward it over ~150ms (re-targeting if already blending)
```

## Wire protocol changes

Three changes to increment 3's protocol (`docs/increment-3-specification.md`, "Wire protocol"). None changes a message's *tag*, and none changes the per-aircraft *packetisation* shape that increment 3's m1 finding flagged as the thing likely to change at scale.

1. **`StateSnapshot` gains a top-level `ack_client_seq` (`uint32`)**, placed after `server_tick` and before `aircraft_count`. This is the highest `client_seq` the server has applied as of this tick — connection-scoped metadata, not a property of any aircraft, so it does not belong in the per-aircraft record (unlike `server_tick`, which is genuinely global, `ack_client_seq` is only meaningful to whichever single client owns the connection this snapshot is sent on). The server already tracks this value internally; this only wires it into the outgoing snapshot.
2. **Each `AircraftState` gains `ang_vel_body_rps` (`float32[3]`)**, **body-frame** roll/pitch/yaw rate in rad/s (JSBSim `p`,`q`,`r`), placed after `vel_local_mps`. The name says `body` deliberately (review finding M2): unlike the position and velocity fields, which are local East/Up/−North, this quantity is body-frame and must be fed straight to `VehicleState::vPQR` with **no** frame rotation. Reconstructing a `VehicleState` for `SetVState()` requires it; increment 3's snapshot never carried it (nothing consumed it). Deriving it is a direct property read (`velocities/p-rad_sec` etc.).
3. **`ControlInput` becomes a redundant multi-command packet** (review finding B2). Instead of one command, each packet carries the client's last **R** commands (R≈6, tunable — see "Open questions"): a `uint32 newest_client_seq`, a `uint8 count` (≤ R), then `count` command records `{int16 elevator, int16 aileron, int16 rudder, uint8 throttle}` for seqs `newest_client_seq, newest_client_seq−1, …` in descending order. The server applies each seq it has not yet applied, in ascending order, one per tick. A single dropped packet therefore loses no command (the next packet re-delivers it); only an R-packet-long gap causes a real hold. This is the Quake/Source redundant-usercmd pattern, and it is what makes the server's ordered command buffer robust to loss.

Field-width discipline carries over unchanged (little-endian, field-by-field, no struct `memcpy`). Per-aircraft `StateSnapshot` record grows from 42 to 54 bytes; the snapshot's total grows by 4 (top-level `ack_client_seq`) + 12 (per aircraft ×1) = 16 bytes — immaterial at one-aircraft scale.

**`ControlInput` send rate changes from increment 3's 60 Hz default to 120 Hz**, matching the simulation tick exactly, so there is one command per physics tick with a monotonically increasing `client_seq` and an unambiguous one-to-one mapping between "the input the client predicted tick N with" and "the input the server applied for that seq." Bandwidth impact is immaterial: a redundant `ControlInput` packet is ≈ 4 + 1 + R×7 ≈ 47 bytes at R=6 (plus ENet/UDP overhead), so 120/s is on the order of ~10–14 KB/s upstream per client — and it does not multiply with player count, since each client only ever sends its own input. The measured benefit (Appendix B) is large: the redundancy holds client and server in exact lockstep through 40% packet loss even under full-amplitude analog stick movement.

## Client-side prediction algorithm

Every physics tick (120 Hz, the same fixed-timestep pattern established in increment 2 and reused unchanged since):

1. Sample local input (same keyboard mapping as `networked_input.gd`, unchanged).
2. Increment `client_seq`.
3. Apply the input to the **local** `FlightSession` (the same one `FlightAircraft`'s inherited machinery already owns) and step it forward one tick. This is the prediction: the aircraft responds this tick, not after a round trip.
4. Push `(client_seq, input)` onto the input ring buffer and `(client_seq, session.GetVState())` onto the state ring buffer. Both are fixed-size circular buffers (Appendix B: negligible cost, ~1 second of ticks costs well under 500 KB, sized generously above any tested latency so it is not a tuning-sensitive parameter).
5. Send a `ControlInput` packet carrying the last R commands (this seq plus the previous R−1 from the input ring buffer), unreliable channel — the redundancy per "Wire protocol changes".
6. Update the rendered transform from the local prediction (subject to the blend described below).

On receiving a `StateSnapshot` (server's ~30 Hz rate, decoupled from the client's 120 Hz prediction rate exactly as increment 3 decoupled snapshot rate from simulation rate):

1. Read `ack_client_seq` and this client's own `AircraftState` entry (matched by `player_id`, same lookup `NetworkClient` already does).
2. Find the state-ring-buffer entry at `ack_client_seq`. If it has already fallen out of the buffer (only possible if the round trip exceeded the buffer's generous window), treat it as an unconditional correction and log a warning — this must degrade safely, not undefined-behaviour, though it should not happen under any latency/loss this increment tests (Appendix B's buffer-sizing measurement is the reason it shouldn't).
3. Convert that buffered prediction into the same local-frame-position + quaternion + body-frame-velocity representation the wire uses, by calling the *same* `geo::computeLocalOffset()`/`geo::computeOrientationQuat()` functions `flight_server` already calls to build its own outgoing snapshots (increment 3's `src/geo/`, reused unchanged) — this guarantees the client compares like with like, not two different derivations that could disagree for reasons unrelated to an actual misprediction.
4. Compare against the received authoritative fields. **Threshold** (starting point, to be tuned empirically during implementation exactly as increment 3 tuned its own thresholds once real data existed): position mismatch beyond 0.5 m in the local frame, or attitude mismatch beyond 2°, triggers a correction; either check passing does not. (Increment 3's own lesson applies here too: compare via a numerically well-conditioned metric — component-wise or a chord-based angle, not `acos(dot)` naively, which is ill-conditioned near small angles.)
5. If within threshold: discard buffer entries older than `ack_client_seq` (no longer needed) and continue predicting from where it already was.
6. If beyond threshold — **reconciliation**:
   a. Reconstruct a JSBSim `VehicleState` from the snapshot's authoritative fields **using the exact recipe in "Reconstruction gate" below** (verified during review — do not improvise the frame algebra): invert the local position offset to geodetic lat/lon (against the `ServerWelcome` origin) into a copy of the session's ellipsoid-configured `vLocation`; build `qAttitudeLocal` from the wire attitude and compose `qAttitudeECI = Ti2l.GetQuaternion() * qAttitudeLocal`; `vPQR` comes straight from `ang_vel_body_rps` (no rotation); body velocity is `qAttitudeLocal.GetT() * vNED`.
   b. `SetVState()` the local `FlightSession` to this reconstructed state.
   c. Replay: for every buffered input from `ack_client_seq + 1` through the latest locally-generated `client_seq`, in order, apply it and step the session forward one tick, overwriting that tick's state-ring-buffer entry with the newly recomputed value.
   d. The final replayed tick becomes the new current prediction. Record that a correction occurred and its magnitude (for the smoothing step below, and for test/telemetry visibility — see "Test plan").

## Reconstruction gate (PASSED during review — recipe is normative)

The reconciliation `VehicleState` reconstruction (step 6a) is the one place reconciliation can *corrupt* state rather than merely fail to help — a wrong local→ECI attitude composition `SetVState()`s the aircraft to a wrong orientation, worse than not reconciling. Review finding M2 made this a hard gate; it was then **passed empirically during review** (Appendix B, "reconstruction gate"): a wire-field → `VehicleState` → `SetVState()` → read-back round trip reproduced attitude to **6×10⁻⁶°**, position to **0.1 mm**, velocity to **0**, across attitudes up to inverted (159° roll) and through vertical (90° pitch), for both a native-quaternion and a Euler wire representation. The verified recipe below is **normative** — the implementation reproduces it, and the round trip becomes a regression test (acceptance criterion 1). Two non-obvious gotchas were found and are baked into the recipe; both would otherwise have cost real debugging:

1. **The composition is `qAttitudeECI = Ti2l.GetQuaternion() * qAttitudeLocal`** — `Ti2l` (ECI→local, `FGPropagate::GetTi2l()`), **not** `Tl2i` as draft 1 wrote. This is copied verbatim from JSBSim's own initialisation (`FGPropagate.cpp` `InitializeDerivatives`), which is the authoritative source for the frame convention; guessing the transpose gives a plausible-looking but wrong attitude.
2. **The reconstruction `FGLocation` must be built by copying the session's existing (ellipsoid-configured) `vLocation` and calling `SetPositionGeodetic()` on that copy** — *never* a bare `FGLocation`, whose default ellipsoid is degenerate and produces a latitude error that grows with displacement (~112 m at 0.001° in the probe, while longitude and altitude stay exact — a nasty, latitude-only, silently-scaling bug).

Recipe (all JSBSim-native math; no Godot, no hand-rolled ECEF/ECI trig):

```
auto P = fdm.GetPropagate();                                   // shared_ptr
FGLocation loc = P->GetVState().vLocation;                     // carries the Earth ellipsoid
loc.SetPositionGeodetic(lon_rad, lat_geod_rad, h_sl_ft);
P->SetLocation(loc);                                           // updates location matrices + inertial pos
FGQuaternion qLocal = /* from wire: euler ctor OR component-assign */;  qLocal.Normalize();
FGQuaternion qECI = P->GetTi2l().GetQuaternion() * qLocal;     // gotcha 1
FGColumnVector3 vUVW = qLocal.GetT() * vNED_ftps;              // Tl2b = qLocal.GetT()
auto vs = P->GetVState();                                      // correct vLocation/vInertialPosition/epa
vs.qAttitudeECI = qECI; vs.vUVW = vUVW; vs.vPQR = {p,q,r};
P->SetVState(vs);
```

`epa` (Earth Position Angle) need not be matched to the server's exactly: `Ti2l` (building `qECI`) and JSBSim's internal `Tl2i` (deriving `qAttitudeLocal` back) use the *same* client-side `epa` and are inverses, so it cancels for the local-attitude round trip — verified. (The `FGQuaternion(q1,q2,q3,q4)` 4-component constructor is private; build a native-quaternion `qLocal` via the default constructor plus `operator()(idx)` component assignment, or use the public Euler constructor `FGQuaternion(phi,tht,psi)`.)

## Smooth error correction (visual only)

The *simulated* state corrects immediately (subsequent ticks must build on the right answer). The *rendered* transform does not snap: on a correction, blend the displayed `Transform3D` from wherever it was toward the newly-corrected prediction over a short, fixed window (starting point: ~150 ms, i.e. roughly 18 physics ticks at 120 Hz — tunable, not load-bearing; the exact constant is an implementer's-discretion detail, not an architectural one). This is display-layer only: it must not feed back into the simulated state, or it would reintroduce exactly the kind of state/display divergence this whole increment exists to close. **Corrections can arrive faster than a blend completes** (under aggressive analog or real jitter, potentially every snapshot); an in-progress blend must **re-target** from its current interpolated pose toward the new correction, never restart from a stale anchor or stack blends, or the smoothing itself judders (review finding m2).

## Test plan

`flight_test_client --mode prediction` runs a real `PredictedSession` (via `predictcore`) against a live `flight_server`, optionally through `net_relay` for latency/loss (reusing increment 3's relay unchanged). Required assertions:

A note the whole plan turns on (review finding M1): because JSBSim replay is bit-deterministic (Appendix B) and, for benign input, prediction already matches the server, **under clean/keyboard/gentle conditions the client's free-running prediction agrees with the server whether or not reconciliation runs at all**. So tests 1 and 2 below would pass with the reconciliation path entirely disabled — they are necessary but *cannot* validate reconciliation. Reconciliation is validated only by test 3, which is therefore specified with a negative control. This is not a weakness to paper over: organic reconciliation is genuinely rare at this increment's one-client, mostly-keyboard scale, and the machinery earns its keep under the real jitter, aggressive analog, and multiple clients of later use — so its coverage rests deliberately on fault injection, and that test must be airtight.

1. **Immediate response (under latency)**: through `net_relay` at ≥100 ms one-way delay, after commanding a step input change, the *local predicted* state (not the last snapshot received) shows the expected directional change within 1–2 physics ticks. The latency is essential to the test: on a clean localhost link even a non-predicting client updates within a few ticks (snapshots arrive every ~4 ticks at 30 Hz), so only under injected delay does "responds immediately" distinguish prediction from its absence (review finding m1). Uses a directly-observable, non-derived signal (attitude/altitude trend from the raw predicted `FlightSample`), like increment 3's input-path test.
2. **Eventual agreement with the server's own ground truth**: once enough wall-clock time has passed for the server's authoritative response to settle (accounting for injected latency), the local prediction's trajectory matches what `flight_server`'s scripted-mode evaluation of the identical input schedule produces (reusing increment 3's server-side ground-truth machinery — validate against the server's authoritative computation, not a self-consistency check the client could pass while wrong). Necessary but not sufficient (see the note above).
3. **Reconciliation produces the *right* state — with a negative control** (the load-bearing reconciliation test): force a real misprediction (an artificial state offset, or a genuine input change dropped for longer than the R-command redundancy via `net_relay`) and assert both (a) the correction-count telemetry is nonzero *and* (b) the corrected rigid-body state matches server ground truth within tolerance. Then assert the **negative control**: the identical scenario with reconciliation disabled **fails** (b). A nonzero count alone proves only that code ran; the negative control is what proves the code is what fixes the state (mirroring and strengthening increment 3's B2 lesson).
4. **Bounded tracking under the correction stream, not single-correction convergence** (corrected per review finding B1): a single correction followed by no further corrections does *not* converge — position drifts without bound as the un-restored actuator leaves a permanent heading offset (Appendix B). So this asserts the property the design actually has: across a **sustained maneuver under the continuous 30 Hz snapshot stream**, the client's tracking error against server ground truth stays within a bounded envelope (a small multiple of the reconciliation threshold), never growing run-away. Draft 1's "assert a single correction converges" would have failed against real physics.
5. **Aggressive-analog resilience** (new, per review finding B2): run tests 1–4 with a full-amplitude fast analog input schedule (not only bang-bang) through `net_relay` at 20% loss, and assert the redundant-command-buffer path keeps client/server tracking within the same bounded envelope — i.e. that the server-input-model desync draft 1 would have had is actually gone. This is the test that would have caught B2, and it targets the controller the game will actually use.
6. Repeat the relevant tests through `net_relay` at the latency/loss values increment 3 validated (100 ms one-way; 20% drop).
7. Increment 1–3 suites continue to pass unchanged.

The Godot client (`PredictedAircraft` in `networked.tscn`) remains a manual verification criterion, consistent with increments 2 and 3: a human flies it under an artificially-latent connection (`net_relay` again) and confirms input feels immediate and corrections are not visually jarring — not CI-gated, documented as performed. This manual criterion is load-bearing for the "feel" half: the automated suite proves state correctness and bounded tracking, not that corrections *look* good.

## Documentation

New C++/GDScript files carry the GPL-3.0 header convention, matching every prior increment. Design decisions with non-obvious rationale — the ECI-quaternion reconstruction, the accepted actuator-state gap, the ring-buffer sizing, the correction threshold — get inline comments pointing back to this spec, following the pattern already established in `flight_aircraft.cpp` and `flight_server`.

## Licence

Unchanged: GPL-3.0-or-later. No new third-party dependencies (this increment adds no new library, only new code using JSBSim and ENet, both already fetched).

## Acceptance criteria

Increment 4 is complete when all hold simultaneously:

1. The reconstruction gate — passed during review (Appendix B) — is reproduced in code as a regression test (the wire-field → `SetVState()` → read-back round trip agrees within tolerance), using the normative recipe so the frame algebra can never silently regress.
2. `scripts/run_tests.sh` on a fresh clone exits 0 — increments 1–3 suites unchanged, and every increment-4 assertion in "Test plan" passes, including the aggressive-analog-under-loss test and the reconciliation negative control, under injected latency and loss.
3. The immediate-response assertion demonstrates prediction responds within 1–2 ticks under injected one-way latency of ≥100 ms.
4. The forced-misprediction test demonstrates both that the rewind-and-replay path runs *and* produces the correct corrected state, and that the negative control (reconciliation disabled) fails — so the test actually validates reconciliation, not merely that code executed. Tracking under the continuous correction stream stays within a bounded envelope across a sustained maneuver.
5. A human has flown `PredictedAircraft` through an artificially-latent connection and confirmed input feels immediate with no jarring visual snaps on correction — documented as performed, not CI-gated.
6. The GitHub Actions workflow runs to completion successfully within budget (20 minutes is expected to remain sufficient — this increment adds no new full-duration wall-clock scenario beyond what increment 3 already runs; confirm during implementation and raise the budget only if measurement says so).
7. The README documents the prediction/reconciliation architecture and how to fly the client through a simulated-latency connection.

## Out of scope, explicitly deferred

Everything in increments 1–3's deferred lists, plus: multiple clients/aircraft and remote-entity interpolation, multi-core server scaling (increment 5), bit-perfect FCS-state reconciliation or JSBSim patching, hit detection/lag compensation (increment 9), jitter buffers or adaptive smoothing beyond a fixed blend, cross-hardware determinism guarantees.

## Open questions for the implementer

At the implementer's discretion; document the choice:

- The exact correction thresholds (0.5 m / 2°, above) and blend duration (~150 ms) are starting points; tune once real client/server mismatch data exists under the tested latency/loss conditions. (Appendix B confirms the 0.5 m / 2° thresholds sit an order of magnitude above the accepted actuator residual's inter-snapshot drift, so they will not false-trigger on it.)
- The command redundancy depth **R** (suggested 6): it must exceed the longest consecutive-packet-loss run the increment tests care about; at R=6 a command survives unless all 6 carrying packets drop (0.4⁶ ≈ 0.4% at 40% loss). Larger R costs a few bytes per packet; smaller R risks real gaps under bursty loss.
- The exact ring buffer depth (suggested: enough ticks for ~1 second, generously above any tested round trip — Appendix B shows this costs nothing worth economising).
- Whether to log correction events (magnitude, frequency) to a CSV for the same kind of post-hoc inspection increment 1–3's CSVs already support; recommended, not required.
- Whether `flight_test_client --mode prediction` forces its misprediction via an artificial state offset or via `net_relay` loss injection, or exposes both as sub-modes.

Not open (settled by review): the reconciliation test is required and needs a negative control (M1); the server **does** change to an ordered command buffer with redundant input (B2); the reconstruction gate must be passed before implementation proceeds (M2); test 4 asserts bounded-envelope tracking, not single-correction convergence (B1).

---

## Appendix A: message and field reference (normative, updates to increment 3's)

`StateSnapshot` (tag 5), current full layout:

| Field | Wire type | Notes |
|---|---|---|
| `server_tick` | `uint32` | unchanged from increment 3 |
| `ack_client_seq` | `uint32` | **new**: highest `client_seq` the server has applied as of this tick |
| `aircraft_count` | `uint8` | unchanged |
| *(per aircraft)* `player_id` | `uint8` | unchanged |
| *(per aircraft)* `pos_local_m` | `float32[3]` | unchanged (E, U, −N) |
| *(per aircraft)* `quat` | `float32[4]` | **semantics change**: carry JSBSim's native `qAttitudeLocal` (body→NED) rather than increment 3's Godot-convention quaternion — see note below |
| *(per aircraft)* `vel_local_mps` | `float32[3]` | unchanged, local frame (E, U, −N) |
| *(per aircraft)* `ang_vel_body_rps` | `float32[3]` | **new**: **body**-frame roll/pitch/yaw rate (JSBSim `p`,`q`,`r`), rad/s — fed to `vPQR` with no rotation |
| *(per aircraft)* `status_flags` | `uint8` | unchanged |

Attitude representation note (settled by the reconstruction gate, Appendix B): the `quat` field carries JSBSim's **native `qAttitudeLocal`** (body→NED), which the server has directly, rather than increment 3's Godot-convention quaternion. This makes reconciliation reconstruction trivial and gimbal-safe (the native quaternion feeds straight into the recipe), and moves the one axis-convention conversion to the **display** side (the `RemoteAircraft`/`PredictedAircraft` node converts native→Godot for rendering) — where a mistake is a visible cosmetic glitch, not silent state corruption. Euler angles were also verified to reconstruct perfectly (including at 90° pitch) and would let the display reuse `geo::computeOrientationQuat()` directly; the native quaternion is preferred for gimbal-safety and because it keeps the wire field a quaternion as in increment 3.

`ControlInput` (tag 4), **redundant multi-command** layout (see "Wire protocol changes"):

| Field | Wire type | Notes |
|---|---|---|
| `newest_client_seq` | `uint32` | seq of the most recent command in this packet |
| `count` | `uint8` | number of command records that follow (≤ R) |
| *(per command, ×count)* `elevator` | `int16` | seqs `newest, newest−1, …` in descending order |
| *(per command)* `aileron` | `int16` | |
| *(per command)* `rudder` | `int16` | |
| *(per command)* `throttle` | `uint8` | |

Send rate 120 Hz (one command generated per tick; each packet re-sends the last R). Fixed-point encodings unchanged from increment 3 (`encodeAxis`/`encodeThrottle`).

JSBSim API surface this increment relies on beyond increment 3's usage (all verified in the reconstruction gate): `FGPropagate::GetVState()`/`SetVState()`, `FGPropagate::GetTi2l()`, `FGPropagate::SetLocation()`, `FGLocation::SetPositionGeodetic()` (called on a copy of the session's `vLocation`), `FGQuaternion::GetT()`, and the property-tree reads `velocities/p-rad_sec`, `velocities/q-rad_sec`, `velocities/r-rad_sec`.

## Appendix B: measured findings from pre-drafting and review validation (informative)

Measured on the same 4-core x86-64 container, Ubuntu 24.04, GCC 13.3.0, JSBSim v1.3.1, Godot 4.5-stable (custom build) used for increment 3's validation. The first four bullets are pre-draft; the "review experiments" block was added during the adversarial review and is what corrected the self-correction claim (B1) and the server-input-model claim (B2).

- **`SetVState()` restores rigid-body state exactly on an immediate round trip**: capturing `GetVState()` and immediately calling `SetVState()` with it, with zero ticks in between, reproduced position/velocity/attitude with max-abs-diff `0.0` across position (m), attitude (deg), velocity (m/s), and body rates (rad/s).
- **`SetVState()` does not restore FCS actuator state — confirmed directly, and the effect is large and immediate**: a 6-tick (50 ms) window with a step aileron input, replayed after `SetVState()` restored position/attitude/velocity to the pre-step snapshot, diverged from the original run within the *first* replayed tick. Direct inspection showed why: immediately after `SetVState()`, the aileron actuator's output property (`fcs/left-aileron-pos-rad`) was `0.049844` — not the pre-disturbance value, but *exactly* the original run's final-tick value. The actuator (private state, no public accessor, not bound to the property tree) was never touched by the restore. Divergence grew roughly linearly with replay-window length (worst-case metric, mixed units: ~3.9 at 1 tick, ~93 at 24 ticks/200 ms, ~360 at 60 ticks/500 ms) — consistent with an immediate actuator-position mismatch propagating forward, not with a slowly-compounding numerical artifact.
- **`GetVState()`/`SetVState()` cost is negligible**: `GetVState()` into a proper O(1) ring-buffer slot measured 0.84 µs/call mean over 200,000 calls (an earlier attempt using `std::vector::erase(begin())` for the "ring" measured ~39 µs/call — an O(n) vector-shift artifact of the wrong data structure, not the actual copy cost; corrected by using a fixed-size circular buffer). `SetVState()` alone measured 0.58 µs/call. `sizeof(VehicleState)` is 1496 bytes; a 300-slot (2.5 s at 120 Hz) ring buffer is under 500 KB. Both are immaterial next to `fdm.Run()`'s own per-tick cost (increment 2 measured ~57 µs mean; this session's cross-check on the same hardware measured ~12 µs mean, both far above the sub-microsecond snapshot cost either way).
- **GDExtension supports subclassing a project-defined GDExtension class**: a probe class `ChildProbe : public FlightAircraft`, registered alongside `FlightAircraft` itself, successfully called an *inherited* method (`initialize()`, `setInitialCondition()`, `trim()`, all defined on `FlightAircraft`) through the subclass, in a real headless Godot process, and returned correct values (`initOk=1 trimOk=1 altitude_m=1524.0`, i.e. 5000 ft correctly converted) alongside a method defined only on the subclass. This grounds `PredictedAircraft : public FlightAircraft` as a proven-workable pattern rather than an assumption.
- **The ECI-attitude-reconstruction building blocks exist and are public** (pre-draft): `FGPropagate::GetTi2l()`, `SetLocation()`, `FGLocation::SetPositionGeodetic()`, `FGQuaternion::GetT()`. The exact composition was initially *guessed* as `Tl2i * qLocal`; the review's reconstruction-gate experiment (next block) corrected it to `Ti2l * qLocal` and verified the full round trip — see "Reconstruction gate" for the normative recipe.

### Review experiments (added during adversarial review)

- **Determinism (premise of the whole increment)**: two independent `FGFDMExec` instances, run sequentially (matching the separate-process client/server reality), given identical IC + trim + per-tick input schedule, produced **max position diff 0.0 m and max attitude diff 0.0° over 540 ticks**. JSBSim replay is bit-deterministic on one binary, so client and server stay in lockstep given the same input stream — any observed divergence is a real misprediction or input-model artifact, never simulation noise.
- **Actuator divergence is NOT self-correcting (corrects draft 1's central claim, review B1)**: with a client put into the realistic post-reconciliation state (rigid body `SetVState()`'d to server truth mid-maneuver, actuator left mispredicted) and then driven with input *identical* to the server's, rigid-body position divergence **grew monotonically**: 0.0004 m at 50 ms, 0.014 m at 500 ms, 0.116 m at 1000 ms, 0.31 m at 1500 ms. Attitude divergence plateaued near ~2.5° — a *persistent* offset, not a decaying one: a transient bank difference during the actuator's re-settle imparts a permanent heading change (heading is neutrally stable), and two aircraft on ~2.5° different headings separate in position without bound. The design is fine only because the **inter-snapshot (~33 ms) drift is tiny** — ~0.1° / sub-mm — so the continuous 30 Hz correction stream re-corrects it before it grows; nothing self-settles. This also confirms the 0.5 m / 2° thresholds sit an order of magnitude above the inter-snapshot residual, so they fire only on genuine mispredictions.
- **Server "most recent input" model desyncs under aggressive analog + loss (review B2)**: feeding a command-buffer server (client's exact one-per-tick sequence = the client's own prediction, by determinism) and a most-recent+loss server the same input stream, with no reconciliation, measured pure input-model divergence. Keyboard/bang-bang and gentle analog: negligible (<0.01 m / 0.11° at 20% loss, because holding a constant value across a drop is correct). **Full-amplitude 2 Hz analog stick waggle** (the expected USB-joystick case): **17 of 90 snapshots exceeded the 2° reconciliation threshold at 20% loss; ~half at 40%** — the client would reconcile on ~1 snapshot in 5 regardless of prediction quality. My initial hypothesis that the model was broken was first *dissolved* by the keyboard result, then *revived* by the analog result — the reason to measure rather than argue.
- **Redundant command-buffer fix (review B2 resolution)**: the Quake/Source pattern — each `ControlInput` packet carries the last R=6 commands, server applies each seq once in order — held **zero divergence from the client's prediction even at 40% loss** under the same aggressive analog input (a command is lost only if all 6 carrying packets drop, ≈0.4%). This is what makes the ordered-command-buffer server robust and the reconciliation comparison exact.
- **Reconstruction gate PASSED (review M2 retired)**: a wire-field → `VehicleState` → `SetVState()` → read-back round trip, float32-transported to mimic the wire, reconstructed **attitude to worst 6×10⁻⁶°, position to 0.1 mm, velocity to 0**, over 18 samples of a flown trajectory reaching 159° roll (inverted) and 57° pitch; a separate representation test held to ~2×10⁻⁶° through 88°/89.5°/89.9°/90° pitch for both Euler and native-quaternion wire forms (no gimbal degradation, because the ambiguity at 90° is in *decomposing* an orientation, not in representing it). The composition is JSBSim's own `qAttitudeECI = Ti2l * qAttitudeLocal` (the draft's `Tl2i` was the wrong transpose). Two gotchas found: the `Ti2l`-vs-`Tl2i` transpose, and that the reconstruction `FGLocation` must be a copy of the session's ellipsoid-configured `vLocation` (a bare `FGLocation` gave a latitude-only error growing to ~112 m at 0.001°). The full recipe is in "Reconstruction gate".

### Implementation experiments (added while implementing; not part of the reviewed draft)

- **Display-side native-quat-to-Godot conversion verified (retires the review addendum's residual risk 3)**: the wire `quat` field's native `qAttitudeLocal` can be converted directly to Godot's body-axes convention — via the rows of `qLocal.GetT()` (Local-to-Body), each remapped through the same fixed NED→Godot axis permutation `computeBodyAxes()` already applies — with **zero Euler decomposition anywhere**. Verified two ways: using real `FGQuaternion`/`FGMatrix33`, and as pure double-precision arithmetic (JSBSim's own quaternion→matrix formula, Stevens & Lewis Eqn. 1.3-32, transcribed with no JSBSim types). Both reproduced the shipped Euler-angle path's result to **0.0 worst-case difference** across a full aggressive pull-up-plus-roll sweep. The pure-math version ships as `geo::computeBodyAxesFromQuat()`, keeping `geo::` JSBSim-free.
- **The server flies unconstrained from its own process start, independent of any client connection (unchanged since increment 3) — this has a real, previously-untested consequence for a *predicting* client**: because a fresh client both trims independently and can take a non-negligible amount of *wall-clock* time to do so (the trim solver's own convergence time, on top of ordinary connect/handshake latency), the server's aircraft can already be well away from the origin — hundreds of metres, in one measured run — by the time the client's first few commands are even applied. Reconciliation handles this correctly (it is, after all, just a large divergence like any other): the very first correction snaps the client into sync, and tracking is sub-metre from then on (confirmed by direct before/after comparison of logged trajectories). The practical consequence is for testing, not for the design: comparing a live networked run's *absolute position* against a freshly-started standalone ground-truth run (e.g. `flight_server --scenario pitch_response`) conflates this one-time bootstrap offset with genuine tracking error. Altitude and attitude are unaffected (they depend on the control history, not on how long the server flew before any client existed) and were used for that comparison instead.
- **Matching a scripted ground-truth schedule exactly requires matching *every* axis trim leaves non-zero, not just the one the schedule appears to touch**: `pitch_response.cpp` only ever writes `fcs/elevator-cmd-norm`, leaving aileron, rudder, and throttle at whatever trim converged to (measured for this IC: aileron ≈ −0.075, rudder ≈ −0.004, throttle ≈ 0.79 — not negligible). A client that sends an explicit `ControlCommand` every tick (as this increment's design requires) must therefore populate those three axes with the *trimmed* values too, not zero/full-scale defaults; doing otherwise measurably diverges from the ground truth within a couple of seconds even though determinism and reconciliation are both working correctly. `trim()` itself was confirmed bit-identical across separate process invocations (not just within one process, which is what the premise above had actually verified), so this was the only real discrepancy once found.
- **End-to-end validation, real implementation (not a probe), one client against a real `flight_server`**: clean link, step schedule — 2 corrections, worst tracking 1.4 m / 0.9°, all test-plan assertions pass. Aggressive-analog schedule at 20% loss (test 5) — 21 corrections (more frequent, as expected under loss), worst tracking 0.9 m / 2.4°, still within the bounded envelope. Forced deterministic misprediction (an 11 m position offset the server never sees) with reconciliation on — recovers, one large correction then normal tracking; with reconciliation off — persists for the rest of the run (measured worst error in the hundreds of metres, compounding with the bootstrap offset above), confirming the negative control fails as required. 100 ms one-way latency — immediate response, eventual agreement, and bounded tracking all still hold.
