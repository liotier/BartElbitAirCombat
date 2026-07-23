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

# Builds only what a dedicated server host needs: the standalone
# flight_server binary and its dependencies (flightcore, netcore,
# Threads). CMake's configure step still fetches godot-cpp too (the top-
# level CMakeLists.txt fetches all three third-party dependencies
# unconditionally), but building only the flight_server target skips
# compiling it - godot-cpp's own several hundred source files are most of
# scripts/run_tests.sh's build time, and a server host never needs them.
#
# Exit codes: 0 success, 2 a required tool is missing, 3 cmake configure
# failed, 4 the build failed.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib.sh"

require_tools cmake git
require_cxx_compiler

cd "$ROOT_DIR"
mkdir -p "$BUILD_DIR"

echo "== Configuring =="
if ! cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release; then
  echo "error: cmake configure failed" >&2
  exit 3
fi

echo "== Building flight_server =="
if ! cmake --build "$BUILD_DIR" --target flight_server --parallel "$(nproc_or_default)"; then
  echo "error: build failed" >&2
  exit 4
fi

echo
echo "Built: $FLIGHT_SERVER"
echo "Start it with: ./scripts/server.sh start"
