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

// The standalone authoritative server (docs/increment-3-specification.md,
// "Server (flight_server)"). No Godot dependency: links flightcore (the
// FlightSession JSBSim wrapper, unchanged from increments 1-2) and
// netcore (the wire protocol). Runs a wall-clock-paced 120 Hz loop,
// applying the connected client's most recent ControlInput (networked
// mode) or a built-in scripted schedule (scripted-scenario mode,
// --scenario), and broadcasts StateSnapshots at a configurable rate.
//
// Exit codes match increments 1-2's convention: 0 clean, 1 a criterion
// failed (scripted mode only), 2 an init/bind/trim execution error.
#include "geo/aircraft_orientation.h"
#include "netcore/net_server.h"
#include "netcore/protocol.h"
#include "netcore/snapshot_log.h"
#include "test_runner.h"

#include "math/FGQuaternion.h"

#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

struct Config {
    uint16_t port = 45300;
    int snapshotHz = 30;
    std::string scenario;  // empty => networked mode
    std::string logName;   // empty => derive from mode/scenario
};

Config parseArgs(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto nextVal = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
        };
        if (arg == "--port") {
            cfg.port = static_cast<uint16_t>(std::atoi(nextVal().c_str()));
        } else if (arg == "--snapshot-hz") {
            cfg.snapshotHz = std::atoi(nextVal().c_str());
        } else if (arg == "--scenario") {
            cfg.scenario = nextVal();
        } else if (arg == "--log-name") {
            cfg.logName = nextVal();
        }
    }
    return cfg;
}

}  // namespace

