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

// flight_test_client: the automated network test harness (docs/
// increment-3-specification.md, "flight_test_client (automated)").
// Godot-free, links netcore only - the same wire-format code the server
// and the Godot client compile. Three modes, selected by --mode:
//
//   fidelity_input  - connects, logs received snapshots, compares them
//                      field-by-field against the server's own sent log
//                      (transport fidelity - spec review B1), then
//                      commands a scripted nose-up input and asserts the
//                      server's authoritative altitude responds (input
//                      path - spec review B2), then disconnects cleanly.
//   version_reject  - handshakes with a deliberately wrong protocol
//                      version and asserts ServerReject + disconnect.
//   resilience      - the fidelity_input flow's liveness checks only
//                      (handshake, snapshots kept flowing, no NaN, clean
//                      teardown), run against a host:port that in
//                      practice is net_relay's listen address rather
//                      than the server directly (spec, "Network
//                      impairment"): not exact-criteria, just liveness.
//
// Per spec review M2, all scenario timing is measured from server_tick
// (carried on every snapshot) and anchored to the reliable
// ServerWelcome, never to wall-clock or an unreliable first snapshot.
#include "netcore/net_client.h"
#include "netcore/protocol.h"
#include "netcore/snapshot_log.h"
#include "predictcore/predicted_session.h"
#include "predictcore/reconciliation_math.h"
#include "predictcore/reconstruction.h"
#include "test_runner.h"

#include "geo/aircraft_orientation.h"
#include "math/FGQuaternion.h"

#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Config {
    std::string mode = "fidelity_input";
    std::string host = "127.0.0.1";
    uint16_t port = 45300;
    std::string serverLog = "server_networked.csv";
    std::string receivedLog = "client_received.csv";

    // --mode prediction only (docs/increment-4-specification.md, "Test
    // plan").
    bool reconciliation = true;         // --reconciliation off: negative control
    std::string inputSchedule = "step"; // step | analog
    std::string predictedLog = "client_predicted.csv";
    std::string groundTruthLog = "server_pitch_response.csv";
    bool forceDesync = false;           // --force-desync: test 3's artificial misprediction
    double durationS = 8.0;
    // Eventual-agreement (test 2) compares against a *zero-latency*
    // standalone ground truth (pitch_response.cpp never experiences input
    // propagation delay). Under injected latency the server's own
    // authoritative trajectory is genuinely, persistently behind that
    // ground truth by roughly the injected delay - a real effect, not a
    // bug - since it can only apply commands once they actually arrive;
    // every reconciliation then pulls this client back toward that
    // lagged truth. The comparison's premise only holds on a clean link,
    // so run_tests.sh passes this for its latency-repeat step.
    bool checkEventualAgreement = true; // --skip-eventual-agreement clears this
};

Config parseArgs(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto nextVal = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
        };
        if (arg == "--mode") cfg.mode = nextVal();
        else if (arg == "--host") cfg.host = nextVal();
        else if (arg == "--port") cfg.port = static_cast<uint16_t>(std::atoi(nextVal().c_str()));
        else if (arg == "--server-log") cfg.serverLog = nextVal();
        else if (arg == "--received-log") cfg.receivedLog = nextVal();
        else if (arg == "--predicted-log") cfg.predictedLog = nextVal();
        else if (arg == "--ground-truth-log") cfg.groundTruthLog = nextVal();
        else if (arg == "--input-schedule") cfg.inputSchedule = nextVal();
        else if (arg == "--duration-s") cfg.durationS = std::atof(nextVal().c_str());
        else if (arg == "--reconciliation") cfg.reconciliation = (nextVal() != "off");
        else if (arg == "--force-desync") cfg.forceDesync = true;
        else if (arg == "--skip-eventual-agreement") cfg.checkEventualAgreement = false;
    }
    return cfg;
}

bool waitForConnect(net::NetClient& client, int timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        client.poll(10, [](const ENetEvent&) {});
        if (client.isConnected()) return true;
    }
    return client.isConnected();
}

bool waitForDisconnect(net::NetClient& client, int timeoutMs) {
    // Checks connection *state* rather than watching for a fresh
    // DISCONNECT *event*: an earlier poll() elsewhere (e.g.
    // doHandshake's, which drains every currently-available event in one
    // sweep) may already have consumed the actual disconnect event by
    // the time this runs, even though NetClient's isConnected() was
    // already updated when it happened.
    if (!client.isConnected()) return true;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline && client.isConnected()) {
        client.poll(10, [](const ENetEvent&) {});
    }
    return !client.isConnected();
}

enum class HandshakeResult { kWelcome, kReject, kTimeout };

struct HandshakeOutcome {
    HandshakeResult result = HandshakeResult::kTimeout;
    net::ServerWelcome welcome;
    net::ServerReject reject;
};

