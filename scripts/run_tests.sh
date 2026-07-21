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
# (docs/increment-1-specification.md, "Test runner"), then the five
# Godot-driven tests (docs/increment-2-specification.md, "Test scenarios").
# All pass/fail evaluation happens inside the test binary or the headless
# test driver script; this script only configures, builds, fetches Godot,
# invokes everything, and relays exit status.
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

if [ "$BINARY_STATUS" -ne 0 ] || [ "$GODOT_OVERALL_STATUS" -ne 0 ]; then
  exit 1
fi
exit 0
