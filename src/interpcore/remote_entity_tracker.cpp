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

#include "remote_entity_tracker.h"

#include <algorithm>
#include <cmath>

namespace interp {

namespace {

// A handful of samples' worth of margin above what kInterpolationDelayS +
// kMaxExtrapolationS (0.4 s combined) could ever need to look back across
// at increment 3/4's established 30 Hz snapshot rate (~12 samples) -
// chosen generously rather than tightly, since the cost is trivial
// (sizeof(net::AircraftState) is under 60 bytes).
constexpr size_t kMaxHistoryPerEntity = 32;

struct Quat {
    double w, x, y, z;
};

Quat toQuat(const float wire[4]) {
    return Quat{static_cast<double>(wire[0]), static_cast<double>(wire[1]),
                static_cast<double>(wire[2]), static_cast<double>(wire[3])};
}

// Hamilton product, q1 on the left - the convention matching JSBSim's own
// attitude-kinematics formula (dq/dt = 1/2 * q * omega_quat, Stevens &
// Lewis), reused here for extrapolation (see extrapolate() below).
Quat multiply(const Quat& a, const Quat& b) {
    return Quat{
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
}

Quat normalize(const Quat& q) {
    double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (n < 1e-12) return Quat{1.0, 0.0, 0.0, 0.0};
    return Quat{q.w / n, q.x / n, q.y / n, q.z / n};
}

// Standard unit-quaternion slerp, taking the shorter arc (negating one
// side when the dot product is negative) and falling back to a normalized
// linear interpolation when the two quaternions are nearly parallel
// (avoids a near-0/0 division as sin(theta_0) -> 0).
Quat slerp(const Quat& a, const Quat& b, double t) {
    double dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    Quat bb = b;
    if (dot < 0.0) {
        bb = Quat{-b.w, -b.x, -b.y, -b.z};
        dot = -dot;
    }
    if (dot > 0.9995) {
        Quat lerp{a.w + t * (bb.w - a.w), a.x + t * (bb.x - a.x),
                  a.y + t * (bb.y - a.y), a.z + t * (bb.z - a.z)};
        return normalize(lerp);
    }
    double theta0 = std::acos(std::clamp(dot, -1.0, 1.0));
    double theta = theta0 * t;
    double sinTheta0 = std::sin(theta0);
    double s1 = std::cos(theta) - dot * std::sin(theta) / sinTheta0;
    double s2 = std::sin(theta) / sinTheta0;
    return Quat{s1 * a.w + s2 * bb.w, s1 * a.x + s2 * bb.x,
                s1 * a.y + s2 * bb.y, s1 * a.z + s2 * bb.z};
}

void writeQuat(const Quat& q, double out[4]) {
    out[0] = q.w;
    out[1] = q.x;
    out[2] = q.y;
    out[3] = q.z;
}

// Dead-reckons `state` forward by dt seconds using its own linear and
// angular velocity: position advances linearly (pos + v*dt); orientation
// composes the EXACT constant-angular-velocity rotation over dt on the
// right of the current attitude - q_new = q (x) exp(1/2 * dt * omega_quat),
// where exp of the pure quaternion 1/2*dt*omega_quat is
// (cos(dt*|omega|/2), sin(dt*|omega|/2) * omega/|omega|) - rather than the
// first-order (small-angle) approximation q + dt*1/2*q(x)omega_quat that
// robotics/games code often uses for a single small integration step. The
// exact form costs one sqrt and a sin/cos pair more than the first-order
// one and removes an error term entirely (an interpcore_tests case checks
// this against a closed-form constant-turn-rate trajectory), which matters
// here because a single extrapolation step can span the full
// kMaxExtrapolationS (0.3 s) at once, not many small physics-tick-sized
// steps - large enough that the first-order approximation's error (O((dt*
// |omega|)^2)) would be visible at realistic turn rates. Both forms share
// the same inherent limitation of any dead-reckoning: they assume omega
// stays constant for the extrapolated window, which this exact form does
// not remove, only computes correctly given that assumption. Reuses
// ang_vel_body_rps (already on the wire for reconciliation reconstruction,
// docs/increment-4-specification.md).
InterpolatedState extrapolate(const net::AircraftState& state, double dt) {
    InterpolatedState out;
    out.valid = true;
    for (int i = 0; i < 3; ++i) {
        out.pos_local_m[i] = static_cast<double>(state.pos_local_m[i]) +
                              static_cast<double>(state.vel_local_mps[i]) * dt;
    }
    Quat q = toQuat(state.quat);
    double wx = static_cast<double>(state.ang_vel_body_rps[0]);
    double wy = static_cast<double>(state.ang_vel_body_rps[1]);
    double wz = static_cast<double>(state.ang_vel_body_rps[2]);
    double wMag = std::sqrt(wx * wx + wy * wy + wz * wz);
    Quat qNew;
    if (wMag < 1e-9) {
        qNew = q;
    } else {
        double halfAngle = 0.5 * dt * wMag;
        double s = std::sin(halfAngle) / wMag;
        Quat deltaQ{std::cos(halfAngle), wx * s, wy * s, wz * s};
        qNew = multiply(q, deltaQ);
    }
    writeQuat(normalize(qNew), out.quat);
    return out;
}

}  // namespace

void RemoteEntityTracker::update(uint8_t playerId,
                                  const net::AircraftState& state,
                                  uint32_t serverTick, double arrivalTimeS) {
    Entity& e = entities_[playerId];
    e.history.push_back(Sample{state, serverTick, arrivalTimeS});
    while (e.history.size() > kMaxHistoryPerEntity) e.history.pop_front();
    // Tracks the newest sample regardless of arrival order: an
    // out-of-order-delivered chunk (possible - both channels are
    // unreliable) must not regress the latest-known anchor used to map a
    // wall-clock renderTimeS onto the server_tick timeline (sample()
    // below).
    if (serverTick >= e.latestServerTick || e.history.size() == 1) {
        e.latestServerTick = serverTick;
        e.latestArrivalTimeS = arrivalTimeS;
    }
}

void RemoteEntityTracker::remove(uint8_t playerId) {
    entities_.erase(playerId);
}

std::vector<uint8_t> RemoteEntityTracker::activePlayerIds() const {
    std::vector<uint8_t> ids;
    ids.reserve(entities_.size());
    for (const auto& [playerId, entity] : entities_) ids.push_back(playerId);
    return ids;
}

InterpolatedState RemoteEntityTracker::sample(uint8_t playerId,
                                               double renderTimeS) const {
    auto it = entities_.find(playerId);
    if (it == entities_.end() || it->second.history.empty()) {
        return InterpolatedState{};
    }
    const Entity& e = it->second;

    // Maps the caller's wall-clock renderTimeS onto this entity's own
    // server-tick timeline: "the newest sample's own tick-time, plus
    // however much wall-clock time has elapsed since it arrived" (review
    // finding N2 - see remote_entity_tracker.h's header comment).
    double targetTimelineS = e.latestServerTick * kTickPeriodS +
                              (renderTimeS - e.latestArrivalTimeS);

    const Sample& newest = e.history.back();
    double newestTimelineS = newest.serverTick * kTickPeriodS;

    if (targetTimelineS >= newestTimelineS) {
        double dt = std::min(targetTimelineS - newestTimelineS,
                              kMaxExtrapolationS);
        return extrapolate(newest.state, dt);
    }

    // Find the bracketing pair by server_tick timeline position (not
    // arrival time) - walk from the back since the common case (a render
    // time just behind the newest sample) needs only 1-2 steps.
    for (size_t i = e.history.size(); i-- > 1;) {
        const Sample& b = e.history[i];
        const Sample& a = e.history[i - 1];
        double ta = a.serverTick * kTickPeriodS;
        double tb = b.serverTick * kTickPeriodS;
        if (targetTimelineS >= ta && targetTimelineS <= tb) {
            double frac = (tb > ta) ? (targetTimelineS - ta) / (tb - ta) : 0.0;
            frac = std::clamp(frac, 0.0, 1.0);
            InterpolatedState out;
            out.valid = true;
            for (int k = 0; k < 3; ++k) {
                out.pos_local_m[k] =
                    static_cast<double>(a.state.pos_local_m[k]) +
                    frac * (static_cast<double>(b.state.pos_local_m[k]) -
                            static_cast<double>(a.state.pos_local_m[k]));
            }
            Quat qa = toQuat(a.state.quat);
            Quat qb = toQuat(b.state.quat);
            writeQuat(slerp(qa, qb, frac), out.quat);
            return out;
        }
    }

    // targetTimelineS is older than every buffered sample (a render time
    // requested far enough in the past that history has already been
    // pruned past it) - the oldest available sample is the closest honest
    // answer.
    InterpolatedState out;
    out.valid = true;
    const Sample& oldest = e.history.front();
    for (int k = 0; k < 3; ++k) {
        out.pos_local_m[k] = static_cast<double>(oldest.state.pos_local_m[k]);
    }
    Quat q = toQuat(oldest.state.quat);
    writeQuat(q, out.quat);
    return out;
}

}  // namespace interp
