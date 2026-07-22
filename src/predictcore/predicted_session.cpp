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

#include "predicted_session.h"

#include "geo/aircraft_orientation.h"
#include "reconciliation_math.h"
#include "reconstruction.h"

#include "math/FGLocation.h"
#include "math/FGQuaternion.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace predict {

namespace {
constexpr double kDegToRad = M_PI / 180.0;
}  // namespace

PredictedSession::PredictedSession(inc1::FlightSession& session)
    : session_(session) {}

void PredictedSession::setOrigin(double originLatDeg, double originLonDeg) {
    originLatDeg_ = originLatDeg;
    originLonDeg_ = originLonDeg;
}

void PredictedSession::setThresholds(double positionM, double attitudeDeg) {
    thresholdPosM_ = positionM;
    thresholdAttDeg_ = attitudeDeg;
}

void PredictedSession::applyCommand(const net::ControlCommand& cmd) {
    session_.setProperty("fcs/elevator-cmd-norm", net::decodeAxis(cmd.elevator));
    session_.setProperty("fcs/aileron-cmd-norm", net::decodeAxis(cmd.aileron));
    session_.setProperty("fcs/rudder-cmd-norm", net::decodeAxis(cmd.rudder));
    session_.setProperty("fcs/throttle-cmd-norm",
                          net::decodeThrottle(cmd.throttle));
}

net::ControlInput PredictedSession::tick(const net::ControlCommand& input) {
    ++clientSeq_;
    applyCommand(input);
    session_.step();
    buffer_.push_back(BufferedTick{clientSeq_, input, session_.getVState()});
    while (buffer_.size() > kRingBufferTicks) buffer_.pop_front();

    net::ControlInput packet;
    packet.newest_client_seq = clientSeq_;
    size_t n = std::min(buffer_.size(),
                         static_cast<size_t>(net::kMaxRedundantCommands));
    packet.commands.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        packet.commands.push_back(buffer_[buffer_.size() - 1 - i].cmd);
    }
    return packet;
}

PredictedSession::ReconcileResult PredictedSession::reconcile(
    uint32_t ackClientSeq, const net::AircraftState& authoritative,
    bool applyCorrection) {
    ReconcileResult result;
    // 0 means the server has not applied any client input yet (client_seq
    // numbering starts at 1 - net_client/predicted_aircraft's ++clientSeq_
    // convention) - nothing to reconcile against.
    if (ackClientSeq == 0) return result;

    auto it = std::find_if(
        buffer_.begin(), buffer_.end(),
        [&](const BufferedTick& bt) { return bt.seq == ackClientSeq; });
    bool fellOutOfBuffer = (it == buffer_.end());
    result.fellOutOfBuffer = fellOutOfBuffer;

    bool needsCorrection = fellOutOfBuffer;
    if (!fellOutOfBuffer) {
        const JSBSim::FGPropagate::VehicleState& predicted = it->state;

        double predLatDeg = predicted.vLocation.GetGeodLatitudeDeg();
        double predLonDeg = predicted.vLocation.GetLongitudeDeg();
        double predAltM = predicted.vLocation.GetGeodAltitude() * inc1::kFt2M;
        geo::LocalOffset predOffset = geo::computeLocalOffset(
            predLatDeg, predLonDeg, originLatDeg_, originLonDeg_);

        // authoritative.pos_local_m is (East, Up, -North).
        double dE = predOffset.east_m - authoritative.pos_local_m[0];
        double dU = predAltM - authoritative.pos_local_m[1];
        double dN = -predOffset.north_m - authoritative.pos_local_m[2];
        double posErrorM = std::sqrt(dE * dE + dU * dU + dN * dN);

        const JSBSim::FGQuaternion& predQ = predicted.qAttitudeLocal;
        double attErrorDeg = quaternionAngleDeg(
            predQ(1), predQ(2), predQ(3), predQ(4), authoritative.quat[0],
            authoritative.quat[1], authoritative.quat[2],
            authoritative.quat[3]);

        result.positionErrorM = posErrorM;
        result.attitudeErrorDeg = attErrorDeg;
        needsCorrection =
            (posErrorM > thresholdPosM_) || (attErrorDeg > thresholdAttDeg_);
    } else {
        // Should not happen under any latency/loss this increment tests
        // (Appendix B's buffer-sizing measurement) - degrade safely rather
        // than undefined behaviour (spec step 2).
        std::fprintf(stderr,
                      "PredictedSession: ack_client_seq %u fell out of the "
                      "state ring buffer; forcing an unconditional "
                      "correction\n",
                      ackClientSeq);
    }

    if (!needsCorrection || !applyCorrection) {
        // The negative control (applyCorrection=false) still trims stale
        // buffer entries but never rewinds the local session - the
        // divergence measured above is left to persist, which is exactly
        // what proves reconciliation (not mere chance) fixes it.
        while (!buffer_.empty() && buffer_.front().seq < ackClientSeq) {
            buffer_.pop_front();
        }
        return result;
    }

    // Reconciliation: reconstruct the authoritative VehicleState, then
    // replay every buffered input after ackClientSeq (spec step 6).
    geo::GeodeticPos geoPos = geo::invertLocalOffset(
        authoritative.pos_local_m[0], -authoritative.pos_local_m[2],
        originLatDeg_, originLonDeg_);
    double qWxyz[4] = {authoritative.quat[0], authoritative.quat[1],
                       authoritative.quat[2], authoritative.quat[3]};
    // authoritative.vel_local_mps is (East, Up, -North) -> NED.
    double vE_mps = authoritative.vel_local_mps[0];
    double vD_mps = -authoritative.vel_local_mps[1];
    double vN_mps = -authoritative.vel_local_mps[2];

    reconstructAndApply(session_, geoPos.lat_deg * kDegToRad,
                         geoPos.lon_deg * kDegToRad,
                         authoritative.pos_local_m[1], qWxyz, vN_mps, vE_mps,
                         vD_mps, authoritative.ang_vel_body_rps[0],
                         authoritative.ang_vel_body_rps[1],
                         authoritative.ang_vel_body_rps[2]);

    std::vector<BufferedTick> replay;
    for (const auto& bt : buffer_) {
        if (bt.seq > ackClientSeq) replay.push_back(bt);
    }
    buffer_.clear();
    for (const auto& bt : replay) {
        applyCommand(bt.cmd);
        session_.step();
        buffer_.push_back(BufferedTick{bt.seq, bt.cmd, session_.getVState()});
    }

    result.corrected = true;
    return result;
}

}  // namespace predict
