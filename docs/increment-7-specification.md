# Increment 7 Specification: Bots

## Status

Revision 3. Pre-drafting validation (process/lifecycle) plus adversarial review (`docs/increment-7-specification-review.md`) folded in, then the review's one open architecture decision resolved by the project owner: **bots run their own local JSBSim** (a true headless client, not a physics-less one — see "The local-physics decision" below). Revision 3 also folds in the owner's capacity/load model and the non-human marking and remote-bot direction. Not yet implemented.

## Goal

`flight_bot`: a genuinely headless version of the networked client — it runs the *same* local `FlightSession` + `PredictedSession` a human's Godot client runs (just without Godot, rendering, or input), sends `ControlInput`, and receives the full snapshot picture. It populates the airspace around a server's spawn point with gently maneuvering aircraft, and is a genuine network client rather than an in-process server-side shortcut, so both the "full airspace picture" perception and the local flight-dynamics model that future combat intelligence needs are present from the start rather than retrofitted.

### The local-physics decision (resolves review finding B1's fork)

The adversarial review measured that a *physics-less* bot cannot keep aircraft airborne: it can't self-determine its airframe's trim (which the server overwrites every tick) and can only read its own state through a 100 ms-delayed interpolating tracker, so it departs within a maneuver cycle (Appendix B). The owner resolved the review's fork toward **giving the bot a real local `FlightSession`**, for a reason beyond just fixing that: the later air-combat increment will "bestow some air-combat smarts unto the bot," and those smarts require the bot to apply its own aircraft's flight-dynamics model to optimise energy (deliberately fuzzed by a per-bot handicap factor). A bot with no physics could never do that. Local physics is therefore load-bearing for the project's direction, not merely the cheaper way to keep this increment's bots airborne — so it is adopted now.

Consequences, all positive for this increment:
- The bot knows its own trim exactly, from its own trim solve — **no server-seeded trim, and therefore no wire-layout change and no protocol-version bump** (revision 2 needed both; revision 3 does not).
- The bot flies its controller closed-loop against its own **zero-latency, full-rate local state**, not a delayed interpolated snapshot — the review's feedback-latency problem disappears.
- It is a true headless *human* client, so it exercises the full client-side prediction/reconciliation path — retiring the review's finding M2 (the bot is now a genuine test client for that path, not only for the server's multi-client handling).

## Non-goals

- **Any actual combat intelligence or decision-making about other aircraft.** Bots fly a fixed, simple maneuver pattern, oblivious to other players — even though they receive full snapshot data about them, and now carry the local physics model a future increment's energy-management AI will use.
- **Difficulty levels, skill tuning, or bot "personalities"** (including the eventual per-bot handicap factor — named as the *reason* for local physics, but not implemented here).
- **Remote / third-party bot hosting and any "bot-programmer competition" scene** — a genuinely valuable future direction (see "Remote bots"), but deferred to its own increment; this increment builds *local* bots only, with the marking and client architecture deliberately designed so remote bots slot in later with no rework.
- **Multiple aircraft types per bot roster** — a bot flies whichever single type increment 6's server is configured for, same as any human client that session.
- **Persistent bot identity across server restarts.**

## Architecture

### `flight_bot`: a headless client with local physics

New standalone binary, `src/bot/main.cpp`, Godot-free, depending on `flightcore` (its own `FlightSession`), `predictcore` (`PredictedSession` — predict/reconcile, reused unmodified from the Godot client), `netcore`, and `interpcore`. On startup it loads and trims the server's configured aircraft type (from `ServerWelcome.aircraft_id`, increment 6) exactly as `PredictedAircraft` does, then runs the identical predict/send/reconcile loop — the bot *is* increment 4/5's client, headless.

Connects like any client: `ClientHello` → `ServerWelcome` (reads `assigned_player_id`, `aircraft_id`) → a `ControlInput` loop → `StateSnapshot` handling. Every *other* aircraft's snapshot entry feeds an `interp::RemoteEntityTracker` (the "full airspace picture", unused by decision logic today, ready for future combat AI). The bot's **own** control loop reads its own **local predicted state** (zero latency, full rate) — the whole point of local physics — not a received snapshot and not the interpolating tracker.

### Maneuver logic

