// Copyright (C) 2026 The BartElbitAirCombat Project
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// CSV schema for logging StateSnapshot contents, shared by flight_server
// (which logs every snapshot it broadcasts, per docs/increment-3-
// specification.md's server responsibility 6) and flight_test_client
// (which logs every snapshot it receives, then reads both logs back to
// perform the transport-fidelity comparison - spec, "flight_test_client
// (automated)"). Defined once here so the two logs are guaranteed to
// share one row format rather than two independently-written ones that
// could quietly drift apart.
#pragma once

#include "protocol.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace net {

void writeSnapshotCsvHeader(std::ostream& out);

// Writes one row per aircraft in `snap` (increment 3 always has exactly
// one, but the loop is written generally).
void writeSnapshotCsvRow(std::ostream& out, const StateSnapshot& snap);

// One CSV row, flattened back out of its aircraft's fields for
// convenient tick-keyed comparison. Increment 5: ack_client_seq moved into
// AircraftState (protocol.h) since it is now a per-aircraft wire field, so
// it no longer needs a separate column here - `state.ack_client_seq`
// carries it. chunk_index/chunk_count are transport framing, not aircraft
// state, and are deliberately not logged: a tick's rows are the same set
// of per-aircraft records regardless of how many packets carried them.
struct LoggedAircraftRow {
    uint32_t server_tick;
    AircraftState state;
};

// Reads a file written by writeSnapshotCsvHeader/Row back into `out`, in
// file order. Returns false (with `error` filled) if the file cannot be
// opened or a row is malformed.
bool readSnapshotCsv(const std::string& path,
                     std::vector<LoggedAircraftRow>& out, std::string& error);

}  // namespace net
