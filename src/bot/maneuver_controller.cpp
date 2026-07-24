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

#include "maneuver_controller.h"

#include <algorithm>
#include <cmath>

namespace bot {

namespace {
double clampd(double v, double lo, double hi) {
    return std::max(lo, std::min(hi, v));
}
// Body roll rate (rad/s) to the same normalised units the bank-error term
// uses, matching the exact gain the review's probe validated (Appendix B,
// probe_inc7_bot3.cpp) rather than re-deriving a new one.
constexpr double kRadToDegOver100 = (180.0 / M_PI) / 100.0;
}  // namespace

ManeuverProfile profileForToken(const std::string& token) {
    if (token == "camel") return ManeuverProfile{8.0, 0.5};
    return ManeuverProfile{};  // c172x, pa28, and any unknown token
}

ManeuverController::ManeuverController(const TrimBaseline& trim,
                                        const ManeuverProfile& profile)
    : trim_(trim), profile_(profile) {}

ControlIntent ManeuverController::update(const OwnState& state,
                                          double simTimeS) const {
    // A 30s cycle: wings-level climb (0-8s), a shallow-bank turn (8-16s),
    // a wings-level descent (16-24s), then level for the remainder
    // (24-30s) - the exact schedule validated in Appendix B, scaled by
    // this airframe's ManeuverProfile.
    double phase = std::fmod(simTimeS, 30.0);
    double targetBankDeg = (phase >= 8.0 && phase < 16.0) ? profile_.targetBankDeg : 0.0;
    double elevatorDelta =
        profile_.pitchScale * ((phase < 8.0)    ? -0.04
                               : (phase < 16.0) ? -0.01
                               : (phase < 24.0) ? 0.025
                                                 : 0.0);

    // Proportional on bank error, derivative (roll-rate) damping so
    // sustained aileron deflection cannot integrate bank without bound
    // (aileron commands roll *rate*, not roll angle) - the open-loop
    // failure mode Appendix B measured directly.
    double aileronCmd = 0.02 * (targetBankDeg - state.bankDeg) -
                         0.30 * (state.rollRateRadS * kRadToDegOver100);
    aileronCmd = clampd(aileronCmd, -0.4, 0.4);

    ControlIntent intent;
    intent.elevator = elevatorDelta;
    intent.aileron = trim_.aileron + aileronCmd;
    // Turn coordination: rudder proportional to the same aileron command.
    intent.rudder = trim_.rudder + 0.4 * aileronCmd;
    intent.throttle = trim_.throttle;
    return intent;
}

}  // namespace bot
