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

// Test 4: power response. See docs/increment-1-specification.md, "Test 4".
//
// A stick-fixed aircraft converts excess power into a phugoid, not a clean
// speed increase, and instantaneous altitude at t=60s is phase-sensitive
// within that phugoid. The primary criterion is therefore specific energy
// height (alt + tas^2/2g), which the phugoid does not affect (spec review
// findings F2/F3). The initial condition trims to level flight at 70 kt
// (not at idle power, which is aerodynamically impossible - finding F1).
#include "test_runner.h"

#include <algorithm>
#include <limits>

namespace inc1 {

TestResult runPowerResponse() {
    TestResult result;
    result.name = "power_response";

    FlightSession session;
    std::string error;
    if (!session.initialize(error)) {
        result.status = TestStatus::kError;
        result.message = error;
        return result;
    }
    session.setInitialCondition(5000.0, 70.0, 0.0, 0.0, 0.0, 0.0);
    if (!session.trim(error)) {
        result.status = TestStatus::kError;
        result.message = error;
        return result;
    }

    CsvLogger logger;
    if (!logger.open("power_response.csv", error)) {
        result.status = TestStatus::kError;
        result.message = error;
        return result;
    }
    std::vector<FlightSample> samples = runLoop(
        session, 60.0,
        [](double t, FlightSession& s) {
            if (t >= 5.0) s.setProperty("fcs/throttle-cmd-norm", 1.0);
        },
        logger);

    const FlightSample& s0 = samples.front();
    const FlightSample& sEnd = samples.back();

    auto energyHeight = [](const FlightSample& s) {
        return s.alt_m + (s.tas_mps * s.tas_mps) / (2.0 * kG0);
    };

    double minIas = std::numeric_limits<double>::infinity();
    double maxIas = -std::numeric_limits<double>::infinity();
    for (const FlightSample& s : samples) {
        minIas = std::min(minIas, s.ias_mps);
        maxIas = std::max(maxIas, s.ias_mps);
    }

    result.criteria.push_back(makeCriterion(
        "energy_height_gain", energyHeight(sEnd) - energyHeight(s0), 100.0,
        ">=", "m"));
    result.criteria.push_back(makeCriterion(
        "altitude_gain", sEnd.alt_m - s0.alt_m, 0.0, ">=", "m"));
    result.criteria.push_back(
        makeCriterion("ias_floor", minIas, 50.0 * kKt2Mps, ">=", "m/s"));
    result.criteria.push_back(
        makeCriterion("ias_ceiling", maxIas, 140.0 * kKt2Mps, "<=", "m/s"));
    result.criteria.push_back(checkFinite(samples));

    result.status =
        std::all_of(result.criteria.begin(), result.criteria.end(),
                    [](const Criterion& c) { return c.passed; })
            ? TestStatus::kPassed
            : TestStatus::kFailed;
    return result;
}

}  // namespace inc1
