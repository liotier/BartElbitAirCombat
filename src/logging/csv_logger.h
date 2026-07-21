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

// One flight-state sample and a writer for the 10 Hz CSV logs required by
// docs/increment-1-specification.md ("Logging").
#pragma once

#include <fstream>
#include <string>

namespace inc1 {

struct FlightSample {
    double time_s = 0.0;
    double lat_deg = 0.0;
    double lon_deg = 0.0;
    double alt_m = 0.0;
    double vel_north_mps = 0.0;
    double vel_east_mps = 0.0;
    double vel_down_mps = 0.0;
    double roll_deg = 0.0;
    double pitch_deg = 0.0;
    double yaw_deg = 0.0;
    double alpha_deg = 0.0;
    double beta_deg = 0.0;
    double ias_mps = 0.0;
    double tas_mps = 0.0;
    double elevator_norm = 0.0;
    double aileron_norm = 0.0;
    double rudder_norm = 0.0;
    double pitch_trim_norm = 0.0;
    double throttle_norm = 0.0;
};

// Writes FlightSample rows to a CSV file with the exact header and column
// order mandated by the specification's "Required columns" section.
class CsvLogger {
public:
    CsvLogger() = default;

    CsvLogger(const CsvLogger&) = delete;
    CsvLogger& operator=(const CsvLogger&) = delete;

    // Opens `path` and writes the header row. Returns false and fills
    // `error` on failure; no other method may be called before this
    // succeeds.
    bool open(const std::string& path, std::string& error);

    void writeRow(const FlightSample& sample);

private:
    std::ofstream out_;
};

}  // namespace inc1
