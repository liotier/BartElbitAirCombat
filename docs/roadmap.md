# Project Roadmap: Derisking Sequence

## Status

Tentative, revisited at the start of each increment. This document exists to keep the increment sequence, its ordering rationale, and the standing design decisions in one place across a project that will span many implementation sessions and, eventually, contributors.

## Vision

A multiplayer WWII air combat game in the spirit of Battlebit Remastered: runs on modest hardware, pseudo-realistic (not simulator-grade) flight and combat, Linux-first, stylised art that keeps the tone light rather than tactical-larp-serious. JSBSim supplies the flight-model credibility Chuck Yeager's Air Combat had; Godot supplies the accessible, moderate-hardware footprint Battlebit has.

## Standing design decisions

These are carried forward across increments rather than re-decided each time. Revisit only if an increment's own review surfaces a concrete reason to.

- **Two-layer language split.** The simulation-facing layer (JSBSim integration, the GDExtension binding exposing it to the engine) is C++, matching JSBSim's native environment. The game layer above it (rules, UI, weapon/game-mode logic, entity orchestration) is **GDScript**, not C++ and not C#. Decided in increment 2 planning: GDScript is Godot's first-class, most-documented language, has no build step (hot-reload while the game runs, which matters for the gameplay-tuning code that gets rewritten dozens of times during playtesting), and is the lower-friction entry point for the kind of drive-by open-source contribution this project hopes to attract, since anyone arriving via Godot tutorials is already oriented in it. C#'s advantages (static typing, raw performance, a larger pool of developers who already know the language) matter less here than they first appear: the CPU-critical path stays in C++ regardless of the game-layer language, and most of the real difficulty in contributing to a project like this is architecture and domain understanding, not language syntax — a shallow, front-loaded cost regardless of which language is chosen.
- **CMake FetchContent, not submodules**, for C++ dependencies (JSBSim in increment 1; godot-cpp from increment 2 onward). Less to manage, and the caching behaviour means iteration doesn't refetch unchanged dependencies.
- **GPL-3.0-or-later**, with LGPL/MIT dependencies (JSBSim, godot-cpp, Godot itself) fetched at build time rather than vendored.
- **The working process**, established in increment 1 and intended to repeat for every increment: draft the increment's specification with the specifics pinned down explicitly (not left for the implementer to guess) → validate the draft's claims empirically wherever they can be checked by actually running something, rather than trusting documentation or plausible-sounding numbers → adversarial review of the draft, focused on whatever kind of wrongness that increment's domain is prone to (flight-dynamics judgment for increment 1; real-time-architecture and threading judgment for increment 2; netcode-specific failure modes — prediction, reconciliation, interest management — for the networking increments) → implement against the hardened spec → verify against the spec's own acceptance criteria → commit.

## The increment sequence

Numbers 2 and 8 were fixed in increment 1's specification (Godot/real-time/rendering/input, and weapons/damage respectively). Everything between is this document's proposal, provisional, and expected to shift as each increment's own review surfaces things the plan didn't anticipate — increment 2 planning already did this once, see below.

1. **JSBSim standalone integration** — done. Validated that JSBSim produces stable, correct flight dynamics for a well-documented aircraft (c172x), driven purely programmatically, in isolation from rendering, networking, or game logic.
2. **Godot real-time integration** — in planning. Fixed-timestep JSBSim loop decoupled from Godot's variable render rate, GDExtension binding reusing increment 1's `FlightSession` abstraction, minimal placeholder rendering, live keyboard/joystick input mapped to `fcs/*` properties. Single aircraft, no networking. Success = flyable, frame-rate-independent, responsive, runs on modest hardware.
3. **Client-server hello world** — authoritative server runs the increment-2 simulation for one aircraft; one client renders it over a real transport. No prediction/interpolation sophistication yet — just prove the architecture closes the loop over an actual network boundary. Transport choice (Godot's ENet-based high-level multiplayer API vs. a custom protocol) gets decided here.
4. **Multiplayer scaling and feel** — multiple concurrent aircraft/clients; server-side performance validation (how many JSBSim instances per tick); client-side interpolation/prediction under simulated latency, jitter, and packet loss. Likely the highest technical risk in the whole sequence; the review pass here should be explicitly netcode-focused rather than general.
5. **WWII aircraft migration** — swap c172x for a real fighter (GPL-licensed FlightGear JSBSim configs are the likely source), re-run an increment-1-style empirical validation harness against it, then a second handling-distinct type to prove the swap path generalises rather than accidentally only working once.
6. **Multiple aircraft types concurrently, plus bots** — Battlebit-likes lean on AI-filled lobbies to avoid feeling empty at low player counts; scoped here, with a split into its own increment if bot behaviour proves a bigger lift than expected.
7. **Damage model foundations** — how battle damage manifests inside JSBSim's own state (engine damage → thrust, control-surface damage → authority, structural failure thresholds), deliberately separated from *how* damage gets applied over the network.
8. **Weapons, ballistics, hit detection, combat netcode** — guns, projectile/hitscan modelling suited to WWII gun velocities and ranges, lag-compensated hit registration tying into increment 7's damage model.

**Beyond increment 8**, this stops being a derisking sequence and becomes ordinary production: more aircraft, maps, game modes, UI, audio, matchmaking. Not part of this roadmap; it begins once the technical risk backbone above is retired.

## Explicitly deferred, unchanged from increment 1

Sound, music, GUI/menus beyond what an increment strictly needs to demonstrate itself, and persistence/save-load all remain out of scope until a later increment names them explicitly.
