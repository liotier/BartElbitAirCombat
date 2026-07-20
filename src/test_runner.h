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

// Shared test infrastructure for the increment-1 scenarios: a JSBSim session
// wrapper implementing the normative initialisation sequence from
// docs/increment-1-specification.md ("Initialisation sequence"), the
// pass/fail criterion and result types, the 120 Hz run loop with 10 Hz
// logging, and the summary table / JSON writers.
#pragma once

#include "logging/csv_logger.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace JSBSim {
class FGFDMExec;
}

namespace inc1 {

// Fixed simulation timestep: 1/120 s (spec, "Timestep").
constexpr double kDt = 1.0 / 120.0;
constexpr double kFt2M = 0.3048;
constexpr double kKt2Mps = 1852.0 / 3600.0;
constexpr double kG0 = 9.80665;

// Wraps a single FGFDMExec instance through the c172x load, initial
// condition and trim sequence. Each scenario constructs its own instance;
// no state is shared between tests.
class FlightSession {
public:
    FlightSession();
    ~FlightSession();

    FlightSession(const FlightSession&) = delete;
    FlightSession& operator=(const FlightSession&) = delete;

    // Sets dt, data paths, loads c172x and disables its built-in output
    // blocks. Returns false and fills `error` if the model fails to load.
    bool initialize(std::string& error);

    // Writes the ic/* properties consumed by RunIC().
    void setInitialCondition(double alt_ft, double vc_kts, double psi_true_deg,
                              double lat_deg, double lon_deg, double gamma_deg);

    // Applies the initial condition, starts the engine, and runs a full
    // trim. Returns false and fills `error` if trim does not converge.
    bool trim(std::string& error);

    void setProperty(const std::string& name, double value);
    double property(const std::string& name) const;

    // Advances the simulation by exactly one integration step (kDt).
    void step();

    FlightSample sample() const;

private:
    std::unique_ptr<JSBSim::FGFDMExec> fdm_;
};

struct Criterion {
    std::string name;
    bool passed = false;
    double actual = 0.0;
    double limit = 0.0;
    std::string comparison;  // "<=" or ">=", such that passed == (actual OP limit)
    std::string unit;
};

Criterion makeCriterion(std::string name, double actual, double limit,
                         std::string comparison, std::string unit);

enum class TestStatus { kPassed, kFailed, kError };

struct TestResult {
    std::string name;
    TestStatus status = TestStatus::kError;
    std::string message;  // populated only when status == kError
    std::vector<Criterion> criteria;
};

// Runs `session` for `duration_s` simulated seconds at 120 Hz. Before each
// step, calls controlInput(t, session) so scenarios can script control
// surface changes. Logs every 12th sample (10 Hz) via `logger`, including
// the t=0 sample before the first step. Returns the full 120 Hz sample
// history, which is the basis for all criterion evaluation (spec,
// "Evaluation basis").
std::vector<FlightSample> runLoop(
    FlightSession& session, double duration_s,
    const std::function<void(double, FlightSession&)>& controlInput,
    CsvLogger& logger);

// The universal criterion required of every test: no NaN/Inf in any logged
// field at any step.
Criterion checkFinite(const std::vector<FlightSample>& samples);

TestResult runTrimStability();
TestResult runPitchResponse();
TestResult runRollResponse();
TestResult runPowerResponse();

void printSummaryTable(const std::vector<TestResult>& results);
void writeSummaryJson(const std::vector<TestResult>& results,
                      const std::string& path);

}  // namespace inc1
