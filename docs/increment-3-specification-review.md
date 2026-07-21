# Increment 3 Specification — Review Record

Date: 2026-07-21. Reviewed: draft 1 of `increment-3-specification.md`. Result: revision 2 of the specification (same file), which this document justifies finding by finding.

This is an adversarial review in the spirit of the increment-1 review: the goal is to find what is *wrong* with the draft, especially defects that would let a correct implementation pass its own tests while the thing the increment exists to prove is actually broken. Where increment 1's review hunted for flight-dynamics wrongness, this one hunts for the failure modes specific to networking: serialization bugs, false-green tests, clock ambiguity, and impairment tests that exercise the wrong layer.

## Method

The transport-layer assumptions (ENet build, localhost UDP, ENet-inside-Godot coexistence, absence of `netem`) were already validated empirically before draft 1 was written — see the draft's Appendix B. This review does not re-litigate those; they hold. Instead it attacks the draft's **test architecture and protocol design**, which draft 1 asserted from reasoning. One cheap empirical check was run to resolve a precision question (float32 quaternion transport); everything else here is a reasoning finding, because the defects are architectural rather than numerical.

## Findings

Severity: **blocking** = a correct implementation of the draft could pass its own tests while leaving the increment's actual goal unproven, or could ship a serialization bug undetected; **major** = would send the implementer down a wrong path or test the wrong thing; **minor** = precision/clarity issue.

### B1 (blocking) — The automated test checks twice-proven physics on client-derived quantities, and is vulnerable to false greens on the exact bugs this increment is most likely to have

Draft 1's automated test has the `flight_test_client` collect received `StateSnapshot`s, **derive** increment-1 quantities (altitude, IAS, pitch, bank, α) from the snapshot's position/quaternion/velocity, and evaluate increment 1's pass criteria against them.

This is the wrong test, two ways:

1. **It re-proves physics that is already twice-proven** (increment 1 standalone, increment 2 through Godot). The server runs the *identical* `FlightSession`; the network layer does not touch the physics math. Re-verifying the c172x's stall behaviour over a socket does not validate anything the socket could have broken.
2. **It is vulnerable to false greens on serialization bugs — precisely the class of bug increment 3 introduces.** Increment 1's criteria carry large margins (e.g. `max_alpha ≥ 12°` against a measured 14.29°). A transport bug — swapped quaternion components, a wrong-endianness field, a velocity axis sign flip, a fixed-point scale error — could still yield *derived* pitch/bank/α values that land inside those loose margins, so the test passes green while the wire format is wrong. A test that can pass while the transport is broken does not test the transport.

Separately, deriving α client-side requires rotating the velocity vector into the body frame and computing `atan2(w,u)` with the correct convention — a non-trivial computation whose bugs also produce plausible-but-wrong numbers.

**Resolution**: decompose the test into three assertions, each made where it can be made rigorously:
- **Transport fidelity** — the client compares each received snapshot **field-by-field against the server's own authoritative state** for that tick (the server logs its snapshots; the client logs what it received; a comparison step asserts they match within float32 tolerance). This catches *any* serialization/endianness/axis bug exactly and immediately, because it compares the same representation on both ends rather than laundering it through physics criteria.
- **Physics preservation** — evaluate increment 1's criteria **server-side**, on the server's full `FlightSample`, where every quantity JSBSim produces is available directly (no derivation). This validates that the server's new wall-clock-paced loop produces correct trajectories (the one genuinely new server-side element — see M3).
- **Input path** — see B2.

The client should not re-derive physics quantities to check physics criteria.

### B2 (blocking) — The increment's core goal (the input path) is left optional by the open questions

The draft's Goal is to prove the full server-authoritative loop *"with the authority direction correct (client sends inputs, server owns state)."* Yet "Open questions for the implementer" permits choosing the server's **scripted-scenario mode** (server applies its own built-in inputs and ignores the network) for the automated scenarios — *"or both."* An implementer could satisfy every listed automated test using only server-scripted mode, in which case the **client→server input path — the very thing the increment exists to prove — is never exercised by automation.**

