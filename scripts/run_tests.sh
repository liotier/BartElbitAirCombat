#!/usr/bin/env bash
# Copyright (C) 2026 The BartElbitAirCombat Project
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

# Orchestrates the full validation pipeline: increment 1's standalone suite
# (docs/increment-1-specification.md, "Test runner"), the five Godot-driven
# tests (docs/increment-2-specification.md, "Test scenarios"), the
# increment-3 networked phase (docs/increment-3-specification.md, "Test
# runner and CI"): physics preservation, transport fidelity, the input
# path, protocol (handshake/version-reject/disconnect), and resilience
# through net_relay - then the increment-4 prediction/reconciliation phase
# (docs/increment-4-specification.md, "Test plan"): the reconstruction-gate
# regression test, prediction correctness on a clean link, reconciliation
# correctness with its required negative control, aggressive-analog
# resilience under loss, and a repeat under injected latency - then the
# increment-5 multi-core/multi-client phase (docs/increment-5-
# specification.md, "Test plan"): interpcore's own unit tests, the MTU
# fragmentation-threshold regression guard, multi-client correctness
# (distinct player IDs, no cross-talk, live remote-entity tracking,
# capacity/slot-reuse), and chunked-StateSnapshot correctness at the
# chunk-boundary aircraft counts - then the increment-6 multi-airframe
# phase (docs/increment-6-specification.md, "Test plan"): the standalone
# catalog load+trim regression test, per-airframe (Camel, pa28)
# aircraft_id round-trip + sustained flight + the airframe-independent
# prediction criteria, aircraft-type mismatch behaviour (default
# disconnect, override, matched-silent), a multiclient re-run under a
# non-c172x type, and the scripted-mode guard - then the increment-7 bot
# phase (docs/increment-7-specification.md, "Test plan"): the airborne-
# endurance gate, --max-bots bot appearance/marking/liveness, bot
# RemoteEntityTracker cross-awareness, the --max-bots/--max-players
# capacity/CPU-leveling model (displacement, refill, the terminal
# kServerFull case), and clean/crash child-process lifecycle (no orphans
# either way). All pass/fail evaluation happens inside the test binaries
# or the headless test driver script; this script only configures,
# builds, fetches Godot, invokes everything, starts/stops the server and
# relay processes it needs, and relays exit status.
#
# Exit codes: 0 all tests passed, 1 a test failed, 2 a required tool is
# missing, 3 cmake configure failed, 4 the build failed, 5 the increment 1
# binary reported an execution error, 6 fetching the Godot editor failed.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_DIR="build"
RESULTS_DIR="results"
TEST_BINARY="$BUILD_DIR/increment1_tests"

# increment 2 spec, "Godot editor binary acquisition": fetched and cached by
# this script, not installed by hand, and never built from source.
GODOT_VERSION="4.5-stable"
GODOT_ASSET="Godot_v${GODOT_VERSION}_linux.x86_64.zip"
GODOT_URL="https://github.com/godotengine/godot/releases/download/${GODOT_VERSION}/${GODOT_ASSET}"
GODOT_CACHE_DIR=".godot-tools"
GODOT_BIN="$GODOT_CACHE_DIR/Godot_v${GODOT_VERSION}_linux.x86_64"

for tool in cmake git curl unzip; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: required tool '$tool' not found on PATH" >&2
    exit 2
  fi
done
if ! command -v c++ >/dev/null 2>&1 \
    && ! command -v g++ >/dev/null 2>&1 \
    && ! command -v clang++ >/dev/null 2>&1; then
  echo "error: no C++ compiler (c++, g++ or clang++) found on PATH" >&2
  exit 2
fi

mkdir -p "$BUILD_DIR"

echo "== Configuring =="
if ! cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release; then
  echo "error: cmake configure failed" >&2
  exit 3
fi

echo "== Building =="
NPROC="$( (command -v nproc >/dev/null 2>&1 && nproc) || echo 2)"
if ! cmake --build "$BUILD_DIR" --parallel "$NPROC"; then
  echo "error: build failed" >&2
  exit 4
fi

if [ ! -x "$GODOT_BIN" ]; then
  echo "== Fetching Godot ${GODOT_VERSION} =="
  mkdir -p "$GODOT_CACHE_DIR"
  if ! curl -fsSL -o "$GODOT_CACHE_DIR/godot.zip" "$GODOT_URL"; then
    echo "error: failed to download Godot editor from $GODOT_URL" >&2
    echo "       verify the exact release asset name at https://github.com/godotengine/godot/releases" >&2
    exit 6
  fi
  unzip -o -q "$GODOT_CACHE_DIR/godot.zip" -d "$GODOT_CACHE_DIR"
  rm -f "$GODOT_CACHE_DIR/godot.zip"
  chmod +x "$GODOT_BIN"
