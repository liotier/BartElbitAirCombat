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

# Builds what a human player needs: the flight_gdext GDExtension
# (godot-cpp, JSBSim, netcore, predictcore, interpcore) and the matching
# Godot editor binary, fetched and cached exactly as scripts/run_tests.sh
# does - already built by that script, or by scripts/build_server.sh's
# shared build/ directory, does not need rebuilding here.
#
# Exit codes: 0 success, 2 a required tool is missing, 3 cmake configure
# failed, 4 the build failed, 6 fetching the Godot editor failed.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib.sh"

require_tools cmake git curl unzip
require_cxx_compiler

cd "$ROOT_DIR"
mkdir -p "$BUILD_DIR"

echo "== Configuring =="
if ! cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release; then
  echo "error: cmake configure failed" >&2
  exit 3
fi

echo "== Building flight_gdext =="
if ! cmake --build "$BUILD_DIR" --target flight_gdext --parallel "$(nproc_or_default)"; then
  echo "error: build failed" >&2
  exit 4
fi

fetch_godot

echo
echo "Built: $FLIGHT_GDEXT"
echo "Godot editor: $GODOT_BIN"
echo "Start the client with: ./scripts/start_client.sh"