**Resolution**: at least one automated **full-loop** test is *required*, not optional: the `flight_test_client` sends a control input over the network and the test asserts the server's authoritative state responds to it (e.g. client commands nose-up at t=2 s; assert server pitch rises through a threshold shortly after). Server-scripted mode remains available as an *additional* streaming-only test, but cannot be the only mode.

### M1 (major) — The in-process impairment layer tests the wrong layer

Draft 1 injects latency/loss by holding packets in an application-level queue. As described, this sits **above** ENet: ENet's own reliability, retransmission, RTT estimation and congestion logic run on the real, fast localhost socket and never see the injected impairment. Such a layer tests only *the application's* tolerance to delayed/dropped application messages — it does **not** exercise ENet's behaviour under an adverse network (reliable-channel retransmit under real latency, ordered delivery under real loss), which is a stated reason for choosing ENet in the first place.

**Resolution**: make the faithful mechanism primary — a **localhost UDP relay proxy**: a tiny process (or thread) that listens on one port, forwards datagrams to the server's port and back, and applies configurable delay + drop to the *actual datagram stream*. The client connects to the relay instead of the server. ENet then sees real impaired UDP and reacts correctly, so the resilience tests exercise the real transport. This needs no `NET_ADMIN` (it is ordinary userspace UDP forwarding), is deterministic with a seeded RNG, and is reusable for increment 4's prediction testing. The application-level queue may remain as a convenience for app-tolerance tests, but must be documented as *not* exercising ENet, and the relay is the mechanism the resilience acceptance criteria are evaluated against.

### M2 (major) — Scenario timeline / clock ownership is ambiguous, and anchored to an unreliable event

Draft 1 has the test client measure scenario time "from the handshake / first snapshot." Two problems: (1) the **first snapshot is unreliable** and may be dropped (especially under the M1 loss test), starting the scenario clock late or never; (2) the client's wall clock is not the server's simulation clock, so "input at t=5 s" (client-scheduled) and "max pitch in [5,10] s" (criterion window) may be evaluated against a timeline that does not match where the server actually applied the input.

**Resolution**: **`server_tick` is the authoritative timeline.** Every snapshot carries it; the client schedules inputs and evaluates all criteria against `server_tick` (converted to seconds via ÷120), never against its own wall clock. The scenario clock anchors to the **reliable `ServerWelcome`** (and thereafter to `server_tick`), never to an unreliable first snapshot. This also makes the reused-scenario criteria robust to snapshot loss.

### M3 (major) — Scenario-reuse is over-weighted; the real increment-3 risk is under-tested

Reusing increment 1's four full scenarios made strong sense in increment 2, where Godot's `_physics_process` was a genuinely *new execution path for the physics math* that could have altered it. In increment 3 the server runs the identical `FlightSession`; the network cannot change the physics math. Re-running four full 60/30/15 s flight scenarios at wall-clock pace (~165 s) therefore mostly re-proves twice-proven physics, while the actual increment-3 risks — serialization correctness, transport fidelity, the input path, resilience, handshake/version/disconnect — get comparatively little coverage.

