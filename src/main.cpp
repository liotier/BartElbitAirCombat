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

// Entry point: runs all four increment-1 scenarios in sequence, writes
// results/*.csv and results/summary.json, prints the summary table, and
// returns the exit status defined in docs/increment-1-specification.md
// ("Exit codes"): 0 all passed, 1 a criterion failed, 2 an execution error
// (e.g. model load or trim failure) occurred.
#include "test_runner.h"

#include <cstdio>
#include <filesystem>
#include <vector>

int main() {
    // The c172x model's own <output> block opens its CSV (with a filename
    // hardcoded in c172x.xml) during LoadModel() itself, before
    // FlightSession::initialize() gets a chance to call DisableOutput() -
    // that only suppresses the periodic data writes, not the file's
    // creation. Running from within results/ (already git-ignored, already
    // wiped clean by scripts/run_tests.sh) contains that stray file instead
    // of leaving it in the repository root.
    std::error_code ec;
    std::filesystem::create_directories("results", ec);
    std::filesystem::current_path("results", ec);
    if (ec) {
        std::fprintf(stderr, "error: could not enter results/ directory: %s\n",
                     ec.message().c_str());
        return 2;
    }

    std::vector<inc1::TestResult> results;
    results.push_back(inc1::runTrimStability());
    results.push_back(inc1::runPitchResponse());
    results.push_back(inc1::runRollResponse());
    results.push_back(inc1::runPowerResponse());

    inc1::printSummaryTable(results);
    inc1::writeSummaryJson(results, "summary.json");

    bool anyError = false;
    bool anyFailed = false;
    for (const auto& r : results) {
        if (r.status == inc1::TestStatus::kError) anyError = true;
        if (r.status == inc1::TestStatus::kFailed) anyFailed = true;
    }
    if (anyError) return 2;
    if (anyFailed) return 1;
    return 0;
}
