# BartElbitAirCombat

Open-source multiplayer WWII air combat game — flight dynamics core built on [JSBSim](https://github.com/JSBSim-Team/jsbsim), real-time integration on [Godot](https://godotengine.org).

## Status

Increment 3 of a derisking sequence. Increment 1 validated that JSBSim can be built, integrated as a library, and driven through the c172x aircraft configuration to produce stable, correct flight dynamics output, in isolation from any rendering, networking, or game logic. Increment 2 validated that the same flight dynamics can run inside Godot's real-time frame loop — a fixed 120 Hz physics tick decoupled from the variable render rate — with live keyboard input reaching the simulation and a minimal placeholder aircraft responding visibly. Increment 3 moves the simulation to a standalone server: a Godot-free `flight_server` owns the authoritative `FlightSession`, a client sends control inputs over a real UDP transport (ENet), and the server streams state snapshots back. Still one client, one aircraft, no prediction or interpolation — those are increment 4.

## Building and running the tests

From a clean clone:

```bash
git clone https://github.com/liotier/BartElbitAirCombat.git
cd BartElbitAirCombat && ./scripts/run_tests.sh
```

`scripts/run_tests.sh` fetches JSBSim, godot-cpp, and ENet via CMake FetchContent, builds the standalone test binary, the networking binaries, and the Godot GDExtension, downloads and caches the Godot editor binary itself, and runs increment 1's four scenarios, increment 2's five Godot-driven tests, and increment 3's networked phase: one scenario re-run server-side to validate the server's own wall-clock-paced loop, a transport-fidelity check (received state compared field-by-field against what the server actually sent), a full-loop input-path check (a client-sent control input demonstrably changing the server's authoritative state), protocol checks (version-mismatch rejection, clean disconnect), and resilience checks run through a userspace UDP relay that injects latency and packet loss into the real datagram stream.

Unlike increment 1's suite, the Godot-driven and networked tests run at real wall-clock pace by design (that pacing is exactly what parts of them validate), so the full run takes a few minutes rather than under a second.

## Client/server architecture

The simulation is authoritative on the server; clients only send control inputs and display whatever state the server streams back:

```
                         control inputs (unreliable, client->server)
   [ client ]  ─────────────────────────────────────────────►  [ flight_server ]
   keyboard / script                                            authoritative FlightSession @ 120 Hz
   apply snapshot to transform  ◄─────────────────────────────  broadcast snapshots @ 30 Hz (configurable)
                         state snapshots (unreliable, server->client)
```

- **`netcore`** (`src/netcore/`) is a small, Godot-free C++ library depending only on [ENet](https://github.com/lsalzman/enet): it defines the wire message types and their little-endian, field-by-field (de)serialization (`protocol.h` is the normative reference for the byte layout), plus thin ENet host wrappers. Both the server and every client form link this same library, so they are guaranteed to speak byte-identical wire format because they compile the same serialization code.
- **`flight_server`** (`src/server/`) is a standalone binary with no Godot dependency. It owns the one `FlightSession`, steps it at a wall-clock-paced 120 Hz, applies the connected client's most recent control input, and broadcasts state snapshots at a configurable rate (30 Hz by default).
- **`flight_test_client`** (`src/net_test/`) is the automated, Godot-free test harness used by `run_tests.sh` (see above).
- **The Godot client** (`NetworkClient` + `RemoteAircraft` GDExtension nodes, `godot/scenes/networked.tscn`) is the human-facing client: it sends keyboard input to the server and applies received snapshots directly to a placeholder aircraft's transform, with no local physics simulation at all. Raw application, no smoothing — visible stutter at the snapshot rate is expected this increment.
- **`net_relay`** (`src/net_test/`) is a userspace UDP relay used only for testing: it forwards real datagrams between a client and the server while injecting a configurable one-way delay and drop probability, so resilience tests exercise ENet's actual behaviour under an impaired link rather than an application-level approximation of one.

### Running a server and connecting manually

Start a server (defaults to port 45300, 30 Hz snapshots):

```bash
./build/flight_server
```

Then open `godot/project.godot` in the Godot 4.5.x editor, open `scenes/networked.tscn` as the scene to run, and press Play — `NetworkClient` connects to `127.0.0.1:45300` on `_ready()`. Keyboard controls (same mapping as the standalone scene below):

| Control | Keys |
|---|---|
| Pitch | W (nose down) / S (nose up) |
| Roll | A (roll left) / D (roll right) |
| Yaw | Q (left) / E (right) |
| Throttle | Page Up (increase) / Page Down (decrease) |

The aircraft starts trimmed at 5,000 ft / 100 kt, same as increments 1-2. Throttle starts at full power rather than zero (see `godot/scripts/networked_input.gd`) since the client cannot read back the server's actual trimmed throttle before its first input arrives.

To connect from a different machine or port, set `server_host`/`server_port` on the `NetworkClient` node (Inspector, or `--port`/no-flag on `flight_server` for the port it listens on).

## Flying the single-process (offline) scene

`scenes/main.tscn` (increment 2, unchanged) still runs a local, non-networked `FlightAircraft` for comparison — open `godot/project.godot`, run that scene, and press Play. Same keyboard mapping as above.

## Expected output

The script prints one line per test with its `passed`, `failed`, or `error` status; any failed criterion is listed underneath with its name, measured value, and limit. The script's own exit status is `0` only when every test — increment 1's four, increment 2's five, and increment 3's networked phase — passed.

Machine-readable output is written alongside the human-readable summary:

- `results/<test_name>.csv` and `results/godot_<test_name>.csv` — one row per 10 Hz sample of simulated flight state, same column schema for both
- `results/summary.json` — increment 1's structured pass/fail/error status and criteria; `results/server_summary.json` — the same, for the server-side scripted-scenario run
- `results/server_<name>.csv` and `results/client_received*.csv` — one row per broadcast/received `StateSnapshot`, keyed by `server_tick`; the transport-fidelity test compares these two field-by-field

## Specification

- [`docs/increment-1-specification.md`](docs/increment-1-specification.md) / [`docs/increment-1-specification-review.md`](docs/increment-1-specification-review.md)
- [`docs/increment-2-specification.md`](docs/increment-2-specification.md)
- [`docs/increment-3-specification.md`](docs/increment-3-specification.md) / [`docs/increment-3-specification-review.md`](docs/increment-3-specification-review.md) — this increment's specification and adversarial review, including the empirical findings that shaped it
- [`docs/roadmap.md`](docs/roadmap.md) — the full derisking sequence

## Licence

GPL-3.0-or-later. See [`LICENSE`](LICENSE). JSBSim (LGPL-2.1) and Godot/godot-cpp (MIT) are fetched or downloaded at build time, not redistributed in this repository.