fi

rm -rf "$RESULTS_DIR"
mkdir -p "$RESULTS_DIR"

echo "== Running increment 1 tests =="
set +e
"$TEST_BINARY"
BINARY_STATUS=$?
set -e

case "$BINARY_STATUS" in
  0) ;;
  2)
    echo "error: increment 1 test binary reported an execution error" >&2
    exit 5
    ;;
  *) ;;
esac

echo "== Running Godot-driven tests =="
GODOT_OVERALL_STATUS=0
for scenario in trim_stability pitch_response roll_response power_response realtime_pacing; do
  echo "-- $scenario --"
  set +e
  TEST_SCENARIO="$scenario" "$GODOT_BIN" --headless --path godot \
    --scene res://scenes/headless_test.tscn
  scenario_status=$?
  set -e
  if [ "$scenario_status" -ne 0 ]; then
    GODOT_OVERALL_STATUS=1
  fi
done

echo "== Running increment 3 networked tests =="

FLIGHT_SERVER="$BUILD_DIR/flight_server"
FLIGHT_TEST_CLIENT="$BUILD_DIR/flight_test_client"
NET_RELAY="$BUILD_DIR/net_relay"
PREDICTCORE_TESTS="$BUILD_DIR/predictcore_tests"
INTERPCORE_TESTS="$BUILD_DIR/interpcore_tests"
CATALOG_TESTS="$BUILD_DIR/catalog_tests"
BOT_ENDURANCE_GATE_TEST="$BUILD_DIR/bot_endurance_gate_test"
NET_PORT=45300
RELAY_PORT=45301

CURRENT_SERVER_PID=""
CURRENT_RELAY_PID=""
# Increment-3 spec, "Test runner and CI": the runner must guarantee the
# server (and relay) are torn down even if a step fails or the script
# exits unexpectedly.
cleanup_net_procs() {
  # Plain "[ -n "$X" ] && cmd" would make this function's own return
  # status nonzero (and abort the whole script under set -e) whenever
  # $X happens to be empty, since that is the common case here (no relay
  # running) - the if-form's condition is exempt from set -e, and "|| true"
  # covers a kill/wait racing an already-dead process.
  if [ -n "$CURRENT_RELAY_PID" ]; then
    kill "$CURRENT_RELAY_PID" 2>/dev/null || true
    wait "$CURRENT_RELAY_PID" 2>/dev/null || true
  fi
  if [ -n "$CURRENT_SERVER_PID" ]; then
    kill "$CURRENT_SERVER_PID" 2>/dev/null || true
    wait "$CURRENT_SERVER_PID" 2>/dev/null || true
  fi
  CURRENT_SERVER_PID=""
  CURRENT_RELAY_PID=""
}
trap cleanup_net_procs EXIT

# Step: physics preservation (one scenario, server-side). Runs at full
# wall-clock pace (~30s); no client needed, flight_server evaluates
# increment 1's criteria itself and reports pass/fail via exit code.
echo "-- physics preservation (server-side pitch_response) --"
set +e
"$FLIGHT_SERVER" --scenario pitch_response
PHYSICS_STATUS=$?
set -e

# Steps: transport fidelity, input path, and (half of) protocol - one
# networked-mode server, one flight_test_client connection covers
# fidelity+input-path+clean-disconnect, a second short connection covers
# version-mismatch rejection.
echo "-- transport fidelity + input path + protocol --"
"$FLIGHT_SERVER" --port "$NET_PORT" --snapshot-hz 30 --log-name networked &
CURRENT_SERVER_PID=$!
sleep 1

set +e
"$FLIGHT_TEST_CLIENT" --mode fidelity_input --host 127.0.0.1 --port "$NET_PORT" \
  --server-log "$RESULTS_DIR/server_networked.csv" \
  --received-log "$RESULTS_DIR/client_received.csv"
FIDELITY_STATUS=$?

"$FLIGHT_TEST_CLIENT" --mode version_reject --host 127.0.0.1 --port "$NET_PORT"
PROTOCOL_STATUS=$?
set -e

cleanup_net_procs

# Step: resilience - latency. A fresh server + net_relay imposing a fixed
# one-way delay on the real datagram stream (spec, "Network impairment");
# asserts liveness/flow/no-NaN, not exact increment-1 criteria.
echo "-- resilience: latency (100ms one-way) --"
"$FLIGHT_SERVER" --port "$NET_PORT" --snapshot-hz 30 --log-name resilience_latency &
CURRENT_SERVER_PID=$!
sleep 1
"$NET_RELAY" --listen-port "$RELAY_PORT" --server-host 127.0.0.1 --server-port "$NET_PORT" \
  --delay-ms 100 --drop-percent 0 --seed 12345 &