int main(int argc, char** argv) {
    Config config = parseArgs(argc, argv);
    bool scripted = !config.scenario.empty();

    // Increment 1's stray-output-file issue (c172x's own <output> block
    // opens a CSV during LoadModel(), before DisableOutput() runs) applies
    // here identically, since FlightSession::initialize() is unchanged;
    // contained the same way (src/main.cpp): run from within results/.
    std::error_code ec;
    std::filesystem::create_directories("results", ec);
    std::filesystem::current_path("results", ec);
    if (ec) {
        std::fprintf(stderr, "error: could not enter results/ directory: %s\n",
                     ec.message().c_str());
        return 2;
    }

    if (scripted && config.scenario != "pitch_response") {
        // Increment 3 spec review M3: full-flight scenario reuse mostly
        // re-proves already-proven physics; one server-side scenario is
        // enough to validate the new wall-clock loop, so only the one
        // the spec itself suggests is wired up here.
        std::fprintf(stderr,
                      "error: unsupported --scenario '%s' (only "
                      "pitch_response is wired up in increment 3)\n",
                      config.scenario.c_str());
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    inc1::FlightSession session;
    std::string error;
    if (!session.initialize(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 2;
    }
    // Same initial condition as increments 1-2's Test 1 / default scene,
    // for both modes - scripted mode additionally reuses pitch_response's
    // exact control-input schedule and duration below.
    session.setInitialCondition(5000.0, 100.0, 0.0, 0.0, 0.0, 0.0);
    if (!session.trim(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 2;
    }

    // The trimmed starting position becomes the session origin announced
    // in ServerWelcome, matching FlightAircraft's reference-point
    // convention (increment 2).
    inc1::FlightSample originSample = session.sample();
    double originLat = originSample.lat_deg;
    double originLon = originSample.lon_deg;

    std::string logName =
        !config.logName.empty() ? config.logName
        : scripted              ? config.scenario
                                 : std::string("networked");
    std::ofstream snapLog("server_" + logName + ".csv");
    if (!snapLog) {
        std::fprintf(stderr, "error: could not open server_%s.csv\n",
                      logName.c_str());
        return 2;
    }
    net::writeSnapshotCsvHeader(snapLog);

    if (enet_initialize() != 0) {
        std::fprintf(stderr, "error: enet_initialize failed\n");
        return 2;
    }
    net::NetServer server;
    // maxClients > 1 so a second connection attempt reaches our own
    // ServerReject(server_full) logic below instead of being silently
    // refused by ENet itself before we ever see it.
    if (!server.start(config.port, /*maxClients=*/4, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        enet_deinitialize();
        return 2;
    }

    ENetPeer* clientPeer = nullptr;
    uint8_t clientPlayerId = 0;
    // Ordered command buffer fed by redundant ControlInput packets (spec,
    // "Wire protocol changes" / review finding B2): pendingCommands holds
    // received-but-not-yet-applied commands, keyed by client_seq;
    // nextExpectedSeq is the next one the server needs; highestSeqSeen is
    // the highest newest_client_seq any packet has ever reported, used to
    // detect a seq that can never arrive anymore (the client's own
    // redundancy window has moved past it in every packet that could
    // still carry it).
    std::map<uint32_t, net::ControlCommand> pendingCommands;
    uint32_t nextExpectedSeq = 1;
    uint32_t highestSeqSeen = 0;
    net::ControlCommand lastAppliedCommand{};
    bool hasInput = false;

    auto onEvent = [&](const ENetEvent& event) {
        if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            if (event.peer == clientPeer) {
                clientPeer = nullptr;
                clientPlayerId = 0;
                hasInput = false;
                pendingCommands.clear();
                nextExpectedSeq = 1;
                highestSeqSeen = 0;
                lastAppliedCommand = net::ControlCommand{};
            }
            return;
        }
        if (event.type != ENET_EVENT_TYPE_RECEIVE) return;

        net::MessageTag tag;
        if (!net::peekMessageTag(event.packet->data, event.packet->dataLength,
                                  tag)) {
            return;
        }
        switch (tag) {
            case net::MessageTag::kClientHello: {
                net::ClientHello hello;
                if (!net::deserializeClientHello(
                        event.packet->data, event.packet->dataLength, hello)) {
                    return;
                }
                if (hello.protocol_version != net::kProtocolVersion) {
                    net::ServerReject reject{
                        static_cast<uint8_t>(net::RejectReason::kVersionMismatch)};
                    server.send(event.peer, net::kChannelReliable,
                                net::serializeServerReject(reject), true);
                    server.flush();
                    // Deliberately does not disconnect the peer here: an
                    // immediate enet_peer_disconnect() right after
                    // queuing a reliable send races that packet's actual
                    // delivery (found by testing - about 40% of runs
                    // never received the reject at all). The rejected
                    // client, once it has the message, disconnects
                    // itself; ENet's own idle timeout reclaims the peer
                    // if it doesn't.
                    return;
                }
                if (clientPeer != nullptr) {
                    net::ServerReject reject{
                        static_cast<uint8_t>(net::RejectReason::kServerFull)};
                    server.send(event.peer, net::kChannelReliable,
                                net::serializeServerReject(reject), true);
                    server.flush();
                    return;
                }
                clientPeer = event.peer;
                clientPlayerId = 1;
                hasInput = false;
                pendingCommands.clear();
                nextExpectedSeq = 1;
                highestSeqSeen = 0;
                lastAppliedCommand = net::ControlCommand{};
                net::ServerWelcome welcome{
                    net::kProtocolVersion, clientPlayerId,
                    static_cast<float>(originLat),
                    static_cast<float>(originLon),
                    static_cast<uint16_t>(config.snapshotHz)};
                server.send(event.peer, net::kChannelReliable,
                            net::serializeServerWelcome(welcome), true);
                break;
            }
            case net::MessageTag::kControlInput: {
                if (event.peer != clientPeer) return;
                net::ControlInput input;
                if (!net::deserializeControlInput(event.packet->data,
                                                   event.packet->dataLength,
                                                   input)) {
                    return;
                }
                // Redundant multi-command packet (spec, "Wire protocol
                // changes" / review finding B2): merge every command not
                // already decided into the ordered buffer. Commands are
                // seqs newest_client_seq, newest_client_seq-1, ...
                // descending; the `i <= newest_client_seq` bound avoids
                // underflowing seq for a short first packet.
                for (size_t i = 0;
                     i < input.commands.size() && i <= input.newest_client_seq;
                     ++i) {
                    uint32_t seq =
                        input.newest_client_seq - static_cast<uint32_t>(i);
                    if (seq >= nextExpectedSeq) {
                        pendingCommands[seq] = input.commands[i];
                    }
                }
                highestSeqSeen = std::max(highestSeqSeen, input.newest_client_seq);
                hasInput = true;
                break;
            }
            case net::MessageTag::kClientBye:
                if (event.peer == clientPeer) server.disconnect(event.peer);
                break;
            default:
                break;
        }
    };

    std::vector<inc1::FlightSample> samples;
    const long scriptedTicks =
        scripted ? std::lround(30.0 / inc1::kDt) : -1;  // pitch_response duration
    if (scripted) {
        samples.reserve(static_cast<size_t>(scriptedTicks) + 1);
        samples.push_back(session.sample());
    }

    const int snapshotInterval =
        std::max(1, static_cast<int>(std::lround(120.0 / config.snapshotHz)));
    uint32_t tick = 0;

    auto stepOneTick = [&]() {
        double t = session.property("simulation/sim-time-sec");
        if (scripted) {
            // Reproduces src/scenarios/pitch_response.cpp's exact
            // schedule so the server-side trajectory matches increment
            // 1's validated one.
            if (t >= 5.0) session.setProperty("fcs/elevator-cmd-norm", -1.0);
        } else if (hasInput) {
            // Apply exactly one command per tick, in ascending client_seq
            // order, holding the last applied command across a gap (spec,
            // "Wire protocol changes"). A seq becomes provably
            // unrecoverable once the client has moved
            // kMaxRedundantCommands past it in every packet that could
            // still have carried it - skip forward past it (still holding
            // the last command for its tick) rather than stalling.
            auto pendingIt = pendingCommands.find(nextExpectedSeq);
            if (pendingIt != pendingCommands.end()) {
                lastAppliedCommand = pendingIt->second;
                ++nextExpectedSeq;
            } else if (highestSeqSeen >=
                       nextExpectedSeq + net::kMaxRedundantCommands) {
                ++nextExpectedSeq;
            }
            pendingCommands.erase(pendingCommands.begin(),
                                   pendingCommands.lower_bound(nextExpectedSeq));

            session.setProperty("fcs/elevator-cmd-norm",
                                 net::decodeAxis(lastAppliedCommand.elevator));
            session.setProperty("fcs/aileron-cmd-norm",
                                 net::decodeAxis(lastAppliedCommand.aileron));
            session.setProperty("fcs/rudder-cmd-norm",
                                 net::decodeAxis(lastAppliedCommand.rudder));
            session.setProperty(
                "fcs/throttle-cmd-norm",
                net::decodeThrottle(lastAppliedCommand.throttle));
        }
        session.step();
        ++tick;
        inc1::FlightSample sample = session.sample();
        if (scripted) samples.push_back(sample);

        if (tick % static_cast<uint32_t>(snapshotInterval) == 0) {
            net::StateSnapshot snap;
            snap.server_tick = tick;
            // nextExpectedSeq starts at 1 whether or not any input has
            // arrived yet, so this is 0 (meaning "nothing applied yet")
            // in both scripted mode and before a networked client's first
            // packet, with no special-casing needed.
            snap.ack_client_seq = nextExpectedSeq - 1;
            net::AircraftState a;
            a.player_id = 1;
            geo::LocalOffset off = geo::computeLocalOffset(
                sample.lat_deg, sample.lon_deg, originLat, originLon);
            a.pos_local_m[0] = static_cast<float>(off.east_m);
            a.pos_local_m[1] = static_cast<float>(sample.alt_m);
            a.pos_local_m[2] = static_cast<float>(-off.north_m);
            // Increment 4 (docs/increment-4-specification.md Appendix A):
            // the wire quat is JSBSim's own native qAttitudeLocal
            // (body->NED), read directly - not re-derived from Euler
            // angles via geo::computeOrientationQuat() - so reconciliation
            // can reconstruct a VehicleState without a lossy Euler
            // round-trip.
            JSBSim::FGQuaternion qLocal = session.getVState().qAttitudeLocal;
            a.quat[0] = static_cast<float>(qLocal(1));
            a.quat[1] = static_cast<float>(qLocal(2));
            a.quat[2] = static_cast<float>(qLocal(3));
            a.quat[3] = static_cast<float>(qLocal(4));
            // Local frame is East/Up/-North, matching position (Up = -Down).
            a.vel_local_mps[0] = static_cast<float>(sample.vel_east_mps);
            a.vel_local_mps[1] = static_cast<float>(-sample.vel_down_mps);
            a.vel_local_mps[2] = static_cast<float>(-sample.vel_north_mps);
            a.ang_vel_body_rps[0] =
                static_cast<float>(session.property("velocities/p-rad_sec"));
            a.ang_vel_body_rps[1] =
                static_cast<float>(session.property("velocities/q-rad_sec"));
            a.ang_vel_body_rps[2] =
                static_cast<float>(session.property("velocities/r-rad_sec"));
            a.status_flags = 0;
            snap.aircraft.push_back(a);

            net::writeSnapshotCsvRow(snapLog, snap);
            snapLog.flush();  // a concurrently-running test client reads this file live
            server.broadcast(net::kChannelUnreliable,
                              net::serializeStateSnapshot(snap), false);
            server.flush();
        }
    };

    // Wall-clock-paced fixed-timestep loop (spec, "Server (flight_server)"
    // item 3): enet_host_service's own timeout doubles as the sleep, so
    // waiting for the next tick and servicing the network are one call.
    auto nextTick = std::chrono::steady_clock::now();
    // Matches increment 2's own measured Godot catch-up behaviour (8
    // ticks after a 100 ms slow frame, increment-2 spec Appendix B) -
    // reusing that empirically-grounded cap rather than an arbitrary one.
    constexpr int kMaxCatchUpTicks = 8;

    bool finished = false;
    while (!g_stop && !finished) {
        auto now = std::chrono::steady_clock::now();
        if (now < nextTick) {
            auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              nextTick - now)
                              .count();
            server.poll(static_cast<uint32_t>(std::max<int64_t>(waitMs, 0)),
                        onEvent);
            continue;
        }
        int caughtUp = 0;
        while (std::chrono::steady_clock::now() >= nextTick &&
               caughtUp < kMaxCatchUpTicks) {
            stepOneTick();
            nextTick += std::chrono::duration_cast<
                std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(inc1::kDt));
            ++caughtUp;
            if (scripted && static_cast<long>(tick) >= scriptedTicks) {
                finished = true;
                break;
            }
        }
        if (caughtUp == kMaxCatchUpTicks) {
            nextTick = std::chrono::steady_clock::now();
        }
        server.poll(0, onEvent);
    }

    int exitCode = 0;
    if (scripted) {
        inc1::TestResult result;
        result.name = "server_" + config.scenario;
        double maxPitch5to10 = -std::numeric_limits<double>::infinity();
        double maxAlpha = -std::numeric_limits<double>::infinity();
        double minIas = std::numeric_limits<double>::infinity();
        for (const inc1::FlightSample& s : samples) {
            if (s.time_s >= 5.0 && s.time_s <= 10.0) {
                maxPitch5to10 = std::max(maxPitch5to10, s.pitch_deg);
            }
            maxAlpha = std::max(maxAlpha, s.alpha_deg);
            minIas = std::min(minIas, s.ias_mps);
        }
        result.criteria.push_back(
            inc1::makeCriterion("max_pitch_5_10", maxPitch5to10, 30.0, ">=", "deg"));
        result.criteria.push_back(
            inc1::makeCriterion("max_alpha", maxAlpha, 12.0, ">=", "deg"));
        result.criteria.push_back(inc1::makeCriterion(
            "min_ias", minIas, 60.0 * inc1::kKt2Mps, "<=", "m/s"));
        result.criteria.push_back(inc1::checkFinite(samples));
        result.status =
            std::all_of(result.criteria.begin(), result.criteria.end(),
                        [](const inc1::Criterion& c) { return c.passed; })
                ? inc1::TestStatus::kPassed
                : inc1::TestStatus::kFailed;

        std::vector<inc1::TestResult> results{result};
        inc1::printSummaryTable(results);
        inc1::writeSummaryJson(results, "server_summary.json");
        exitCode = (result.status == inc1::TestStatus::kPassed) ? 0 : 1;
    }

    server.stop();
    enet_deinitialize();
    return exitCode;
}
