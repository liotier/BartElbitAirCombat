# Increment 7 Specification: Bots

## Status

Draft. Pre-drafting validation complete (Appendix B); not yet adversarially reviewed or implemented.

## Goal

A simple, headless version of the networked client (`flight_bot`) that populates the airspace around a server's spawn point with gently maneuvering aircraft — useful both for a server configured with a target bot count and for automated testing — architected from the start as a genuine network client, not an in-process server-side shortcut, so the "full airspace picture" perception plumbing future combat intelligence will need already exists rather than being retrofitted later.

## Non-goals

- **Any actual combat intelligence or decision-making about other aircraft.** Bots fly a fixed, simple maneuver pattern, oblivious to other players — even though they *do* receive full snapshot data about them (see Goal).
- **Difficulty levels, skill tuning, or bot "personalities."**
- **Multiple aircraft types per bot roster** — a bot flies whichever single type increment 6's server is configured for, same as any human client that session.
- **Persistent bot identity across server restarts.**
- **Any change to human-client behaviour or protocol** beyond the `PlayerLeft`-driven despawn path that already exists (increment 5).

## Architecture

### `flight_bot`: a genuinely headless client

New standalone binary, `src/bot/main.cpp` (mirroring `src/server/`'s own directory convention), Godot-free, depending only on `netcore` and `interpcore` — critically, **not** `flightcore`/JSBSim. A bot never runs its own local physics; it trusts the server's own last-broadcast `StateSnapshot` entry for its own player_id as "where am I now." Nobody is judging a bot's own responsiveness the way a human judges feel, so there is no reason to predict locally — an ordinary amount of network latency in a bot reacting to its own state is entirely fine.

Connects exactly like any real client: `ClientHello` → `ServerWelcome` (reads `assigned_player_id` and `aircraft_id`, increment 6) → a `ControlInput` loop → `StateSnapshot` handling. On receiving a chunk, **every** entry — including its own — is fed into an `interp::RemoteEntityTracker`, exactly the way `PredictedAircraft` already does for other players. The bot's own most-recent tracked entry gives it "where am I" for the maneuver logic below; every *other* entry gives it the "full airspace picture" the Goal calls for — unused by any decision logic today, but exercised end-to-end and ready for whenever combat intelligence becomes a real increment.

### Maneuver logic

A simple, deterministic, time-based repeating pattern — climb for `T1` seconds, bank into a shallow turn for `T2`, descend for `T1`, level out for `T3`, repeat — computed from elapsed time and the bot's own last-known attitude/altitude (from its own tracked entry), sent as `ControlInput` at the same cadence a human client uses. A soft bias back toward the aircraft's own trimmed level-flight attitude prevents an unbounded departure (stall, spiral) across a long, unsupervised session — not a full autopilot, just enough of a safety net that "doesn't need to be fancy" doesn't also mean "eventually crashes and stops populating the airspace."

### Server-side spawn, capacity, and refill policy

`flight_server` gains `--bots N` (target count, default 0). Unlike `--stress-aircraft` (increment 5 — deliberately outside `--max-clients` capacity accounting, frozen/trimmed, never maneuvers, exists purely for chunking-load testing), bots share the *same* player_id/capacity pool as real clients: the whole point is that a bot occupies a slot a human could otherwise take, and gets displaced the moment one does.

At startup, and after any human connect/disconnect changes the count, `flight_server` maintains `bots_active = max(0, N - humans_connected)`:

- If a human connect would exceed `--max-clients` and at least one bot currently occupies a slot, the server despawns one bot **first** — freeing its player_id and sending the standard increment-5 `PlayerLeft` broadcast every other client already handles — then proceeds with the human's normal onboarding. A human is never rejected purely because bots are occupying slots meant for them.
- If a human disconnects and `bots_active + humans_connected < N`, the server spawns a new bot to refill back toward N (per explicit decision: refill, not "stay down until restart").

"Despawn a bot" means sending that bot's own process `SIGTERM` and removing it from the server's child-process tracking table — validated directly (Appendix B): `flight_bot` already handles `SIGTERM` identically to any client's own clean disconnect, so no new server→client wire message is needed; the OS signal *is* the mechanism.

`flight_server`'s existing multi-client connection-acceptance path needs zero special-casing to accept a bot — a bot is a real ENet connection over localhost UDP to the server's own listening port, going through the identical `ClientHello`/player_id-allocation/onboarding pipeline every human client already uses (validated directly, Appendix B). The only new server-side code is: the count-maintenance policy above, and the `fork()`/`exec()`/track-pid/`waitpid`-on-exit bookkeeping.

## Wire protocol changes

None. A bot is indistinguishable on the wire from a human client — same `ClientHello`, same `ControlInput`, same `StateSnapshot`, same `PlayerLeft`. This is a direct consequence of the headless-client architecture: the entire wire-format investment of increments 3-6 is reused as-is, no new message or field.

## Test plan

1. `flight_server --bots N`, no humans connected: confirm N bot player_ids appear in a connected `flight_test_client`'s view within a few ticks of startup, each with genuinely evolving (non-frozen) altitude/attitude over time — the key thing distinguishing a bot from a `--stress-aircraft` synthetic.
2. `flight_server --bots 2 --max-clients 3`, then connect 2 real (simulated) clients: confirm bots are displaced (`PlayerLeft` fires for each) as humans take their slots, and that a third real client can still connect — total capacity is respected, not blocked by lingering bots.
3. Disconnect a human client; confirm a bot respawns to refill back toward N within a few ticks.
4. Child-process lifecycle: confirm `flight_server` cleanly terminates every bot child it spawned on its own shutdown (`SIGTERM` to `flight_server` itself) — no orphaned bot processes left running after the parent exits.
5. A bot's own `RemoteEntityTracker` genuinely receives *other* aircraft's snapshot data, not just its own — confirmed by starting 2 bots and checking (via a debug/self-test path in `flight_bot`, or an external observer) that each is aware of the other, even though neither acts on it.
6. Full `scripts/run_tests.sh` passes end-to-end across increments 1-7.

## Documentation

`README.md` gains a `--bots` usage example alongside the existing `--max-clients`/`--stress-aircraft` documentation.

## Licence

No change. `flight_bot` is new source in this repository under the same GPL-3.0-or-later as everything else; no new third-party dependency (reuses `netcore`/`interpcore`, already-fetched ENet transitively).

## Acceptance criteria

1. `flight_bot` connects to a real `flight_server` exactly like any human client, with no server-side special-casing (validated directly, Appendix B).
2. `flight_server --bots N` spawns up to N bots at startup, sharing the same player_id/capacity pool as real clients (unlike `--stress-aircraft`).
3. A human connect displaces a bot the moment a slot is needed; a human disconnect refills a bot back toward N.
4. Every spawned bot process is cleanly terminated (`SIGTERM`, `waitpid`-reaped, no zombies), both when individually despawned and when `flight_server` itself shuts down.
5. Bots fly a genuinely evolving (climb/bank/descend/level) pattern, not a frozen/trimmed state — visually and numerically distinct from `--stress-aircraft`.
6. Bots receive full snapshot data for every other aircraft, not just their own, even though nothing acts on it yet.
7. The full `scripts/run_tests.sh` suite passes end-to-end across increments 1-7.
8. *(pending human verification, matching increment 4/5/6's own equivalent criteria)* A human confirms bots visibly populate the airspace around a server's spawn point and look reasonably sane in flight — not glitchy, spinning, or frozen.

## Out of scope, explicitly deferred

Everything in increments 1-6's deferred lists, plus: combat intelligence/decision-making (a later increment, once real combat exists to react to), difficulty/personality tuning, persistent bot identity across restarts, multiple aircraft types per bot roster (increment 8's concern, applied uniformly here).

## Open questions for the implementer

- Exact values for `T1`/`T2`/`T3` (climb/turn/descend/level durations) and the maneuver's angle/rate magnitudes — "lazy," per the request, but the precise numbers are a feel/tuning question, not an architecture one. Pick reasonable small values (a few degrees of bank, a few hundred feet of altitude change) and adjust after the pending human-verification check.
- Whether `flight_server` should log bot spawn/despawn events distinctly from human connect/disconnect, for operator visibility — a small, low-risk addition, not load-bearing for anything in this spec's acceptance criteria.

## Appendix A: message and field reference (normative, updates to increment 6's)

No changes — see "Wire protocol changes" above. Every message `flight_bot` sends or receives is byte-identical to what a human client already sends or receives.

## Appendix B: measured findings from pre-drafting validation (informative)

**Child-process spawn and lifecycle, validated directly** (`probe_bot_parent.cpp`/`probe_bot_child.cpp` — a real `flight_server`, a parent process that `fork()`/`exec()`s a minimal stand-in client, no code shared with or borrowed from the eventual `flight_bot` beyond reusing `netcore`'s own `NetClient`):

- The forked child successfully connected to a real, already-running `flight_server` over real ENet/UDP, received `ServerWelcome`, and was assigned `player_id=1` — no server-side special-casing needed; the server's existing multi-client onboarding path handled it exactly like any other connection.
- The parent's `kill(pid, 0)` liveness check confirmed the child was genuinely alive and running before signalling it.
- `SIGTERM` sent from the parent was caught by the child and produced a clean, orderly disconnect — the same signal-handling shape `flight_server` itself already uses (`volatile std::sig_atomic_t` flag, checked in the main loop, `src/server/main.cpp`), confirming this pattern is safe to reuse for bot lifecycle management.
- `waitpid()` reaped the child with exit code 0. A second `kill(pid, 0)` after reaping correctly returned `ESRCH` ("No such process") — confirming no zombie process was left behind.
- The server itself (`flight_server`, a real process throughout, not a probe stand-in) remained healthy and running throughout the entire connect/disconnect cycle, confirmed via its own log and an explicit status check afterward — no crash, no hang, no leaked state.

This directly grounds the "zero server-side special-casing" and "no zombie processes" claims in the Architecture section and acceptance criteria above, rather than assuming standard POSIX process-lifecycle behaviour would simply work.
