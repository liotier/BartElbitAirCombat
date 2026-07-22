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
    out << "server_tick,player_id,pos_e,pos_u,pos_n,quat_x,quat_y,quat_z,"
           "quat_w,vel_e,vel_u,vel_n,status_flags\n";
}

void writeSnapshotCsvRow(std::ostream& out, const StateSnapshot& snap) {
    // 9 significant decimal digits round-trips a float32 exactly, so the
    // text file introduces no precision loss beyond what the wire
    // already carries (the transport-fidelity comparison's tolerance
    // exists to catch real bugs, not to absorb text formatting).
    out << std::setprecision(9);
    for (const AircraftState& a : snap.aircraft) {
        out << snap.server_tick << ',' << static_cast<int>(a.player_id) << ','
            << a.pos_local_m[0] << ',' << a.pos_local_m[1] << ','
            << a.pos_local_m[2] << ',' << a.quat[0] << ',' << a.quat[1] << ','
            << a.quat[2] << ',' << a.quat[3] << ',' << a.vel_local_mps[0]
            << ',' << a.vel_local_mps[1] << ',' << a.vel_local_mps[2] << ','
            << static_cast<int>(a.status_flags) << '\n';
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
        if (values.size() != 13) {
            error = "malformed row (expected 13 fields, got " +
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
        row.state.status_flags = static_cast<uint8_t>(values[12]);
        out.push_back(row);
    }
    return true;
}

}  // namespace net