HandshakeOutcome doHandshake(net::NetClient& client, uint8_t versionToSend, int timeoutMs) {
    net::ClientHello hello{versionToSend};
    client.send(net::kChannelReliable, net::serializeClientHello(hello), true);
    client.flush();
    HandshakeOutcome outcome;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline &&
           outcome.result == HandshakeResult::kTimeout) {
        client.poll(10, [&](const ENetEvent& event) {
            if (event.type != ENET_EVENT_TYPE_RECEIVE) return;
            net::MessageTag tag;
            if (!net::peekMessageTag(event.packet->data, event.packet->dataLength, tag)) return;
            if (tag == net::MessageTag::kServerWelcome &&
                net::deserializeServerWelcome(event.packet->data, event.packet->dataLength, outcome.welcome)) {
                outcome.result = HandshakeResult::kWelcome;
            } else if (tag == net::MessageTag::kServerReject &&
                       net::deserializeServerReject(event.packet->data, event.packet->dataLength, outcome.reject)) {
                outcome.result = HandshakeResult::kReject;
            }
        });
    }
    return outcome;
}

bool fieldsClose(const float* a, const float* b, int count, float tolerance) {
    for (int i = 0; i < count; ++i) {
        if (std::fabs(a[i] - b[i]) > tolerance) return false;
    }
    return true;
}

// Compares the client's received-snapshot log against the server's
// sent-snapshot log, matched by server_tick (spec review B1's
// resolution). float32 tolerance per spec's open question: expected to
// be an exact match on a clean link (same bits serialized then
// deserialized, no arithmetic in between), so the tolerance exists only
// to absorb CSV text-formatting precision, not genuine wire imprecision
// - see docs/increment-3-specification.md Appendix B for why this is
// looser than it needs to be by a wide margin.
constexpr float kFidelityTolerance = 1e-4f;

bool compareSnapshotLogs(const std::string& serverLogPath,
                          const std::string& receivedLogPath, int& matched,
                          int& mismatched) {
    matched = 0;
    mismatched = 0;
    std::vector<net::LoggedAircraftRow> sent, received;
    std::string error;
    if (!net::readSnapshotCsv(serverLogPath, sent, error)) {
        std::fprintf(stderr, "fidelity: %s\n", error.c_str());
        return false;
    }
    if (!net::readSnapshotCsv(receivedLogPath, received, error)) {
        std::fprintf(stderr, "fidelity: %s\n", error.c_str());
        return false;
    }
    std::map<uint32_t, net::AircraftState> sentByTick;
    for (const auto& row : sent) sentByTick[row.server_tick] = row.state;

    for (const auto& row : received) {
        auto it = sentByTick.find(row.server_tick);
        if (it == sentByTick.end()) continue;  // not itself a fidelity failure - see spec residual risk 3
        const net::AircraftState& s = it->second;
        const net::AircraftState& c = row.state;
        bool rowOk = s.player_id == c.player_id &&
                     fieldsClose(s.pos_local_m, c.pos_local_m, 3, kFidelityTolerance) &&
                     fieldsClose(s.quat, c.quat, 4, kFidelityTolerance) &&
                     fieldsClose(s.vel_local_mps, c.vel_local_mps, 3, kFidelityTolerance) &&
                     fieldsClose(s.ang_vel_body_rps, c.ang_vel_body_rps, 3,
                                 kFidelityTolerance) &&
                     s.status_flags == c.status_flags;
        if (rowOk) {
            ++matched;
        } else {
            ++mismatched;
            std::fprintf(stderr, "fidelity mismatch at server_tick=%u\n", row.server_tick);
        }
    }
    return mismatched == 0 && matched > 0;
}

// Empirically-tuned threshold (docs/increment-3-specification.md
// Appendix B): full-aft-elevator at cruise power produces a fast, clear
// climb well above this within the test's ~4.5 s nose-up window; the
// value is far below that, so this is a liveness-of-the-input-path
// check, not a tight physics assertion (those run server-side).
constexpr float kAltitudeGainThresholdM = 3.0f;

// ---------------------------------------------------------------------
// --mode prediction (docs/increment-4-specification.md, "Test plan")
// ---------------------------------------------------------------------

constexpr double kDegToRad = M_PI / 180.0;