CURRENT_RELAY_PID=$!
sleep 0.5

set +e
"$FLIGHT_TEST_CLIENT" --mode resilience --host 127.0.0.1 --port "$RELAY_PORT" \
  --received-log "$RESULTS_DIR/client_received_latency.csv"
LATENCY_STATUS=$?
set -e

cleanup_net_procs

# Step: resilience - loss. Same shape, 20% drop instead of delay; this is
# the test that actually exercises ENet's reliable-channel retransmit
# (impossible with an above-ENet impairment queue - spec review M1).
echo "-- resilience: loss (20% drop) --"
"$FLIGHT_SERVER" --port "$NET_PORT" --snapshot-hz 30 --log-name resilience_loss &
CURRENT_SERVER_PID=$!
sleep 1
"$NET_RELAY" --listen-port "$RELAY_PORT" --server-host 127.0.0.1 --server-port "$NET_PORT" \
  --delay-ms 0 --drop-percent 20 --seed 12345 &
CURRENT_RELAY_PID=$!
sleep 0.5

set +e
"$FLIGHT_TEST_CLIENT" --mode resilience --host 127.0.0.1 --port "$RELAY_PORT" \
  --received-log "$RESULTS_DIR/client_received_loss.csv"
LOSS_STATUS=$?
set -e

cleanup_net_procs

NET3_OVERALL_STATUS=0
for status in "$PHYSICS_STATUS" "$FIDELITY_STATUS" "$PROTOCOL_STATUS" "$LATENCY_STATUS" "$LOSS_STATUS"; do
  if [ "$status" -ne 0 ]; then
    NET3_OVERALL_STATUS=1
  fi
done

echo "== Running increment 4 prediction tests =="

# Step: the reconstruction-gate regression test (docs/increment-4-
# specification.md, "Reconstruction gate", acceptance criterion 1) - pure
# JSBSim, no server/client needed.
echo "-- reconstruction gate (regression test) --"
set +e
"$PREDICTCORE_TESTS"
GATE_STATUS=$?
set -e

# Step: prediction correctness on a clean link - tests 1 (immediate
# response), 2 (eventual agreement), 4 (bounded envelope tracking).
# Reuses server_pitch_response.csv, already produced above by the
# physics-preservation step (identical IC + schedule, so it is valid
# ground truth here too).
echo "-- prediction: step schedule, clean link --"
"$FLIGHT_SERVER" --port "$NET_PORT" --snapshot-hz 30 --log-name networked_predict_step &
CURRENT_SERVER_PID=$!
sleep 1

set +e
"$FLIGHT_TEST_CLIENT" --mode prediction --host 127.0.0.1 --port "$NET_PORT" \
  --input-schedule step --duration-s 8 \
  --predicted-log "$RESULTS_DIR/client_predicted_step.csv" \
  --ground-truth-log "$RESULTS_DIR/server_pitch_response.csv"
PREDICT_STEP_STATUS=$?
set -e

cleanup_net_procs

# Step: reconciliation correctness and its required negative control
# (test 3, review finding M1) - a deterministic forced misprediction
# (docs/increment-4-specification.md's "Open questions": an artificial
# state offset, chosen over probabilistic loss-forcing so this is not
# flaky), once with reconciliation enabled (must recover) and once
# disabled (must persist - proving the corrective code, not chance, is
# what fixes the state).
echo "-- prediction: forced misprediction, reconciliation on --"
"$FLIGHT_SERVER" --port "$NET_PORT" --snapshot-hz 30 --log-name networked_predict_forced_on &
CURRENT_SERVER_PID=$!
sleep 1

set +e
"$FLIGHT_TEST_CLIENT" --mode prediction --host 127.0.0.1 --port "$NET_PORT" \
  --input-schedule step --duration-s 8 --force-desync --reconciliation on \
  --predicted-log "$RESULTS_DIR/client_predicted_forced_on.csv" \
  --ground-truth-log "$RESULTS_DIR/server_pitch_response.csv"
PREDICT_FORCED_ON_STATUS=$?
set -e

cleanup_net_procs

echo "-- prediction: forced misprediction, reconciliation off (negative control) --"
"$FLIGHT_SERVER" --port "$NET_PORT" --snapshot-hz 30 --log-name networked_predict_forced_off &
CURRENT_SERVER_PID=$!
sleep 1

