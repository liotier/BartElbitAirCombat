# Increment 3 Specification: Client–Server Hello World

## Status

Draft, informed by empirical validation performed ahead of drafting (Appendix B). This is increment 3 of the derisking sequence in `docs/roadmap.md`. It builds on increments 1 and 2: the `FlightSession` abstraction, `flightcore` library, the c172x aircraft, and the four validated flight-dynamics scenarios are reused, not reimplemented.

Before this specification was written, the following were verified by building and running real code (details and measured numbers in Appendix B):

- ENet v1.3.18 fetches and builds cleanly via CMake FetchContent alongside the existing JSBSim and godot-cpp dependencies; the ENet API used below compiles as written.
- Localhost UDP works in a restricted sandbox; a full ENet connection establishes and passes both reliable and unreliable packets each direction, with a measured localhost round-trip of tens of microseconds.
- Our own statically-linked ENet, compiled into a GDExtension and loaded into a running Godot process, connects to a standalone Godot-free C++ ENet server and round-trips a packet — proving no symbol collision with Godot's own bundled ENet, which was the single riskiest architectural assumption.
- Kernel-level network impairment (`tc`/`netem`) is unavailable in the sandbox, so latency/loss testing must be done with an in-process impairment layer. This specification turns that constraint into a designed, first-class testing feature rather than a workaround.

## Goal

Validate that the authoritative flight simulation can run on a standalone server, driven by control inputs sent over a real network transport from a separate client, with resulting state streamed back to the client and displayed — proving the full server-authoritative loop closes over an actual socket boundary with the authority direction correct (client sends *inputs*, server owns *state*).

## Non-goals

Explicitly out of scope; must not be implemented:

- Client-side prediction, server reconciliation, or rollback of any kind (increment 4)
- Snapshot interpolation or any smoothing of received state — raw application only; visible stutter at the snapshot rate is **expected and acceptable** this increment (increment 4)
- More than one simultaneous client, or more than one aircraft (increment 4+)
- Interest management / relevance filtering logic beyond the trivial "one client, one aircraft, always relevant" case — though the *message-addressing shape* must already assume a per-client server-side decision (see "Wire protocol")
- Any anti-cheat, encryption, authentication, or DDoS mitigation
- NAT traversal, matchmaking, server discovery, or lobbies
- WWII aircraft (still c172x; later increment)
- Weapons, damage, hit detection (increment 8)
- Persistent player accounts or state
- Production deployment, containerization, or multi-host operation (all testing is localhost)

## Repository structure

Builds on increments 1–2. New and changed paths:

```
.
├── CMakeLists.txt                      (changed: adds ENet, netcore, flight_server, flight_test_client; flight_gdext gains netcore)
├── README.md                           (changed)
├── .github/workflows/ci.yml            (changed)
├── src/
│   ├── ... (increments 1-2 unchanged)
│   ├── netcore/                        (new: Godot-free networking core)
│   │   ├── protocol.h / protocol.cpp       (message types, wire (de)serialization)
│   │   ├── net_server.h / net_server.cpp   (ENet server host wrapper)
│   │   ├── net_client.h / net_client.cpp   (ENet client host wrapper)
│   │   └── impairment.h / impairment.cpp   (in-process latency/loss injection, testing only)
│   ├── server/
│   │   └── main.cpp                    (new: the flight_server binary)
│   ├── net_test/
│   │   └── main.cpp                    (new: the flight_test_client binary — scripted, criteria-evaluating)
│   └── godot_ext/
│       ├── ... (increment 2 unchanged)
│       ├── network_client.h / .cpp     (new: NetworkClient node — connects, sends input, receives snapshots)
│       └── remote_aircraft.h / .cpp    (new: Node3D that applies received snapshots to its transform)
├── godot/
│   ├── ... (increment 2 unchanged)
│   ├── scenes/networked.tscn           (new: the networked client scene)
│   └── scripts/networked_input.gd      (new: keyboard -> NetworkClient input)
├── scripts/
│   └── run_tests.sh                    (changed: adds a networked-test phase)
└── docs/
    └── increment-3-specification.md    (this document)
```

