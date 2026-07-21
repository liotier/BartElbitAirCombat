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

// Test 1: trim stability. See docs/increment-1-specification.md, "Test 1".
#include "test_runner.h"

#include <algorithm>
#include <cmath>

namespace inc1 {

TestResult runTrimStability() {
    TestResult result;
    result.name = "trim_stability";

    FlightSession session;
    std::string error;
    if (!session.initialize(error)) {
        result.status = TestStatus::kError;
        result.message = error;
        return result;
    }
    session.setInitialCondition(/*alt_ft=*/5000.0, /*vc_kts=*/100.0,
                                 /*psi_true_deg=*/0.0, /*lat_deg=*/0.0,
                                 /*lon_deg=*/0.0, /*gamma_deg=*/0.0);
    if (!session.trim(error)) {
        result.status = TestStatus::kError;
        result.message = error;
        return result;
    }

    CsvLogger logger;
    if (!logger.open("trim_stability.csv", error)) {
        result.status = TestStatus::kError;
        result.message = error;
        return result;
    }
    std::vector<FlightSample> samples =
        runLoop(session, 60.0, [](double, FlightSession&) {}, logger);

    const FlightSample& s0 = samples.front();
    const FlightSample& sEnd = samples.back();

    double maxAbsBank = 0.0;
    for (const FlightSample& s : samples) {
        maxAbsBank = std::max(maxAbsBank, std::fabs(s.roll_deg));
    }

    result.criteria.push_back(makeCriterion(
        "altitude_drift_abs", std::fabs(sEnd.alt_m - s0.alt_m), 61.0, "<=",
        "m"));
    result.criteria.push_back(makeCriterion(
        "ias_drift_abs", std::fabs(sEnd.ias_mps - s0.ias_mps), 5.0 * kKt2Mps,
        "<=", "m/s"));
    result.criteria.push_back(makeCriterion(
        "pitch_drift_abs", std::fabs(sEnd.pitch_deg - s0.pitch_deg), 5.0,
        "<=", "deg"));
    result.criteria.push_back(
        makeCriterion("max_bank_abs", maxAbsBank, 2.0, "<=", "deg"));
    result.criteria.push_back(checkFinite(samples));

    result.status =
        std::all_of(result.criteria.begin(), result.criteria.end(),
                    [](const Criterion& c) { return c.passed; })
            ? TestStatus::kPassed
            : TestStatus::kFailed;
    return result;
}

}  // namespace inc1