set +e
"$FLIGHT_TEST_CLIENT" --mode prediction --host 127.0.0.1 --port "$NET_PORT" \
  --input-schedule step --duration-s 8 --force-desync --reconciliation off \
  --predicted-log "$RESULTS_DIR/client_predicted_forced_off.csv" \
  --ground-truth-log "$RESULTS_DIR/server_pitch_response.csv"
PREDICT_FORCED_OFF_STATUS=$?
set -e

cleanup_net_procs

# Step: aggressive-analog resilience under loss (test 5, review finding
# B2) - the test that would have caught the increment-3 "most recent
# input" server model's desync under a realistic (joystick-like)
# controller; verifies the redundant-command-buffer fix actually holds.
echo "-- prediction: aggressive analog, 20% loss --"
"$FLIGHT_SERVER" --port "$NET_PORT" --snapshot-hz 30 --log-name networked_predict_analog_loss &
CURRENT_SERVER_PID=$!
sleep 1
"$NET_RELAY" --listen-port "$RELAY_PORT" --server-host 127.0.0.1 --server-port "$NET_PORT" \
  --delay-ms 0 --drop-percent 20 --seed 54321 &
CURRENT_RELAY_PID=$!
sleep 0.5

set +e
"$FLIGHT_TEST_CLIENT" --mode prediction --host 127.0.0.1 --port "$RELAY_PORT" \
  --input-schedule analog --duration-s 8 \
  --predicted-log "$RESULTS_DIR/client_predicted_analog_loss.csv"
PREDICT_ANALOG_LOSS_STATUS=$?
set -e

cleanup_net_procs

# Step: repeat the immediate-response / bounded-tracking assertions under
# 100 ms one-way latency (test 6; also acceptance criterion 3, which
# specifically requires >=100 ms injected latency for "immediate
# response" to be a meaningful assertion rather than something a
# non-predicting client would also pass). --skip-eventual-agreement:
# test 2 compares against server_pitch_response.csv, a *zero-latency*
# standalone ground truth - under real injected latency the server's own
# authoritative trajectory is genuinely, persistently behind that (it can
# only apply a command once it actually arrives), so every reconciliation
# pulls this client toward a lagged truth. That is correct behaviour, not
# a bug, but it breaks test 2's comparison premise (confirmed by direct
# measurement); immediate-response and bounded-tracking, which this step
# exists to repeat under latency, do not depend on that premise and are
# still asserted.
echo "-- prediction: step schedule, 100ms latency --"
"$FLIGHT_SERVER" --port "$NET_PORT" --snapshot-hz 30 --log-name networked_predict_step_latency &
CURRENT_SERVER_PID=$!
sleep 1
"$NET_RELAY" --listen-port "$RELAY_PORT" --server-host 127.0.0.1 --server-port "$NET_PORT" \
  --delay-ms 100 --drop-percent 0 --seed 54321 &
CURRENT_RELAY_PID=$!
sleep 0.5

set +e
"$FLIGHT_TEST_CLIENT" --mode prediction --host 127.0.0.1 --port "$RELAY_PORT" \
  --input-schedule step --duration-s 8 --skip-eventual-agreement \
  --predicted-log "$RESULTS_DIR/client_predicted_step_latency.csv" \
  --ground-truth-log "$RESULTS_DIR/server_pitch_response.csv"
PREDICT_STEP_LATENCY_STATUS=$?
set -e

cleanup_net_procs
trap - EXIT

NET4_OVERALL_STATUS=0
for status in "$GATE_STATUS" "$PREDICT_STEP_STATUS" "$PREDICT_FORCED_ON_STATUS" \
              "$PREDICT_FORCED_OFF_STATUS" "$PREDICT_ANALOG_LOSS_STATUS" \
              "$PREDICT_STEP_LATENCY_STATUS"; do
  if [ "$status" -ne 0 ]; then
    NET4_OVERALL_STATUS=1
  fi
done

echo "== Running increment 5 concurrency/scaling tests =="
trap cleanup_net_procs EXIT

# Step: interpcore unit tests (docs/increment-5-specification.md, "Test
# plan" items 1-4) - synthetic data, no server/client needed.
echo "-- interpcore unit tests --"
set +e
"$INTERPCORE_TESTS"
INTERPCORE_STATUS=$?
set -e

# Step: the MTU fragmentation-threshold regression test (test-plan item 9)
# - pure serialization-size check, no server/client needed. Guards against
# a future AircraftState field silently pushing a full chunk over the
# unreliable-send threshold (Appendix B's MTU finding).
echo "-- MTU regression (kMaxAircraftPerChunk stays under the fragmentation threshold) --"
set +e
"$FLIGHT_TEST_CLIENT" --mode mtu_regression
MTU_STATUS=$?
set -e

