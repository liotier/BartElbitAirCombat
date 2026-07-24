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

// The increment-7 bot "intelligence" (docs/increment-7-specification.md,
// "The core/intelligence seam"): a narrow API - given the bot's own
// dynamics state, return a control intent - deliberately independent of
// JSBSim, the wire protocol, or anything else the bot *core* owns, so this
// is the exact boundary a future third-party bot SDK publishes. Takes no
// dependency on flightcore/netcore/predictcore; a plain scalar interface
// callable from the standalone endurance-gate regression test just as
// easily as from the real networked flight_bot.
#pragma once

#include <string>

namespace bot {

// The trim-relative baseline this controller perturbs around (spec,
// "Maneuver logic": "Commands are expressed relative to the bot's own
// trimmed control values"). Read once, right after the bot's own trim()
// converges. No separate elevator baseline: JSBSim's trim writes its
// pitch solution to pitch-trim-cmd-norm, a property this controller never
// touches, leaving fcs/elevator-cmd-norm's own baseline at 0 after a trim
// (confirmed empirically, docs/increment-1-specification.md's own review
// history) - so the elevator schedule below is already relative to trim
// with no baseline to add back.
struct TrimBaseline {
    double aileron = 0.0;
    double rudder = 0.0;
    double throttle = 0.0;
};

// Own-aircraft attitude/rate state the controller reacts to each tick -
// deliberately just these two fields (spec, Appendix B: the validated
// controller closes the loop on bank only; pitch stays an open-loop
// schedule, which held airborne across the full endurance-gate duration).
struct OwnState {
    double bankDeg = 0.0;
    double rollRateRadS = 0.0;
};

struct ControlIntent {
    double elevator = 0.0;
    double aileron = 0.0;
    double rudder = 0.0;
    double throttle = 0.0;
};

// Per-airframe maneuver amplitude (docs/increment-7-specification.md,
// "Open questions for the implementer": gains are airframe-sensitive).
// The closed-loop gains themselves (bank-error proportional, roll-rate
// damping, rudder coordination) are shared across every airframe -
// measured to generalise; only the *amplitude* of the maneuver needs
// tuning per airframe. c172x/pa28 use the full-amplitude default; Camel's
// much smaller stall margin (65 kt trim vs. 100 kt) needs a shallower
// bank target and gentler pitch schedule to stay out of a spiral
// (measured: the full-amplitude profile spirals it, the shallow one flies
// cleanly for 400s+ - see docs/increment-7-specification-review.md
// Appendix B and this increment's own extended validation).
struct ManeuverProfile {
    double targetBankDeg = 15.0;
    double pitchScale = 1.0;
};

// Looks up the per-airframe profile by catalog token; unknown tokens get
// the full-amplitude GA default.
ManeuverProfile profileForToken(const std::string& token);

// A closed-loop attitude-target schedule - shallow-bank turn / gentle
// climb-descent / level, repeating every 30s - proportional on bank
// error, rate-damped, with aileron-proportional rudder coordination
// (docs/increment-7-specification.md, "Maneuver logic"). Trim-blind and
// open-loop control both measurably depart every airframe (Appendix B);
// this shape is what keeps c172x/pa28/Camel(shallow) airborne.
class ManeuverController {
public:
    ManeuverController(const TrimBaseline& trim, const ManeuverProfile& profile);

    // `simTimeS` should be the bot's own simulation clock (JSBSim's
    // simulation/sim-time-sec) so the maneuver schedule is driven by
    // simulated, not wall-clock, time.
    ControlIntent update(const OwnState& state, double simTimeS) const;

private:
    TrimBaseline trim_;
    ManeuverProfile profile_;
};

}  // namespace bot
