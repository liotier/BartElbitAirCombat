# BartElbitAirCombat

Open-source multiplayer WWII air combat game — flight dynamics core built on [JSBSim](https://github.com/JSBSim-Team/jsbsim), real-time integration on [Godot](https://godotengine.org).

## Status

Increment 4 of a derisking sequence. Increment 1 validated that JSBSim can be built, integrated as a library, and driven through the c172x aircraft configuration to produce stable, correct flight dynamics output, in isolation from any rendering, networking, or game logic. Increment 2 validated that the same flight dynamics can run inside Godot's real-time frame loop — a fixed 120 Hz physics tick decoupled from the variable render rate — with live keyboard input reaching the simulation and a minimal placeholder aircraft responding visibly. Increment 3 moved the simulation to a standalone server: a Godot-free `flight_server` owns the authoritative `FlightSession`, a client sends control inputs over a real UDP transport (ENet), and the server streams state snapshots back — but that client had no local simulation, so input always waited for a round trip. Increment 4 closes that "feel" gap: the client predicts its own aircraft locally and reconciles with the server's authoritative snapshots, so input responds immediately even under real latency. Still one client, one aircraft — multiple clients and multi-core server scaling are increment 5.

## Building and running the tests

From a clean clone:

```bash
git clone https://github.com/liotier/BartElbitAirCombat.git
cd BartElbitAirCombat && ./scripts/run_tests.sh
```

`scripts/run_tests.sh` fetches JSBSim, godot-cpp, and ENet via CMake FetchContent, builds the standalone test binary, the networking binaries, and the Godot GDExtension, downloads and caches the Godot editor binary itself, and runs increment 1's four scenarios, increment 2's five Godot-driven tests, increment 3's networked phase (one scenario re-run server-side to validate the server's own wall-clock-paced loop, a transport-fidelity check, a full-loop input-path check, protocol checks, and resilience checks through a userspace UDP relay), and increment 4's prediction/reconciliation phase: a reconstruction-gate regression test, prediction correctness on a clean link (immediate response, eventual agreement with server ground truth, bounded tracking), reconciliation correctness with its required negative control (a forced misprediction that must recover with reconciliation enabled and persist with it disabled), aggressive-analog-input resilience under 20% packet loss, and a repeat of the latency-sensitive checks under 100 ms one-way latency.

Unlike increment 1's suite, the Godot-driven and networked tests run at real wall-clock pace by design (that pacing is exactly what parts of them validate), so the full run takes a few minutes rather than under a second.

## Client/server architecture

The simulation is authoritative on the server. The client also runs the *same* simulation locally to predict its own aircraft's response immediately, then reconciles with the server's authoritative snapshots as they arrive:

```
 client, every physics tick (120 Hz):
   sample input -> apply to LOCAL FlightSession -> step -> buffer (client_seq, input, resulting state)
   -> send ControlInput{ newest client_seq, last 6 commands } (unreliable)  -> display updates immediately

 server, ordered command buffer:
   for each tick: apply the buffered command for the next expected client_seq (hold last across a gap)
   -> step authoritative FlightSession
   -> broadcast StateSnapshot{..., ack_client_seq = highest seq applied} at 30 Hz (unreliable)

 client, on receiving a StateSnapshot:
   look up buffered state at ack_client_seq -> compare to the snapshot's authoritative state
   within tolerance (0.5 m / 2 deg) -> discard old buffer entries, done
   beyond tolerance -> reconstruct the authoritative rigid-body state, replay every buffered
                       input since ack_client_seq, blend the *rendered* transform over ~150 ms
```

Only rigid-body state (position, velocity, attitude, body rates) is rewound and replayed; flight-control-system internals (actuators, filters) are not, and are left to drift by a bounded, sub-degree amount between snapshots — a deliberate simplification, not an oversight (see `docs/increment-4-specification.md`, "Status").

