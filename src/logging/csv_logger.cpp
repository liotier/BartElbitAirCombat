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

#include "csv_logger.h"

#include <iomanip>
#include <stdexcept>

namespace inc1 {

CsvLogger::CsvLogger(const std::string& path) : out_(path) {
    if (!out_) {
        throw std::runtime_error("failed to open CSV output file: " + path);
    }
    out_ << "time_s,lat_deg,lon_deg,alt_m,vel_north_mps,vel_east_mps,vel_down_mps,"
            "roll_deg,pitch_deg,yaw_deg,alpha_deg,beta_deg,ias_mps,tas_mps,"
            "elevator_norm,aileron_norm,rudder_norm,pitch_trim_norm,throttle_norm\n";
    out_ << std::fixed << std::setprecision(6);
}

void CsvLogger::writeRow(const FlightSample& s) {
    out_ << s.time_s << ',' << s.lat_deg << ',' << s.lon_deg << ',' << s.alt_m
         << ',' << s.vel_north_mps << ',' << s.vel_east_mps << ','
         << s.vel_down_mps << ',' << s.roll_deg << ',' << s.pitch_deg << ','
         << s.yaw_deg << ',' << s.alpha_deg << ',' << s.beta_deg << ','
         << s.ias_mps << ',' << s.tas_mps << ',' << s.elevator_norm << ','
         << s.aileron_norm << ',' << s.rudder_norm << ',' << s.pitch_trim_norm
         << ',' << s.throttle_norm << '\n';
}

}  // namespace inc1
