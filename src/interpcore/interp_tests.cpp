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

// interpcore_tests: docs/increment-5-specification.md "Test plan" items
// 1-4 (the interpcore unit-level tests) - synthetic data, no
// FlightSession/JSBSim, no network. Godot-free; links interpcore +
// netcore only.
#include "interpcore/remote_entity_tracker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr double kPi = 3.14159265358979323846;

// net::AircraftState's position/quaternion fields are float32 (matching
// the wire), so every expected-vs-actual comparison below is inherently
// limited by float32 storage precision, not just interpcore's own math -
// e.g. a coordinate of ~50 has a float32 ULP of ~6e-6 (confirmed directly:
// storing repeating-binary-fraction values like -50.4 rounds by a
// fraction of that). These tolerances sit comfortably above that noise
// floor (by 1-2 orders of magnitude) while still catching a real
// algorithmic error, which would be orders of magnitude larger (a wrong
// axis, sign, or frame would show up as whole meters/degrees, not
// micro-units).
constexpr double kPosTolM = 1e-4;
constexpr double kAttTolDeg = 1e-4;

net::AircraftState makeState(uint8_t playerId, const double pos[3],
                              const double quat[4], const double vel[3],
                              const double angVel[3]) {
    net::AircraftState s;
    s.player_id = playerId;
    for (int i = 0; i < 3; ++i) s.pos_local_m[i] = static_cast<float>(pos[i]);
    for (int i = 0; i < 4; ++i) s.quat[i] = static_cast<float>(quat[i]);
    for (int i = 0; i < 3; ++i) s.vel_local_mps[i] = static_cast<float>(vel[i]);
    for (int i = 0; i < 3; ++i) s.ang_vel_body_rps[i] = static_cast<float>(angVel[i]);
    return s;
}

// Closed-form attitude for constant BODY-frame angular velocity omega=(0,0,r)
// (yaw rate only, a fixed body axis - keeps the test's expected answer
// simple and exact): q(t) = q0 (x) (cos(r*t/2), 0, 0, sin(r*t/2)).
void closedFormQuat(double q0[4], double r, double t, double out[4]) {
    double halfAngle = 0.5 * r * t;
    double dw = std::cos(halfAngle), dz = std::sin(halfAngle);
    // Hamilton product q0 * (dw, 0, 0, dz), matching remote_entity_tracker
    // .cpp's own convention (component order w,x,y,z).
    out[0] = q0[0] * dw - q0[3] * dz;
    out[1] = q0[1] * dw + q0[2] * dz;
    out[2] = -q0[1] * dz + q0[2] * dw;
    out[3] = q0[0] * dz + q0[3] * dw;
}

double quatAngleDiffDeg(const double a[4], const double b[4]) {
    // Relative-rotation angle via 2*atan2(|v|,|w|) on a^-1 * b (a is unit,
    // so its inverse is its conjugate) - numerically stable near 0,
    // matching predictcore's reconciliation_math.cpp convention.
    double aw = a[0], ax = -a[1], ay = -a[2], az = -a[3];  // conjugate(a)
    double bw = b[0], bx = b[1], by = b[2], bz = b[3];
    double rw = aw * bw - ax * bx - ay * by - az * bz;
    double rx = aw * bx + ax * bw + ay * bz - az * by;
    double ry = aw * by - ax * bz + ay * bw + az * bx;
    double rz = aw * bz + ax * by - ay * bx + az * bw;
    double vNorm = std::sqrt(rx * rx + ry * ry + rz * rz);
    return 2.0 * std::atan2(vNorm, std::fabs(rw)) * 180.0 / kPi;
}

int g_failures = 0;

void check(bool cond, const char* name, const char* detail) {
    std::printf("%-55s %s%s%s\n", name, cond ? "PASS" : "FAIL",
                detail[0] ? " - " : "", detail);
    if (!cond) ++g_failures;
}

