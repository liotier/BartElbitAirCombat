# Increment 3 Specification: Client–Server Hello World

## Status

Revision 2 — reviewed. Supersedes draft 1; the adversarial review that produced this revision is in `docs/increment-3-specification-review.md`, which justifies the test-architecture and impairment changes below finding by finding. This is increment 3 of the derisking sequence in `docs/roadmap.md`. It builds on increments 1 and 2: the `FlightSession` abstraction, `flightcore` library, the c172x aircraft, and the four validated flight-dynamics scenarios are reused, not reimplemented.

Before draft 1 was written, the following were verified by building and running real code (details and measured numbers in Appendix B):

- ENet v1.3.18 fetches and builds cleanly via CMake FetchContent alongside the existing JSBSim and godot-cpp dependencies; the ENet API used below compiles as written.
- Localhost UDP works in a restricted sandbox; a full ENet connection establishes and passes both reliable and unreliable packets each direction, with a measured localhost round-trip of tens of microseconds.
- Our own statically-linked ENet, compiled into a GDExtension and loaded into a running Godot process, connects to a standalone Godot-free C++ ENet server and round-trips a packet — proving no symbol collision with Godot's own bundled ENet, which was the single riskiest architectural assumption.
- Kernel-level network impairment (`tc`/`netem`) is unavailable in the sandbox, so latency/loss testing uses a userspace UDP relay proxy (see "Network impairment").

The review then reshaped how this increment is *tested*: because the server runs the identical `FlightSession`, the network layer cannot alter the physics, so re-verifying flight dynamics over the wire proves little and risks false greens on serialization bugs. The tests below instead assert **transport fidelity** (client-received state compared field-by-field against server-authoritative state), **physics preservation** (increment 1 criteria evaluated server-side where the full state exists), and the **input path** (client-sent input demonstrably changing server state) — the three things the network layer can actually get wrong.

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
│   │   ├── protocol.h / protocol.cpp       (message types, field-by-field wire (de)serialization)
│   │   ├── net_server.h / net_server.cpp   (ENet server host wrapper)
│   │   └── net_client.h / net_client.cpp   (ENet client host wrapper)
│   ├── server/
│   │   └── main.cpp                    (new: the flight_server binary — networked + scripted-scenario modes)
│   ├── net_test/
│   │   ├── main.cpp                    (new: the flight_test_client binary — fidelity + input-path harness)
│   │   └── net_relay.cpp               (new: userspace UDP relay proxy for latency/loss testing)
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

- **`netcore`** — a static C++ library depending **only on ENet** (not on JSBSim, flightcore, or Godot). It defines the message types, their little-endian field-by-field wire (de)serialization, and thin server/client ENet host wrappers. Keeping it dependency-light makes it independently testable and guarantees the server and client speak byte-identical wire format because they compile the *same* serialization code. (Network impairment for testing lives in the separate `net_relay` proxy, not in `netcore` — see "Network impairment".)
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