# Step: multi-client correctness - distinct player-ID assignment, no
# cross-talk between clients' own aircraft, live interpcore-based tracking
# of other clients' aircraft (test-plan items 5, 8), plus capacity
# (kServerFull) and slot reuse after a disconnect (test 6) - a small
# --max-players (increment 7 renamed increment 5's --max-clients; 0 bots
# here, so it behaves identically) so the 4th connection attempt in this
# same run is a real over-capacity case.
echo "-- multiclient: distinct IDs, no cross-talk, live tracking, capacity/slot-reuse --"
"$FLIGHT_SERVER" --port "$NET_PORT" --snapshot-hz 30 --max-players 3 \
  --log-name networked_multiclient &
CURRENT_SERVER_PID=$!
sleep 1

set +e
"$FLIGHT_TEST_CLIENT" --mode multiclient --host 127.0.0.1 --port "$NET_PORT" \
  --num-clients 3 --test-capacity
MULTICLIENT_STATUS=$?
set -e

cleanup_net_procs

# Step: chunking correctness at scale (test-plan item 7) - a server
# started with --stress-aircraft (synthetic, unpiloted aircraft counted
# separately from real clients, review finding M2) alongside 2 real
# clients, checked at the chunk-boundary values a ceil(N/16) computation
# is most likely to get wrong (review finding m1): exactly one full chunk
# (16), one chunk plus a straggler (17), and exactly two full chunks (32),
# plus a count comfortably past the boundary (30).
NET5_CHUNK_STATUS=0
for total in 16 17 30 32; do
  stress=$((total - 2))
  echo "-- chunking: $total total aircraft (2 real + $stress stress) --"
  "$FLIGHT_SERVER" --port "$NET_PORT" --snapshot-hz 30 --max-players 8 \
    --stress-aircraft "$stress" --log-name "networked_chunk_${total}" &
  CURRENT_SERVER_PID=$!
  sleep 1.5

  set +e
  "$FLIGHT_TEST_CLIENT" --mode multiclient --host 127.0.0.1 --port "$NET_PORT" \
    --num-clients 2 --expect-total-aircraft "$total"
  chunk_status=$?
  set -e
  if [ "$chunk_status" -ne 0 ]; then
    NET5_CHUNK_STATUS=1
  fi

  cleanup_net_procs
done

trap - EXIT

NET5_OVERALL_STATUS=0
for status in "$INTERPCORE_STATUS" "$MTU_STATUS" "$MULTICLIENT_STATUS" \
              "$NET5_CHUNK_STATUS"; do
  if [ "$status" -ne 0 ]; then
    NET5_OVERALL_STATUS=1
  fi
done

echo "== Running increment 6 multi-airframe tests =="
trap cleanup_net_procs EXIT

# Step: standalone load+trim regression test (test-plan item 1) - pure
# JSBSim, no server/client needed. Pins down c172x/Camel/pa28's exact
# catalog LoadModel string + IC as a regression guard.
echo "-- catalog load+trim regression (c172x, Camel, pa28) --"
set +e
"$CATALOG_TESTS"
CATALOG_STATUS=$?
set -e

