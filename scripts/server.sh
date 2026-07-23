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

# Starts/stops/checks flight_server as a background process for human
# testing. scripts/run_tests.sh manages its own server processes directly
# (foreground, torn down by its own trap) and has no use for this script.
#
# Usage:
#   scripts/server.sh start [flight_server args...]
#   scripts/server.sh stop
#   scripts/server.sh status
#
# Exit codes: 0 success, 1 the requested operation could not be completed
# (already running / not running / binary missing), 2 a required tool is
# missing or usage is wrong.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib.sh"

require_tools kill

PID_FILE="$RUN_DIR/server.pid"
LOG_FILE="$RUN_DIR/server.log"

usage() {
  echo "usage: $0 {start [flight_server args...]|stop|status}" >&2
  exit 2
}

# Deliberately written so a "not running" result (a false test, the common
# case) never trips this script's own set -e - only ever called as an if
# condition, which is exempt (same reasoning as run_tests.sh's own
# cleanup_net_procs).
is_running() {
  [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null
}

cmd_start() {
  if is_running; then
    echo "flight_server already running (pid $(cat "$PID_FILE")) - see $LOG_FILE"
    exit 1
  fi
  if [ ! -x "$FLIGHT_SERVER" ]; then
    echo "error: $FLIGHT_SERVER not found - run scripts/build_server.sh first" >&2
    exit 1
  fi
  mkdir -p "$RUN_DIR"
  nohup "$FLIGHT_SERVER" "$@" >"$LOG_FILE" 2>&1 &
  local pid=$!
  echo "$pid" >"$PID_FILE"
  # flight_server trims and binds its socket before it can fail on a bad
  # flag or an already-bound port - give it a moment before declaring
  # success, so a fast failure is reported here rather than discovered
  # later by a confused client.
  sleep 0.5
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "error: flight_server exited immediately - see $LOG_FILE" >&2
    rm -f "$PID_FILE"
    exit 1
  fi
  echo "flight_server started (pid $pid) - log: $LOG_FILE"
}

cmd_stop() {
  if ! is_running; then
    echo "flight_server is not running"
    rm -f "$PID_FILE"
    exit 1
  fi
  local pid
  pid="$(cat "$PID_FILE")"
  # flight_server installs a SIGTERM handler for a clean shutdown
  # (src/server/main.cpp) - SIGKILL is only the fallback.
  kill -TERM "$pid"
  for _ in $(seq 1 50); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  if kill -0 "$pid" 2>/dev/null; then
    echo "warning: flight_server (pid $pid) did not exit after SIGTERM, sending SIGKILL" >&2
    kill -KILL "$pid" 2>/dev/null || true
  fi
  rm -f "$PID_FILE"
  echo "flight_server stopped"
}

cmd_status() {
  if is_running; then
    echo "flight_server running (pid $(cat "$PID_FILE")) - log: $LOG_FILE"
  else
    echo "flight_server not running"
    exit 1
  fi
}

[ $# -ge 1 ] || usage
action="$1"
shift
case "$action" in
  start) cmd_start "$@" ;;
  stop) cmd_stop ;;
  status) cmd_status ;;
  *) usage ;;
esac
