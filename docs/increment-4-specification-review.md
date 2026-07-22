# Increment 4 Specification — Review Record

Date: 2026-07-22. Reviewed: draft 1 of `increment-4-specification.md`. Result: revision 2 of the specification (same file), which this document justifies finding by finding.

This is an adversarial review in the spirit of the increment-1 and increment-3 reviews: the goal is to find what is *wrong* with the draft, especially defects that would let a correct implementation pass its own tests while the thing the increment exists to prove — playable *feel* under real latency and loss, with the controller the game will actually use — is not achieved. Increment 3's review hunted for serialization/false-green/impairment-layer defects; this one hunts for the failure modes specific to client-side prediction: unverified determinism assumptions, a reconciliation loop whose tests can't tell it apart from doing nothing, a server input model that desyncs under the target controller, and frame-conversion bugs in state reconstruction.

## Method

Unlike a pure reasoning review, the load-bearing claims here were **checked by building and running real code against the same JSBSim v1.3.1 the project already fetches**, because they are empirical (does JSBSim replay deterministically? does the accepted actuator gap actually self-correct? does the server input model hold up under analog input and loss?). Four standalone probes were built and run; their numbers appear inline below and are collected in the revised spec's Appendix B. Two of the draft's claims were *refuted* by experiment, one hypothesised defect was *dissolved* by experiment and then partially *revived* by a sharper test, and several were confirmed sound — which is the point of insisting on measurement over argument.

## Findings

Severity: **blocking** = a correct implementation of the draft could pass its own tests while the increment's actual goal (good feel under latency/loss with the real controller) is unmet, or the draft asserts something demonstrably false that the design rests on; **major** = would send the implementer down a wrong path or leave a real risk untested; **minor** = clarity/precision.

### B1 (blocking) — The "bounded and self-correcting" divergence claim is false, and the convergence test built on it would fail

Draft 1's central justification for accepting JSBSim's un-restored flight-control-system state is that the resulting divergence is *"bounded, self-correcting"* — *"let secondary/filtered systems settle over the next few ticks, since their error is bounded by how far they could have moved in the rewound window and decays as consistent input continues to arrive."* Test 4 codifies this: *"after a forced correction, assert the aircraft's rigid-body state converges to within tolerance of ground truth within a bounded number of subsequent ticks."*

**Measured, this is wrong.** Experiment (Appendix B, "actuator self-correction"): put a client mid-maneuver into the realistic reconciliation state — rigid body `SetVState()`'d to server truth, actuator left at its mispredicted position — then drive it and the server with *identical* input going forward and measure rigid-body divergence:

| time after single correction | position divergence | attitude divergence |
|---|---|---|
| 50 ms | 0.0004 m | 0.17° |
| 500 ms | 0.014 m | 2.5° |
| 1000 ms | 0.116 m | 2.5° |
| 1500 ms | 0.31 m | 2.3° |

The position error does **not** decay — it grows monotonically and without bound. The reason is physical, not numerical: a transient bank-angle difference during the actuator's re-settle turns the aircraft slightly, leaving a **permanent heading offset** (heading is neutrally stable — nothing restores an aircraft to a previous heading), and two aircraft on ~2.5° different headings separate in position forever. "Self-correcting" describes the opposite of what happens.

The design is nonetheless fine — but for a different reason than the draft gives, and that difference matters for the test. What actually keeps the error bounded is the **continuous 30 Hz correction stream**: the *inter-snapshot* drift (over ~33 ms) is only ~0.1° / sub-millimetre, so each fresh snapshot re-corrects the tiny accumulated drift long before it grows. The system is bounded by *re-correction frequency*, not by *settling*.

Consequences, both of which must be fixed:
1. The rationale (Status section, and the actuator paragraph) must be rewritten to state the real mechanism. Claiming self-correction will mislead whoever tunes thresholds or debugs a "why does it keep drifting" report later.
2. **Test 4 as written would fail** (or would only "pass" with a tolerance so loose and a window so short it tests nothing). A single correction followed by no further corrections does not converge. The test must instead assert that **under the continuous snapshot stream, the client's tracking error against server ground truth stays within a bounded envelope** — never exceeding some multiple of the reconciliation threshold — across a sustained maneuver. That is the property the design actually has and the one that matters.

