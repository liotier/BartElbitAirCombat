# Increment 7 Specification: Bots

## Status

Revision 4. Pre-drafting validation (process/lifecycle) plus adversarial review (`docs/increment-7-specification-review.md`) folded in, then the review's one open architecture decision resolved by the project owner: **bots run their own local JSBSim** (a true headless client, not a physics-less one — see "The local-physics decision" below), along with the owner's capacity/load model and non-human marking (revision 3). Revision 4 adds two owner-directed refinements from the bot-interface discussion: **one unified bot interface** — local *and* remote bots self-declare identically via `ClientHello`, so there is a single server-facing bot interface and a structurally guaranteed level playing field — and a **clean bot core / intelligence seam**, the internal boundary that becomes the eventual public third-party bot SDK (WASM-sandboxed; a named future increment). Not yet implemented.

## Goal

`flight_bot`: a genuinely headless version of the networked client — it runs the *same* local `FlightSession` + `PredictedSession` a human's Godot client runs (just without Godot, rendering, or input), sends `ControlInput`, and receives the full snapshot picture. It populates the airspace around a server's spawn point with gently maneuvering aircraft, and is a genuine network client rather than an in-process server-side shortcut, so both the "full airspace picture" perception and the local flight-dynamics model that future combat intelligence needs are present from the start rather than retrofitted.

### The local-physics decision (resolves review finding B1's fork)

The adversarial review measured that a *physics-less* bot cannot keep aircraft airborne: it can't self-determine its airframe's trim (which the server overwrites every tick) and can only read its own state through a 100 ms-delayed interpolating tracker, so it departs within a maneuver cycle (Appendix B). The owner resolved the review's fork toward **giving the bot a real local `FlightSession`**, for a reason beyond just fixing that: the later air-combat increment will "bestow some air-combat smarts unto the bot," and those smarts require the bot to apply its own aircraft's flight-dynamics model to optimise energy (deliberately fuzzed by a per-bot handicap factor). A bot with no physics could never do that. Local physics is therefore load-bearing for the project's direction, not merely the cheaper way to keep this increment's bots airborne — so it is adopted now.

Consequences, all positive for this increment:
- The bot knows its own trim exactly, from its own trim solve — **no server-seeded trim** (revision 2 needed it; the local-physics bot does not). The only wire change this increment carries is the small `ClientHello` bot-declaration flag from the unified-interface refinement below, not anything physics-related.
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

Connects like any client: `ClientHello` (with the bot-declaration flag set — see "One unified bot interface") → `ServerWelcome` (reads `assigned_player_id`, `aircraft_id`) → a `ControlInput` loop → `StateSnapshot` handling. Every *other* aircraft's snapshot entry feeds an `interp::RemoteEntityTracker` (the "full airspace picture", unused by decision logic today, ready for future combat AI). The bot's **own** control loop reads its own **local predicted state** (zero latency, full rate) — the whole point of local physics — not a received snapshot and not the interpolating tracker.

### The core / intelligence seam (the future SDK boundary, drawn now)

`flight_bot` is built as two clearly-separated parts with a narrow interface between them, even though in this increment both are C++ and both are ours:

- **The bot *core* (BartElbit-supplied platform):** everything that talks to the game — the ENet client, protocol serialization, the local `FlightSession`/`PredictedSession`, the `interpcore` perception buffer, reconciliation, lifecycle, and the flight primitives (the attitude-hold/energy autopilot the maneuver logic uses). It is C++ necessarily, because it owns JSBSim. It knows nothing decision-specific.
- **The bot *intelligence* (the decision logic):** a module behind a narrow API — *given the current world (own dynamics/energy state + the airspace picture), return a control intent.* This increment's intelligence is the simple maneuver controller below, written in C++ against that API.

The seam is deliberate and load-bearing for the project's direction, not incidental structure: it is the exact boundary that becomes the **public third-party bot SDK** — the core is the platform we publish, the intelligence is what a third party (or we) writes against it. Drawing it cleanly now, while it is all C++ and all ours, means the future increment that publishes the SDK and adds a **WASM-sandboxed** intelligence host (so untrusted competition bots can run safely, in any source language, all seeing the *same* core-computed energy state — the guarantee that keeps the field level) can do so without touching the core. Two rules make the seam real from day one: the core carries no `if (this particular maneuver)` logic, and the intelligence never reaches past the API to the ENet/JSBSim internals directly. Nothing WASM-related is *built* here; only the seam that makes it cheap later. See "Out of scope" for the deferred SDK increment.

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

### One unified bot interface (local and remote bots are the same thing)

There is exactly one server-facing bot interface, and both local and remote bots use it identically — the owner's requirement, for simplicity and for a level playing field. Concretely:

- **Every bot self-declares** by setting a bot flag in its `ClientHello` — local bots (which the server forks) set it just like remote bots (which the server cannot fork and so could not otherwise recognise). The server therefore has a *single* code path for "this peer is a bot", and its fork-tracking table is now purely a *lifecycle* concern (which processes it owns and must reap), no longer the source of truth for what is a bot. This is why the flag is brought forward into this increment rather than deferred with remote hosting: it is what makes the interface genuinely single from day one.
- **The level playing field is structural, not policed.** Because *every* bot — local or remote — is a separate process that sees the world only through the wire protocol, no bot can gain an informational or timing edge from where it runs. A local bot on loopback has no access to server internals a remote bot lacks; the separate-process design (chosen originally for the perception plumbing) is what guarantees this. The only real difference between a local and a remote bot is deployment location, which affects **server-host CPU cost** — a local bot runs its JSBSim on the server host (two instances there), a remote bot runs it on its own machine (one instance on the server host, like a human) — not the interface.

**Remote / third-party bot hosting itself is still deferred** (the "bot-programmer competition scene"): what this increment builds is *local* bots, but on the unified interface, so a future increment adds remote bots by (a) publishing the bot core + intelligence-seam as an SDK, (b) hosting third-party (WASM-sandboxed) intelligence, and (c) teaching the CPU-leveling model that remote bots cost one server-host instance, not two — with **no change to the server-facing interface**, which is already what a remote bot would use.

### Marking bots as non-human (revised: server sets the bit from the self-declaration)

The non-human marker stays one bit of `AircraftState.status_flags` (the `uint8` increment 5 reserved), which the server sets for any peer that declared itself a bot in `ClientHello` — uniform for local and remote bots. The Godot client's `RemoteAircraftSpawner` reads it and gives bot aircraft a visually distinct marker from human players.

## Wire protocol changes

**One small layout change: a bot-declaration flag in `ClientHello`, which bumps `kProtocolVersion` to 3.** `ClientHello` gains a single `uint8` `client_flags` field (bit 0 = "I am a bot"); a human client leaves it clear, every bot sets it. This is the unified-interface refinement (above) — bringing it into this increment, rather than deferring it, is a deliberate reversal of revision 3's "no wire change", made because a single bot interface is worth the bump. Per increment 6's standing rule, the layout change bumps the version (2 → 3).

The non-human *marker* remains a defined bit of the existing `AircraftState.status_flags` (no layout change there). The local-physics decision still means **no server-seeded trim** — the only new wire content this increment adds is the one `ClientHello` flag byte. `ControlInput`, `StateSnapshot`, `ServerWelcome`, `PlayerLeft` layouts are unchanged.

## Test plan

1. **Airborne-endurance gate (the acceptance test that matters, review finding B1)** — the bot, running its real local `FlightSession` + controller, keeps its aircraft airborne (no ground contact, spiral, or stall) for **≥ 3 minutes** of continuous unsupervised maneuvering, per airframe in the bot fleet. Developed and passed *before* the human "looks sane" check. c172x and pa28 are tractable with a stabilised controller (Appendix B); the Camel is gated on this test, not assumed (Out of scope).
2. `flight_server --max-bots N`, no humans: N bot player_ids appear in a connected `flight_test_client`'s view within a few ticks, each **marked non-human** (status_flags bit) and with genuinely evolving *and bounded/airborne* altitude/attitude — the real distinction from a frozen `--stress-aircraft`.
3. Capacity/CPU-leveling model: with `--max-bots B --max-players P`, connect humans past P and confirm (a) the first P humans add on top of the bots (airspace grows toward B+P), (b) humans beyond P each displace one bot (airspace holds at B+P), (c) the displacing human's slot is allocated only after the bot's disconnect is processed (finding M1), and (d) a human is never rejected while a bot occupies a slot.
4. Disconnect a human below the P threshold; confirm a bot refills back toward B.
5. Child-process lifecycle, clean: `SIGTERM` to `flight_server` terminates every bot child, no orphans (Appendix B).
6. Child-process lifecycle, **crash** (finding m1): `SIGKILL` the server and confirm its bots exit on their own (connection loss), no orphans.
7. A bot's `RemoteEntityTracker` receives *other* aircraft's snapshot data, not just its own (start 2 bots; each is aware of the other) — the perception path for future combat AI.
8. Unified interface + marking, end to end: a bot's `ClientHello` carries the bot flag, the server sets the non-human `status_flags` bit on that aircraft from it, and a human/`flight_test_client` connection (flag clear) is not marked — confirming the single "is a bot" path and that the marking derives from the self-declaration, not from fork-knowledge.
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
6. **One unified bot interface**: every bot (local — and, structurally, any future remote bot) self-declares via the `ClientHello` bot flag; the server has a single "is a bot" code path, and its fork-tracking is purely lifecycle. Bot aircraft carry the non-human `status_flags` bit end to end (set from that declaration); human aircraft do not; the Godot client renders bots distinctly.
7. **The bot is built as a clean core / intelligence split**: the core (connection, physics, perception, flight primitives) carries no decision-specific logic and the intelligence reaches the game only through the core's narrow API — the seam that becomes the future public SDK. `kProtocolVersion` is 3 (the `ClientHello` bot flag).
8. Bots fly a genuinely evolving *and bounded/airborne* pattern — distinct from both a frozen `--stress-aircraft` and from an aircraft "evolving" into a spiral.
9. Bots receive full snapshot data for every other aircraft (the future-combat-AI perception path), even though nothing acts on it yet.
10. The full `scripts/run_tests.sh` suite passes end-to-end across increments 1-7.
11. *(pending human verification)* A human confirms bots visibly populate the airspace, are clearly distinguishable from human players, and look reasonably sane in flight — backed by the automated endurance gate, so no human is asked to judge an aircraft that is quietly departing.