// Test 1 (+ its sub-assertion): interpolation correctness for uniform
// linear motion + uniform rotation at a known rate, and invariance to
// arrival-time jitter given identical server_ticks (review finding N2).
void testInterpolationCorrectness() {
    const double kTickPeriod = interp::RemoteEntityTracker::kTickPeriodS;
    double vel[3] = {10.0, 0.0, 5.0};
    double angVel[3] = {0.0, 0.0, 0.5};  // 0.5 rad/s yaw
    double q0[4] = {1.0, 0.0, 0.0, 0.0};
    double pos0[3] = {0.0, 100.0, 0.0};

    auto feed = [&](interp::RemoteEntityTracker& t, uint8_t pid,
                     const double arrivalTimes[61]) {
        for (uint32_t tick = 0; tick <= 60; ++tick) {
            double simT = tick * kTickPeriod;
            double pos[3] = {pos0[0] + vel[0] * simT, pos0[1] + vel[1] * simT,
                              pos0[2] + vel[2] * simT};
            double q[4];
            closedFormQuat(q0, angVel[2], simT, q);
            net::AircraftState s = makeState(pid, pos, q, vel, angVel);
            t.update(pid, s, tick, arrivalTimes[tick]);
        }
    };

    double regularArrivals[61];
    for (int i = 0; i <= 60; ++i) regularArrivals[i] = i * kTickPeriod;

    interp::RemoteEntityTracker tracker;
    feed(tracker, 5, regularArrivals);

    // Sample at an intermediate instant strictly between two fed ticks.
    double renderT = 30.5 * kTickPeriod;
    // targetTimelineS = latestServerTick*tick + (renderT - latestArrival);
    // with the regular feed, latestServerTick=60, latestArrival=60*tick,
    // so targetTimelineS collapses to exactly renderT here - construct the
    // call accordingly (sample()'s renderTimeS is in the SAME clock as the
    // arrivalTimeS values passed to update()).
    interp::InterpolatedState result = tracker.sample(5, renderT);

    double expectedPos[3] = {pos0[0] + vel[0] * renderT,
                              pos0[1] + vel[1] * renderT,
                              pos0[2] + vel[2] * renderT};
    double expectedQuat[4];
    closedFormQuat(q0, angVel[2], renderT, expectedQuat);

    double posErr = 0.0;
    for (int i = 0; i < 3; ++i) {
        posErr = std::max(posErr, std::fabs(result.pos_local_m[i] - expectedPos[i]));
    }
    double attErrDeg = quatAngleDiffDeg(result.quat, expectedQuat);

    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "pos_err=%.9f m att_err=%.9f deg (valid=%d)", posErr,
                  attErrDeg, result.valid);
    check(result.valid && posErr < kPosTolM && attErrDeg < kAttTolDeg,
          "interpolation_correctness", detail);

    // Sub-assertion (review finding N2): identical server_ticks, heavily
    // jittered arrival times, must produce byte-identical output - the
    // interpolation clock is server_tick, never arrival_time.
    double jitteredArrivals[61];
    unsigned seed = 12345;
    for (int i = 0; i <= 60; ++i) {
        seed = seed * 1103515245u + 12345u;
        double jitterS = (static_cast<double>(seed % 1000) / 1000.0 - 0.5) * 0.05;
        jitteredArrivals[i] = i * kTickPeriod + jitterS;
    }
    interp::RemoteEntityTracker jitteredTracker;
    feed(jitteredTracker, 5, jitteredArrivals);
    // Query at the same simulation instant, expressed relative to THIS
    // tracker's own (jittered) latest-arrival anchor, so both trackers are
    // asked for the identical targetTimelineS.
    double jitteredRenderT = jitteredArrivals[60] + (renderT - regularArrivals[60]);
    interp::InterpolatedState jitteredResult =
        jitteredTracker.sample(5, jitteredRenderT);

    bool identical = jitteredResult.valid == result.valid;
    for (int i = 0; identical && i < 3; ++i) {
        identical = jitteredResult.pos_local_m[i] == result.pos_local_m[i];
    }
    for (int i = 0; identical && i < 4; ++i) {
        identical = jitteredResult.quat[i] == result.quat[i];
    }
    std::snprintf(detail, sizeof(detail), "jittered vs regular arrival, same server_tick");
    check(identical, "interpolation_immune_to_arrival_jitter", detail);
}

