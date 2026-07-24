# Pending human verification

Items that need a person with a screen, hands, and (sometimes) a joystick —
not something the agent can do from this environment. Checked off here once
actually performed, with a note on what was found. This is separate from
`docs/roadmap.md` (which tracks the increment sequence): entries here can
span increments and stay open across sessions until someone with a PC does
the check.

## Open

- [ ] **Increment 4 acceptance criterion 5** — fly `PredictedAircraft`
  through an artificially-latent connection and confirm input feels
  immediate with no jarring visual snap on correction. Steps are in
  README.md, "Flying under simulated latency" (`flight_server` +
  `net_relay --delay-ms 100`, then point `PredictedAircraft.server_port`
  at the relay). What's already been verified by the agent, so this is
  purely the qualitative "does it feel right" judgement, not a
  connectivity check: a headless Godot run of `networked.tscn` against a
  live server connects, predicts, and reconciles with no errors
  (docs/increment-4-specification.md Appendix B), and the identical
  underlying algorithm is exercised automatically by
  `flight_test_client --mode prediction` under 100ms latency and 20% loss
  in `scripts/run_tests.sh`.

- [ ] **Increment 5 acceptance criterion 7** — run at least two Godot clients
  simultaneously (or one Godot client alongside a `flight_test_client
  --mode multiclient`-simulated peer) and confirm *other* players'
  aircraft render smoothly via `interpcore` — no visible stutter on
  ordinary snapshot jitter, no obvious "snap." Steps are in README.md,
  "Flying with other players." What's already been verified by the
  agent, so this is purely the qualitative "does it look right"
  judgement, not a correctness check: `flight_test_client --mode
  multiclient` connects several real clients against a live server and
  confirms, in `scripts/run_tests.sh`, distinct player-ID assignment, no
  cross-talk between clients' own aircraft (measured: opposite-signed
  elevator input produces opposite-signed altitude deltas), live
  `interpcore`-based tracking of other clients' aircraft within ~5 m of
  their raw broadcast altitude, and chunked-snapshot correctness with
  zero duplicates at aircraft counts spanning the chunking boundary
  (16/17/30/32 total aircraft). The interpolation math and data path are
  exercised and correct end to end; only the visual smoothness judgement
  itself needs a human with a display.

- [ ] **Increment 6 acceptance criterion 10** — fly a Camel- or pa28-
  configured session in the real Godot client (`./build/flight_server
  --aircraft camel` or `pa28`, then `./scripts/start_client.sh --aircraft
  camel`/`pa28`) and confirm it actually looks and feels sane, not just
  "doesn't crash" — Camel specifically is the airframe most likely to feel
  marginal, since its climb plateaus near stall under sustained full-aft
  elevator (docs/increment-6-specification.md Appendix B). What's already
  been verified by the agent, so this is purely the qualitative judgement:
  both airframes load and trim via the exact validated catalog IC
  (`catalog_tests`), fly a full networked session with no NaN/divergence/
  crash, and pass the same airframe-independent prediction criteria
  (immediate response, bounded-envelope tracking, forced-desync recovery)
  as c172x, all exercised automatically in `scripts/run_tests.sh`.

- [ ] **Increment 7 acceptance criterion 11** — start a server with
  `--max-bots` set (e.g. `./build/flight_server --max-bots 3 --max-players
  8`), connect a Godot client (`./scripts/start_client.sh`), and confirm
  bots visibly populate the airspace, are clearly distinguishable from
  human players (the `RemoteAircraftSpawner`'s bot marker), and look
  reasonably sane in flight for each catalog airframe. What's already been
  verified by the agent, so this is purely the qualitative judgement, not
  a liveness/correctness check: the airborne-endurance gate
  (`bot_endurance_gate_test`) confirms all three airframes (c172x, Camel,
  pa28) stay airborne and bounded for well over the required 3 minutes
  with the real maneuver controller; `flight_test_client --mode
  observe_bots` confirms bots appear promptly, carry the non-human
  `status_flags` marker, and genuinely evolve (not a frozen
  `--stress-aircraft`); and the capacity/displacement/refill model and
  clean/crash child-process lifecycle are all exercised automatically in
  `scripts/run_tests.sh`. Camel is included in the bot fleet (a
  shallower-bank, gentler-pitch maneuver profile than the GA airframes
  keeps it airborne - see `docs/increment-7-specification.md` Appendix B
  and `bot::profileForToken`), so this check should cover all three, not
  just c172x/pa28.

- [ ] **Real joystick/HOTAS input, once implemented** — not built yet (see
  "Not yet scheduled" below). Once it is: confirm a real device's axes map
  sensibly (twist/pedal rudder, throttle slider), that Godot's default SDL
  gamepad mapping recognizes the device or that a custom mapping via
  `Input.add_joy_mapping()` was needed, and that analog input feels good
  in the air - the automated tests only exercise scripted/synthetic input
  schedules, never a physical stick.

## Not yet scheduled

- Joystick/HOTAS input support itself (reading raw `Input.get_joy_axis()`
  values in `predicted_input.gd` alongside the current keyboard mapping),
  and later, per-device joystick profiles/settings (axis remapping, dead
  zones, curves) - explicitly deferred per-user discussion, not a current
  focus. Adding the entry above once it lands.
