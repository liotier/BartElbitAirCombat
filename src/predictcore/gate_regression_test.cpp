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

// predictcore_tests: the increment-4 reconstruction-gate regression test
// (docs/increment-4-specification.md, "Reconstruction gate", acceptance
// criterion 1) - reproduces the review's own gate experiment in real code,
// so the frame algebra (Ti2l composition, ellipsoid-configured FGLocation)
// can never silently regress. Flies one "truth" session through an
// aggressive pull-up-plus-roll maneuver (reaching inverted and near-
// vertical attitudes, same shape as the review's probe), and at intervals
// reconstructs a *fresh* session's VehicleState from truth's own
// wire-representable fields via predict::reconstructAndApply(), then
// asserts the fresh session's resulting state matches truth closely.
// Godot-free; links predictcore + flightcore only.
#include "predictcore/reconstruction.h"
#include "test_runner.h"

#include "math/FGQuaternion.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace {

constexpr double kDegToRad = M_PI / 180.0;

double angleDiffDeg(double a, double b) {
    double d = a - b;
    while (d > 180.0) d -= 360.0;
    while (d < -180.0) d += 360.0;
    return std::fabs(d);
}

bool makeTrimmedSession(inc1::FlightSession& session) {
    std::string error;
    if (!session.initialize(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return false;
    }
    session.setInitialCondition(5000.0, 100.0, 0.0, 0.0, 0.0, 0.0);
    if (!session.trim(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main() {
    // c172x's own <output> block can create a stray JSBout172B.csv during
    // LoadModel(), before FlightSession's DisableOutput() call has a
    // chance to suppress it (increment 1 spec review finding F7);
    // contained the same way flight_server does (chdir into results/
    // first) rather than leaking it into whatever directory this binary
    // is invoked from.
    std::error_code ec;
    std::filesystem::create_directories("results", ec);
    std::filesystem::current_path("results", ec);

    inc1::FlightSession truth;
    if (!makeTrimmedSession(truth)) return 2;

    double worstPosM = 0.0;
    double worstAttDeg = 0.0;
    double worstVelMps = 0.0;
    int samples = 0;

    // Same shape as the review's reconstruction-gate probe: an aggressive
    // pull-up (sweeps through vertical) plus a roll segment (reaches
    // inverted), sampled every 20 ticks.
    for (int t = 0; t < 600; ++t) {
        truth.setProperty("fcs/elevator-cmd-norm", -0.9);
        truth.setProperty("fcs/aileron-cmd-norm",
                           (t > 240 && t < 360) ? 0.6 : 0.0);
        truth.step();
        if (t % 20 != 0) continue;

        inc1::FlightSample s = truth.sample();

        double p = truth.property("velocities/p-rad_sec");
        double q = truth.property("velocities/q-rad_sec");
        double r = truth.property("velocities/r-rad_sec");
        JSBSim::FGQuaternion qLocal = truth.getVState().qAttitudeLocal;
        double qWxyz[4] = {qLocal(1), qLocal(2), qLocal(3), qLocal(4)};

        inc1::FlightSession fresh;
        if (!makeTrimmedSession(fresh)) return 2;
        predict::reconstructAndApply(fresh, s.lat_deg * kDegToRad,
                                      s.lon_deg * kDegToRad, s.alt_m, qWxyz,
                                      s.vel_north_mps, s.vel_east_mps,
                                      s.vel_down_mps, p, q, r);
        inc1::FlightSample r_s = fresh.sample();

        double dLat_m = (r_s.lat_deg - s.lat_deg) * 111320.0;
        double dLon_m = (r_s.lon_deg - s.lon_deg) * 111320.0 *
                        std::cos(s.lat_deg * kDegToRad);
        double dAlt_m = r_s.alt_m - s.alt_m;
        double posErrM =
            std::sqrt(dLat_m * dLat_m + dLon_m * dLon_m + dAlt_m * dAlt_m);
        double attErrDeg =
            std::max({angleDiffDeg(r_s.roll_deg, s.roll_deg),
                       angleDiffDeg(r_s.pitch_deg, s.pitch_deg),
                       angleDiffDeg(r_s.yaw_deg, s.yaw_deg)});
        double velErrMps = std::max(
            {std::fabs(r_s.vel_north_mps - s.vel_north_mps),
             std::fabs(r_s.vel_east_mps - s.vel_east_mps),
             std::fabs(r_s.vel_down_mps - s.vel_down_mps)});

        worstPosM = std::max(worstPosM, posErrM);
        worstAttDeg = std::max(worstAttDeg, attErrDeg);
        worstVelMps = std::max(worstVelMps, velErrMps);
        ++samples;
        std::printf(
            "t=%d roll=%.1f pitch=%.1f yaw=%.1f  pos_err=%.6f m  "
            "att_err=%.6f deg  vel_err=%.6f m/s\n",
            t, s.roll_deg, s.pitch_deg, s.yaw_deg, posErrM, attErrDeg,
            velErrMps);
    }

    std::printf("\n=== RECONSTRUCTION GATE REGRESSION (%d samples) ===\n",
                samples);
    std::printf("worst position error:  %.6f m\n", worstPosM);
    std::printf("worst attitude error:  %.6f deg\n", worstAttDeg);
    std::printf("worst velocity error:  %.6f m/s\n", worstVelMps);

    // Loose relative to the review's measured worst case (0.1 mm / 6e-6
    // deg / 0.0 m/s) - tight enough to catch a real algebra regression
    // (e.g. the Ti2l/Tl2i swap the review found, which produced
    // gross-not-subtle errors) without being brittle to platform-level
    // floating-point variation.
    bool ok = samples > 0 && worstPosM < 0.01 && worstAttDeg < 0.01 &&
              worstVelMps < 0.01;
    std::printf("gate_regression: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