// Test 2: graceful one-drop handling - skip a tick's update() call, assert
// interpolation using the next available bracketing pair is still smooth
// and correct.
void testGracefulOneDropHandling() {
    const double kTickPeriod = interp::RemoteEntityTracker::kTickPeriodS;
    double vel[3] = {20.0, 0.0, 0.0};
    double angVel[3] = {0.0, 0.0, 0.0};
    double q0[4] = {1.0, 0.0, 0.0, 0.0};
    double pos0[3] = {0.0, 50.0, 0.0};

    interp::RemoteEntityTracker tracker;
    for (uint32_t tick = 0; tick <= 40; ++tick) {
        if (tick == 20) continue;  // simulate one dropped chunk
        double simT = tick * kTickPeriod;
        double pos[3] = {pos0[0] + vel[0] * simT, pos0[1], pos0[2]};
        net::AircraftState s = makeState(9, pos, q0, vel, angVel);
        tracker.update(9, s, tick, simT);
    }

    // Render right in the middle of the dropped tick's span (between tick
    // 19 and tick 21).
    double renderT = 20.0 * kTickPeriod;
    interp::InterpolatedState result = tracker.sample(9, renderT);
    double expectedX = pos0[0] + vel[0] * renderT;
    double err = std::fabs(result.pos_local_m[0] - expectedX);

    char detail[128];
    std::snprintf(detail, sizeof(detail), "pos_err=%.9f m across the drop", err);
    check(result.valid && err < kPosTolM, "graceful_one_drop_handling", detail);
}

// Test 3: bounded extrapolation - feed a sequence, then stop; assert the
// tracker dead-reckons correctly for the capped window and then holds.
void testBoundedExtrapolation() {
    const double kTickPeriod = interp::RemoteEntityTracker::kTickPeriodS;
    double vel[3] = {15.0, 0.0, 0.0};
    double angVel[3] = {0.0, 0.0, 1.0};  // 1 rad/s yaw - deliberately brisk
    double q0[4] = {1.0, 0.0, 0.0, 0.0};
    double pos0[3] = {0.0, 200.0, 0.0};

    interp::RemoteEntityTracker tracker;
    uint32_t lastTick = 30;
    double lastArrival = lastTick * kTickPeriod;
    for (uint32_t tick = 0; tick <= lastTick; ++tick) {
        double simT = tick * kTickPeriod;
        double pos[3] = {pos0[0] + vel[0] * simT, pos0[1], pos0[2]};
        double q[4];
        closedFormQuat(q0, angVel[2], simT, q);
        net::AircraftState s = makeState(3, pos, q, vel, angVel);
        tracker.update(3, s, tick, simT);
    }

    // Within the extrapolation window (0.2 s past the newest sample):
    // exact closed-form check, since the extrapolation formula is itself
    // exact for genuinely constant angular velocity (remote_entity_tracker
    // .cpp's extrapolate()).
    double dtWithin = 0.2;
    interp::InterpolatedState within =
        tracker.sample(3, lastArrival + dtWithin);
    double expectedPosWithin = pos0[0] + vel[0] * (lastArrival + dtWithin);
    double qExpectedWithin[4];
    closedFormQuat(q0, angVel[2], lastArrival + dtWithin, qExpectedWithin);
    double posErrWithin = std::fabs(within.pos_local_m[0] - expectedPosWithin);
    double attErrWithin = quatAngleDiffDeg(within.quat, qExpectedWithin);
    char detail[192];
    std::snprintf(detail, sizeof(detail),
                  "dt=%.2fs pos_err=%.9f m att_err=%.9f deg", dtWithin,
                  posErrWithin, attErrWithin);
    check(within.valid && posErrWithin < kPosTolM && attErrWithin < kAttTolDeg,
          "bounded_extrapolation_within_window", detail);

    // Well past the cap (kMaxExtrapolationS=0.3s): position/attitude must
    // match extrapolating by EXACTLY the cap, not by the larger requested
    // dt - i.e. holding, not continuing to project forward.
    double dtCap = interp::RemoteEntityTracker::kMaxExtrapolationS;
    double dtFar = dtCap + 5.0;  // 5s further past the cap
    interp::InterpolatedState atCap = tracker.sample(3, lastArrival + dtCap);
    interp::InterpolatedState farPast = tracker.sample(3, lastArrival + dtFar);
    double holdPosErr = 0.0;
    for (int i = 0; i < 3; ++i) {
        holdPosErr = std::max(
            holdPosErr, std::fabs(farPast.pos_local_m[i] - atCap.pos_local_m[i]));
    }
    double holdAttErr = quatAngleDiffDeg(farPast.quat, atCap.quat);
    std::snprintf(detail, sizeof(detail),
                  "far-past-cap pose vs at-cap pose: pos_diff=%.9f m "
                  "att_diff=%.9f deg (must be ~0, i.e. holding)",
                  holdPosErr, holdAttErr);
    check(farPast.valid && holdPosErr < 1e-9 && holdAttErr < 1e-9,
          "bounded_extrapolation_holds_past_cap", detail);
}

