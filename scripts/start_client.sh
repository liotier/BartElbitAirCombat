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

# Launches the cached Godot editor binary straight into the networked
# multiplayer scene - not the project's single-player default scene
# (scenes/main.tscn), and not the bare editor UI - so a human tester just
# needs one command rather than knowing to open the project, pick the
# right scene, and press Play.
#
# Usage: scripts/start_client.sh [--server HOST[:PORT]] [--aircraft {c172x|camel|pa28}]
#
# Without --server, PredictedAircraft connects to its own Inspector
# default (127.0.0.1:45300, i.e. a server started with default flags on
# the same machine). --server sets SERVER_HOST/SERVER_PORT, which
# PredictedAircraft::_ready() reads as an override (src/godot_ext/
# predicted_aircraft.cpp) - use this to connect to a server on another
# machine or a non-default port.
#
# --aircraft sets AIRCRAFT (docs/increment-6-specification.md,
# "Server-authoritative type selection"), overriding the Inspector-set
# aircraft_type property - must match the server's own --aircraft or the
# client warns loudly and disconnects (see ALLOW_AIRCRAFT_MISMATCH in
# predicted_aircraft.cpp to instead observe that behaviour).
#
# Exit codes: 0 the client ran and was closed normally, 1 a bad --server/
# --aircraft value or usage error, 8 the GDExtension or Godot editor binary
# has not been built/fetched yet (run scripts/build_client.sh first).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib.sh"

SERVER_ARG=""
AIRCRAFT_ARG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --server)
      [ $# -ge 2 ] || { echo "error: --server needs a value" >&2; exit 1; }
      SERVER_ARG="$2"
      shift 2
      ;;
    --server=*)
      SERVER_ARG="${1#--server=}"
      shift
      ;;
    --aircraft)
      [ $# -ge 2 ] || { echo "error: --aircraft needs a value" >&2; exit 1; }
      AIRCRAFT_ARG="$2"
      shift 2
      ;;
    --aircraft=*)
      AIRCRAFT_ARG="${1#--aircraft=}"
      shift
      ;;
    *)
      echo "usage: $0 [--server HOST[:PORT]] [--aircraft {c172x|camel|pa28}]" >&2
      exit 1
      ;;
  esac
done

if [ -n "$AIRCRAFT_ARG" ]; then
  case "$AIRCRAFT_ARG" in
    c172x|camel|pa28) export AIRCRAFT="$AIRCRAFT_ARG" ;;
    *)
      echo "error: --aircraft must be one of c172x, camel, pa28 (got '$AIRCRAFT_ARG')" >&2
      exit 1
      ;;
  esac
fi

if [ -n "$SERVER_ARG" ]; then
  export SERVER_HOST="${SERVER_ARG%%:*}"
  case "$SERVER_ARG" in
    *:*) export SERVER_PORT="${SERVER_ARG##*:}" ;;
  esac
  if [ -z "$SERVER_HOST" ]; then
    echo "error: --server value must not be empty" >&2
    exit 1
  fi
fi

if [ ! -x "$GODOT_BIN" ]; then
  echo "error: Godot editor not found at $GODOT_BIN - run scripts/build_client.sh first" >&2
  exit 8
fi
if [ ! -f "$FLIGHT_GDEXT" ]; then
  echo "error: $FLIGHT_GDEXT not found - run scripts/build_client.sh first" >&2
  exit 8
fi

echo "Connecting to ${SERVER_HOST:-127.0.0.1}:${SERVER_PORT:-45300} (aircraft=${AIRCRAFT:-c172x})"
exec "$GODOT_BIN" --path "$ROOT_DIR/godot" res://scenes/networked.tscn
