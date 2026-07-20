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

// Test 2: pitch response. See docs/increment-1-specification.md, "Test 2".
//
// The c172x's aerodynamic tables clamp at alpha = 16.05 deg (c172x.xml,
// <alphalimits>), but under sustained full aft stick the aircraft departs
// (drops a wing) before reaching that clamp, with dynamic alpha peaking
// near 14 deg. Criteria are set below the clamp accordingly (spec review
// finding F4); do not raise them back toward 16 deg.
#include "test_runner.h"

#include <algorithm>
#include <limits>

namespace inc1 {

TestResult runPitchResponse() {
    TestResult result;
    result.name = "pitch_response";

    FlightSession session;
    std::string error;
    if (!session.initialize(error)) {
        result.status = TestStatus::kError;
        result.message = error;
        return result;
    }
    session.setInitialCondition(5000.0, 100.0, 0.0, 0.0, 0.0, 0.0);
    if (!session.trim(error)) {
        result.status = TestStatus::kError;
        result.message = error;
        return result;
    }

    CsvLogger logger("pitch_response.csv");
    std::vector<FlightSample> samples = runLoop(
        session, 30.0,
        [](double t, FlightSession& s) {
            if (t >= 5.0) s.setProperty("fcs/elevator-cmd-norm", -1.0);
        },
        logger);

    double maxPitch5to10 = -std::numeric_limits<double>::infinity();
    double maxAlpha = -std::numeric_limits<double>::infinity();
    double minIas = std::numeric_limits<double>::infinity();
    for (const FlightSample& s : samples) {
        if (s.time_s >= 5.0 && s.time_s <= 10.0) {
            maxPitch5to10 = std::max(maxPitch5to10, s.pitch_deg);
        }
        maxAlpha = std::max(maxAlpha, s.alpha_deg);
        minIas = std::min(minIas, s.ias_mps);
    }

    result.criteria.push_back(
        makeCriterion("max_pitch_5_10", maxPitch5to10, 30.0, ">=", "deg"));
    result.criteria.push_back(
        makeCriterion("max_alpha", maxAlpha, 12.0, ">=", "deg"));
    result.criteria.push_back(
        makeCriterion("min_ias", minIas, 60.0 * kKt2Mps, "<=", "m/s"));
    result.criteria.push_back(checkFinite(samples));

    result.status =
        std::all_of(result.criteria.begin(), result.criteria.end(),
                    [](const Criterion& c) { return c.passed; })
            ? TestStatus::kPassed
            : TestStatus::kFailed;
    return result;
}

}  // namespace inc1