// Test 4: multi-entity independence - interleaved updates for distinct
// player_ids must not cross-talk.
void testMultiEntityIndependence() {
    const double kTickPeriod = interp::RemoteEntityTracker::kTickPeriodS;
    double q0[4] = {1.0, 0.0, 0.0, 0.0};
    double angVelZero[3] = {0.0, 0.0, 0.0};

    struct Entity {
        uint8_t id;
        double vel[3];
        double pos0[3];
    };
    Entity entities[3] = {
        {1, {5.0, 0.0, 0.0}, {0.0, 10.0, 0.0}},
        {2, {0.0, 0.0, 8.0}, {100.0, 10.0, 0.0}},
        {3, {-3.0, 0.0, -3.0}, {-50.0, 10.0, 200.0}},
    };

    interp::RemoteEntityTracker tracker;
    for (uint32_t tick = 0; tick <= 30; ++tick) {
        double simT = tick * kTickPeriod;
        // Interleaved: all three entities updated within the same tick,
        // in a fixed order, exercising the shared unordered_map.
        for (const Entity& e : entities) {
            double pos[3] = {e.pos0[0] + e.vel[0] * simT, e.pos0[1],
                              e.pos0[2] + e.vel[2] * simT};
            net::AircraftState s = makeState(e.id, pos, q0, e.vel, angVelZero);
            tracker.update(e.id, s, tick, simT);
        }
    }

    double renderT = 15.5 * kTickPeriod;
    bool allOk = true;
    char detail[256] = "";
    for (const Entity& e : entities) {
        interp::InterpolatedState r = tracker.sample(e.id, renderT);
        double expected[3] = {e.pos0[0] + e.vel[0] * renderT, e.pos0[1],
                               e.pos0[2] + e.vel[2] * renderT};
        double err = 0.0;
        for (int i = 0; i < 3; ++i) {
            err = std::max(err, std::fabs(r.pos_local_m[i] - expected[i]));
        }
        if (!r.valid || err > kPosTolM) {
            allOk = false;
            std::snprintf(detail, sizeof(detail),
                          "player_id=%u pos_err=%.9f m (valid=%d)", e.id, err,
                          r.valid);
        }
    }
    check(allOk, "multi_entity_independence",
          allOk ? "" : detail);

    // remove() only affects the removed entity.
    tracker.remove(entities[1].id);
    interp::InterpolatedState removed = tracker.sample(entities[1].id, renderT);
    interp::InterpolatedState stillThere = tracker.sample(entities[0].id, renderT);
    check(!removed.valid && stillThere.valid, "remove_only_affects_target",
          "");
}

}  // namespace

int main() {
    testInterpolationCorrectness();
    testGracefulOneDropHandling();
    testBoundedExtrapolation();
    testMultiEntityIndependence();

    std::printf("\n=== interpcore_tests: %s (%d failure%s) ===\n",
                g_failures == 0 ? "PASS" : "FAIL", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
