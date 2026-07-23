# Increment 7 Specification: Bots

## Status

Revision 2. Pre-drafting validation (process/lifecycle) plus adversarial review (`docs/increment-7-specification-review.md`, findings B1/M1/M2/m1/m2/m3) folded in. **One open decision remains for the project owner before implementation** — whether the bot stays physics-less (the design written below) or gains a real local `FlightSession` (see "Open questions", finding B1's fork). Not yet implemented.

## Goal

A simple, headless **control-and-perception** client (`flight_bot`) — it sends `ControlInput` and receives the full snapshot picture, exactly the I/O of a human client, but (in the design written here) without the human client's local prediction — that populates the airspace around a server's spawn point with gently maneuvering aircraft. Useful both for a server configured with a target bot count and for automated testing, architected as a genuine network client rather than an in-process server-side shortcut so the "full airspace picture" perception plumbing future combat intelligence will need already exists rather than being retrofitted later. (It is deliberately *not* called a "headless human client": the human client's defining feature is client-side prediction/reconciliation, which the physics-less design omits — so as a test client it exercises the server's multi-client path but not the prediction path, review finding M2.)

## Non-goals

- **Any actual combat intelligence or decision-making about other aircraft.** Bots fly a fixed, simple maneuver pattern, oblivious to other players — even though they *do* receive full snapshot data about them (see Goal).
- **Difficulty levels, skill tuning, or bot "personalities."**
- **Multiple aircraft types per bot roster** — a bot flies whichever single type increment 6's server is configured for, same as any human client that session.
- **Persistent bot identity across server restarts.**
- **Any change to human-client behaviour or protocol** beyond the `PlayerLeft`-driven despawn path that already exists (increment 5).

## Architecture

### `flight_bot`: a genuinely headless client

New standalone binary, `src/bot/main.cpp` (mirroring `src/server/`'s own directory convention), Godot-free, depending only on `netcore` and `interpcore` — critically, **not** `flightcore`/JSBSim. A bot never runs its own local physics; it trusts the server's own last-broadcast `StateSnapshot` entry for its own player_id as "where am I now." Nobody is judging a bot's own responsiveness the way a human judges feel, so there is no reason to predict locally — an ordinary amount of network latency in a bot reacting to its own state is entirely fine.

Connects exactly like any real client: `ClientHello` → `ServerWelcome` (reads `assigned_player_id`, `aircraft_id`, **and its seeded trim vector** — see Wire protocol changes) → a `ControlInput` loop → `StateSnapshot` handling. On receiving a chunk, every *other* aircraft's entry is fed into an `interp::RemoteEntityTracker` — the "full airspace picture" the Goal calls for, exactly as `PredictedAircraft` does, unused by decision logic today but exercised end-to-end for future combat intelligence. The bot's **own** entry is read directly from the latest raw `AircraftState`, **not** through the tracker: the tracker deliberately renders ~100 ms in the past (`kInterpolationDelayS`) and extrapolates, which is right for smoothing others for display but wrong as feedback for a control loop, which wants the freshest real datum (review finding B1).

### Maneuver logic

Keeping an aircraft in gentle maneuvers is a **closed-loop attitude-control** problem, not an open-loop schedule of control deflections — this was measured, not assumed (review finding B1, Appendix B): aileron commands roll *rate*, so a fixed "hold aileron for the turn" deflection integrates bank without bound and spirals, and a trim-blind bot that sends zero aileron/rudder loses the airframe's non-zero roll/yaw trim and departs within one maneuver cycle. So:

- The pattern is a schedule of **attitude targets**, not control deflections: target a shallow bank (e.g. ~15°) for the turn phase and wings-level otherwise; target a gentle climb/descent pitch for the climb/descend phases; level between. `T1`/`T2`/`T3` phase durations are as before.
- Each tick, the bot runs a small **stabilised controller** toward the current phase's target — proportional on attitude error, derivative (body-rate) damping to stop the roll/pitch integrating into a departure, and rudder–aileron turn coordination — reading its own current attitude and body rates from the latest raw snapshot (above). This is the load-bearing part, not an optional "soft bias": without the rate damping and coordination, the aircraft departs (measured).
- The controller commands are expressed **relative to the seeded trim vector** (aileron = trim_aileron + control_output, etc.): the server supplies the trim baseline the physics-less bot cannot compute, and the bot perturbs around it. Elevator is the exception — the trim's pitch solution lives in `pitch-trim-cmd-norm`, which the server never overwrites, so elevator commands around zero are already around trim.

This is a small autopilot, deliberately simple, but it *is* an autopilot with feedback — the empirical finding is that nothing less keeps the aircraft up. Its gains are airframe-sensitive; see the endurance gate (Test plan) and the Camel caveat (Out of scope).

### Server-side spawn, capacity, and refill policy

`flight_server` gains `--bots N` (target count, default 0). Unlike `--stress-aircraft` (increment 5 — deliberately outside `--max-clients` capacity accounting, frozen/trimmed, never maneuvers, exists purely for chunking-load testing), bots share the *same* player_id/capacity pool as real clients: the whole point is that a bot occupies a slot a human could otherwise take, and gets displaced the moment one does.

The server drives toward `bots_active = max(0, N - humans_connected)`, re-evaluated and corrected on each *actual* ENet connect/disconnect event — an eventually-consistent target it converges to, **not** an invariant held every instant (review finding m2): spawn and despawn are both asynchronous (fork + connect + increment-5 async onboard on one side, `SIGTERM` + disconnect on the other), each spanning many ticks, so instantaneous maintenance is neither possible nor needed. A mid-flight refill bot counts toward the target the moment it is `fork()`ed (not only once welcomed), so a burst of connects cannot over-spawn.

- If a human connect would exceed `--max-clients` and at least one bot currently occupies a slot, the server despawns one bot to make room. **Sequencing matters and is specified explicitly (review finding M1):** a bot's player_id is freed by increment 5's existing path when its ENet `DISCONNECT` event is processed, which arrives some round-trips *after* the `SIGTERM`, not synchronously with it. So the server initiates the bot's `SIGTERM` (and its `PlayerLeft` broadcast) and completes the displacing human's player_id allocation only *once the freed slot is actually observed* — the human's onboarding is sequenced behind the bot's real disconnect, not merely behind the signal. A human is never rejected purely because bots occupy slots meant for them, and never double-allocated a slot the bot hasn't yet vacated.
- If a human disconnects and `bots_active + humans_connected < N`, the server spawns a new bot to refill back toward N (per explicit decision: refill, not "stay down until restart").

At startup, `--bots N` spawns bots within the existing `--max-clients` bound (so N is already capped at a modest number); if a future large fleet is ever wanted, spawns should be paced across a few ticks rather than issued all at once, to avoid an unpaced fork+connect+trim burst (review finding m3) — not load-bearing at the intended scale, noted for the bound.

"Despawn a bot" means sending that bot's own process `SIGTERM` and removing it from the server's child-process tracking table — validated directly (Appendix B): `flight_bot` handles `SIGTERM` as a clean disconnect, so the OS signal *is* the despawn mechanism, no new server→client wire message needed for it.

**Crash safety (review finding m1)**: Appendix B validated *clean* shutdown only, but a `flight_server` crash (`SIGKILL`, segfault) does not run the reap path, and POSIX does not kill children with their parent — orphaned bots would keep running against a dead port. So `flight_bot` **exits on loss of its server connection** (ENet disconnect/timeout), which also cleanly handles the "server restarted" case; on Linux the implementer may additionally set `prctl(PR_SET_PDEATHSIG, SIGTERM)` in the child after `fork()` for defence in depth. "No orphaned processes" must hold under crash, not only under clean exit.

`flight_server`'s existing multi-client connection-acceptance path needs zero special-casing to accept a bot — a bot is a real ENet connection over localhost UDP to the server's own listening port, going through the identical `ClientHello`/player_id-allocation/onboarding pipeline every human client already uses (validated directly, Appendix B). The only new server-side code is: the count-maintenance policy above, and the `fork()`/`exec()`/track-pid/`waitpid`-on-exit bookkeeping.

## Wire protocol changes

**One addition** (revised from draft 1's "none" — review finding B1). A physics-less bot cannot compute its own airframe's non-zero trimmed control values (aileron, rudder, throttle), which differ per airframe and which the server overwrites every tick; the server *has* them (it trims every aircraft), so it must seed them to the bot. Carried on **`ServerWelcome`**, extended with three trim floats (`trim_aileron`, `trim_rudder`, `trim_throttle`) alongside the `aircraft_id` increment 6 already adds — or, if preferred, a small dedicated `TrimBaseline` message sent immediately after welcome. Either way this is a wire-layout change, so per increment 6's standing rule **`kProtocolVersion` bumps again, to 3.**

The bot's *own outbound* traffic is still byte-identical to a human client's — `ClientHello`, `ControlInput`, and its handling of `StateSnapshot`/`PlayerLeft` are unchanged; only the server→client welcome grows a field that human clients simply ignore. (A human client already knows its own trim from its local `FlightSession`, so the seeded values are redundant for it and harmless.)

1. **Airborne-endurance gate (the acceptance test that actually matters, review finding B1)** — the bot's controller, run headless against a real server-side `FlightSession` for each airframe in the bot fleet, keeps the aircraft airborne (no ground contact, no spiral past a bank limit, no stall) for **≥ 3 minutes** of continuous unsupervised maneuvering. This is developed and run *before* the network path and *before* any human "looks sane" check, so controller tuning is not entangled with ENet timing. c172x and pa28 are confirmed tractable with a stabilised controller (Appendix B); the Camel is gated on this test, not assumed (see Out of scope).
2. `flight_server --bots N`, no humans connected: confirm N bot player_ids appear in a connected `flight_test_client`'s view within a few ticks of startup, each with genuinely evolving (non-frozen) altitude/attitude that stays *bounded and airborne* over time — not merely non-frozen (an aircraft spiralling into the ground is also "non-frozen"), the real distinction from a `--stress-aircraft` synthetic.
3. `flight_server --bots 2 --max-clients 3`, then connect 2 real (simulated) clients: confirm bots are displaced (`PlayerLeft` fires for each) as humans take their slots, that the displacing human's slot is allocated only after the bot's disconnect is actually processed (finding M1), and that a third real client can still connect — total capacity respected, not blocked by lingering bots.
4. Disconnect a human client; confirm a bot respawns to refill back toward N within a few ticks.
5. Child-process lifecycle, clean: `flight_server` `SIGTERM` terminates every bot child it spawned — no orphans after a clean exit (validated in Appendix B's pre-drafting probe).
6. Child-process lifecycle, **crash** (finding m1): `SIGKILL` the `flight_server` and confirm its bots exit on their own (server-connection loss), leaving no orphaned processes — the case clean shutdown doesn't cover.
7. A bot's own `RemoteEntityTracker` genuinely receives *other* aircraft's snapshot data, not just its own — confirmed by starting 2 bots and checking (via a debug/self-test path in `flight_bot`, or an external observer) that each is aware of the other, even though neither acts on it.
8. Full `scripts/run_tests.sh` passes end-to-end across increments 1-7.

## Documentation

`README.md` gains a `--bots` usage example alongside the existing `--max-clients`/`--stress-aircraft` documentation.

## Licence

No change. `flight_bot` is new source in this repository under the same GPL-3.0-or-later as everything else; no new third-party dependency (reuses `netcore`/`interpcore`, already-fetched ENet transitively).

## Acceptance criteria

1. **Every airframe in the bot fleet passes the airborne-endurance gate** (test plan item 1): ≥ 3 minutes continuous unsupervised maneuvering with no ground contact, spiral, or stall. This is the criterion the increment lives or dies by; the rest assume it.
2. `flight_bot` connects to a real `flight_server` exactly like any human client, with no server-side special-casing beyond the seeded trim vector (connection path validated in Appendix B).
3. `flight_server --bots N` spawns up to N bots at startup, sharing the same player_id/capacity pool as real clients (unlike `--stress-aircraft`).
4. A human connect displaces a bot the moment a slot is needed — with the human's slot allocated only after the displaced bot's disconnect is actually processed (finding M1) — and a human disconnect refills a bot back toward N.
5. Every spawned bot is cleanly terminated with no zombies when despawned or on clean server shutdown (`SIGTERM`/`waitpid`), **and leaves no orphan when the server crashes** (bot exits on connection loss, finding m1).
6. Bots fly a genuinely evolving *and bounded/airborne* pattern (climb/shallow-turn/descend/level) — distinct from both a frozen `--stress-aircraft` synthetic and from an aircraft "evolving" into a spiral.
7. Bots receive full snapshot data for every other aircraft, not just their own, even though nothing acts on it yet.
8. `kProtocolVersion` is 3, reflecting the seeded-trim wire change.
9. The full `scripts/run_tests.sh` suite passes end-to-end across increments 1-7.
10. *(pending human verification, matching increment 4/5/6's own equivalent criteria)* A human confirms bots visibly populate the airspace and look reasonably sane in flight — now backed by the automated endurance gate, so a human is never asked to judge an aircraft that is quietly departing.

## Out of scope, explicitly deferred

Everything in increments 1-6's deferred lists, plus: combat intelligence/decision-making (a later increment, once real combat exists to react to), difficulty/personality tuning, persistent bot identity across restarts, multiple aircraft types per bot roster (increment 8's concern, applied uniformly here).

**The Camel in the bot fleet is gated, not assumed** (review finding B1): a stabilised controller flew c172x and pa28 solidly but still spiralled the Camel in testing (Appendix B) — a low-power, low-speed, spiral-prone WWI biplane trimmed near its own energy margin is a materially harder control target. If a conservatively-tuned controller cannot pass the endurance gate for the Camel, the first bot implementation ships with the GA airframes only (c172x, pa28) and the Camel is deferred from the *bot* fleet — the same "shelve the hard airframe, revisit post-demo" discipline increment 6 applied to p51d/dr1/L17. (The Camel remains a fully valid *human*-flown airframe; this is only about whether the simple bot autopilot can fly it unsupervised.)

- **PRIMARY — physics-less bot vs. bot with a real local `FlightSession` (owner's decision, review finding B1's fork).** The spec above is written for a **physics-less** bot, which requires: server-seeded trim (a wire change + version bump), a closed-loop controller reading raw own-state snapshots, and per-airframe controller tuning that (measured) does not yet fly the Camel. Giving the bot **a real local `FlightSession`** — a true headless human client running its own `PredictedSession` like the Godot client — *dissolves* all of that: it would know its own trim (no seeding, no wire change, no version bump), fly closed-loop against zero-latency local state (the Camel's delayed-feedback problem largely goes away), and exercise the full prediction/reconciliation path as a genuine test client (retiring finding M2). The cost is a per-bot trim (~12 ms) on spawn plus local physics stepping — the server already steps that aircraft, so it is stepped twice — against the "populate the airspace cheaply" goal. **This choice changes what gets built and should be made before implementation starts.** Recommendation deferred to the owner; the review record lays out both arms.
- Exact `T1`/`T2`/`T3` durations, target bank/pitch magnitudes, and controller gains — a feel/tuning question settled against the endurance gate (test plan item 1), not an architecture one. Reasonable small values (≈15° bank, a few hundred feet of altitude change) then tuned until the gate passes for each fleet airframe.
- Whether `flight_server` should log bot spawn/despawn events distinctly from human connect/disconnect, for operator visibility — a small, low-risk addition, not load-bearing for any acceptance criterion.

## Appendix A: message and field reference (normative, updates to increment 6's)

**`ServerWelcome`** gains three trailing trim floats (revised from draft 1's "no changes", review finding B1):

| Field | Wire type | Notes |
|---|---|---|
| ...all increment-6 fields... | | unchanged, including `aircraft_id` |
| `trim_aileron` | `float32` | the server's trimmed `fcs/aileron-cmd-norm` for this aircraft |
| `trim_rudder` | `float32` | trimmed `fcs/rudder-cmd-norm` |
| `trim_throttle` | `float32` | trimmed `fcs/throttle-cmd-norm` |

(Elevator is intentionally absent: the trim's pitch solution lives in `pitch-trim-cmd-norm`, which the server never overwrites, so the bot's elevator commands around zero are already around trim.) A human client ignores these fields — it knows its own trim from its local `FlightSession`. If a dedicated `TrimBaseline` message is preferred over extending `ServerWelcome`, it carries the same three floats plus the target `player_id`. Either way `kProtocolVersion` becomes **3**.

`ClientHello`, `ControlInput`, `StateSnapshot`, `ServerReject`, `PlayerLeft` — unchanged in layout. The bot's own outbound traffic is byte-identical to a human client's.

## Appendix B: measured findings from pre-drafting validation (informative)

**Child-process spawn and lifecycle, validated directly** (`probe_bot_parent.cpp`/`probe_bot_child.cpp` — a real `flight_server`, a parent process that `fork()`/`exec()`s a minimal stand-in client, no code shared with or borrowed from the eventual `flight_bot` beyond reusing `netcore`'s own `NetClient`):

- The forked child successfully connected to a real, already-running `flight_server` over real ENet/UDP, received `ServerWelcome`, and was assigned `player_id=1` — no server-side special-casing needed; the server's existing multi-client onboarding path handled it exactly like any other connection.
- The parent's `kill(pid, 0)` liveness check confirmed the child was genuinely alive and running before signalling it.
- `SIGTERM` sent from the parent was caught by the child and produced a clean, orderly disconnect — the same signal-handling shape `flight_server` itself already uses (`volatile std::sig_atomic_t` flag, checked in the main loop, `src/server/main.cpp`), confirming this pattern is safe to reuse for bot lifecycle management.
- `waitpid()` reaped the child with exit code 0. A second `kill(pid, 0)` after reaping correctly returned `ESRCH` ("No such process") — confirming no zombie process was left behind.
- The server itself (`flight_server`, a real process throughout, not a probe stand-in) remained healthy and running throughout the entire connect/disconnect cycle, confirmed via its own log and an explicit status check afterward — no crash, no hang, no leaked state.

This directly grounds the "zero server-side special-casing" and "no zombie processes" claims in the Architecture section and acceptance criteria above, rather than assuming standard POSIX process-lifecycle behaviour would simply work. (It validated *clean* shutdown; the *crash*-orphan case is finding m1, handled by the bot exiting on connection loss.)

### Review measurements — bot flight behaviour (added during adversarial review; `probe_inc7_bot{,2,3}.cpp`)

Three probes replicated the server's exact per-tick command application (`applyClientCommand`: set elevator/aileron/rudder/throttle-cmd-norm from the client command each tick, leaving the trim's `pitch-trim-cmd-norm` untouched) and drove a real `FlightSession` for each of the three increment-6 airframes for 90 s (three maneuver cycles), flagging departure (NaN / ground contact / spiral > 80° / stall < 20 kt). These reshaped the maneuver design from "simple time-based schedule" into a stabilised closed-loop controller (finding B1):

| control style | c172x | Camel | pa28 |
|---|---|---|---|
| **trim-blind** (aileron/rudder = 0, absolute throttle) | DEPARTED (spiral) | DEPARTED (spiral, 24 kt) | DEPARTED (ground) |
| trim-aware, aggressive open-loop | airborne | DEPARTED (spiral) | DEPARTED (ground) |
| trim-aware, gentle open-loop | airborne | DEPARTED (spiral) | airborne |
| trim-aware, **closed-loop bank-hold** | airborne | DEPARTED (spiral) | airborne |
| trim-aware, **stabilised** (bank error + roll-rate damping + rudder coordination) | **airborne** (bank held 16°) | **DEPARTED** (spiral 81°, 35 kt) | **airborne** (bank held 13°) |

Reading:

- **Trim-blindness departs every airframe.** JSBSim's trim writes non-zero solutions into `aileron-cmd-norm`/`rudder-cmd-norm` (c172x −0.075/−0.004; Camel +0.102/−0.064; each different) and a different throttle each (c172x 0.792, Camel 0.301). A physics-less bot sending zero aileron/rudder and a guessed throttle discards them and departs. → the server must seed the trim vector.
- **Open-loop deflection is the wrong model.** Aileron commands roll rate, so any sustained net aileron integrates bank without bound — a fixed "hold aileron for the turn" schedule spirals rather than settling into a shallow turn. → closed-loop attitude-hold with rate damping is load-bearing, not a "soft bias."
- **The Camel is materially harder.** A stabilised, coordinated, trim-aware controller flew c172x and pa28 solidly (bank held at target, altitude within ~50 m over 90 s) but still spiralled the Camel (bank 81°, speed bled 65 → 35 kt). This does not prove the Camel unflyable — a per-airframe-tuned autopilot with speed protection likely can — but proves a single simple airframe-blind controller does not, hence the endurance gate and the Camel caveat. A missing-property read, incidentally, returns 0 quietly rather than throwing (confirmed in increment 6's review), so the bot's own-state reads are safe across airframes.
