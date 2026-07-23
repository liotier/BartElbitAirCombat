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
