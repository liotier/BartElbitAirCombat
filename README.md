# BartElbitAirCombat

Open-source multiplayer WWII air combat game — flight dynamics core built on [JSBSim](https://github.com/JSBSim-Team/jsbsim).

## Status

Increment 1 of a derisking sequence. This increment validates that JSBSim can be built, integrated as a library, and driven through the c172x aircraft configuration to produce stable, correct flight dynamics output, in isolation from any rendering, networking, or game logic.

## Building and running the tests

From a clean clone:

```bash
git clone https://github.com/liotier/BartElbitAirCombat.git
cd BartElbitAirCombat && ./scripts/run_tests.sh
```

`scripts/run_tests.sh` fetches JSBSim via CMake FetchContent, builds the test binary, runs all four scenarios, and prints a summary.

## Expected output

The script prints one line per test (`trim_stability`, `pitch_response`, `roll_response`, `power_response`) with its `passed`, `failed`, or `error` status. Any failed criterion is listed underneath with its name, measured value, and limit. A final `overall: PASS` or `overall: FAIL` line summarises the run, and the script's own exit status is `0` only when every test passed.

Machine-readable output is written alongside the human-readable summary:

- `results/<test_name>.csv` — one row per 10 Hz sample of simulated flight state
- `results/summary.json` — structured pass/fail/error status and criteria for CI consumption

## Specification

- [`docs/increment-1-specification.md`](docs/increment-1-specification.md) — the specification this increment implements
- [`docs/increment-1-specification-review.md`](docs/increment-1-specification-review.md) — the review that produced it, with measured reference values from an actual JSBSim build

## Licence

GPL-3.0-or-later. See [`LICENSE`](LICENSE). JSBSim is LGPL-2.1-licensed and is fetched at build time, not redistributed in this repository.