Keeping an aircraft in gentle maneuvers is a closed-loop attitude-control problem, not an open-loop deflection schedule — measured, not assumed (review finding B1, Appendix B): aileron commands roll *rate*, so sustained deflection integrates bank without bound. With local physics the controller has the best possible inputs (exact trim, zero-latency attitude and body rates), but it is still a small autopilot with feedback, not a time-based deflection script:

- The pattern is a schedule of **attitude targets** — a shallow bank (≈15°) for the turn phase, wings-level otherwise; a gentle climb/descent pitch for those phases; level between. `T1`/`T2`/`T3` phase durations as before.
- Each tick the controller drives toward the current phase's target: proportional on attitude error, derivative (body-rate) damping to stop roll/pitch integrating into a departure, rudder–aileron turn coordination — all reading the bot's own local state.
- Commands are expressed relative to the bot's own trimmed control values (which it knows locally); elevator perturbs around the trim's `pitch-trim-cmd-norm` baseline.

Gains are airframe-sensitive; see the endurance gate (Test plan) and the Camel caveat (Out of scope).

### Server-side capacity, spawn, and CPU-leveling

`flight_server` gains two caps:
- `--max-bots B`: bots that fill the airspace when the server is otherwise empty.
- `--max-players P`: humans admitted "on top of" the bots before bots begin yielding slots.

Policy (the owner's load model): the airspace fills to **B** bots when empty. As humans connect, up to **P** of them are admitted on top of the bots — the airspace grows from B toward **B + P** — and each human beyond P is still admitted, now by **displacing one bot** (the airspace holds at B + P, its composition shifting from bot to human), until at B + P humans it is all-human with no bots. Total aircraft never exceeds **B + P**, and a human is never turned away in favour of a bot.

**Why this shape, and why the two caps are separate — CPU-leveling.** A *local* bot now runs its own JSBSim (the decision above) as a forked child *on the server host*, so it costs the server host **two** JSBSim instances: its own local-prediction copy plus the server's authoritative copy. A human costs the server host **one** — only the authoritative copy, since the human's prediction runs on their own machine. Because a displaced local bot frees two instances while the arriving human costs one, the server's JSBSim load **peaks at the B-bots-plus-P-humans transition** (2B + P instances) and then *eases* as further humans displace bots (each displacement nets −1). The busiest-by-headcount server is deliberately not the highest-CPU one, and a box provisioned for 2B + P never overloads however the bot/human mix shifts — this is exactly the "levels the CPU requirements across the scaling" the model is designed for. (Ratio note: the owner's stated rule is one human displaces one bot; a stricter budget-preserving variant — two humans per bot, holding 2·bots + humans constant — is possible if a flat CPU profile is ever wanted over the eased one. One human per bot is specified here.)

The count is driven toward this target on each *actual* ENet connect/disconnect event — eventually-consistent, not an instantaneous invariant (review finding m2): spawn (fork + connect + increment-5 async onboard) and despawn (`SIGTERM` + disconnect) each span many ticks. A mid-flight refill bot counts toward the target the moment it is `fork()`ed, so a connect burst cannot over-spawn.

**Displacement sequencing is specified explicitly (review finding M1):** a bot's player_id is freed by increment 5's existing path when its ENet `DISCONNECT` is processed — some round-trips *after* the `SIGTERM`, not synchronously. So the server initiates the bot's `SIGTERM` (and its `PlayerLeft` broadcast) and completes the displacing human's player_id allocation only once the freed slot is actually observed — the human's onboarding is sequenced behind the bot's real disconnect, never merely behind the signal, so a slot is never double-allocated.

**Spawn/despawn mechanism.** "Despawn a bot" is `SIGTERM` to its process plus removal from the child-tracking table; `flight_bot` handles `SIGTERM` as a clean disconnect (validated, Appendix B), so the OS signal *is* the despawn mechanism. **Crash safety (review finding m1):** Appendix B validated clean shutdown only, but a `flight_server` crash (`SIGKILL`, segfault) does not run the reap path and POSIX does not kill children with their parent — so `flight_bot` also **exits on loss of its server connection** (ENet disconnect/timeout), which additionally handles the "server restarted" case; on Linux the implementer may set `prctl(PR_SET_PDEATHSIG, SIGTERM)` after `fork()` for defence in depth.

