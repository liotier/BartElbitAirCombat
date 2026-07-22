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

#include <enet/enet.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

struct Config {
    std::string mode = "fidelity_input";
    std::string host = "127.0.0.1";
    uint16_t port = 45300;
    std::string serverLog = "server_networked.csv";
    std::string receivedLog = "client_received.csv";
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
            net::ControlInput input;
            input.client_seq = ++clientSeq;
            input.elevator = noseUpPhase ? net::encodeAxis(-1.0) : net::encodeAxis(0.0);
            input.aileron = 0;
            input.rudder = 0;
            // Full throttle throughout (including baseline) so a single
            // ControlInput schedule never yanks throttle down from
            // whatever trim set it to - see docs/increment-3-
            // specification.md's implementation notes for why this test
            // does not attempt to hold the exact trimmed throttle.
            input.throttle = net::encodeThrottle(1.0);
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
    } else {
        std::fprintf(stderr, "error: unknown --mode '%s'\n", cfg.mode.c_str());
        return 2;
    }
    std::printf("%s: %s\n", cfg.mode.c_str(), rc == 0 ? "PASS" : "FAIL");
    return rc;
}