### B2 (blocking) — The server's "most recent input" model is kept unchanged and called a virtue, but it desyncs under the controller the game targets

Draft 1 keeps `flight_server` *"unchanged in behaviour … all of this increment's complexity lives client-side … the smallest possible diff,"* retaining increment 3's "apply the most-recently-received `ControlInput` each tick" model. My first instinct was that this is a blocking flaw; the honest thing was to measure it, and measurement first **dissolved** the concern, then a sharper test **revived** it.

Experiment (Appendix B, "server input model"): feed a "command-buffer" server (applies the client's inputs one-per-tick in order — which, by the verified determinism result below, *is* the client's own prediction) and a "most-recent + packet-loss" server the same input stream, and compare their trajectories with **no reconciliation**, so the divergence is purely the input model's fault.

- **Keyboard / gentle analog input: fine.** Piecewise-constant (bang-bang) input, or a slow smooth stick, at up to 20% loss: worst divergence < 0.01 m / 0.11°, zero threshold exceedances. Holding a dropped value across a gap is *correct* when the value isn't changing, and at a 120 Hz send rate a gentle stick barely changes between packets. This is why the concern first looked baseless.
- **Aggressive analog input: not fine.** A full-amplitude 2 Hz stick waggle (about as fast as a human moves a stick) — which is *the expected typical case*, since a USB joystick/gamepad, not the keyboard, will be the normal controller — at 20% loss produces **17 of 90 snapshots exceeding the 2° reconciliation threshold** from the input model alone; at 40% loss, ~half. That is the client reconciling on roughly one snapshot in five *no matter how perfect its prediction is*, purely because the server simulated a different input-per-tick sequence than the client predicted.

Because the increment's automated tests will, if written like increment 3's, drive **scripted bang-bang input**, they would pass green while the input model silently fails for the analog controller the game is built around — a false green of exactly the kind this review series exists to catch.

**Resolution — adopt the actual Quake/Source mechanism, which draft 1 under-described.** Those engines do not merely "apply the latest input"; the client sends **redundant** recent commands in every packet and the server consumes them as an **ordered command buffer**, applying each sequence exactly once, in order, holding only across a genuine multi-packet gap. Verified (Appendix B, "command buffer fix"): with 6-deep redundancy, the command-buffer server holds **zero** divergence from the client's prediction even at **40% loss** with aggressive analog input (where the most-recent model exceeded threshold on ~half of snapshots), because a command is only truly lost if all 6 packets carrying it drop (0.4⁶ ≈ 0.4%). This also makes the reconciliation comparison *exact* — server-state-at-`ack_seq` becomes bit-identical to client-buffered-state-at-`ack_seq`, since both applied each command for exactly one tick.

So `flight_server` **does** change this increment: `ControlInput` carries the last *R* commands (R≈6, tunable), and the server maintains a small ordered input buffer keyed by `client_seq`. The cost is a larger `ControlInput` packet (≈46 B vs 12 B — still trivial and still non-multiplying across players). The draft's "server unchanged is a virtue" framing is affirmatively wrong for the target controller and must be replaced. (If the user chooses to keep increment 4 keyboard-only and defer analog robustness, the spec must say so explicitly and the tests must not claim analog resilience — but given the stated intent to use joysticks, the review recommends doing the command-buffer change now, as it is cheap and is the mature standard the project's economics favour.)

### M1 (major) — Under this increment's own test conditions reconciliation almost never fires, so the suite validates it through a single synthetic fault-injection test — which must therefore be airtight