Increments 1 and 2 acceptance criteria continue to hold unchanged.

## Build environment

Same toolchain as increment 2, plus ENet (fetched, below). No new system dependencies — ENet is small, self-contained C. The `curl`/`unzip` requirement from increment 2 (Godot binary) is unchanged.

### ENet acquisition

Fetched via CMake FetchContent, pinned to the newest stable tag:

```cmake
FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.18
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(enet)
```

This exports a CMake target named `enet` (a static library `libenet.a`). Headers are included as `#include <enet/enet.h>` after adding `${enet_SOURCE_DIR}/include` to the target's include directories. Validated: configures and builds in seconds, no option or target conflict with JSBSim or godot-cpp.

ENet is chosen deliberately over Godot's high-level `MultiplayerAPI`. It is a UDP library with mixed reliable/unreliable channels, purpose-built by Lee Salzman for the Cube engine's FPS networking — the closest possible lineage to this project's needs — and using it directly (rather than Godot's high-level replication) keeps the wire protocol under our own control and, critically, keeps the **server free of any Godot dependency**.

### ENet and Godot coexistence (validated)

Godot bundles its own copy of ENet internally. Our server and client both statically link *our* ENet (v1.3.18). This does not collide: the Godot editor binary exports zero `enet_*` symbols to its dynamic symbol table, so a GDExtension's statically-linked ENet symbols resolve locally within the `.so` and are never interposed by Godot's copy. This was confirmed both by symbol-table inspection and by a runtime round-trip: a GDExtension linking our ENet, loaded into Godot, successfully connected to a standalone server and exchanged a packet (Appendix B). No special linker flags (`-Bsymbolic` etc.) are required, though the implementer may add `-fvisibility=hidden` to `netcore` as defense-in-depth.

## Architecture

Three executables/libraries, layered so the wire format is defined exactly once and shared by every participant:

- **`netcore`** — a static C++ library depending **only on ENet** (not on JSBSim, flightcore, or Godot). It defines the message types, their little-endian wire (de)serialization, thin server/client ENet host wrappers, and the testing impairment layer. Keeping it dependency-light makes it independently testable and guarantees the server and client speak byte-identical wire format because they compile the *same* serialization code.
- **`flight_server`** — a standalone C++ binary (no Godot) linking `flightcore` + `netcore`. Owns the authoritative `FlightSession`, runs it at a wall-clock-paced 120 Hz, applies received client inputs, and broadcasts state snapshots at a configurable lower rate.
- **The client** exists in two forms that share `netcore` and therefore the identical protocol:
  - **`flight_test_client`** — a standalone C++ binary (no Godot) linking `netcore`, used for automated testing. It connects, sends a *scripted* control-input schedule (increment 1's scenarios), collects received snapshots, and evaluates increment 1's pass criteria in C++. This is increment 3's analogue of increment 1's `increment1_tests` binary: headless, deterministic, fast, CI-friendly.
  - **The Godot client** — the `NetworkClient` GDExtension node plus a `RemoteAircraft` node, driven by `networked_input.gd`. This is the *human-facing* client: it captures keyboard input, sends it to the server, receives snapshots, and applies them (raw, no interpolation) to a `RemoteAircraft`'s transform via the already-validated `computeAircraftTransform()`. It runs no `FlightSession` — in increment 3 the client is a pure display of authoritative server state.

Both client forms exercise the same `netcore` protocol against the same server, so the automated C++ tests give rigorous coverage of the exact wire path the Godot client uses.

```
                         control inputs (unreliable, client->server)
   [ client ]  ─────────────────────────────────────────────►  [ flight_server ]
   keyboard / script                                            authoritative FlightSession @ 120 Hz
   apply snapshot to transform  ◄─────────────────────────────  broadcast snapshots @ 30 Hz (configurable)
                         state snapshots (unreliable, server->client)
```

## Wire protocol

All multi-byte fields are **little-endian** (documented assumption; all target platforms are x86-64). ENet does not byte-swap payloads — `netcore` owns all serialization. Every packet begins with a 1-byte message-type tag. A 1-byte protocol-version constant is exchanged in the handshake so mismatched builds fail cleanly and loudly rather than misinterpreting each other's bytes (a failure mode real projects in this space have hit).

Field-width discipline, applied from this increment even though one client exercises it trivially, because wire-format decisions become hard caps later (Battlebit's 254-player limit is a byte-indexed player ID; that lesson is taken deliberately here):

- **Player IDs are `uint8`.** One byte, 0 reserved for "server/none". This bounds a session to 255 players — comfortably beyond the 128-per-side target and a deliberate, documented choice, not an accident of using a wider type.
- **Control-axis inputs are `int16`, fixed-point** (value × 32767, clamped to axis range). A stick cannot be commanded finer than 1/32767; `int16` is a comfortable-margin narrow default. `int8` is viable and noted as a future tightening.
- Snapshot spatial fields are `float32`, not `float64` — Godot's transform math is single-precision anyway (increment 2), and single-precision covers a local play area to sub-metre precision.

### Messages

| Tag | Name | Dir | Channel | Reliability | Payload |
|----|------|-----|---------|-------------|---------|
| 1 | `ClientHello` | C→S | 0 | reliable | `uint8` protocol_version |
| 2 | `ServerWelcome` | S→C | 0 | reliable | `uint8` protocol_version, `uint8` assigned_player_id, `float32` origin_lat_deg, `float32` origin_lon_deg, `uint16` snapshot_hz |
| 3 | `ServerReject` | S→C | 0 | reliable | `uint8` reason_code (1 = version mismatch, 2 = server full) |
| 4 | `ControlInput` | C→S | 1 | unreliable | `uint32` client_seq, `int16` elevator, `int16` aileron, `int16` rudder, `int16` throttle |
| 5 | `StateSnapshot` | S→C | 1 | unreliable | `uint32` server_tick, `uint8` aircraft_count, then per aircraft: `uint8` player_id, `float32[3]` pos_local_m (E,U,−N), `float32[4]` quat, `float32[3]` vel_local_mps, `uint8` status_flags |
| 6 | `ClientBye` | C→S | 0 | reliable | (empty) |

Notes:
- The `StateSnapshot` carries an `aircraft_count` and a per-aircraft list even though increment 3 always sends exactly one. This is the deliberate "server decides what this client needs to know" shape: the server builds each client's snapshot from a per-client set of relevant aircraft. In increment 3 that set is trivially {the one aircraft}; increment 4+ makes the set selection smarter without changing the message structure.
- `pos_local_m` is in the same local East-Up-(−North) frame `computeAircraftTransform()` already uses (increment 2), relative to the session origin the server announces in `ServerWelcome`. The client feeds these straight into the validated transform function.
- Control channel (1) is unreliable: latest input wins, and a dropped input packet simply means the server holds the previous input one more tick — correct behaviour, no reliability needed. The `client_seq` lets the server ignore out-of-order stale inputs.
- Handshake/teardown (channel 0) is reliable and ordered.

### Rates (all configurable; stated defaults)

- **Simulation tick: 120 Hz**, fixed, matching JSBSim (increments 1–2). Not configurable.
- **Snapshot broadcast: 30 Hz default.** Deliberately below the sim rate to make the send-rate/sim-rate decoupling real and visible from day one. Anchored loosely to Battlebit's 60 Hz but set lower here so raw-application stutter is obvious (and acceptable — smoothing is increment 4). Tunable; the true value gets chosen empirically in increment 4 when there is more than one client to measure against.
- **Client input send: 60 Hz default.** Above snapshot rate, below sim rate.

## Server (`flight_server`)

A standalone binary. Responsibilities:

1. Parse config (listen port, snapshot rate, optional scripted-scenario mode, optional impairment settings) from command-line flags and/or environment variables.
2. Create an ENet server host. Initialize one authoritative `FlightSession` (the c172x, initialized and trimmed exactly per increment 1's sequence — the same 5000 ft / 100 kt or scenario-specific initial condition).
3. Run a **wall-clock-paced fixed-timestep loop** at 120 Hz: accumulate elapsed real time, step the `FlightSession` as many 120 Hz ticks as have elapsed (with a sane maximum catch-up per wake to avoid a spiral of death — the standalone equivalent of the cap Godot applies internally, which increment 2 measured), sleeping the remainder. On each tick, apply the most-recently-received `ControlInput` for the connected client. Every N ticks (per snapshot rate), broadcast a `StateSnapshot`.
4. Service ENet: accept a `ClientHello` (version-check → `ServerWelcome` or `ServerReject`), receive `ControlInput`, handle `ClientBye`/timeout disconnect cleanly.
5. Optionally (scripted-scenario mode, for automated tests) *ignore* network input and instead apply a built-in scripted input schedule to its own `FlightSession`, while still broadcasting snapshots — this lets a test verify the pure server→client streaming path in isolation. The default mode applies network-received input (the full loop).
6. Optionally log the authoritative trajectory to `results/server_<scenario>.csv` (increment 1 schema) for direct comparison against what the client received.

Exit codes mirror increments 1–2 conventions (0 clean, non-zero on init/bind/trim failure).

## Clients

### `flight_test_client` (automated)

A standalone binary. For a given scenario (selected by env var / flag, reusing increment 1's four scenario names), it:

1. Connects to `flight_server` on localhost, completes the handshake.
2. Runs its own 60 Hz input loop, sending the scenario's scripted `ControlInput` schedule (e.g. `pitch_response` → elevator −1.0 from t≥5 s). Time is measured from the handshake / first snapshot.
3. Collects every received `StateSnapshot`, converting each to the increment-1 quantities (altitude, IAS, pitch, bank, α, etc. — derived from the snapshot's position/quaternion/velocity, plus any fields added as needed).
4. On completion, evaluates increment 1's exact pass criteria for that scenario against the received trajectory and exits 0/1 accordingly, printing the same criterion-by-criterion breakdown style as increments 1–2.

Because localhost RTT is negligible (Appendix B) and the scripted inputs are step functions, the received trajectory is expected to reproduce increment 1's physics within the same comfortable margins increment 2 achieved — with one documented nuance: input is applied at the server on snapshot/input-rate granularity, so a "t≥5 s" transition may land within ±1 input-tick of t=5 s. Increment 1's criterion windows ([5,10] s etc.) absorb this comfortably.

Note on airspeed: the snapshot carries velocity but not JSBSim's calibrated-airspeed instrument value directly. The client derives true airspeed from the velocity vector; where a criterion is specified on indicated/calibrated airspeed, either (a) add an `ias` field to the snapshot, or (b) evaluate that criterion against true airspeed with the small documented IAS/TAS difference at 5000 ft folded into the tolerance. The implementer chooses; if (a), keep the field `int16` fixed-point (knots × 100) per the width discipline.

### Godot client (`NetworkClient` + `RemoteAircraft`, human-facing)

- `NetworkClient` (GDExtension `Node`): owns the ENet client host, connects on `_ready`, exposes methods to set the local control input and signals/【properties for the latest received snapshot. Services ENet in `_physics_process`.
- `RemoteAircraft` (GDExtension `Node3D`): given the latest snapshot for its player id, sets its transform via `computeAircraftTransform()` (validated, increment 2). No `FlightSession`, no stepping — raw application. Stutter at 30 Hz against a higher render rate is expected.
- `networked_input.gd`: same keyboard map as increment 2's `flight_input.gd`, but instead of driving a local `FlightSession` it calls `NetworkClient.set_input(...)`.
- `godot/scenes/networked.tscn`: `RemoteAircraft` with the same placeholder mesh + chase camera as increment 2, plus `NetworkClient` and the input node.

The Godot client is validated **manually** (a human starts a server, presses Play, flies it, confirms it responds with correct sign conventions and visibly-authoritative behaviour), consistent with increment 2's manual criterion. Its underlying transport path is covered automatically by `flight_test_client` (same `netcore`) and was de-risked by the pre-drafting runtime probe.

## In-process impairment layer (testing)

Because `tc`/`netem` is unavailable in the target sandbox (Appendix B), `netcore` includes an optional impairment layer, enabled only via explicit config (off in production): outgoing (and/or incoming) packets are held in a small queue stamped with a release time (added latency) and dropped with a configurable probability (loss). This is *better* than kernel netem for automated testing because it is deterministic and reproducible with a seeded RNG.

It supports two required automated tests:

- **Latency resilience**: with, e.g., 100 ms of added one-way delay, the full loop still functions and the connection stays up. (The scripted-scenario trajectory will shift in time by the delay; this test asserts connection liveness, snapshot flow, and absence of NaN/divergence — *not* exact increment-1 criteria, which are asserted only on the clean-localhost run.)
- **Loss resilience**: with, e.g., 20% loss on the unreliable channels, the client still tracks the aircraft (each snapshot is absolute state, so loss reduces update rate but cannot accumulate error), reliable handshake/teardown still complete, and no divergence/NaN occurs.

This layer is also the groundwork increment 4 needs to test prediction/reconciliation under controlled impairment.

## Test runner and CI

`scripts/run_tests.sh` gains a third phase after increments 1 (standalone) and 2 (Godot), all of which must still pass:

1. Build everything (adds `netcore`, `flight_server`, `flight_test_client`; `flight_gdext` now also links `netcore`).
2. For each of the four reused scenarios: start `flight_server` (background, clean localhost), run `flight_test_client` for that scenario, collect its exit code, stop the server.
3. Run the protocol tests: connect/handshake/version-mismatch-rejection/clean-disconnect; latency-resilience; loss-resilience.
4. Overall exit 0 only if increments 1–2 suites *and* every increment-3 scenario and protocol test pass.

Each server+client scenario runs at wall-clock pace (the server is real-time-paced), so the four reused scenarios take ~165 s combined, as in increment 2. The protocol tests are short. Combined with the (cached) builds this stays within the **20-minute** CI budget carried over from increment 2; build caching remains required. The Godot client's manual validation is not part of CI.

Server and client are separate processes talking over localhost UDP — confirmed to work in the CI-like sandbox (Appendix B). The runner must start the server before the client and guarantee it is torn down afterward (trap/kill), and should use a fixed or per-run-unique port to avoid collisions.

## Documentation

New C++ and GDScript files carry the GPL-3.0 header convention. README gains: the client/server architecture in brief, how to start a server and connect the Godot client manually, and that `run_tests.sh` now also drives the networked suite. The wire protocol table (this document's "Messages") is the normative reference; the implementer should keep a copy or a pointer to it in a header comment in `protocol.h`.

## Licence

Unchanged: GPL-3.0-or-later for this project. ENet is MIT-licensed (compatible), fetched at build time, not redistributed here. The Quake-lineage prediction/reconciliation *pattern* referenced for increment 4 is not used in this increment; no third-party netcode source is incorporated.

## Acceptance criteria

Increment 3 is complete when all hold simultaneously:

1. `scripts/run_tests.sh` on a fresh clone (clean Debian 12 / Ubuntu 24.04) exits 0 — increments 1–2 suites unchanged, all four reused scenarios pass over the network loop, and all protocol tests (handshake, version-rejection, disconnect, latency-resilience, loss-resilience) pass.
2. The four reused scenarios' received trajectories satisfy increment 1's pass criteria, demonstrating the networked authoritative loop reproduces the validated physics.
3. A human has started `flight_server`, connected the Godot client (`networked.tscn`), flown the placeholder aircraft with the keyboard, and confirmed correct, visibly server-authoritative response — documented as performed (not CI-gated), as in increment 2.
4. The GitHub Actions workflow runs to completion successfully on push within the 20-minute budget.
5. The README is sufficient for a competent developer to build, run the server, connect a client, and run the tests without additional explanation.

## Out of scope, explicitly deferred

Everything in increments 1–2 deferred lists, plus: client-side prediction / reconciliation / interpolation (increment 4), multiple clients and aircraft (increment 4+), interest-management *logic* (increment 4+), empirical tuning of the snapshot rate (increment 4), security/anti-cheat/encryption, NAT traversal / matchmaking / discovery, and any non-localhost deployment.

## Open questions for the implementer

At the implementer's discretion; document the choice:

- Whether to add an explicit `ias` field to `StateSnapshot` or derive airspeed client-side from velocity (see `flight_test_client` note).
- Whether the four automated networked scenarios drive input from the client (full-loop test, preferred) or use the server's scripted-scenario mode (streaming-only test) — or both.
- The exact server catch-up cap per wake, and whether to use `enet_host_service` timeouts or a separate sleep for pacing.
- Whether `RemoteAircraft` and `FlightAircraft` (increment 2) share a base class or stay separate (they diverge: one steps a sim, one applies snapshots).
- Impairment RNG seed handling for reproducible CI.

---

## Appendix A: message and field reference (normative)

Endianness little-endian throughout; message layouts exactly as the "Messages" table. Fixed-point encodings:

| Quantity | Wire type | Encoding |
|---|---|---|
| control axis (elevator/aileron/rudder) | `int16` | round(value × 32767), value ∈ [−1, 1] |
| throttle | `int16` | round(value × 32767), value ∈ [0, 1] |
| player id | `uint8` | 0 = server/none, 1–255 = players |
| position component | `float32` | metres, local E/U/−N frame vs. session origin |
| orientation | `float32[4]` | quaternion (x, y, z, w) |
| velocity component | `float32` | m/s, local frame |
| server_tick | `uint32` | 120 Hz tick counter since server start |
| client_seq | `uint32` | client input sequence counter |
| snapshot_hz | `uint16` | server's snapshot broadcast rate |
| indicated airspeed (if added) | `int16` | round(knots × 100) |

ENet usage surface (verified to compile/link/run, Appendix B): `enet_initialize`, `enet_deinitialize`, `enet_address_set_host`, `enet_host_create`, `enet_host_connect`, `enet_host_service`, `enet_host_flush`, `enet_host_destroy`, `enet_peer_send`, `enet_packet_create`, `enet_packet_destroy`; events `ENET_EVENT_TYPE_{CONNECT,RECEIVE,DISCONNECT,NONE}`; flag `ENET_PACKET_FLAG_RELIABLE` (omit for unreliable).

## Appendix B: measured findings from pre-drafting validation (informative)

Measured on a 4-core x86-64 container, Ubuntu 24.04, GCC 13.3.0, ENet v1.3.18, against the increment-2 Godot 4.5 build.

- **ENet FetchContent**: configures in ~3 s, builds `libenet.a` in seconds. No conflict with JSBSim/godot-cpp CMake state. The ENet C API named in Appendix A compiled as written on first attempt.
- **Localhost UDP**: a raw Python UDP round-trip succeeded (first-packet ~850 µs, thread-startup-dominated). A full ENet loopback (server + client hosts in one process) established a connection in **152 µs** and measured an application-level **round-trip of 15 µs**; one reliable and one unreliable packet were delivered correctly in each direction.
- **ENet inside Godot (the crux)**: a GDExtension linking our own ENet v1.3.18, loaded into the Godot editor binary running headless, connected to a standalone Godot-free `netcore`-style C++ server on localhost:45200 and round-tripped a `"FLIGHT"` packet (`connected=1 echoed=1`, `NETPROBE PASS`). The Godot binary exports **zero** `enet_*` symbols dynamically, so no interposition of our statically-linked copy is possible. This retires the primary architectural risk of running our own ENet in the same process as Godot's bundled one.
- **GDExtension registration**: same one-time `--headless --import` behaviour as increment 2 — it writes `extension_list.cfg` (portable `res://` paths) and then crashes in an unrelated editor-layout phase; committing `.godot/` sidesteps it, exactly as increment 2 documented.
- **Network impairment**: `tc`, `netem`, and `ip` are unavailable in the sandbox (no `NET_ADMIN`). Latency/loss testing therefore uses the in-process impairment layer specified above, which is also more deterministic for CI than kernel netem.
- **Reused transport verification path**: standalone server binary and Godot-loaded GDExtension client both linked the *same* ENet and exchanged packets — the concrete proof behind the `netcore`-shared-by-both-sides architecture.