- **Player IDs are `uint8`.** One byte, 0 reserved for "server/none". This bounds a session to 255 players — matching, with exactly one ID to spare, Battlebit's own 254-player precedent cited above (not a "128-per-side", i.e. 256-total, target: two sides of 128 would need 256 distinct IDs, one more than a reserved-zero `uint8` can give). A deliberate, documented choice, not an accident of using a wider type; a genuine 128-per-side target would need `uint16`.
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
- The `StateSnapshot` carries an `aircraft_count` and a per-aircraft list even though increment 3 always sends exactly one. This is the deliberate "server decides what this client needs to know" shape: the server builds each client's snapshot from a per-client set of relevant aircraft. In increment 3 that set is trivially {the one aircraft}; increment 4+ makes the set selection smarter without changing the per-aircraft *record layout*. (The *packetisation* will change: at scale, 128 aircraft × ~45 B ≈ 5.7 KB exceeds a safe ~1400 B UDP datagram, so snapshots will be split across multiple datagrams — a known increment-4 concern, not a claim that the wire structure is final.)
- `pos_local_m` is in the same local East-Up-(−North) frame `computeAircraftTransform()` already uses (increment 2), relative to the session origin the server announces in `ServerWelcome`. The client feeds these straight into the validated transform function.
- Control channel (1) is unreliable: latest input wins, and a dropped input packet simply means the server holds the previous input one more tick — correct behaviour, no reliability needed. The `client_seq` lets the server ignore out-of-order stale inputs.
- Handshake/teardown (channel 0) is reliable and ordered.
- **Serialization is field-by-field** into a byte buffer, in the order and widths tabulated above. No packed struct is `memcpy`'d to or from the wire — that would reintroduce the compiler-padding and host-endianness dependence the little-endian discipline exists to remove.
- `origin_lat/lon` are `float32` (~1 m resolution at temperate latitudes). Harmless here because positions travel as *local* offsets from the origin and the increment-3 origin is (0,0); if absolute georeferencing is ever needed, widen the origin to `float64` (local offsets stay `float32`).

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
5. In **scripted-scenario mode** (used by the physics-preservation test), apply a built-in scripted input schedule to its own `FlightSession` — increment 1's scenarios, at exact sim-ticks (t=5 s = tick 600) so the trajectory reproduces increment 1 deterministically — while still broadcasting snapshots. In the default **networked mode** it applies network-received input (the full loop). Which mode is active is a config flag.
6. **Log every broadcast snapshot** to `results/server_<scenario>.csv` (the exact bytes it sent, decoded to the snapshot fields, keyed by `server_tick`). This is not optional: it is the reference the client's received log is compared against for the transport-fidelity test.
7. In scripted-scenario mode, **evaluate increment 1's pass criteria server-side** against its own full `FlightSample` at 120 Hz — exactly as `increment1_tests` does, reusing that code via `flightcore` — and report the result. This is where physics preservation is asserted, because the server has every quantity JSBSim produces (altitude, IAS, α, …) directly, with no derivation.

Exit codes mirror increments 1–2 conventions (0 clean, 1 a criterion/assertion failed, 2 init/bind/trim execution error).

## Clients

### `flight_test_client` (automated)

A standalone binary linking `netcore` (Godot-free). It is the harness for the three network-specific assertions. Time is always measured from `server_tick` carried in snapshots (the authoritative timeline), and the scenario clock anchors to the reliable `ServerWelcome`, never to an unreliable first snapshot — so criterion timing and input scheduling are robust to snapshot loss.

The client does **not** re-derive physics quantities to check physics criteria (that is done server-side; see B1 in the review). Its jobs are:

- **Transport fidelity**: log every received `StateSnapshot` (fields, keyed by `server_tick`). A comparison step then asserts, field-by-field, that the client's received log matches the server's sent log (`results/server_<scenario>.csv`) within float32 tolerance. This catches any serialization, endianness, axis-mapping or fixed-point bug exactly, because it compares the same representation on both ends rather than laundering it through loose physics criteria. This is the primary correctness test of the increment.
- **Input path** (required, full-loop): in networked mode, the client sends a scripted `ControlInput` (e.g. nose-up from server-tick ≥ some t) and the test asserts the server's authoritative state — as seen in the received snapshots — responds (e.g. pitch rises through a threshold within a bounded time after the input). This proves the client→server input path, the authority direction the increment exists to validate. It uses a short duration, not a full flight scenario.

Airspeed/α on the wire: the automated tests do not need them (physics criteria are server-side). The snapshot therefore stays the compact rigid-body record above. If the *Godot* client later wants airspeed for a HUD, adding a fixed-point `ias` field (knots × 100, `int16`) is a clean forward step — deferred to UI work, not needed here.

### Godot client (`NetworkClient` + `RemoteAircraft`, human-facing)