`--max-bots` bots are spawned at startup within the load budget; a large fleet should be spawned paced across a few ticks rather than all at once (review finding m3) — not load-bearing at the intended scale, noted for the bound.

### Marking bots as non-human

Bots — local now, remote later — must be **clearly marked as non-human** so a client can render and label them distinctly (owner's requirement). This uses one bit of `AircraftState.status_flags`, the `uint8` increment 5 already reserved for exactly this kind of use — so it is a *defined bit, not a new field*, and therefore **not a wire-layout change** (an old client simply ignores the bit; `kProtocolVersion` stays at increment 6's value of 2). The server knows which aircraft are bots because it forked them, and sets the bit when building their `AircraftState`. The Godot client's `RemoteAircraftSpawner` reads it and gives bot aircraft a visually distinct marker from human players.

### Remote bots (forward-looking; hosting deferred)

Because a bot is already a genuine network client, a **remote, third-party bot** — someone else's program, connecting from their own machine to compete in the airspace (the "bot-programmer scene" the owner envisions) — is architecturally just a client that connects and declares itself a bot. Two things make this increment forward-compatible with it, without building it:
- The non-human `status_flags` bit is defined uniformly, so a remote bot is marked exactly like a local one ("remote bots should be like local bots: clearly marked as non-humans").
- A remote bot self-declares via a future one-bit flag in `ClientHello` (the server can't fork it, so it can't otherwise know) — deferred with the rest of remote-bot hosting, and noted here so the field is added deliberately when that increment lands.

A remote bot's local physics runs on *its* machine, so it costs the server host one JSBSim instance (like a human), not two — a detail the CPU-leveling model will revisit when remote bots are actually scheduled. The hosting, the self-declaration flag, any competition/scoring framework, and any sandboxing of third-party bot code are all **out of scope here**, deferred to their own increment.

## Wire protocol changes

**No wire-layout change; no protocol-version bump.** The local-physics decision removed revision 2's server-seeded-trim field, and the non-human marker is a *defined bit* of the already-present, already-reserved `AircraftState.status_flags` `uint8` — not a new field. `kProtocolVersion` stays 2 (increment 6's value). The bot's own outbound traffic (`ClientHello`, `ControlInput`) and `StateSnapshot`/`PlayerLeft` handling are byte-identical to a human client's. (The future remote-bot `ClientHello` self-declaration bit, when added, *will* be a layout change and bump the version then — not now.)

## Test plan

1. **Airborne-endurance gate (the acceptance test that matters, review finding B1)** — the bot, running its real local `FlightSession` + controller, keeps its aircraft airborne (no ground contact, spiral, or stall) for **≥ 3 minutes** of continuous unsupervised maneuvering, per airframe in the bot fleet. Developed and passed *before* the human "looks sane" check. c172x and pa28 are tractable with a stabilised controller (Appendix B); the Camel is gated on this test, not assumed (Out of scope).
2. `flight_server --max-bots N`, no humans: N bot player_ids appear in a connected `flight_test_client`'s view within a few ticks, each **marked non-human** (status_flags bit) and with genuinely evolving *and bounded/airborne* altitude/attitude — the real distinction from a frozen `--stress-aircraft`.
3. Capacity/CPU-leveling model: with `--max-bots B --max-players P`, connect humans past P and confirm (a) the first P humans add on top of the bots (airspace grows toward B+P), (b) humans beyond P each displace one bot (airspace holds at B+P), (c) the displacing human's slot is allocated only after the bot's disconnect is processed (finding M1), and (d) a human is never rejected while a bot occupies a slot.
4. Disconnect a human below the P threshold; confirm a bot refills back toward B.
5. Child-process lifecycle, clean: `SIGTERM` to `flight_server` terminates every bot child, no orphans (Appendix B).
6. Child-process lifecycle, **crash** (finding m1): `SIGKILL` the server and confirm its bots exit on their own (connection loss), no orphans.
7. A bot's `RemoteEntityTracker` receives *other* aircraft's snapshot data, not just its own (start 2 bots; each is aware of the other) — the perception path for future combat AI.
8. The non-human `status_flags` bit is set on bot aircraft and clear on human/`flight_test_client` aircraft, end to end.
9. Full `scripts/run_tests.sh` passes end-to-end across increments 1-7.