## Out of scope, explicitly deferred

Everything in increments 1-6's deferred lists, plus: combat intelligence / energy-management AI and the per-bot handicap factor (a later increment — local physics is built now *for* it, but no smarts here); the **bot platform / third-party bot SDK** — publishing the bot core + intelligence seam as a public API, hosting remote third-party bots, the WASM-sandboxed intelligence runtime, the "bot-programmer competition" scene, and any scoring/sandboxing of third-party code (its own named future increment, `docs/roadmap.md` "Bot platform and third-party bots"); difficulty/personality tuning; persistent bot identity across restarts; multiple aircraft types per bot roster (increment 8). Note: this increment *does* build the unified server-facing bot interface (the `ClientHello` self-declaration) and the internal core/intelligence seam, precisely so that future increment adds remote bots and the SDK without reopening either.

**The Camel in the bot fleet is gated, not assumed** (review finding B1): a stabilised controller flew c172x and pa28 solidly but still spiralled the Camel in testing (Appendix B) — a low-power, low-speed, spiral-prone WWI biplane trimmed near its own energy margin is a materially harder control target. Local physics gives the controller its best possible inputs (exact trim, zero-latency state), which may be enough where the physics-less version was not; but if a conservatively-tuned controller still cannot pass the endurance gate for the Camel, the first bot implementation ships with the GA airframes only (c172x, pa28) and the Camel is deferred from the *bot* fleet — the "shelve the hard airframe, revisit post-demo" discipline increment 6 applied to p51d/dr1/L17. The Camel remains a fully valid *human*-flown airframe; this is only about the simple bot autopilot flying it unsupervised.

## Open questions for the implementer

- Exact `T1`/`T2`/`T3` durations, target bank/pitch magnitudes, and controller gains — a feel/tuning question settled against the endurance gate, not architecture. Reasonable small values (≈15° bank, a few hundred feet) then tuned until the gate passes for each fleet airframe.
- Whether the "double JSBSim on the server host per local bot" cost (2B instances at full bot load) wants a lower default `--max-bots` than a human-only server's `--max-clients` would suggest — a provisioning default, informed by increment 5's per-aircraft step-cost measurements, not a correctness question.
- Whether `flight_server` logs bot spawn/despawn distinctly from human connect/disconnect, for operator visibility — small, low-risk, not load-bearing for any acceptance criterion.

## Appendix A: message and field reference (normative, updates to increment 6's)

**One layout change.** `ClientHello` gains a `client_flags` `uint8`; one previously-reserved bit of `AircraftState.status_flags` (`uint8`, present since increment 5) is now defined:

`ClientHello`, updated layout:

| Field | Wire type | Notes |
|---|---|---|
| `protocol_version` | `uint8` | now 3 |
| `client_flags` | `uint8` | **new**; bit 0 (`0x01`) = "I am a bot" (self-declaration, set by every bot local or remote; clear for humans). Other bits reserved. |

`AircraftState.status_flags` bit definition:

| status_flags bit | meaning |
|---|---|
| bit 0 (`0x01`) | aircraft is bot-controlled (non-human); set by the server for any peer whose `ClientHello` declared it a bot, clear for humans |

`ServerWelcome`, `ControlInput`, `StateSnapshot`, `ServerReject`, `PlayerLeft` keep their increment-6 layout. **`kProtocolVersion` becomes 3** (the `ClientHello` layout change, per increment 6's standing rule).

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