- `NetworkClient` (GDExtension `Node`): owns the ENet client host, connects on `_ready`, exposes methods to set the local control input and signals/【properties for the latest received snapshot. Services ENet in `_physics_process`.
- `RemoteAircraft` (GDExtension `Node3D`): given the latest snapshot for its player id, applies `pos_local_m` and `quat` directly to its transform (`Transform3D(Basis(Quaternion(...)), Vector3(...))`). No `FlightSession`, no stepping — raw application. Stutter at 30 Hz against a higher render rate is expected. (Implementation note, Appendix B: the wire already carries a local-frame position and a quaternion — the same axis convention `computeAircraftTransform()` targets, computed server-side by `geo::computeOrientationQuat()`/`geo::computeLocalOffset()` — so the client applies them directly rather than converting back through lat/lon and Euler angles just to re-derive what the server already resolved; doing so would also reintroduce the gimbal-lock fragility the quaternion exists to avoid.)
- `networked_input.gd`: same keyboard map as increment 2's `flight_input.gd`, but instead of driving a local `FlightSession` it calls `NetworkClient.set_input(...)`.
- `godot/scenes/networked.tscn`: `RemoteAircraft` with the same placeholder mesh + chase camera as increment 2, plus `NetworkClient` and the input node.

The Godot client is validated **manually** (a human starts a server, presses Play, flies it, confirms it responds with correct sign conventions and visibly-authoritative behaviour), consistent with increment 2's manual criterion. Its underlying transport path is covered automatically by `flight_test_client` (same `netcore`) and was de-risked by the pre-drafting runtime probe.

## Network impairment (testing)

`tc`/`netem` is unavailable in the target sandbox (Appendix B), and — more importantly — an application-level delay/drop queue would sit *above* ENet, so ENet's own reliability, retransmit, RTT and congestion logic would run on the real fast link and never see the impairment. That tests only the application's tolerance to delayed/dropped messages, not the transport's behaviour under an adverse network, which is a stated reason for choosing ENet.

The faithful mechanism, and the one the resilience acceptance criteria are evaluated against, is a **userspace UDP relay proxy** (`net_relay`, part of `netcore`/tools): a small process that listens on a local port, forwards datagrams to the server and back, and applies a configurable one-way delay and drop probability (seeded RNG, reproducible) to the *actual datagram stream*. The client connects to the relay instead of the server; ENet then sees real impaired UDP and reacts correctly. This needs no `NET_ADMIN` — it is ordinary userspace forwarding — and is directly reusable for increment 4's prediction testing.

Required automated tests, run through the relay:

- **Latency resilience**: with, e.g., 100 ms one-way delay, the full loop still functions, the connection stays up, snapshots keep flowing, and no NaN/divergence occurs. (Trajectory timing shifts by the delay; this asserts liveness and flow, not exact increment-1 criteria — those are asserted server-side on the clean run.)
- **Loss resilience**: with, e.g., 20% drop, the client still tracks the aircraft (each snapshot is absolute state, so loss reduces update rate but cannot accumulate error), the *reliable* handshake/teardown still complete despite drops on the same impaired link (this is the test that actually exercises ENet's retransmit — impossible with an above-ENet queue), and no divergence/NaN occurs.

## Test runner and CI

`scripts/run_tests.sh` gains a third phase after increments 1 (standalone) and 2 (Godot), all of which must still pass. The increment-3 tests are deliberately network-specific rather than a re-run of all four flight scenarios: the network cannot alter the physics (the server runs the identical `FlightSession`), so the risks worth testing are serialization, the input path, and resilience — not stall behaviour over a socket. The phase runs:

1. Build everything (adds `netcore`, `flight_server`, `flight_test_client`, `net_relay`; `flight_gdext` now also links `netcore`).
2. **Physics preservation (one scenario, server-side)**: start `flight_server` in scripted-scenario mode for a single scenario (e.g. `pitch_response`), which evaluates increment 1's criteria against its own full `FlightSample` and logs its sent snapshots. This validates the server's new wall-clock-paced loop produces a correct trajectory. Runs at wall-clock pace (~30 s for one scenario).
3. **Transport fidelity**: `flight_test_client` connects (clean localhost), logs received snapshots, and a comparison asserts them field-by-field against the server's sent log within float32 tolerance.
4. **Input path (full-loop, required)**: short test — client sends a scripted input, assert the server's received-snapshot state responds within a bounded time.
5. **Protocol**: handshake, version-mismatch rejection, clean disconnect.
6. **Resilience (through `net_relay`)**: latency and loss tests.
7. Overall exit 0 only if increments 1–2 suites *and* every increment-3 assertion pass.

Only the single physics-preservation scenario runs at full wall-clock duration; the rest are short. Combined with the (cached) builds this stays comfortably within the **20-minute** CI budget carried over from increment 2; build caching remains required. The Godot client's manual validation is not part of CI.

Server and client are separate processes talking over localhost UDP — confirmed to work in the CI-like sandbox (Appendix B). The runner must start the server before the client and guarantee it is torn down afterward (trap/kill), and should use a fixed or per-run-unique port to avoid collisions.

## Documentation

New C++ and GDScript files carry the GPL-3.0 header convention. README gains: the client/server architecture in brief, how to start a server and connect the Godot client manually, and that `run_tests.sh` now also drives the networked suite. The wire protocol table (this document's "Messages") is the normative reference; the implementer should keep a copy or a pointer to it in a header comment in `protocol.h`.

## Licence

Unchanged: GPL-3.0-or-later for this project. ENet is MIT-licensed (compatible), fetched at build time, not redistributed here. The Quake-lineage prediction/reconciliation *pattern* referenced for increment 4 is not used in this increment; no third-party netcode source is incorporated.

## Acceptance criteria

Increment 3 is complete when all hold simultaneously:

1. `scripts/run_tests.sh` on a fresh clone (clean Debian 12 / Ubuntu 24.04) exits 0 — increments 1–2 suites unchanged, and every increment-3 assertion passes: physics-preservation (one scenario, server-side, increment 1 criteria), transport-fidelity (client-received matches server-sent field-by-field), input-path (client input changes server state), protocol (handshake, version-rejection, disconnect), and resilience through `net_relay` (latency, loss).
2. The transport-fidelity comparison passes — the client received byte-faithful state — and the server-side physics-preservation scenario satisfies increment 1's criteria, so both the transport and the server's real-time loop are proven correct.
3. A human has started `flight_server`, connected the Godot client (`networked.tscn`), flown the placeholder aircraft with the keyboard, and confirmed correct, visibly server-authoritative response — documented as performed (not CI-gated), as in increment 2.
4. The GitHub Actions workflow runs to completion successfully on push within the 20-minute budget.
5. The README is sufficient for a competent developer to build, run the server, connect a client, and run the tests without additional explanation.

## Out of scope, explicitly deferred

Everything in increments 1–2 deferred lists, plus: client-side prediction / reconciliation / interpolation (increment 4), multiple clients and aircraft (increment 4+), interest-management *logic* (increment 4+), empirical tuning of the snapshot rate (increment 4), security/anti-cheat/encryption, NAT traversal / matchmaking / discovery, and any non-localhost deployment.

## Open questions for the implementer

At the implementer's discretion; document the choice. (Note: the input-path test is **not** optional — it is a required acceptance criterion; only the items below are open.)

- The exact server catch-up cap per wake, and whether to use `enet_host_service` timeouts or a separate sleep for pacing.
- Whether `RemoteAircraft` and `FlightAircraft` (increment 2) share a base class or stay separate (they diverge: one steps a sim, one applies snapshots).
- `net_relay` implementation shape (separate process vs. thread) and its RNG seed handling for reproducible CI.
- The float32 tolerance used by the transport-fidelity comparison (must be tight enough to catch a real bug, loose enough to absorb float32 round-trip — the measured quaternion round-trip error of ~0.003° and metre-scale position round-trip suggest a tolerance well under any physically meaningful error).
- Whether `flight_server` runs scripted-scenario mode as a distinct binary mode or a separate small harness reusing its loop.

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
- **Network impairment**: `tc`, `netem`, and `ip` are unavailable in the sandbox (no `NET_ADMIN`). Latency/loss testing therefore uses the userspace UDP relay proxy specified in "Network impairment", which faithfully impairs the datagram stream ENet sees (unlike an above-ENet queue) and is more deterministic for CI than kernel netem.
- **Reused transport verification path**: standalone server binary and Godot-loaded GDExtension client both linked the *same* ENet and exchanged packets — the concrete proof behind the `netcore`-shared-by-both-sides architecture.
- **float32 orientation transport** (review m2): a float32 quaternion round-trip preserves extracted Euler angles to a max **0.002°** over 200 000 random attitudes, rising only to **0.003°** at 89.9° pitch (near gimbal lock). Single-precision orientation on the wire is therefore ample; the transport-fidelity comparison tolerance can sit far below any physically meaningful error, and no smallest-three encoding is needed for precision reasons.
- **Shared orientation math, cross-checked** (implementation finding): the server must compute the wire quaternion without depending on Godot (architecture requirement, "flight_server ... no Godot"), but it must still land on exactly the rotation `computeAircraftTransform()` would show, or the Godot client's manual criterion (3) would silently render the wrong attitude with no automated test able to catch it (the automated transport-fidelity test compares client-received bytes against server-sent bytes, not against a geometric ground truth). Resolution: `computeAircraftTransform()`'s direction-cosine-matrix computation (roll/pitch/yaw → right/up/forward axes in Godot's engine convention) was factored out, unchanged, into a Godot-free function (`geo::computeBodyAxes`, `src/geo/aircraft_orientation.h`), and a new Godot-free `geo::computeOrientationQuat()` derives the equivalent quaternion from those same axes via the standard four-case (Shepperd) matrix-to-quaternion method — the same class of algorithm Godot's own `Basis::get_quaternion()` uses internally. Both `flight_aircraft.cpp` (Godot side, builds a `Basis`) and the server (Godot-free, builds the wire quaternion) now call the same axis computation, rather than two independent derivations that could silently disagree on sign or axis convention. Cross-checked numerically against Godot's actual `Basis(right, up, -forward).get_quaternion()` over a dense grid (5° steps, roll ∈ [−180°,180°], pitch ∈ [−89°,89°], yaw ∈ [−180°,180°], 191 844 points, excluding the exact ±90° pitch gimbal-lock pole common to any Euler representation): worst-case component-wise difference **1.8×10⁻⁷**, i.e. single-precision rounding noise, not a formula disagreement. (An initial check using `2·acos(dot)` as the error metric misleadingly reported ~0.056° at one grid point; `acos` is ill-conditioned near 1 and amplifies float32 rounding into an apparent angle error, so the comparison was redone component-wise, matching how the real transport-fidelity test actually compares wire fields.)
- **`enet_peer_disconnect()` immediately after a reliable send races that send** (implementation finding, found by testing, not by reasoning): the server's original `ServerReject` handling sent the rejection reliably, flushed, and then immediately called `enet_peer_disconnect()` on the same peer. Automated repeated-connection testing (15 rapid-fire `flight_test_client --mode version_reject` runs against one server) showed roughly 25–40% of runs never received the reject at all - the client's handshake wait simply timed out - despite the server's own event trace confirming it processed every `ClientHello` and queued a reply every time. The disconnect request does not guarantee the just-queued reliable packet goes out first. **Resolution**: the server now only sends the rejection and does not disconnect the peer itself; the *client*, once it has the message in hand, disconnects itself (no race, since nothing further needs to be delivered to it). Repeating the same 15-run test after this change: 15/15 passed. This is the general lesson, not specific to this one message: on this ENet version, "send a final reliable message, then hang up" from the same side that sent it is unsafe without `enet_peer_disconnect_later()` (not currently used anywhere in this codebase) or, as done here, leaving teardown to whichever side no longer needs anything delivered to it.
- **Client-commanded throttle vs. server-trimmed throttle** (implementation finding): `flight_server` does not touch any control axis until it has received at least one `ControlInput` (so a freshly connected client doesn't stomp the trim solver's chosen level-flight throttle before the player touches anything) - but once *any* input arrives, all four axes are applied together from it, since the wire format has no per-axis "leave unchanged" value. A client that starts its local throttle tracking at 0 would therefore cut power to zero the instant its first packet lands. Both `flight_test_client`'s input-path test and `networked_input.gd` start their commanded throttle at full power instead (not an attempt to reproduce the server's actual trimmed value, which the wire protocol does not expose to the client) - a deliberate, documented choice, not an oversight; a future increment adding a "hold current value" wire convention or telemetry readback could remove the need for it.