# Steps: per-airframe pipeline generalisation (test-plan items 2-3) - for
# each of Camel and pa28: confirm ServerWelcome.aircraft_id round-trips
# correctly (via --mode aircraft_mismatch's matched-pair path), that the
# connection flies for a sustained period with no NaN/crash (--mode
# resilience, airframe-agnostic - it builds no local FlightSession), and
# the airframe-independent prediction criteria (immediate response,
# bounded-envelope tracking, forced-desync recovery with its negative
# control) - the increment's central claim (review finding M1).
# --skip-eventual-agreement throughout: that check's ground truth
# (server_pitch_response.csv) is c172x-only (test-plan item 3).
NET6_PREDICT_STATUS=0
for ac in camel pa28; do
  echo "-- aircraft_id round-trip + sustained flight: --aircraft $ac --"
  "$FLIGHT_SERVER" --aircraft "$ac" --port "$NET_PORT" --snapshot-hz 30 \
    --log-name "networked_${ac}" &
  CURRENT_SERVER_PID=$!
  sleep 1

  set +e
  "$FLIGHT_TEST_CLIENT" --mode aircraft_mismatch --host 127.0.0.1 --port "$NET_PORT" \
    --aircraft "$ac"
  ac_id_status=$?
  "$FLIGHT_TEST_CLIENT" --mode resilience --host 127.0.0.1 --port "$NET_PORT" \
    --received-log "$RESULTS_DIR/client_received_${ac}.csv"
  ac_sustained_status=$?
  set -e
  if [ "$ac_id_status" -ne 0 ] || [ "$ac_sustained_status" -ne 0 ]; then
    NET6_PREDICT_STATUS=1
  fi

  cleanup_net_procs

  echo "-- prediction (immediate response, bounded envelope): --aircraft $ac --"
  "$FLIGHT_SERVER" --aircraft "$ac" --port "$NET_PORT" --snapshot-hz 30 \
    --log-name "networked_${ac}_predict_step" &
  CURRENT_SERVER_PID=$!
  sleep 1

  set +e
  "$FLIGHT_TEST_CLIENT" --mode prediction --host 127.0.0.1 --port "$NET_PORT" \
    --aircraft "$ac" --input-schedule step --duration-s 8 --skip-eventual-agreement \
    --predicted-log "$RESULTS_DIR/client_predicted_${ac}_step.csv"
  ac_step_status=$?
  set -e
  if [ "$ac_step_status" -ne 0 ]; then
    NET6_PREDICT_STATUS=1
  fi

  cleanup_net_procs

  echo "-- prediction (forced misprediction, reconciliation on): --aircraft $ac --"
  "$FLIGHT_SERVER" --aircraft "$ac" --port "$NET_PORT" --snapshot-hz 30 \
    --log-name "networked_${ac}_predict_forced_on" &
  CURRENT_SERVER_PID=$!
  sleep 1

  set +e
  "$FLIGHT_TEST_CLIENT" --mode prediction --host 127.0.0.1 --port "$NET_PORT" \
    --aircraft "$ac" --duration-s 8 --skip-eventual-agreement \
    --force-desync --reconciliation on \
    --predicted-log "$RESULTS_DIR/client_predicted_${ac}_forced_on.csv"
  ac_forced_on_status=$?
  set -e
  if [ "$ac_forced_on_status" -ne 0 ]; then
    NET6_PREDICT_STATUS=1
  fi

  cleanup_net_procs

  echo "-- prediction (forced misprediction, reconciliation off - negative control): --aircraft $ac --"
  "$FLIGHT_SERVER" --aircraft "$ac" --port "$NET_PORT" --snapshot-hz 30 \
    --log-name "networked_${ac}_predict_forced_off" &
  CURRENT_SERVER_PID=$!
  sleep 1

  set +e
  "$FLIGHT_TEST_CLIENT" --mode prediction --host 127.0.0.1 --port "$NET_PORT" \
    --aircraft "$ac" --duration-s 8 --skip-eventual-agreement \
    --force-desync --reconciliation off \
    --predicted-log "$RESULTS_DIR/client_predicted_${ac}_forced_off.csv"
  ac_forced_off_status=$?
  set -e
  if [ "$ac_forced_off_status" -ne 0 ]; then
    NET6_PREDICT_STATUS=1
  fi

  cleanup_net_procs
done

# Step: mismatch behaviour (test-plan item 4, review finding M3) - a
# --aircraft camel server, tried against a mismatched (pa28-configured,
# then pa28-configured-with-override) client and a matched (camel-
# configured) one.
echo "-- aircraft-type mismatch: warn+disconnect by default, override, matched-silent --"
"$FLIGHT_SERVER" --aircraft camel --port "$NET_PORT" --snapshot-hz 30 \
  --log-name networked_mismatch &
CURRENT_SERVER_PID=$!
sleep 1

set +e
"$FLIGHT_TEST_CLIENT" --mode aircraft_mismatch --host 127.0.0.1 --port "$NET_PORT" \
  --aircraft pa28
MISMATCH_DEFAULT_STATUS=$?
"$FLIGHT_TEST_CLIENT" --mode aircraft_mismatch --host 127.0.0.1 --port "$NET_PORT" \
  --aircraft pa28 --allow-aircraft-mismatch
MISMATCH_OVERRIDE_STATUS=$?
"$FLIGHT_TEST_CLIENT" --mode aircraft_mismatch --host 127.0.0.1 --port "$NET_PORT" \
  --aircraft camel
MISMATCH_MATCHED_STATUS=$?
set -e

cleanup_net_procs

# Step: increment 5's multiclient test, re-run with --aircraft camel
# (test-plan item 5) - confirms the airframe choice is orthogonal to
# everything increment 5 built, not merely individually compatible.
echo "-- multiclient re-run: --aircraft camel --"
"$FLIGHT_SERVER" --aircraft camel --port "$NET_PORT" --snapshot-hz 30 --max-players 3 \
  --log-name networked_multiclient_camel &
CURRENT_SERVER_PID=$!
sleep 1

