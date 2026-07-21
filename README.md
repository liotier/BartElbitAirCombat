# BartElbitAirCombat

Open-source multiplayer WWII air combat game — flight dynamics core built on [JSBSim](https://github.com/JSBSim-Team/jsbsim), real-time integration on [Godot](https://godotengine.org).

## Status

Increment 2 of a derisking sequence. Increment 1 validated that JSBSim can be built, integrated as a library, and driven through the c172x aircraft configuration to produce stable, correct flight dynamics output, in isolation from any rendering, networking, or game logic. Increment 2 validates that the same flight dynamics can run inside Godot's real-time frame loop — a fixed 120 Hz physics tick decoupled from the variable render rate — with live keyboard input reaching the simulation and a minimal placeholder aircraft responding visibly. Still a single aircraft, still no networking.

## Building and running the tests

From a clean clone:

```bash
git clone https://github.com/liotier/BartElbitAirCombat.git
cd BartElbitAirCombat && ./scripts/run_tests.sh
```

`scripts/run_tests.sh` fetches JSBSim and godot-cpp via CMake FetchContent, builds the standalone test binary and the Godot GDExtension, downloads and caches the Godot editor binary itself, and runs both increment 1's four scenarios and increment 2's five Godot-driven tests (the same four scenarios reused through Godot's tick loop, plus a real-time pacing check).

Unlike increment 1's suite, the Godot-driven tests run at real wall-clock pace by design (that pacing is exactly what they validate), so the full run takes a few minutes rather than under a second.

## Flying it interactively

Open `godot/project.godot` in the Godot 4.5.x editor and press Play. Keyboard controls:

| Control | Keys |
|---|---|
| Pitch | W (nose down) / S (nose up) |
| Roll | A (roll left) / D (roll right) |
| Yaw | Q (left) / E (right) |
| Throttle | Page Up (increase) / Page Down (decrease) |

The aircraft starts trimmed at 5,000 ft / 100 kt, matching increment 1's Test 1 initial condition.

## Expected output

The script prints one line per test with its `passed`, `failed`, or `error` status; any failed criterion is listed underneath with its name, measured value, and limit. The script's own exit status is `0` only when every test — increment 1's four and increment 2's five — passed.

Machine-readable output is written alongside the human-readable summary:

- `results/<test_name>.csv` and `results/godot_<test_name>.csv` — one row per 10 Hz sample of simulated flight state, same column schema for both
- `results/summary.json` — increment 1's structured pass/fail/error status and criteria

## Specification

- [`docs/increment-1-specification.md`](docs/increment-1-specification.md) / [`docs/increment-1-specification-review.md`](docs/increment-1-specification-review.md)
- [`docs/increment-2-specification.md`](docs/increment-2-specification.md) — this increment's specification, including the empirical findings that shaped it
- [`docs/roadmap.md`](docs/roadmap.md) — the full derisking sequence

## Licence

GPL-3.0-or-later. See [`LICENSE`](LICENSE). JSBSim (LGPL-2.1) and Godot/godot-cpp (MIT) are fetched or downloaded at build time, not redistributed in this repository.