bool makeTrimmedSession(inc1::FlightSession& session) {
    std::string error;
    if (!session.initialize(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return false;
    }
    // Same initial condition as flight_server's own (spec, "Reconstruction
    // gate" premise: identical IC + trim + input schedule => bit-identical
    // replay, Appendix B).
    session.setInitialCondition(5000.0, 100.0, 0.0, 0.0, 0.0, 0.0);
    if (!session.trim(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return false;
    }
    return true;
}

// Builds the same AircraftState representation flight_server's own
// snapshot-building code does (server/main.cpp), so the client's own
// logged predictions and the server's logged ground truth are directly,
// field-by-field comparable (spec step 3: "compares like with like").
net::AircraftState samplePredictedState(inc1::FlightSession& session,
                                          double originLatDeg,
                                          double originLonDeg) {
    inc1::FlightSample s = session.sample();
    net::AircraftState a;
    a.player_id = 0;  // unused for this log
    geo::LocalOffset off = geo::computeLocalOffset(s.lat_deg, s.lon_deg,
                                                     originLatDeg, originLonDeg);
    a.pos_local_m[0] = static_cast<float>(off.east_m);
    a.pos_local_m[1] = static_cast<float>(s.alt_m);
    a.pos_local_m[2] = static_cast<float>(-off.north_m);
    JSBSim::FGQuaternion qLocal = session.getVState().qAttitudeLocal;
    a.quat[0] = static_cast<float>(qLocal(1));
    a.quat[1] = static_cast<float>(qLocal(2));
    a.quat[2] = static_cast<float>(qLocal(3));
    a.quat[3] = static_cast<float>(qLocal(4));
    a.vel_local_mps[0] = static_cast<float>(s.vel_east_mps);
    a.vel_local_mps[1] = static_cast<float>(-s.vel_down_mps);
    a.vel_local_mps[2] = static_cast<float>(-s.vel_north_mps);
    a.ang_vel_body_rps[0] =
        static_cast<float>(session.property("velocities/p-rad_sec"));
    a.ang_vel_body_rps[1] =
        static_cast<float>(session.property("velocities/q-rad_sec"));
    a.ang_vel_body_rps[2] =
        static_cast<float>(session.property("velocities/r-rad_sec"));
    return a;
}

void writePredictedCsvHeader(std::ostream& out) {
    out << "client_seq,pos_e,pos_u,pos_n,quat_x,quat_y,quat_z,quat_w,vel_e,"
           "vel_u,vel_n,ang_vel_p,ang_vel_q,ang_vel_r\n";
}

void writePredictedCsvRow(std::ostream& out, uint32_t seq,
                           const net::AircraftState& a) {
    out << std::setprecision(9);
    out << seq << ',' << a.pos_local_m[0] << ',' << a.pos_local_m[1] << ','
        << a.pos_local_m[2] << ',' << a.quat[0] << ',' << a.quat[1] << ','
        << a.quat[2] << ',' << a.quat[3] << ',' << a.vel_local_mps[0] << ','
        << a.vel_local_mps[1] << ',' << a.vel_local_mps[2] << ','
        << a.ang_vel_body_rps[0] << ',' << a.ang_vel_body_rps[1] << ','
        << a.ang_vel_body_rps[2] << '\n';
}

struct PredictedRow {
    uint32_t client_seq;
    net::AircraftState state;
};

bool readPredictedCsv(const std::string& path, std::vector<PredictedRow>& out,
                       std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "failed to open predicted log: " + path;
        return false;
    }
    out.clear();
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string field;
        std::vector<double> values;
        while (std::getline(ss, field, ',')) values.push_back(std::stod(field));
        if (values.size() != 14) {
            error = "malformed predicted-log row (expected 14 fields, got " +
                    std::to_string(values.size()) + "): " + line;
            return false;
        }
        PredictedRow row;
        row.client_seq = static_cast<uint32_t>(values[0]);
        row.state.pos_local_m[0] = static_cast<float>(values[1]);
        row.state.pos_local_m[1] = static_cast<float>(values[2]);
        row.state.pos_local_m[2] = static_cast<float>(values[3]);
        row.state.quat[0] = static_cast<float>(values[4]);
        row.state.quat[1] = static_cast<float>(values[5]);
        row.state.quat[2] = static_cast<float>(values[6]);
        row.state.quat[3] = static_cast<float>(values[7]);
        row.state.vel_local_mps[0] = static_cast<float>(values[8]);
        row.state.vel_local_mps[1] = static_cast<float>(values[9]);
        row.state.vel_local_mps[2] = static_cast<float>(values[10]);
        row.state.ang_vel_body_rps[0] = static_cast<float>(values[11]);
        row.state.ang_vel_body_rps[1] = static_cast<float>(values[12]);
        row.state.ang_vel_body_rps[2] = static_cast<float>(values[13]);
        out.push_back(row);
    }
    return true;
}

// step: hold level ~5s (600 ticks) then command full nose-up, matching
// src/scenarios/pitch_response.cpp's exact schedule so test 2 can compare
// against flight_server --scenario pitch_response's already-existing
// ground truth (same IC, same schedule, same 120 Hz tick => tick-for-tick
// comparable). Critically, pitch_response.cpp never touches throttle,
// aileron, or rudder - they stay at whatever trim converged to (measured:
// throttle ~0.79, aileron ~-0.075, rudder ~-0.004 - not negligible) - so
// trimmedElevator/Aileron/Rudder/Throttle (read once right after this
// client's own trim()) are used rather than hardcoded zeros; zeroing
// aileron/rudder from tick 0 (as a real player's untouched stick would)
// measurably diverges from the ground truth within a couple of seconds
// (found by direct comparison). analog: full-amplitude ~2 Hz aileron
// waggle throughout (there is no ground truth to match, so a clean
// bang-bang/full-range input is fine), the aggressive-analog input test 5
// needs (Appendix B's server-input-model experiments used the identical
// shape).
net::ControlCommand sampleScriptedInput(const std::string& schedule, long tick,
                                          double trimmedAileron,
                                          double trimmedRudder,
                                          double trimmedThrottle) {
    net::ControlCommand cmd;
    if (schedule == "analog") {
        double t = static_cast<double>(tick) / 120.0;
        cmd.aileron = net::encodeAxis(std::sin(2.0 * M_PI * 2.0 * t));
        cmd.throttle = net::encodeThrottle(1.0);
    } else {
        bool noseUp = tick >= 600;
        cmd.elevator = noseUp ? net::encodeAxis(-1.0) : net::encodeAxis(0.0);
        cmd.aileron = net::encodeAxis(trimmedAileron);
        cmd.rudder = net::encodeAxis(trimmedRudder);
        cmd.throttle = net::encodeThrottle(trimmedThrottle);
    }
    return cmd;
}