set +e
"$FLIGHT_TEST_CLIENT" --mode multiclient --host 127.0.0.1 --port "$NET_PORT" \
  --aircraft camel --num-clients 3 --test-capacity
MULTICLIENT_CAMEL_STATUS=$?
set -e

cleanup_net_procs
trap - EXIT

# Step: scripted-mode guard (test-plan item 6, review finding m1) - a
# non-c172x --aircraft combined with --scenario must be rejected at
# startup, not silently run through c172x-tuned pass thresholds.
echo "-- scripted-mode guard: --aircraft camel --scenario pitch_response is rejected --"
set +e
"$FLIGHT_SERVER" --aircraft camel --scenario pitch_response
GUARD_EXIT=$?
set -e
if [ "$GUARD_EXIT" -eq 0 ]; then
  echo "error: --aircraft camel --scenario pitch_response should have been rejected but exited 0" >&2
  GUARD_STATUS=1
else
  GUARD_STATUS=0
fi

NET6_OVERALL_STATUS=0
for status in "$CATALOG_STATUS" "$NET6_PREDICT_STATUS" "$MISMATCH_DEFAULT_STATUS" \
              "$MISMATCH_OVERRIDE_STATUS" "$MISMATCH_MATCHED_STATUS" \
              "$MULTICLIENT_CAMEL_STATUS" "$GUARD_STATUS"; do
  if [ "$status" -ne 0 ]; then
    NET6_OVERALL_STATUS=1
  fi
done

echo "== Running increment 7 bot tests =="
trap cleanup_net_procs EXIT

# Step: airborne-endurance gate (test-plan item 1, the acceptance test
# this increment lives or dies by) - the real bot::ManeuverController
# against a real FlightSession, standalone, no server/client needed.
echo "-- bot airborne-endurance gate (c172x, Camel, pa28; >=180s each) --"
set +e
"$BOT_ENDURANCE_GATE_TEST"
ENDURANCE_STATUS=$?
set -e

# Step: --max-bots with no other humans - bot player_ids appear, each
# marked non-human and genuinely evolving/bounded (test-plan item 2), the
# real distinction from a frozen --stress-aircraft. --max-players 1 gives
# the observing flight_test_client connection itself room to sit "on top
# of" the bots without displacing one (this connection is unavoidably a
# real, non-bot peer from the server's point of view).
echo "-- --max-bots 3: bots appear, marked non-human, evolving --"
"$FLIGHT_SERVER" --max-bots 3 --max-players 1 --port "$NET_PORT" --snapshot-hz 30 \
  --log-name networked_bots_only &
CURRENT_SERVER_PID=$!
sleep 2

set +e
"$FLIGHT_TEST_CLIENT" --mode observe_bots --host 127.0.0.1 --port "$NET_PORT" \
  --expect-bot-count 3 --duration-s 10
OBSERVE_BOTS_STATUS=$?
set -e

cleanup_net_procs

# Step: RemoteEntityTracker cross-awareness (test-plan item 7) - two bots,
# each aware of the other via its own perception path (future combat AI's
# eventual input). Bots inherit the server's stdout across fork() and log
# their own tracked-other count once per simulated second.
echo "-- bot RemoteEntityTracker cross-awareness (2 bots aware of each other) --"
"$FLIGHT_SERVER" --max-bots 2 --max-players 0 --port "$NET_PORT" --snapshot-hz 30 \
  --log-name networked_bots_tracking > "$RESULTS_DIR/server_bots_tracking.log" 2>&1 &
CURRENT_SERVER_PID=$!
sleep 3
if grep -q "tracked_others=1" "$RESULTS_DIR/server_bots_tracking.log"; then
  BOT_TRACKING_STATUS=0
else
  echo "error: no bot reported tracking another bot" >&2
  BOT_TRACKING_STATUS=1
fi
echo "bot_cross_awareness: $([ "$BOT_TRACKING_STATUS" -eq 0 ] && echo PASS || echo FAIL)"

cleanup_net_procs

# Step: capacity/CPU-leveling model (test-plan item 3) - bots fill to B
# when empty, humans add on top up to P then displace bots one-for-one
# (the displacing human's slot allocated only after the bot's actual
# disconnect is processed, finding M1), total never exceeds B+P, and the
# terminal kServerFull case fires only once all B+P slots are human.
# Reuses increment 5's own multiclient machinery (distinct IDs - so no
# double-allocation ever occurred - no cross-talk, capacity/slot-reuse)
# plus the humans_not_marked_bot check (test-plan item 8).
echo "-- capacity/CPU-leveling: --max-bots 2 --max-players 3 (B+P=5) --"
"$FLIGHT_SERVER" --max-bots 2 --max-players 3 --port "$NET_PORT" --snapshot-hz 30 \
  --log-name networked_bots_capacity &
