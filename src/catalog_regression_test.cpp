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

// catalog_tests: the increment-6 standalone load+trim regression test
// (docs/increment-6-specification.md, "Test plan" item 1) - pins down the
// exact validated LoadModel-string/IC combination for every catalog entry
// as a regression guard, the same role predictcore_tests'/interpcore_
// tests' checks play for their own increments. A future JSBSim version
// bump or config edit that breaks Camel's or pa28's trim fails this test
// immediately rather than getting discovered by a confused human tester.
// Godot-free, no server/client; links flightcore only.
#include "aircraft_catalog.h"
#include "test_runner.h"

#include <cstdio>
#include <filesystem>

int main() {
    // c172x's own <output> block can create a stray JSBout172B.csv during
    // LoadModel(), before FlightSession's DisableOutput() call has a
    // chance to suppress it (increment 1 spec review finding F7);
    // contained the same way flight_server/predictcore_tests do (chdir
    // into results/ first).
    std::error_code ec;
    std::filesystem::create_directories("results", ec);
    std::filesystem::current_path("results", ec);

    bool allOk = true;
    for (const aircraft::CatalogEntry& entry : aircraft::all()) {
        inc1::FlightSession session;
        std::string error;
        bool ok = session.initialize(error, entry.load_model);
        if (ok) {
            session.setInitialCondition(entry.canonical_alt_ft,
                                         entry.canonical_vc_kts, 0.0, 0.0, 0.0,
                                         0.0);
            ok = session.trim(error);
        }
        std::string status = ok ? std::string("PASS") : ("FAIL (" + error + ")");
        std::printf(
            "%-8s load_model=%-8s alt_ft=%-6.0f vc_kts=%-5.0f -> %s\n",
            entry.token.c_str(), entry.load_model.c_str(),
            entry.canonical_alt_ft, entry.canonical_vc_kts, status.c_str());
        allOk = allOk && ok;
    }
    std::printf("catalog_regression: %s\n", allOk ? "PASS" : "FAIL");
    return allOk ? 0 : 1;
}
