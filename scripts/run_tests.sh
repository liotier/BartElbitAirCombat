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

# Orchestrates the increment-1 validation pipeline end to end, per
# docs/increment-1-specification.md ("Test runner"). All pass/fail
# evaluation happens inside the test binary; this script only configures,
# builds, invokes it, and relays its exit status.
#
# Exit codes: 0 all tests passed, 1 a test failed, 2 a required tool is
# missing, 3 cmake configure failed, 4 the build failed, 5 the test binary
# reported an execution error (e.g. trim did not converge).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_DIR="build"
RESULTS_DIR="results"
TEST_BINARY="$BUILD_DIR/increment1_tests"

for tool in cmake git; do
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

rm -rf "$RESULTS_DIR"
mkdir -p "$RESULTS_DIR"

echo "== Running tests =="
set +e
"$TEST_BINARY"
BINARY_STATUS=$?
set -e

case "$BINARY_STATUS" in
  0) exit 0 ;;
  1) exit 1 ;;
  2)
    echo "error: test binary reported an execution error" >&2
    exit 5
    ;;
  *)
    echo "error: test binary exited with unexpected status $BINARY_STATUS" >&2
    exit 5
    ;;
esac