## Documentation

`README.md` gains `--max-bots`/`--max-players` usage and a note that bot aircraft are marked and rendered distinctly from humans, alongside the existing `--max-clients`/`--stress-aircraft` documentation. (`--max-clients` from increment 5 is subsumed by `--max-players`; the relationship is spelled out in the README.)

## Licence

No change. `flight_bot` is new source under the same GPL-3.0-or-later; it links the same `flightcore`/`predictcore`/`netcore`/`interpcore` already in the tree — no new third-party dependency.

## Acceptance criteria

1. **Every airframe in the bot fleet passes the airborne-endurance gate** (≥ 3 min, no ground contact/spiral/stall) — the criterion the increment lives or dies by.
2. `flight_bot` runs a real local `FlightSession` + `PredictedSession` (a true headless client), connecting exactly like a human client with no server-side special-casing beyond the fork/track bookkeeping.
3. `flight_server --max-bots B --max-players P` implements the capacity/CPU-leveling model: bots fill to B when empty; humans add on top up to P then displace bots; total never exceeds B + P; a human is never rejected for a bot.
4. A human connect displaces a bot only once past the additive threshold, with the human's slot allocated after the displaced bot's disconnect is processed (finding M1); a human disconnect refills a bot.
5. Every spawned bot is cleanly terminated with no zombies when despawned or on clean server shutdown, **and leaves no orphan when the server crashes** (bot exits on connection loss, finding m1).
6. Bot aircraft carry the non-human `status_flags` bit end to end; human aircraft do not; the Godot client renders bots distinctly.
7. Bots fly a genuinely evolving *and bounded/airborne* pattern — distinct from both a frozen `--stress-aircraft` and from an aircraft "evolving" into a spiral.
8. Bots receive full snapshot data for every other aircraft (the future-combat-AI perception path), even though nothing acts on it yet.
9. `kProtocolVersion` is unchanged at 2 (no wire-layout change this increment).
10. The full `scripts/run_tests.sh` suite passes end-to-end across increments 1-7.
11. *(pending human verification)* A human confirms bots visibly populate the airspace, are clearly distinguishable from human players, and look reasonably sane in flight — backed by the automated endurance gate, so no human is asked to judge an aircraft that is quietly departing.

## Out of scope, explicitly deferred

Everything in increments 1-6's deferred lists, plus: combat intelligence / energy-management AI and the per-bot handicap factor (a later increment — local physics is built now *for* it, but no smarts here); remote/third-party bot hosting, the bot-programmer competition scene, the `ClientHello` self-declaration flag, and any third-party-code sandboxing (their own future increment); difficulty/personality tuning; persistent bot identity across restarts; multiple aircraft types per bot roster (increment 8).

**The Camel in the bot fleet is gated, not assumed** (review finding B1): a stabilised controller flew c172x and pa28 solidly but still spiralled the Camel in testing (Appendix B) — a low-power, low-speed, spiral-prone WWI biplane trimmed near its own energy margin is a materially harder control target. Local physics gives the controller its best possible inputs (exact trim, zero-latency state), which may be enough where the physics-less version was not; but if a conservatively-tuned controller still cannot pass the endurance gate for the Camel, the first bot implementation ships with the GA airframes only (c172x, pa28) and the Camel is deferred from the *bot* fleet — the "shelve the hard airframe, revisit post-demo" discipline increment 6 applied to p51d/dr1/L17. The Camel remains a fully valid *human*-flown airframe; this is only about the simple bot autopilot flying it unsupervised.

## Open questions for the implementer

- Exact `T1`/`T2`/`T3` durations, target bank/pitch magnitudes, and controller gains — a feel/tuning question settled against the endurance gate, not architecture. Reasonable small values (≈15° bank, a few hundred feet) then tuned until the gate passes for each fleet airframe.
- Whether the "double JSBSim on the server host per local bot" cost (2B instances at full bot load) wants a lower default `--max-bots` than a human-only server's `--max-clients` would suggest — a provisioning default, informed by increment 5's per-aircraft step-cost measurements, not a correctness question.
- Whether `flight_server` logs bot spawn/despawn distinctly from human connect/disconnect, for operator visibility — small, low-risk, not load-bearing for any acceptance criterion.

