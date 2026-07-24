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

// bot_endurance_gate_test: the increment-7 airborne-endurance gate
// (docs/increment-7-specification.md, "Test plan" item 1 / acceptance
// criterion 1) - the real local FlightSession plus the real
// bot::ManeuverController, run standalone (no server/client/ENet) for
// >=3 minutes per catalog airframe, flagging departure (ground contact,
// spiral, stall, NaN) exactly as the pre-drafting/review probes did.
// Godot-free; links flightcore only.
#include "aircraft_catalog.h"
#include "bot/maneuver_controller.h"
#include "test_runner.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace {

// Spec requires >=3 minutes (180s); run comfortably past that so a slow-
// building instability just past the minimum would still be caught.
constexpr double kGateDurationS = 200.0;

struct Result {
    bool departed = false;
    std::string why;
    double minAltM = 0.0;
    double maxBankDeg = 0.0;
    double minVcKts = 0.0;
    double finalAltM = 0.0;
};

Result flyEnduranceGate(const aircraft::CatalogEntry& entry) {
    Result r;
    inc1::FlightSession session;
    std::string error;
    if (!session.initialize(error, entry.load_model)) {
        r.departed = true;
        r.why = "initialize failed: " + error;
        return r;
    }
    session.setInitialCondition(entry.canonical_alt_ft, entry.canonical_vc_kts,
                                 0.0, 0.0, 0.0, 0.0);
    if (!session.trim(error)) {
        r.departed = true;
        r.why = "trim failed: " + error;
        return r;
    }

    bot::TrimBaseline trim;
    trim.aileron = session.property("fcs/aileron-cmd-norm");
    trim.rudder = session.property("fcs/rudder-cmd-norm");
    trim.throttle = session.property("fcs/throttle-cmd-norm");
    bot::ManeuverController controller(trim, bot::profileForToken(entry.token));

    double startAltM = session.property("position/h-sl-ft") * inc1::kFt2M;
    r.minAltM = startAltM;
    r.finalAltM = startAltM;
    r.minVcKts = entry.canonical_vc_kts;

    long nsteps = std::lround(kGateDurationS * 120.0);
    for (long i = 0; i < nsteps; ++i) {
        bot::OwnState state;
        state.bankDeg = session.property("attitude/phi-deg");
        state.rollRateRadS = session.property("velocities/p-rad_sec");
        bot::ControlIntent intent =
            controller.update(state, session.property("simulation/sim-time-sec"));
        session.setProperty("fcs/elevator-cmd-norm", intent.elevator);
        session.setProperty("fcs/aileron-cmd-norm", intent.aileron);
        session.setProperty("fcs/rudder-cmd-norm", intent.rudder);
        session.setProperty("fcs/throttle-cmd-norm", intent.throttle);
        session.step();

        double altM = session.property("position/h-sl-ft") * inc1::kFt2M;
        double absBankDeg = std::fabs(session.property("attitude/phi-deg"));
        double vcKts = session.property("velocities/vc-kts");
        r.minAltM = std::min(r.minAltM, altM);
        r.maxBankDeg = std::max(r.maxBankDeg, absBankDeg);
        r.minVcKts = std::min(r.minVcKts, vcKts);
        r.finalAltM = altM;

        if (std::isnan(altM)) {
            r.departed = true;
            r.why = "NaN";
            break;
        }
        if (altM < 30.0) {
            r.departed = true;
            r.why = "hit ground";
            break;
        }
        if (absBankDeg > 80.0) {
            r.departed = true;
            r.why = "spiral (bank > 80deg)";
            break;
        }
        if (vcKts < 20.0) {
            r.departed = true;
            r.why = "stall (< 20kt)";
            break;
        }
    }
    return r;
}

}  // namespace

int main() {
    // Matches every other JSBSim-driving binary in this project (increment
    // 1 spec review finding F7): contain any stray <output>-block file by
    // chdir'ing into results/ before the first LoadModel().
    std::error_code ec;
    std::filesystem::create_directories("results", ec);
    std::filesystem::current_path("results", ec);

    bool allOk = true;
    std::printf("=== BOT AIRBORNE-ENDURANCE GATE (%.0fs per airframe, gate needs >=180s) ===\n",
                kGateDurationS);
    for (const aircraft::CatalogEntry& entry : aircraft::all()) {
        Result r = flyEnduranceGate(entry);
        bool ok = !r.departed;
        std::printf(
            "  %-6s -> %s  minAlt=%.0fm maxBank=%.0fdeg minVc=%.0fkt finalAlt=%.0fm%s\n",
            entry.token.c_str(), ok ? "PASS" : "FAIL", r.minAltM, r.maxBankDeg,
            r.minVcKts, r.finalAltM, ok ? "" : ("  [" + r.why + "]").c_str());
        allOk = allOk && ok;
    }
    std::printf("bot_endurance_gate: %s\n", allOk ? "PASS" : "FAIL");
    return allOk ? 0 : 1;
}
