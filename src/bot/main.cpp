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

// flight_bot: a genuinely headless client (docs/increment-7-
// specification.md, "flight_bot: a headless client with local physics").
// Runs the *same* local FlightSession + PredictedSession a human's Godot
// client runs - just without Godot, rendering, or input - flying the
// bot::ManeuverController "intelligence" behind the core/intelligence
// seam. Godot-free; links flightcore, predictcore, netcore, interpcore.
//
// Always spawned by flight_server's own fork()/exec() (--host/--port/
// --aircraft passed via argv, since the server already knows its own
// --aircraft and where it's listening - spec, "flight_bot: a headless
// client with local physics"); relies on inheriting flight_server's own
// already-`results/`-chdir'd cwd for JSBSim's stray-output-file
// containment (increment 1 spec review finding F7) rather than
// redundantly chdir'ing again itself, which would nest a nonexistent
// second results/ directory - a standalone invocation for debugging may
// leave a stray file in whatever directory it was run from, an accepted
// rough edge for a non-primary use case.
//
// Exit codes: 0 clean shutdown (SIGTERM, handled as a clean disconnect -
// spec, "Spawn/despawn mechanism"), 1 lost its server connection
// unexpectedly (crash safety, finding m1) or a genuine aircraft-type
// mismatch (increment 6's default: warn and exit), 2 an init/connect/
// handshake execution error.
#include "aircraft_catalog.h"
#include "bot/maneuver_controller.h"
#include "interpcore/remote_entity_tracker.h"
#include "netcore/net_client.h"
#include "netcore/protocol.h"
#include "predictcore/predicted_session.h"
#include "test_runner.h"

#include <enet/enet.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

struct Config {
    std::string host = "127.0.0.1";
    uint16_t port = 45300;
    std::string aircraft = "c172x";
};

Config parseArgs(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto nextVal = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
        };
        if (arg == "--host") {
            cfg.host = nextVal();
        } else if (arg == "--port") {
            cfg.port = static_cast<uint16_t>(std::atoi(nextVal().c_str()));
        } else if (arg == "--aircraft") {
            cfg.aircraft = nextVal();
        }
    }
    return cfg;
}

bool waitForConnect(net::NetClient& client, int timeoutMs) {
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        client.poll(10, [](const ENetEvent&) {});
        if (client.isConnected()) return true;
    }
    return client.isConnected();
}

struct HandshakeOutcome {
    bool welcomed = false;
    bool rejected = false;
    net::ServerWelcome welcome;
    net::ServerReject reject;
};

// Same shape as flight_test_client's own doHandshake() - deliberately not
// shared (flight_bot links no code specific to flight_test_client, and
// the two evolve independently per this project's established practice
// for client-side orchestration logic, distinct from the shared
// flightcore/netcore/predictcore/interpcore libraries).
HandshakeOutcome doHandshake(net::NetClient& client, int timeoutMs) {
    net::ClientHello hello{net::kProtocolVersion, net::kClientFlagIsBot};
    client.send(net::kChannelReliable, net::serializeClientHello(hello), true);
    client.flush();
    HandshakeOutcome outcome;
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline && !outcome.welcomed &&
           !outcome.rejected) {
        client.poll(10, [&](const ENetEvent& event) {
            if (event.type != ENET_EVENT_TYPE_RECEIVE) return;
            net::MessageTag tag;
            if (!net::peekMessageTag(event.packet->data,
                                      event.packet->dataLength, tag)) {
                return;
            }
            if (tag == net::MessageTag::kServerWelcome &&
                net::deserializeServerWelcome(event.packet->data,
                                               event.packet->dataLength,
                                               outcome.welcome)) {
                outcome.welcomed = true;
            } else if (tag == net::MessageTag::kServerReject &&
                       net::deserializeServerReject(event.packet->data,
                                                     event.packet->dataLength,
                                                     outcome.reject)) {
                outcome.rejected = true;
            }
        });
    }
    return outcome;
}

double nowSeconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

int main(int argc, char** argv) {
    Config config = parseArgs(argc, argv);

    const aircraft::CatalogEntry* entry = aircraft::findByToken(config.aircraft);
    if (!entry) {
        std::fprintf(stderr, "flight_bot: error: unknown --aircraft '%s'\n",
                     config.aircraft.c_str());
        return 2;
    }

    // "On startup it loads and trims its aircraft type before connecting"
    // (spec, "flight_bot: a headless client with local physics") - the
    // same trim-before-connect sequence increment 6 specifies for
    // PredictedAircraft.
    inc1::FlightSession session;
    std::string error;
    if (!session.initialize(error, entry->load_model)) {
        std::fprintf(stderr, "flight_bot: error: initialize failed: %s\n",
                     error.c_str());
        return 2;
    }
    session.setInitialCondition(entry->canonical_alt_ft,
                                 entry->canonical_vc_kts, 0.0, 0.0, 0.0, 0.0);
    if (!session.trim(error)) {
        std::fprintf(stderr, "flight_bot: error: trim failed: %s\n",
                     error.c_str());
        return 2;
    }

    bot::TrimBaseline trim;
    trim.aileron = session.property("fcs/aileron-cmd-norm");
    trim.rudder = session.property("fcs/rudder-cmd-norm");
    trim.throttle = session.property("fcs/throttle-cmd-norm");
    bot::ManeuverController controller(trim, bot::profileForToken(entry->token));

    predict::PredictedSession predicted(session);
    interp::RemoteEntityTracker remoteTracker;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (enet_initialize() != 0) {
        std::fprintf(stderr, "flight_bot: error: enet_initialize failed\n");
        return 2;
    }
    net::NetClient client;
    if (!client.connect(config.host, config.port, error)) {
        std::fprintf(stderr, "flight_bot: error: %s\n", error.c_str());
        enet_deinitialize();
        return 2;
    }
    if (!waitForConnect(client, 5000)) {
        std::fprintf(stderr, "flight_bot: error: connect timed out\n");
        client.stop();
        enet_deinitialize();
        return 2;
    }

    HandshakeOutcome hs = doHandshake(client, 5000);
    if (!hs.welcomed) {
        std::fprintf(stderr,
                     "flight_bot: error: handshake failed (rejected=%d "
                     "reason=%u)\n",
                     hs.rejected, hs.reject.reason_code);
        client.stop();
        enet_deinitialize();
        return 2;
    }
    double originLat = hs.welcome.origin_lat_deg;
    double originLon = hs.welcome.origin_lon_deg;
    uint8_t myPlayerId = hs.welcome.assigned_player_id;

    // Increment 6's standard mismatch check (docs/increment-7-
    // specification.md, "flight_bot: a headless client with local
    // physics": "for a server-spawned bot can only fire on a genuine bug
    // - in which case the bot warns and exits ... rather than flying
    // mispredicted"). No override flag: unlike a human tester, a bot has
    // no one to decide to fly mismatched on purpose.
    const aircraft::CatalogEntry* serverEntry =
        aircraft::findById(hs.welcome.aircraft_id);
    std::string serverToken =
        serverEntry ? serverEntry->token
                    : ("unknown(" + std::to_string(hs.welcome.aircraft_id) + ")");
    if (serverToken != entry->token) {
        std::fprintf(stderr,
                     "flight_bot: error: aircraft-type MISMATCH - this bot is "
                     "configured for '%s', server is running '%s' - exiting\n",
                     entry->token.c_str(), serverToken.c_str());
        client.stop();
        enet_deinitialize();
        return 1;
    }
    predicted.setOrigin(originLat, originLon);
    std::printf("flight_bot: connected, player_id=%u aircraft=%s\n",
                myPlayerId, entry->token.c_str());

    auto onEvent = [&](const ENetEvent& event) {
        if (event.type != ENET_EVENT_TYPE_RECEIVE) return;
        net::MessageTag tag;
        if (!net::peekMessageTag(event.packet->data, event.packet->dataLength,
                                  tag)) {
            return;
        }
        if (tag == net::MessageTag::kPlayerLeft) {
            net::PlayerLeft left;
            if (net::deserializePlayerLeft(event.packet->data,
                                            event.packet->dataLength, left)) {
                remoteTracker.remove(left.player_id);
            }
            return;
        }
        if (tag != net::MessageTag::kStateSnapshot) return;
        net::StateSnapshot snap;
        if (!net::deserializeStateSnapshot(event.packet->data,
                                            event.packet->dataLength, snap)) {
            return;
        }
        double arrivalTimeS = nowSeconds();
        for (const net::AircraftState& a : snap.aircraft) {
            if (a.player_id == myPlayerId) {
                predicted.reconcile(a.ack_client_seq, a);
                continue;
            }
            // "Every other aircraft's snapshot entry feeds an
            // interp::RemoteEntityTracker (the 'full airspace picture',
            // unused by decision logic today, ready for future combat
            // AI)" (spec, "flight_bot: a headless client with local
            // physics" / test-plan item 7).
            remoteTracker.update(a.player_id, a, snap.server_tick, arrivalTimeS);
        }
    };

    auto nextTick = std::chrono::steady_clock::now();
    constexpr auto kTickPeriod = std::chrono::duration<double>(1.0 / 120.0);
    long tick = 0;
    long nextStatusTick = 120;  // one status line per sim-second

    while (!g_stop) {
        client.poll(0, onEvent);
        if (!client.isConnected()) {
            // Crash safety (finding m1): a server crash never runs
            // flight_server's own reap path, and POSIX does not kill
            // children with their parent - so this bot exits itself the
            // moment it notices the connection is gone (ENet's own
            // disconnect/timeout), rather than flying on, orphaned,
            // against nothing.
            std::fprintf(stderr, "flight_bot: lost connection, exiting\n");
            client.stop();
            enet_deinitialize();
            return 1;
        }

        auto now = std::chrono::steady_clock::now();
        if (now < nextTick) {
            auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              nextTick - now)
                              .count();
            client.poll(static_cast<uint32_t>(std::max<int64_t>(waitMs, 0)),
                        onEvent);
            continue;
        }

        bot::OwnState state;
        state.bankDeg = session.property("attitude/phi-deg");
        state.rollRateRadS = session.property("velocities/p-rad_sec");
        bot::ControlIntent intent =
            controller.update(state, session.property("simulation/sim-time-sec"));

        net::ControlCommand cmd;
        cmd.elevator = net::encodeAxis(intent.elevator);
        cmd.aileron = net::encodeAxis(intent.aileron);
        cmd.rudder = net::encodeAxis(intent.rudder);
        cmd.throttle = net::encodeThrottle(intent.throttle);
        net::ControlInput packet = predicted.tick(cmd);
        client.send(net::kChannelUnreliable, net::serializeControlInput(packet),
                    false);

        ++tick;
        if (tick >= nextStatusTick) {
            nextStatusTick += 120;
            std::printf("flight_bot: t=%.0fs alt_ft=%.0f tracked_others=%zu\n",
                        session.property("simulation/sim-time-sec"),
                        session.property("position/h-sl-ft"),
                        remoteTracker.activePlayerIds().size());
        }
        nextTick +=
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                kTickPeriod);
    }

    // SIGTERM: flight_server's own despawn mechanism (spec, "Spawn/
    // despawn mechanism") - handled as a clean disconnect.
    client.send(net::kChannelReliable, net::serializeClientBye(), true);
    client.flush();
    client.disconnect();
    for (int i = 0; i < 50 && client.isConnected(); ++i) {
        client.poll(10, [](const ENetEvent&) {});
    }
    client.stop();
    enet_deinitialize();
    return 0;
}