Two verified facts combine into a coverage gap. First, JSBSim replay is **bit-deterministic**: two independent instances, identical IC + trim + per-tick input, ran to a max divergence of **0.0** over 540 ticks (Appendix B, "determinism"). Second, for bang-bang input, holding across dropped packets is harmless (B2). Together they mean that under clean localhost, under constant-delay latency, and under loss-with-keyboard-input, the client's *free-running prediction already matches the server ground truth* — so **tests 1 and 2 would pass with the entire reconciliation path disabled**. Determinism, the thing that makes prediction possible, also makes prediction indistinguishable from prediction-plus-reconciliation whenever conditions are benign.

That leaves **test 3 (forced misprediction) as the only test that exercises reconciliation at all**, and draft 1 specifies it only loosely ("assert the correction-count telemetry is nonzero"). A nonzero count proves the code path ran, not that it produced the right answer. Resolution:
- Harden test 3: after forcing a real divergence (artificial offset or a dropped genuine input change), assert the corrected rigid-body state **matches server ground truth within tolerance**, *and* include a negative control — the same scenario with reconciliation disabled must **fail** the assertion. A test that passes whether or not the feature works is not a test of the feature.
- State plainly in the spec that organic reconciliation is rare at this increment's scale and inputs, so reconciliation coverage rests on deliberate fault injection — this is acceptable (the machinery earns its keep under real internet jitter, aggressive analog, and the multiple clients of increment 5), but it must be named, not left implicit.

### M2 (major) — The angular-rate wire field is body-frame but named "local", and the whole VState reconstruction is unverified

Two problems in the state-reconstruction path, which is the one place reconciliation can actively *corrupt* state rather than merely fail to help:

1. **Frame-naming bug waiting to happen.** Draft 1 adds `ang_vel_local_rps` and groups it "after `vel_local_mps`". But JSBSim's `vPQR` — the thing `SetVState()` needs and the thing `velocities/p-rad_sec` etc. report — is **body-frame** angular rate, not local/world frame. Naming a body-frame quantity `_local_` and sitting it next to a genuinely local-frame velocity invites an implementer to apply a frame rotation that must not be applied (or to skip one that must). Rename to `ang_vel_body_rps` and note explicitly: body-frame, no rotation on the wire, fed straight to `vPQR`.
2. **The reconstruction is unverified and non-trivial.** Draft 1 itself flags the ECI-attitude reconstruction (`Tl2i.GetQuaternion() * qLocal`) as *"not yet round-trip-tested … the first implementation task."* Given that a wrong composition makes every reconciliation `SetVState()` to a wrong attitude — worse than not reconciling — this must be elevated from "first task" to a **required pre-implementation gate with an explicit pass criterion**: reconstruct a `VehicleState` purely from the wire-level fields (local-frame position/quaternion/velocity + body rates), `SetVState()` a fresh trimmed instance to it, read back Euler angles / position / velocity / body rates, and require agreement with the source within a stated tolerance (the float32 transport error floor, ~10⁻³°, is the natural bound). Implementation does not proceed past a failing gate. The building blocks were confirmed to exist and be public (`GetTl2i`, `GetEarthPositionAngle`, `SetInertialOrientation`), so this is verifiable up front.

### m1 (minor) — The immediate-response test is only meaningful under injected latency

Test 1 asserts the local prediction responds within 1–2 ticks. On a clean localhost link a *non-predicting* client also updates quickly (snapshots arrive every ~4 ticks at 30 Hz), so on localhost the test barely distinguishes prediction from its absence. It must be run **through `net_relay` at ≥100 ms** one-way delay, where a non-predicting client could not possibly respond within 1–2 ticks — that is the condition under which "responds immediately" actually means something. Note it explicitly so the test isn't written localhost-only.

### m2 (minor) — Overlapping correction blends must re-target, not restart

The ~150 ms rendered-transform blend, combined with corrections that (under aggressive analog or real jitter) can arrive every 33 ms, means blends will routinely overlap. The blend must re-target from the current interpolated pose toward the new correction, not restart from a stale anchor or stack — otherwise the visual smoothing itself judders. A one-line constraint, but worth stating so it isn't discovered as a bug.

