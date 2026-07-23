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

# Shared constants and helpers for the tester-facing scripts
# (build_server.sh, build_client.sh, server.sh, start_client.sh). Meant to
# be sourced, not run directly - it assumes the caller has already set
# -euo pipefail and defines SCRIPT_DIR from its own BASH_SOURCE.

ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
RUN_DIR="$ROOT_DIR/run"

FLIGHT_SERVER="$BUILD_DIR/flight_server"
FLIGHT_GDEXT="$ROOT_DIR/godot/bin/libflight_gdext.so"

# Same version/asset/URL scripts/run_tests.sh uses, kept identical so both
# scripts share one .godot-tools cache instead of fetching it twice.
GODOT_VERSION="4.5-stable"
GODOT_ASSET="Godot_v${GODOT_VERSION}_linux.x86_64.zip"
GODOT_URL="https://github.com/godotengine/godot/releases/download/${GODOT_VERSION}/${GODOT_ASSET}"
GODOT_CACHE_DIR="$ROOT_DIR/.godot-tools"
GODOT_BIN="$GODOT_CACHE_DIR/Godot_v${GODOT_VERSION}_linux.x86_64"

require_tools() {
  for tool in "$@"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      echo "error: required tool '$tool' not found on PATH" >&2
      exit 2
    fi
  done
}

require_cxx_compiler() {
  if ! command -v c++ >/dev/null 2>&1 \
      && ! command -v g++ >/dev/null 2>&1 \
      && ! command -v clang++ >/dev/null 2>&1; then
    echo "error: no C++ compiler (c++, g++ or clang++) found on PATH" >&2
    exit 2
  fi
}

nproc_or_default() {
  (command -v nproc >/dev/null 2>&1 && nproc) || echo 2
}

# Fetches and caches the Godot editor binary, matching scripts/run_tests.sh's
# own acquisition logic exactly (increment 2 spec, "Godot editor binary
# acquisition") so a build_client.sh run and a run_tests.sh run never fetch
# it twice.
fetch_godot() {
  if [ -x "$GODOT_BIN" ]; then
    return 0
  fi
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
}
