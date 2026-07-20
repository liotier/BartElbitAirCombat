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

// Test 3: roll response. See docs/increment-1-specification.md, "Test 3".
//
// After the first +60 deg crossing the aircraft keeps rolling through
// inverted and the Euler roll angle wraps between +180 and -180 deg. The
// "no left bank" check is therefore windowed to end at that first crossing
// rather than applied to the whole run (spec review finding F5).
#include "test_runner.h"

#include <algorithm>
#include <limits>

namespace inc1 {

TestResult runRollResponse() {
    TestResult result;
    result.name = "roll_response";

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

    CsvLogger logger("roll_response.csv");
    std::vector<FlightSample> samples = runLoop(
        session, 15.0,
        [](double t, FlightSession& s) {
            if (t >= 5.0) s.setProperty("fcs/aileron-cmd-norm", 1.0);
        },
        logger);

    double maxBank5to8 = -std::numeric_limits<double>::infinity();
    for (const FlightSample& s : samples) {
        if (s.time_s >= 5.0 && s.time_s <= 8.0) {
            maxBank5to8 = std::max(maxBank5to8, s.roll_deg);
        }
    }

    // First crossing of +60 deg at or after input onset; falls back to the
    // end of the run if the aircraft never reaches +60 deg, so the window
    // below stays well-defined even when criterion 1 fails.
    double crossingTime = samples.back().time_s;
    for (const FlightSample& s : samples) {
        if (s.time_s >= 5.0 && s.roll_deg >= 60.0) {
            crossingTime = s.time_s;
            break;
        }
    }
    double minBankPreCrossing = std::numeric_limits<double>::infinity();
    for (const FlightSample& s : samples) {
        if (s.time_s >= 5.0 && s.time_s <= crossingTime) {
            minBankPreCrossing = std::min(minBankPreCrossing, s.roll_deg);
        }
    }

    result.criteria.push_back(
        makeCriterion("max_bank_5_8", maxBank5to8, 60.0, ">=", "deg"));
    result.criteria.push_back(makeCriterion(
        "min_bank_pre_crossing", minBankPreCrossing, -10.0, ">=", "deg"));
    result.criteria.push_back(checkFinite(samples));

    result.status =
        std::all_of(result.criteria.begin(), result.criteria.end(),
                    [](const Criterion& c) { return c.passed; })
            ? TestStatus::kPassed
            : TestStatus::kFailed;
    return result;
}

}  // namespace inc1
