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

// The reusable predict/buffer/reconcile/replay algorithm (docs/increment-
// 4-specification.md, "Architecture"): shared unchanged by the Godot
// client (PredictedAircraft) and flight_test_client's --mode prediction,
// so both exercise the identical reconciliation code. Godot-free; wraps a
// caller-owned FlightSession by reference rather than owning one itself,
// since the caller (a GDExtension node or a standalone test binary) is
// responsible for that session's lifetime and initial condition/trim.
#pragma once

#include "netcore/protocol.h"
#include "test_runner.h"

#include <cstdint>
#include <deque>

namespace predict {

class PredictedSession {
public:
    explicit PredictedSession(inc1::FlightSession& session);

    // The ServerWelcome-provided reference point, needed to convert a
    // received StateSnapshot's local-frame position back to geodetic
    // coordinates for reconstruction (docs/increment-4-specification.md,
    // "Reconstruction gate").
    void setOrigin(double originLatDeg, double originLonDeg);

    // Starting-point thresholds (spec, "Client-side prediction algorithm"
    // step 4) - tunable, not load-bearing at these defaults.
    void setThresholds(double positionM, double attitudeDeg);

    // One physics tick (120 Hz): applies `input` to the local session,
    // steps it, buffers (client_seq, input, resulting state), and returns
    // the redundant ControlInput packet to send (the last
    // min(kMaxRedundantCommands, buffered) commands, newest first).
    net::ControlInput tick(const net::ControlCommand& input);

    struct ReconcileResult {
        bool corrected = false;
        bool fellOutOfBuffer = false;
        double positionErrorM = 0.0;
        double attitudeErrorDeg = 0.0;
    };

    // Called when a StateSnapshot carrying this client's own AircraftState
    // arrives; `authoritative` must already be the entry the caller
    // matched by player_id (spec step 1) - this class has no notion of
    // player IDs. Compares the buffered prediction at ack_client_seq
    // against `authoritative`; if beyond threshold and applyCorrection is
    // true, reconstructs and replays every buffered input since
    // ack_client_seq (spec step 6). applyCorrection=false still measures
    // and returns the divergence but never corrects the local session -
    // the negative control the increment's own test plan requires (test
    // 3 must demonstrate that disabling reconciliation lets a forced
    // misprediction persist, not just skip measuring it).
    ReconcileResult reconcile(uint32_t ackClientSeq,
                               const net::AircraftState& authoritative,
                               bool applyCorrection = true);

    uint32_t currentSeq() const { return clientSeq_; }
    size_t bufferedTicks() const { return buffer_.size(); }

private:
    struct BufferedTick {
        uint32_t seq;
        net::ControlCommand cmd;
        JSBSim::FGPropagate::VehicleState state;
    };

    void applyCommand(const net::ControlCommand& cmd);

    inc1::FlightSession& session_;
    uint32_t clientSeq_ = 0;
    double originLatDeg_ = 0.0;
    double originLonDeg_ = 0.0;
    double thresholdPosM_ = 0.5;
    double thresholdAttDeg_ = 2.0;
    std::deque<BufferedTick> buffer_;

    // 10 s at 120 Hz. Appendix B measured a 300-tick (2.5 s) buffer as
    // "well under 500 KB" and generous for steady-state round trips; a
    // real end-to-end run on a loaded machine found a bigger source of
    // delay than round-trip latency, though: this client's own trim()
    // blocks the network poll loop entirely (nothing is sent or received
    // while it runs), and its wall-clock duration is not bounded under
    // real system load, during which the server - which flies
    // continuously regardless of client connections - keeps advancing.
    // 10 s costs nothing worth economising (sizeof(VehicleState) is 1496
    // bytes, so this is ~1.75 MB) and comfortably covers that bootstrap
    // window rather than just steady-state jitter.
    static constexpr size_t kRingBufferTicks = 1200;
};

}  // namespace predict