// Shared by fidelity_input and resilience: connects, handshakes, sends a
// scripted input schedule (hold level, then command nose-up) while
// logging every received snapshot, then disconnects. `strict` selects
// whether the transport-fidelity file comparison and the tighter
// input-path threshold apply (fidelity_input) or only liveness/no-NaN/
// clean-teardown are asserted (resilience, spec: "asserts liveness and
// flow, not exact increment-1 criteria").
int runScriptedInputFlow(const Config& cfg, bool strict) {
    if (enet_initialize() != 0) {
        std::fprintf(stderr, "error: enet_initialize failed\n");
        return 2;
    }
    net::NetClient client;
    std::string error;
    int connectTimeoutMs = strict ? 2000 : 5000;  // resilience: relay adds real latency
    if (!client.connect(cfg.host, cfg.port, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        enet_deinitialize();
        return 2;
    }
    if (!waitForConnect(client, connectTimeoutMs)) {
        std::printf("connect: FAIL\n");
        enet_deinitialize();
        return 1;
    }
    std::printf("connect: PASS\n");

    HandshakeOutcome hs = doHandshake(client, net::kProtocolVersion, connectTimeoutMs);
    bool handshakeOk = hs.result == HandshakeResult::kWelcome;
    std::printf("handshake: %s\n", handshakeOk ? "PASS" : "FAIL");
    if (!handshakeOk) {
        client.stop();
        enet_deinitialize();
        return 1;
    }
    std::printf("  assigned_player_id=%u snapshot_hz=%u origin=(%.6f,%.6f)\n",
                hs.welcome.assigned_player_id, hs.welcome.snapshot_hz,
                hs.welcome.origin_lat_deg, hs.welcome.origin_lon_deg);

    std::ofstream receivedLog(cfg.receivedLog);
    net::writeSnapshotCsvHeader(receivedLog);

    bool anyNan = false;
    int snapshotCount = 0;
    float lastAltitude = 0.0f;
    bool haveAnySnapshot = false;
    float baselineAltitude = 0.0f;
    bool sentNoseUp = false;
    uint32_t noseUpStartTick = 0;
    uint32_t latestTick = 0;
    float maxAltitudeAfterInput = -std::numeric_limits<float>::infinity();

    auto onSnapshot = [&](const ENetEvent& event) {
        if (event.type != ENET_EVENT_TYPE_RECEIVE) return;
        net::MessageTag tag;
        if (!net::peekMessageTag(event.packet->data, event.packet->dataLength, tag)) return;
        if (tag != net::MessageTag::kStateSnapshot) return;
        net::StateSnapshot snap;
        if (!net::deserializeStateSnapshot(event.packet->data, event.packet->dataLength, snap)) return;
        net::writeSnapshotCsvRow(receivedLog, snap);
        ++snapshotCount;
        for (const auto& a : snap.aircraft) {
            for (float v : a.pos_local_m) if (!std::isfinite(v)) anyNan = true;
            for (float v : a.quat) if (!std::isfinite(v)) anyNan = true;
            for (float v : a.vel_local_mps) if (!std::isfinite(v)) anyNan = true;
        }
        if (!snap.aircraft.empty()) {
            haveAnySnapshot = true;
            latestTick = snap.server_tick;
            lastAltitude = snap.aircraft[0].pos_local_m[1];
            if (!sentNoseUp) {
                baselineAltitude = lastAltitude;
            } else if (snap.server_tick > noseUpStartTick) {
                maxAltitudeAfterInput = std::max(maxAltitudeAfterInput, lastAltitude);
            }
        }
    };

    const auto testDuration = strict ? std::chrono::milliseconds(6000)
                                      : std::chrono::milliseconds(9000);
    const auto noseUpAt = strict ? std::chrono::milliseconds(1500)
                                  : std::chrono::milliseconds(3000);
    auto start = std::chrono::steady_clock::now();
    auto lastInputSend = start - std::chrono::milliseconds(100);
    uint32_t clientSeq = 0;

    while (std::chrono::steady_clock::now() - start < testDuration) {
        client.poll(10, onSnapshot);

        auto elapsed = std::chrono::steady_clock::now() - start;
        // Client input send: 60 Hz default (spec, "Rates"); a ~16 ms
        // local loop period approximates that without a separate timer.
        if (std::chrono::steady_clock::now() - lastInputSend >=
            std::chrono::milliseconds(16)) {
            lastInputSend = std::chrono::steady_clock::now();
            bool noseUpPhase = elapsed >= noseUpAt;
            if (noseUpPhase && !sentNoseUp) {
                sentNoseUp = true;
                noseUpStartTick = latestTick;
            }
            // No redundancy needed (count=1): this test's server-side
            // command buffer accepts single-command packets fine (it just
            // sees no benefit from redundancy) - increment 3's tests are
            // not exercising loss-resilience of the input path itself.
            net::ControlInput input;
            input.newest_client_seq = ++clientSeq;
            net::ControlCommand cmd;
            cmd.elevator = noseUpPhase ? net::encodeAxis(-1.0) : net::encodeAxis(0.0);
            cmd.aileron = 0;
            cmd.rudder = 0;
            // Full throttle throughout (including baseline) so a single
            // ControlInput schedule never yanks throttle down from
            // whatever trim set it to - see docs/increment-3-
            // specification.md's implementation notes for why this test
            // does not attempt to hold the exact trimmed throttle.
            cmd.throttle = net::encodeThrottle(1.0);
            input.commands.push_back(cmd);
            client.send(net::kChannelUnreliable, net::serializeControlInput(input), false);
        }
    }

    client.send(net::kChannelReliable, net::serializeClientBye(), true);
    client.flush();
    client.disconnect();
    bool disconnected = waitForDisconnect(client, 3000);
    std::printf("clean_disconnect: %s\n", disconnected ? "PASS" : "FAIL");

    receivedLog.close();
    client.stop();
    enet_deinitialize();

    bool noNan = !anyNan;
    std::printf("no_nan: %s\n", noNan ? "PASS" : "FAIL");
    std::printf("snapshots_received: %d\n", snapshotCount);
    bool flowOk = snapshotCount > 0 && haveAnySnapshot;
    std::printf("snapshot_flow: %s\n", flowOk ? "PASS" : "FAIL");

    bool overall = handshakeOk && disconnected && noNan && flowOk;

    if (strict) {
        bool inputPathOk = (maxAltitudeAfterInput - baselineAltitude) >= kAltitudeGainThresholdM;
        std::printf("input_path: %s (baseline_alt_m=%.3f max_alt_after_m=%.3f gain_m=%.3f)\n",
                    inputPathOk ? "PASS" : "FAIL", baselineAltitude, maxAltitudeAfterInput,
                    maxAltitudeAfterInput - baselineAltitude);
        overall = overall && inputPathOk;

        int matched = 0, mismatched = 0;
        bool fidelityOk = compareSnapshotLogs(cfg.serverLog, cfg.receivedLog, matched, mismatched);
        std::printf("transport_fidelity: %s (matched=%d mismatched=%d)\n",
                    fidelityOk ? "PASS" : "FAIL", matched, mismatched);
        overall = overall && fidelityOk;
    } else {
        std::printf("resilience: %s\n", overall ? "PASS" : "FAIL");
    }

    return overall ? 0 : 1;
}

int runVersionReject(const Config& cfg) {
    if (enet_initialize() != 0) {
        std::fprintf(stderr, "error: enet_initialize failed\n");
        return 2;
    }
    net::NetClient client;
    std::string error;
    if (!client.connect(cfg.host, cfg.port, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        enet_deinitialize();
        return 2;
    }
    if (!waitForConnect(client, 2000)) {
        std::printf("connect: FAIL\n");
        enet_deinitialize();
        return 1;
    }

    HandshakeOutcome hs = doHandshake(client, net::kProtocolVersion + 1, 2000);
    bool rejectedForVersion =
        hs.result == HandshakeResult::kReject &&
        hs.reject.reason_code == static_cast<uint8_t>(net::RejectReason::kVersionMismatch);
    std::printf("version_reject: %s\n", rejectedForVersion ? "PASS" : "FAIL");

    // The server only sends the rejection; it does not itself initiate
    // the disconnect (an immediate send-then-disconnect on the same peer
    // races the reject packet's own delivery - see net_server's
    // ClientHello handling). Now that this client has the message in
    // hand, it tears the connection down itself.
    client.disconnect();
    bool disconnected = waitForDisconnect(client, 2000);
    std::printf("version_reject_disconnect: %s\n", disconnected ? "PASS" : "FAIL");

    client.stop();
    enet_deinitialize();
    return (rejectedForVersion && disconnected) ? 0 : 1;
}

// Runs a real PredictedSession against a live flight_server (optionally
// through net_relay), exercising docs/increment-4-specification.md's
// "Test plan" assertions 1-5. `cfg.reconciliation=false` gives test 3 its
// required negative control; `cfg.forceDesync` gives test 3 a
// deterministic artificial misprediction (an ~11 m position offset the
// server never sees, injected via the same verified
// predict::reconstructAndApply() the real reconciliation path uses - see
// "Open questions": chosen over probabilistic loss-forcing so the test is
// not flaky). `cfg.inputSchedule="analog"` is test 5's aggressive-analog
// resilience scenario.
int runPredictionMode(const Config& cfg) {
    // Resolve caller-relative log paths to absolute *before* chdir'ing:
    // this mode is the first in flight_test_client to construct a
    // FlightSession, and c172x's own <output> block can create a stray
    // JSBout172B.csv during LoadModel() before FlightSession's
    // DisableOutput() call has a chance to suppress it (same issue
    // .gitignore already documents for flight_server/the Godot
    // GDExtension - increment 1 spec review finding F7). flight_server
    // contains it by chdir'ing into results/ itself; done the same way
    // here rather than leaking the stray file into whatever directory
    // this binary happens to be invoked from.
    std::string predictedLogAbs =
        std::filesystem::absolute(cfg.predictedLog).string();
    std::string groundTruthLogAbs =
        std::filesystem::absolute(cfg.groundTruthLog).string();
    std::error_code ec;
    std::filesystem::create_directories("results", ec);
    std::filesystem::current_path("results", ec);

    if (enet_initialize() != 0) {
        std::fprintf(stderr, "error: enet_initialize failed\n");
        return 2;
    }
    net::NetClient client;
    std::string error;
    if (!client.connect(cfg.host, cfg.port, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        enet_deinitialize();
        return 2;
    }
    if (!waitForConnect(client, 5000)) {
        std::printf("connect: FAIL\n");
        enet_deinitialize();
        return 1;
    }
    std::printf("connect: PASS\n");

    HandshakeOutcome hs = doHandshake(client, net::kProtocolVersion, 5000);
    bool handshakeOk = hs.result == HandshakeResult::kWelcome;
    std::printf("handshake: %s\n", handshakeOk ? "PASS" : "FAIL");
    if (!handshakeOk) {
        client.stop();
        enet_deinitialize();
        return 1;
    }
    double originLat = hs.welcome.origin_lat_deg;
    double originLon = hs.welcome.origin_lon_deg;
    uint8_t myPlayerId = hs.welcome.assigned_player_id;

    inc1::FlightSession session;
    if (!makeTrimmedSession(session)) {
        client.stop();
        enet_deinitialize();
        return 2;
    }
    double trimmedAileron = session.property("fcs/aileron-cmd-norm");
    double trimmedRudder = session.property("fcs/rudder-cmd-norm");
    double trimmedThrottle = session.property("fcs/throttle-cmd-norm");
    predict::PredictedSession predicted(session);
    predicted.setOrigin(originLat, originLon);
    predicted.setThresholds(0.5, 2.0);  // spec starting points

    std::ofstream predictedLog(predictedLogAbs);
    writePredictedCsvHeader(predictedLog);

    bool anyNan = false;
    int correctionCount = 0;
    double maxPosErrorM = 0.0;
    double maxAttErrorDeg = 0.0;
    bool anyFellOutOfBuffer = false;

    // Immediate-response tracking (test 1 / acceptance criterion 3): pitch
    // *rate* (q), not pitch angle (theta) - checked against the *local
    // prediction*, not a received snapshot. theta is an integral of q and
    // (confirmed empirically) barely moves in 2 ticks even once the
    // control surface is fully deflected; q is the directly-affected
    // quantity and shows a clear, immediate, monotonically growing signal
    // starting the very first tick the new command is applied.
    double qAtNoseUp = 0.0;
    double qTwoTicksLater = 0.0;
    bool haveImmediateResponseSample = false;

    // The server flies its aircraft continuously from its own process
    // start, independent of when any client connects (unchanged increment
    // 3 behaviour) - so a client's freshly-trimmed local prediction
    // (starting at the origin) can legitimately be far from the server's
    // *current* position the moment it first connects, if the server was
    // already flying beforehand. Reconciliation correctly snaps to it on
    // the first ack (confirmed empirically: a large one-time correction,
    // then normal sub-metre tracking from then on) - that startup
    // transient is expected, not a "bounded envelope" violation, so it is
    // excluded from the envelope tracking below (though still counted as
    // a real correction).
    constexpr uint32_t kBootstrapWarmupSeq = 60;

    auto onEvent = [&](const ENetEvent& event) {
        if (event.type != ENET_EVENT_TYPE_RECEIVE) return;
        net::MessageTag tag;
        if (!net::peekMessageTag(event.packet->data, event.packet->dataLength, tag)) return;
        if (tag != net::MessageTag::kStateSnapshot) return;
        net::StateSnapshot snap;
        if (!net::deserializeStateSnapshot(event.packet->data, event.packet->dataLength, snap)) return;
        for (const net::AircraftState& a : snap.aircraft) {
            if (a.player_id != myPlayerId) continue;
            for (float v : a.pos_local_m) if (!std::isfinite(v)) anyNan = true;
            for (float v : a.quat) if (!std::isfinite(v)) anyNan = true;
            // Negative control (cfg.reconciliation=false): still measure
            // the divergence via reconcile()'s applyCorrection=false path,
            // just never act on it - otherwise there is nothing to assert
            // "persists" against.
            predict::PredictedSession::ReconcileResult r =
                predicted.reconcile(snap.ack_client_seq, a, cfg.reconciliation);
            if (snap.ack_client_seq > kBootstrapWarmupSeq) {
                maxPosErrorM = std::max(maxPosErrorM, r.positionErrorM);
                maxAttErrorDeg = std::max(maxAttErrorDeg, r.attitudeErrorDeg);
            }
            anyFellOutOfBuffer = anyFellOutOfBuffer || r.fellOutOfBuffer;
            if (r.corrected) ++correctionCount;
            break;
        }
    };

    constexpr int kForceDesyncTick = 200;  // well after trim settles

    auto nextTick = std::chrono::steady_clock::now();
    constexpr auto kTickPeriod = std::chrono::duration<double>(1.0 / 120.0);
    long tick = 0;
    const long totalTicks = std::lround(cfg.durationS * 120.0);

    while (tick < totalTicks) {
        client.poll(0, onEvent);
        auto now = std::chrono::steady_clock::now();
        if (now < nextTick) {
            auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(nextTick - now).count();
            client.poll(static_cast<uint32_t>(std::max<int64_t>(waitMs, 0)), onEvent);
            continue;
        }

        if (cfg.forceDesync && tick == kForceDesyncTick) {
            inc1::FlightSample s = session.sample();
            JSBSim::FGQuaternion qTrue = session.getVState().qAttitudeLocal;
            double qWxyz[4] = {qTrue(1), qTrue(2), qTrue(3), qTrue(4)};
            double p = session.property("velocities/p-rad_sec");
            double q = session.property("velocities/q-rad_sec");
            double r = session.property("velocities/r-rad_sec");
            // ~11 m north offset (0.0001 deg lat) - well above the 0.5 m
            // threshold, attitude/velocity/rates left exactly as they
            // were, so this is a pure, deterministic position
            // misprediction the server never sees.
            predict::reconstructAndApply(
                session, (s.lat_deg + 0.0001) * kDegToRad, s.lon_deg * kDegToRad,
                s.alt_m, qWxyz, s.vel_north_mps, s.vel_east_mps, s.vel_down_mps,
                p, q, r);
        }

        net::ControlCommand cmd = sampleScriptedInput(
            cfg.inputSchedule, tick, trimmedAileron, trimmedRudder, trimmedThrottle);
        net::ControlInput packet = predicted.tick(cmd);
        client.send(net::kChannelUnreliable, net::serializeControlInput(packet), false);

        net::AircraftState predState = samplePredictedState(session, originLat, originLon);
        writePredictedCsvRow(predictedLog, predicted.currentSeq(), predState);

        if (cfg.inputSchedule == "step") {
            if (tick == 600) {
                qAtNoseUp = session.property("velocities/q-rad_sec");
            } else if (tick == 602) {
                qTwoTicksLater = session.property("velocities/q-rad_sec");
                haveImmediateResponseSample = true;
            }
        }

        ++tick;
        nextTick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(kTickPeriod);
    }

    client.send(net::kChannelReliable, net::serializeClientBye(), true);
    client.flush();
    client.disconnect();
    bool disconnected = waitForDisconnect(client, 3000);
    std::printf("clean_disconnect: %s\n", disconnected ? "PASS" : "FAIL");
    predictedLog.close();
    client.stop();
    enet_deinitialize();

    bool noNan = !anyNan;
    std::printf("no_nan: %s\n", noNan ? "PASS" : "FAIL");
    std::printf("buffer_window_sufficient: %s\n", anyFellOutOfBuffer ? "FAIL" : "PASS");
    std::printf("correction_count=%d max_pos_error_m=%.4f max_att_error_deg=%.4f\n",
                correctionCount, maxPosErrorM, maxAttErrorDeg);

    bool overall = handshakeOk && disconnected && noNan && !anyFellOutOfBuffer;

    // Test 4 (corrected per review finding B1): bounded envelope, not
    // single-correction convergence. A small multiple of the 0.5 m / 2 deg
    // thresholds - not "must never exceed the threshold at all" (a
    // reconciling snapshot is expected to observe some error right up to
    // the moment it corrects).
    if (!cfg.forceDesync) {
        bool boundedEnvelope = maxPosErrorM < 2.5 && maxAttErrorDeg < 10.0;
        std::printf("bounded_envelope_tracking: %s\n", boundedEnvelope ? "PASS" : "FAIL");
        overall = overall && boundedEnvelope;
    }

    // Test 1 / acceptance criterion 3: local prediction responds within
    // 1-2 ticks, checked directly against the local session (not a
    // snapshot) - see sampleScriptedInput()'s docstring for why this only
    // applies to the "step" schedule.
    if (cfg.inputSchedule == "step") {
        // Threshold picked from measured data: baseline (trimmed, level)
        // q is ~0.026 rad/s of residual noise; by 2 ticks after the
        // command, q has grown past 0.08 rad/s - comfortably above 0.02.
        bool immediateResponse =
            haveImmediateResponseSample && (qTwoTicksLater - qAtNoseUp) > 0.02;
        std::printf(
            "immediate_response: %s (q_at_input=%.4f q_2_ticks_later=%.4f)\n",
            immediateResponse ? "PASS" : "FAIL", qAtNoseUp, qTwoTicksLater);
        overall = overall && immediateResponse;
    }

    // Test 3's forced-misprediction path: with reconciliation on, expect
    // at least one correction; the negative control (reconciliation off)
    // is asserted by the caller comparing two separate runs' final error.
    if (cfg.forceDesync) {
        bool reconciliationRan = !cfg.reconciliation || correctionCount > 0;
        std::printf("forced_misprediction_reconciled: %s\n",
                    reconciliationRan ? "PASS" : "FAIL");
        overall = overall && reconciliationRan;
        if (cfg.reconciliation) {
            // After the one forced correction, tracking should have
            // returned to the normal bounded envelope, not stayed at the
            // ~11 m forced offset.
            bool recovered = maxPosErrorM < 15.0;  // the correction itself observes ~11m once
            overall = overall && recovered;
        } else {
            // Negative control: uncorrected, the ~11 m offset must
            // persist and be observed as a real, large tracking error.
            bool staysDesynced = maxPosErrorM > 5.0;
            std::printf("negative_control_stays_desynced: %s (max_pos_error_m=%.4f)\n",
                        staysDesynced ? "PASS" : "FAIL", maxPosErrorM);
            overall = overall && staysDesynced;
        }
    }

    // Test 2: eventual agreement with flight_server's own scripted-mode
    // ground truth (server_pitch_response.csv, produced separately by
    // run_tests.sh using the identical IC+schedule) - compares the last
    // 1 s of both trajectories, matched by tick count (client_seq ==
    // server_tick, both counting ticks since their own schedule's t=0).
    // Only meaningful for the "step" schedule, which mirrors
    // pitch_response.cpp's exact input, and only on a clean (near-zero-
    // latency) link - see checkEventualAgreement's doc comment for why.
    //
    // Compares altitude and attitude, not absolute east/north position:
    // flight_server's aircraft flies continuously from its own process
    // start regardless of when a client connects (unchanged increment 3
    // behaviour), and this client's own trim() takes real wall-clock time
    // before its first command is even sent - so by the time this client
    // is in sync, the server's *absolute* position already reflects
    // however long it flew uncontrolled beforehand, which
    // pitch_response.cpp's standalone run never experiences. Altitude and
    // attitude are unaffected by that bootstrap offset (confirmed
    // empirically: they already agreed closely even when east/north were
    // hundreds of metres apart), so they are what "the same input
    // schedule produces the same response" actually means here.
    if (cfg.inputSchedule == "step" && cfg.checkEventualAgreement) {
        std::vector<PredictedRow> predictedRows;
        std::vector<net::LoggedAircraftRow> groundTruthRows;
        std::string readError;
        bool haveBoth = readPredictedCsv(predictedLogAbs, predictedRows, readError) &&
                        net::readSnapshotCsv(groundTruthLogAbs, groundTruthRows, readError);
        bool eventualAgreement = false;
        int compared = 0, withinTolerance = 0;
        double worstAltErr = 0.0, worstAttErr = 0.0;
        if (haveBoth) {
            std::map<uint32_t, net::AircraftState> truthByTick;
            for (const auto& row : groundTruthRows) truthByTick[row.server_tick] = row.state;
            for (const auto& row : predictedRows) {
                if (row.client_seq < static_cast<uint32_t>(totalTicks - 120)) continue;  // last ~1s only
                auto it = truthByTick.find(row.client_seq);
                if (it == truthByTick.end()) continue;
                double altErr = std::fabs(row.state.pos_local_m[1] - it->second.pos_local_m[1]);
                double attErr = predict::quaternionAngleDeg(
                    row.state.quat[0], row.state.quat[1], row.state.quat[2],
                    row.state.quat[3], it->second.quat[0], it->second.quat[1],
                    it->second.quat[2], it->second.quat[3]);
                ++compared;
                worstAltErr = std::max(worstAltErr, altErr);
                worstAttErr = std::max(worstAttErr, attErr);
                if (altErr < 2.0 && attErr < 2.0) ++withinTolerance;  // loose: "eventually agrees", not a tight gate
            }
            eventualAgreement = compared > 0 && withinTolerance == compared;
        }
        std::printf(
            "eventual_agreement: %s (%s, compared=%d within_tolerance=%d "
            "worst_alt_err_m=%.4f worst_att_err_deg=%.4f)\n",
            eventualAgreement ? "PASS" : "FAIL",
            haveBoth ? "compared" : readError.c_str(), compared, withinTolerance,
            worstAltErr, worstAttErr);
        overall = overall && eventualAgreement;
    }

    return overall ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg = parseArgs(argc, argv);
    int rc;
    if (cfg.mode == "fidelity_input") {
        rc = runScriptedInputFlow(cfg, /*strict=*/true);
    } else if (cfg.mode == "resilience") {
        rc = runScriptedInputFlow(cfg, /*strict=*/false);
    } else if (cfg.mode == "version_reject") {
        rc = runVersionReject(cfg);
    } else if (cfg.mode == "prediction") {
        rc = runPredictionMode(cfg);
    } else {
        std::fprintf(stderr, "error: unknown --mode '%s'\n", cfg.mode.c_str());
        return 2;
    }
    std::printf("%s: %s\n", cfg.mode.c_str(), rc == 0 ? "PASS" : "FAIL");
    return rc;
}