The one genuinely new server-side element is the **wall-clock-paced accumulator loop** (increment 1 ran as-fast-as-possible; increment 2 used Godot's pacing). A single scenario, evaluated server-side (per B1), is sufficient to validate that the server's pacing produces a correct trajectory.

**Resolution**: keep **one** reused scenario as a server-side integration/pacing sanity check; make the bulk of the increment-3 automated suite **network-specific** — transport fidelity (B1), input path (B2), resilience under the relay (M1), handshake/version-rejection/clean-disconnect — none of which need full flight durations. This cuts CI time and, more importantly, points the tests at what can actually be wrong.

### m1 (minor) — Forward-looking "message structure won't change" claim is false past MTU

The draft states increment 4+ makes relevance selection smarter *"without changing the message structure."* At scale this is false: 128 aircraft × ~45 B ≈ 5.7 KB per snapshot exceeds a safe ~1400 B UDP datagram, forcing per-snapshot splitting across multiple packets (and unreliable-fragment handling) — a message-structure change. **Resolution**: soften to "without changing the per-aircraft record layout"; note explicitly that snapshot *splitting* across datagrams is a known increment-4 concern driven by MTU.

### m2 (minor, verified OK) — float32 quaternion transport is fine

A concern that transporting orientation as float32 quaternion (rather than double) might perturb client-side Euler extraction near gimbal lock was checked numerically: over 200 000 random attitudes, max Euler error from a float32 round-trip is **0.002°**, rising only to **0.003°** at 89.9° pitch. This is negligible against every criterion margin. **Resolution**: no change; recorded here so the width choice is not second-guessed later, and so nobody prematurely "optimizes" it into a smallest-three encoding for a precision reason that does not exist.

### m3 (minor) — throttle fixed-point wastes half the int16 range

`throttle` ∈ [0,1] encoded as value×32767 uses only 0…32767. Harmless, marginally wasteful. **Resolution**: optional — use `uint16` for throttle, or leave it; noted for completeness.

### m4 (minor) — Forbid struct-memcpy serialization explicitly

The draft implies field-by-field serialization ("netcore owns all serialization") but does not forbid the tempting shortcut of `memcpy`-ing a packed struct onto the wire, which reintroduces padding- and endianness-dependence the little-endian discipline is meant to remove. **Resolution**: state explicitly that messages are serialized field-by-field to a byte buffer; no struct is memcpy'd to or from the wire.

### m5 (minor) — float32 origin lat/lon is imprecise for absolute georeferencing

`ServerWelcome` sends `origin_lat/lon` as float32 (~1 m resolution at temperate latitudes). Harmless in increment 3 because positions are transmitted as *local* float32 offsets from that origin and the origin is (0,0); but a future absolute-position need would inherit a silent ~1 m bias. **Resolution**: note it; if absolute georeferencing is ever required, send the origin as float64.

### Verified as sound (no change)

- ENet choice, version pin (v1.3.18), and the empirically-proven coexistence with Godot's bundled ENet.
- `uint8` player IDs and the deliberate narrow-field discipline.
- The reliable/unreliable channel split (handshake/teardown reliable; input/state unreliable).
- The `netcore`-shared-by-both-ends architecture guaranteeing byte-identical wire format — this is exactly what makes the B1 field-by-field fidelity test possible.
- The Godot client as a manual (non-CI) criterion, with its transport path covered automatically via the shared `netcore` and de-risked by the pre-drafting runtime probe.
- Server wall-clock-paced fixed-timestep loop with a catch-up cap.
- Little-endian wire convention and the protocol-version handshake.

## Implementation model recommendation

**Recommendation: Sonnet, in a session that can build and run**, unchanged from increments 1–2. After the revisions above, every judgment call is pinned: the tests assert transport fidelity against server-authoritative state (mechanical, no physics derivation), the input-path test is required and concrete, impairment uses a relay whose behaviour is well-defined, and the timeline is `server_tick`. The remaining work is careful C++ transcription with a self-checking definition of done (`run_tests.sh` exit 0, CI green), which Sonnet handles well and can verify end-to-end.

Escalate to a frontier model only if the relay-proxy resilience tests reveal ENet behaviour under loss that needs real netcode judgment to interpret — but that is increment 4's territory (prediction/reconciliation), where the roadmap already calls for a netcode-focused review pass.

## Residual risks

1. **α/IAS on the wire vs derived**: the revision evaluates physics criteria server-side, sidestepping client derivation entirely for the automated tests. But the *Godot* client will still want airspeed (and eventually α-like data) for a future HUD; when that arrives, decide then whether to add fixed-point `ias` to the snapshot or derive it — out of scope here, flagged for increment 4/UI work.
2. **Relay proxy fidelity**: a userspace UDP relay faithfully impairs the datagram stream but adds its own small real latency; tests should measure against the relay's configured impairment, not assume zero baseline.
3. **Snapshot rate vs criterion sampling** (now mostly moot, since physics criteria move server-side at 120 Hz): the single retained client-visible scenario is evaluated on 30 Hz-decimated data; its criteria windows are wide enough to be robust, but this should be confirmed in the first implementation run.