### m3 (minor, verified helpful) — The 120 Hz `ControlInput` send-rate change is sound and doubly justified

Draft 1 raises the send rate from 60 to 120 Hz for reconciliation-alignment reasons. The experiments incidentally confirm a second benefit: at 120 Hz, consecutive packets differ so little that even the (soon-to-be-replaced) most-recent model tolerates gentle analog and all keyboard input under 20% loss. The change is correct; recorded here so it is not second-guessed as gratuitous.

## Verified as sound (no change)

- **JSBSim replay determinism** (the premise the whole increment rests on): bit-identical across independent instances given identical input. Client and server, same binary, stay in lockstep — so any divergence the tests see is a real misprediction or input-model artifact, never simulation noise.
- **`GetVState()`/`SetVState()` rigid-body restore** is exact on an immediate round trip, and snapshot cost is negligible (<1 µs; a generous ring buffer is well under 500 KB) — unchanged from the pre-draft validation.
- **GDExtension subclassing** (`PredictedAircraft : FlightAircraft`) works — unchanged from the pre-draft probe.
- **The 0.5 m / 2° reconciliation thresholds are well-chosen**: the inter-snapshot drift from the accepted actuator gap is ~0.1° / sub-millimetre (B1 data), an order of magnitude under the thresholds, so the thresholds will not false-trigger on the actuator residual — they fire only on genuine mispredictions. (They *will* fire on the B2 input-model desync under aggressive analog — which is why B2 fixes the cause rather than loosening the thresholds.)
- **`predictcore` shared between the Godot client and the test harness** — the same shared-code discipline increment 3 used for the wire protocol, and the only way the automated tests exercise the real client's reconciliation code.

## Implementation model recommendation

**Recommendation: Sonnet, in a session that can build and run**, unchanged from increments 1–3, *after* the revisions above are folded in. The B2 command-buffer change is a genuine netcode addition rather than transcription, but it is a thoroughly standard, well-documented pattern (redundant usercmds + in-order apply) now pinned down with measured parameters, not a research problem. The one place to escalate to a frontier model is if the M2 reconstruction gate reveals ECI/local frame subtleties that resist a clean round-trip — that is real coordinate-frame judgment, and better to stop and escalate at a failing gate than to guess.

## Post-review addendum: reconstruction gate passed

The M2 reconstruction gate — the highest-risk unknown the review carried forward — was **passed during the review** rather than left for implementation, since it was empirically checkable. A wire-field → `VehicleState` → `SetVState()` → read-back round trip reconstructed attitude to 6×10⁻⁶°, position to 0.1 mm, and velocity to 0, across attitudes to inverted (159° roll) and through 90° pitch, for both Euler and native-quaternion wire forms. Two concrete gotchas were found and folded into the spec's normative recipe: the composition is JSBSim's own `Ti2l * qAttitudeLocal` (the draft's `Tl2i` was the wrong transpose), and the reconstruction `FGLocation` must copy the session's ellipsoid-configured `vLocation` (a bare one gives a latitude-only error growing to ~112 m). The spec now also settles the wire attitude representation on JSBSim-native `qAttitudeLocal` (gimbal-safe, reconstruction-trivial), moving the axis-convention conversion to the non-corrupting display side. This retires residual risk 1 below and removes the one reason the recommendation gave to escalate from Sonnet.

## Residual risks

1. **Blend feel** is inherently subjective and only the manual criterion covers it; the automated suite proves *state correctness and bounded tracking*, not that corrections look good. The manual test under `net_relay` is load-bearing for the "feel" half and cannot be replaced by CI.
2. **Increment 5 interactions**: the command-buffer/redundant-input change is sized for one client; per-client input buffers and the snapshot fan-out cost are an increment-5 scaling concern, flagged not solved here.
3. **Display-side native→Godot attitude conversion** is new (the wire now carries native `qAttitudeLocal`); it is non-corrupting (cosmetic if wrong) and covered by the manual criterion, but should get its own small forward-direction check during implementation.