- **`netcore`** (`src/netcore/`) is a small, Godot-free C++ library depending only on [ENet](https://github.com/lsalzman/enet): it defines the wire message types and their little-endian, field-by-field (de)serialization (`protocol.h` is the normative reference for the byte layout), plus thin ENet host wrappers. Every participant links this same library, so they are guaranteed to speak byte-identical wire format because they compile the same serialization code.
- **`predictcore`** (`src/predictcore/`) is a Godot-free C++ library depending on `flightcore` and `netcore`: it defines `PredictedSession`, the reusable predict/buffer/reconcile/replay algorithm, and the reconstruction-gate recipe that turns a received snapshot back into a JSBSim rigid-body state. Shared unmodified by the Godot client and `flight_test_client`, so both exercise the identical reconciliation code.
- **`flight_server`** (`src/server/`) is a standalone binary with no Godot dependency. It owns the one `FlightSession`, steps it at a wall-clock-paced 120 Hz, applies the connected client's input through an ordered command buffer fed by redundant `ControlInput` packets (robust to packet loss without needing the client to resend anything extra), and broadcasts state snapshots at a configurable rate (30 Hz by default).
- **`flight_test_client`** (`src/net_test/`) is the automated, Godot-free test harness used by `run_tests.sh` (see above); `--mode prediction` runs a real `PredictedSession` against a live server.
- **The Godot client** (`PredictedAircraft` GDExtension node, subclassing `FlightAircraft`; `godot/scenes/networked.tscn`) is the human-facing client: it owns both the local predicting `FlightSession` and the network connection, sends keyboard input directly to its own inherited control-surface properties, and blends the rendered transform across any reconciliation correction rather than snapping it. `NetworkClient` + `RemoteAircraft` (increment 3's dumb, non-predicting display path) remain in the codebase unused this increment — they become relevant again once increment 5 adds other clients' aircraft to display.
- **`net_relay`** (`src/net_test/`) is a userspace UDP relay used only for testing: it forwards real datagrams between a client and the server while injecting a configurable one-way delay and drop probability, so resilience tests exercise ENet's actual behaviour under an impaired link rather than an application-level approximation of one.

### Running a server and connecting manually

Start a server (defaults to port 45300, 30 Hz snapshots):

```bash
./build/flight_server
```

Then open `godot/project.godot` in the Godot 4.5.x editor, open `scenes/networked.tscn` as the scene to run, and press Play — `PredictedAircraft` trims its own local `FlightSession` and connects to `127.0.0.1:45300` on `_ready()`. Keyboard controls (same mapping as the standalone scene below):

| Control | Keys |
|---|---|
| Pitch | W (nose down) / S (nose up) |
| Roll | A (roll left) / D (roll right) |
| Yaw | Q (left) / E (right) |
| Throttle | Page Up (increase) / Page Down (decrease) |

The aircraft starts trimmed at 5,000 ft / 100 kt, same as increments 1-3, and (unlike increment 3's non-predicting client) throttle starts at whatever this client's own local trim converges to, since it has a real local simulation to read back from.

To connect from a different machine or port, set `server_host`/`server_port` on the `PredictedAircraft` node (Inspector, or `--port`/no-flag on `flight_server` for the port it listens on).

### Flying under simulated latency

`net_relay` (see above) sits between the Godot client and the server, so you can feel prediction working under a realistic connection rather than only on localhost's near-zero latency:

```bash
./build/flight_server
./build/net_relay --listen-port 45301 --server-host 127.0.0.1 --server-port 45300 --delay-ms 100 --drop-percent 0 --seed 1
```

Then set `PredictedAircraft`'s `server_port` to `45301` (still `127.0.0.1`) before pressing Play. Input should still feel immediate — that is the local prediction responding without waiting for a round trip — and any reconciliation correction should blend in over about 150 ms rather than snap. Raise `--drop-percent` (e.g. to 20) to also exercise the redundant-command-buffer path under packet loss.

## Flying the single-process (offline) scene

`scenes/main.tscn` (increment 2, unchanged) still runs a local, non-networked `FlightAircraft` for comparison — open `godot/project.godot`, run that scene, and press Play. Same keyboard mapping as above.

## Expected output

The script prints one line per test with its `passed`, `failed`, or `error` status; any failed criterion is listed underneath with its name, measured value, and limit. The script's own exit status is `0` only when every test — increment 1's four, increment 2's five, increment 3's networked phase, and increment 4's prediction phase — passed.

Machine-readable output is written alongside the human-readable summary:

- `results/<test_name>.csv` and `results/godot_<test_name>.csv` — one row per 10 Hz sample of simulated flight state, same column schema for both
- `results/summary.json` — increment 1's structured pass/fail/error status and criteria; `results/server_summary.json` — the same, for the server-side scripted-scenario run
- `results/server_<name>.csv` and `results/client_received*.csv` — one row per broadcast/received `StateSnapshot`, keyed by `server_tick`; the transport-fidelity test compares these two field-by-field
- `results/client_predicted_*.csv` — one row per predicted physics tick, keyed by `client_seq`; increment 4's prediction tests compare this against `server_pitch_response.csv` (already produced by the physics-preservation step, same initial condition and schedule) to check eventual agreement

## Specification

- [`docs/increment-1-specification.md`](docs/increment-1-specification.md) / [`docs/increment-1-specification-review.md`](docs/increment-1-specification-review.md)
- [`docs/increment-2-specification.md`](docs/increment-2-specification.md)
- [`docs/increment-3-specification.md`](docs/increment-3-specification.md) / [`docs/increment-3-specification-review.md`](docs/increment-3-specification-review.md)
- [`docs/increment-4-specification.md`](docs/increment-4-specification.md) / [`docs/increment-4-specification-review.md`](docs/increment-4-specification-review.md) — this increment's specification and adversarial review, including the empirical findings that shaped it
- [`docs/roadmap.md`](docs/roadmap.md) — the full derisking sequence

## Licence

GPL-3.0-or-later. See [`LICENSE`](LICENSE). JSBSim (LGPL-2.1) and Godot/godot-cpp (MIT) are fetched or downloaded at build time, not redistributed in this repository.