CURRENT_SERVER_PID=$!
sleep 2

set +e
"$FLIGHT_TEST_CLIENT" --mode multiclient --host 127.0.0.1 --port "$NET_PORT" \
  --num-clients 5 --test-capacity --expect-total-aircraft 5
CAPACITY_STATUS=$?
set -e

# Step: a human disconnect refills a bot (test-plan item 4) - the
# multiclient run above already disconnected every one of its clients by
# the time it returned, so the server should already have reconciled back
# toward --max-bots by now.
sleep 1
set +e
"$FLIGHT_TEST_CLIENT" --mode observe_bots --host 127.0.0.1 --port "$NET_PORT" \
  --expect-bot-count 2 --duration-s 6
REFILL_STATUS=$?
set -e

cleanup_net_procs
trap - EXIT

# Step: child-process lifecycle, clean (test-plan item 5) - SIGTERM to
# flight_server terminates every bot child, no orphans. flight_server's
# own shutdown path blocks until every bot child actually exits before it
# returns, so no orphan is possible by the time `wait` below unblocks;
# the brief sleep is only for the OS process table to catch up for `ps`.
echo "-- child-process lifecycle: clean SIGTERM, no orphans --"
"$FLIGHT_SERVER" --max-bots 3 --max-players 0 --port "$NET_PORT" --snapshot-hz 30 \
  --log-name networked_bots_shutdown_clean &
CLEAN_SERVER_PID=$!
sleep 2
kill -TERM "$CLEAN_SERVER_PID" || true
wait "$CLEAN_SERVER_PID" 2>/dev/null || true
sleep 0.3
if pgrep -f flight_bot >/dev/null 2>&1; then
  echo "error: bot child(ren) survived a clean server shutdown" >&2
  CLEAN_SHUTDOWN_STATUS=1
else
  CLEAN_SHUTDOWN_STATUS=0
fi
echo "bot_clean_shutdown_no_orphans: $([ "$CLEAN_SHUTDOWN_STATUS" -eq 0 ] && echo PASS || echo FAIL)"

# Step: child-process lifecycle, crash (test-plan item 6, finding m1) -
# SIGKILL the server itself, so it can run no cleanup code at all; its
# bots must exit on their own (PDEATHSIG fast path, or ENet connection-
# loss detection as the backstop) - polled for, since this genuinely
# takes a little real wall-clock time, unlike the clean-shutdown case.
echo "-- child-process lifecycle: server crash (SIGKILL), bots self-exit --"
"$FLIGHT_SERVER" --max-bots 3 --max-players 0 --port "$NET_PORT" --snapshot-hz 30 \
  --log-name networked_bots_shutdown_crash &
CRASH_SERVER_PID=$!
sleep 2
kill -KILL "$CRASH_SERVER_PID" || true
wait "$CRASH_SERVER_PID" 2>/dev/null || true
CRASH_SHUTDOWN_STATUS=1
for _ in $(seq 1 30); do
  if ! pgrep -f flight_bot >/dev/null 2>&1; then
    CRASH_SHUTDOWN_STATUS=0
    break
  fi
  sleep 0.2
done
if [ "$CRASH_SHUTDOWN_STATUS" -ne 0 ]; then
  echo "error: bot child(ren) survived the server's SIGKILL (crash safety, finding m1)" >&2
  pkill -KILL -f flight_bot 2>/dev/null || true
fi
echo "bot_crash_safety_no_orphans: $([ "$CRASH_SHUTDOWN_STATUS" -eq 0 ] && echo PASS || echo FAIL)"

NET7_OVERALL_STATUS=0
for status in "$ENDURANCE_STATUS" "$OBSERVE_BOTS_STATUS" "$BOT_TRACKING_STATUS" \
              "$CAPACITY_STATUS" "$REFILL_STATUS" "$CLEAN_SHUTDOWN_STATUS" \
              "$CRASH_SHUTDOWN_STATUS"; do
  if [ "$status" -ne 0 ]; then
    NET7_OVERALL_STATUS=1
  fi
done

if [ "$BINARY_STATUS" -ne 0 ] || [ "$GODOT_OVERALL_STATUS" -ne 0 ] || \
   [ "$NET3_OVERALL_STATUS" -ne 0 ] || [ "$NET4_OVERALL_STATUS" -ne 0 ] || \
   [ "$NET5_OVERALL_STATUS" -ne 0 ] || [ "$NET6_OVERALL_STATUS" -ne 0 ] || \
   [ "$NET7_OVERALL_STATUS" -ne 0 ]; then
  exit 1
fi
exit 0
