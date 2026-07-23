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

#include "snapshot_log.h"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace net {

void writeSnapshotCsvHeader(std::ostream& out) {
    // Increment 5: ack_client_seq moves to the end, reflecting its move
    // into AircraftState (protocol.h) - it is per-aircraft now, not a
    // once-per-row top-level value. quat_* columns carry JSBSim's native
    // q(1..4) components (increment 4 on), not increment 3's Godot-
    // convention (x,y,z,w) - see protocol.h's AircraftState::quat.
    out << "server_tick,player_id,pos_e,pos_u,pos_n,quat_x,"
           "quat_y,quat_z,quat_w,vel_e,vel_u,vel_n,ang_vel_p,ang_vel_q,"
           "ang_vel_r,status_flags,ack_client_seq\n";
}

void writeSnapshotCsvRow(std::ostream& out, const StateSnapshot& snap) {
    // 9 significant decimal digits round-trips a float32 exactly, so the
    // text file introduces no precision loss beyond what the wire
    // already carries (the transport-fidelity comparison's tolerance
    // exists to catch real bugs, not to absorb text formatting). Called
    // once per chunk (increment 5), so a tick's full row set accumulates
    // across however many chunks broadcast that tick - chunk_index/
    // chunk_count themselves are not logged (see snapshot_log.h).
    out << std::setprecision(9);
    for (const AircraftState& a : snap.aircraft) {
        out << snap.server_tick << ',' << static_cast<int>(a.player_id)
            << ',' << a.pos_local_m[0] << ',' << a.pos_local_m[1] << ','
            << a.pos_local_m[2] << ',' << a.quat[0] << ',' << a.quat[1] << ','
            << a.quat[2] << ',' << a.quat[3] << ',' << a.vel_local_mps[0]
            << ',' << a.vel_local_mps[1] << ',' << a.vel_local_mps[2] << ','
            << a.ang_vel_body_rps[0] << ',' << a.ang_vel_body_rps[1] << ','
            << a.ang_vel_body_rps[2] << ',' << static_cast<int>(a.status_flags)
            << ',' << a.ack_client_seq << '\n';
    }
}

bool readSnapshotCsv(const std::string& path,
                     std::vector<LoggedAircraftRow>& out,
                     std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "failed to open snapshot log: " + path;
        return false;
    }
    out.clear();
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string field;
        std::vector<double> values;
        while (std::getline(ss, field, ',')) {
            values.push_back(std::stod(field));
        }
        if (values.size() != 17) {
            error = "malformed row (expected 17 fields, got " +
                    std::to_string(values.size()) + "): " + line;
            return false;
        }
        LoggedAircraftRow row;
        row.server_tick = static_cast<uint32_t>(values[0]);
        row.state.player_id = static_cast<uint8_t>(values[1]);
        row.state.pos_local_m[0] = static_cast<float>(values[2]);
        row.state.pos_local_m[1] = static_cast<float>(values[3]);
        row.state.pos_local_m[2] = static_cast<float>(values[4]);
        row.state.quat[0] = static_cast<float>(values[5]);
        row.state.quat[1] = static_cast<float>(values[6]);
        row.state.quat[2] = static_cast<float>(values[7]);
        row.state.quat[3] = static_cast<float>(values[8]);
        row.state.vel_local_mps[0] = static_cast<float>(values[9]);
        row.state.vel_local_mps[1] = static_cast<float>(values[10]);
        row.state.vel_local_mps[2] = static_cast<float>(values[11]);
        row.state.ang_vel_body_rps[0] = static_cast<float>(values[12]);
        row.state.ang_vel_body_rps[1] = static_cast<float>(values[13]);
        row.state.ang_vel_body_rps[2] = static_cast<float>(values[14]);
        row.state.status_flags = static_cast<uint8_t>(values[15]);
        row.state.ack_client_seq = static_cast<uint32_t>(values[16]);
        out.push_back(row);
    }
    return true;
}

}  // namespace net