## Appendix A: message and field reference (normative, updates to increment 6's)

**No layout changes.** One previously-reserved bit of `AircraftState.status_flags` (`uint8`, present since increment 5) is now defined:

| status_flags bit | meaning |
|---|---|
| bit 0 (`0x01`) | aircraft is bot-controlled (non-human); set by the server for bots, clear for humans |

All messages keep their increment-6 layout; `kProtocolVersion` stays 2. (Reserved for a future increment, not added here: a `ClientHello` self-declaration bit for remote bots, which *will* be a layout change and bump the version at that time.)

## Appendix B: measured findings from pre-drafting validation (informative)

**Child-process spawn and lifecycle, validated directly** (`probe_bot_parent.cpp`/`probe_bot_child.cpp` — a real `flight_server`, a parent that `fork()`/`exec()`s a minimal stand-in client):

- The forked child connected to a real running `flight_server` over real ENet/UDP, received `ServerWelcome`, and was assigned `player_id=1` — no server-side special-casing; the existing multi-client onboarding path handled it like any connection.
- `kill(pid, 0)` confirmed liveness before signalling. `SIGTERM` produced a clean, orderly disconnect (the same `volatile std::sig_atomic_t` pattern `flight_server` itself uses). `waitpid()` reaped it with exit 0; a second `kill(pid, 0)` returned `ESRCH` — no zombie.
- The server stayed healthy throughout the connect/disconnect cycle (log + explicit status check) — no crash, hang, or leaked state.

This grounds the "zero server-side special-casing" and "no zombie" claims. It validated *clean* shutdown; the *crash*-orphan case is finding m1, handled by the bot exiting on connection loss.

### Review measurements — bot flight behaviour (`probe_inc7_bot{,2,3}.cpp`)

Three probes replicated the server's exact per-tick command application (set elevator/aileron/rudder/throttle-cmd-norm from the client command each tick, leaving the trim's `pitch-trim-cmd-norm` untouched) and drove a real `FlightSession` per airframe for 90 s, flagging departure (NaN / ground / spiral > 80° / stall < 20 kt). These are what established that a bot must (a) know its own trim and (b) fly a stabilised closed-loop controller — i.e. what motivated the local-physics decision, which gives the bot both directly:

| control style | c172x | Camel | pa28 |
|---|---|---|---|
| **trim-blind** (aileron/rudder = 0, absolute throttle) | DEPARTED (spiral) | DEPARTED (spiral, 24 kt) | DEPARTED (ground) |
| trim-aware, aggressive open-loop | airborne | DEPARTED (spiral) | DEPARTED (ground) |
| trim-aware, gentle open-loop | airborne | DEPARTED (spiral) | airborne |
| trim-aware, **closed-loop bank-hold** | airborne | DEPARTED (spiral) | airborne |
| trim-aware, **stabilised** (bank error + roll-rate damping + rudder coordination) | **airborne** (bank held 16°) | **DEPARTED** (spiral 81°, 35 kt) | **airborne** (bank held 13°) |

Reading:

- **Trim-blindness departs every airframe.** JSBSim's trim writes non-zero solutions into `aileron-cmd-norm`/`rudder-cmd-norm` (c172x −0.075/−0.004; Camel +0.102/−0.064; each different) and a different throttle each (c172x 0.792, Camel 0.301). A bot that doesn't know these discards them and departs. With **local physics, the bot knows its own trim exactly** — this failure mode is designed out, not patched over.
- **Open-loop deflection is the wrong model.** Aileron commands roll rate, so fixed deflection integrates bank without bound. → closed-loop attitude-hold with rate damping is load-bearing; local physics gives it zero-latency, full-rate feedback.
- **The Camel is materially harder.** A stabilised, coordinated, trim-aware controller flew c172x and pa28 solidly but still spiralled the Camel (bank 81°, 65 → 35 kt). This does not prove the Camel unflyable — local physics plus a per-airframe-tuned controller with speed protection may well fly it — but proves a single simple airframe-blind controller does not, hence the endurance gate and the Camel caveat. (A missing-property read returns 0 quietly rather than throwing — confirmed in increment 6's review — so the bot's own-state reads are safe across airframes.)
