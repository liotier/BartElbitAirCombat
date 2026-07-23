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

#include "predicted_aircraft.h"

#include "geo/aircraft_orientation.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <enet/enet.h>

#include <chrono>

namespace godot {

namespace {
double nowSeconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

void PredictedAircraft::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_server_host", "host"),
                         &PredictedAircraft::setServerHost);
    ClassDB::bind_method(D_METHOD("get_server_host"),
                         &PredictedAircraft::getServerHost);
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "server_host"),
                 "set_server_host", "get_server_host");

    ClassDB::bind_method(D_METHOD("set_server_port", "port"),
                         &PredictedAircraft::setServerPort);
    ClassDB::bind_method(D_METHOD("get_server_port"),
                         &PredictedAircraft::getServerPort);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "server_port"), "set_server_port",
                 "get_server_port");

    ClassDB::bind_method(D_METHOD("is_connected_to_server"),
                         &PredictedAircraft::isConnectedToServer);

    ClassDB::bind_method(D_METHOD("get_correction_count"),
                         &PredictedAircraft::getCorrectionCount);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "correction_count"), "",
                 "get_correction_count");

    // Increment 5: the only remote-entity method GDScript needs directly -
    // the dynamic RemoteAircraft spawner polls this each tick to know
    // which player_ids currently need a node. hasRemote()/
    // getRemotePosition()/getRemoteOrientation() are called by
    // RemoteAircraft in C++ and are not bound.
    ClassDB::bind_method(D_METHOD("get_active_remote_player_ids"),
                         &PredictedAircraft::getActiveRemotePlayerIds);
}

void PredictedAircraft::_ready() {
    if (!initialize()) return;
    // Same trimmed initial condition as increments 1-3's default scene
    // (5000 ft / 100 kt, level, due north at the origin).
    setInitialCondition(5000.0f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    if (!trim()) return;

    predicted_ = std::make_unique<predict::PredictedSession>(session_);
    // Provisional - refLat_/refLon_ (this client's own trim result) should
    // equal the server's origin by determinism, but is overwritten with
    // the server's own ServerWelcome.origin_lat_deg/lon_deg the moment it
    // arrives (handleEvent below), so rendering and reconciliation always
    // agree with the server on which reference point positions are
    // relative to.
    predicted_->setOrigin(refLat_, refLon_);

    if (enet_initialize() != 0) {
        UtilityFunctions::printerr("PredictedAircraft: enet_initialize failed");
        return;
    }
    enetInitialized_ = true;

    // SERVER_HOST/SERVER_PORT let scripts/start_client.sh point a launched
    // client at a real server without editing the Inspector-set
    // server_host/server_port properties by hand - same pattern as
    // headless_test_driver.gd's TEST_SCENARIO, just read here in C++
    // instead of GDScript since server_host/server_port are already this
    // node's own properties.
    OS* os = OS::get_singleton();
    if (os->has_environment("SERVER_HOST")) {
        serverHost_ = os->get_environment("SERVER_HOST");
    }
    if (os->has_environment("SERVER_PORT")) {
        serverPort_ = os->get_environment("SERVER_PORT").to_int();
    }

    std::string host(serverHost_.utf8().get_data());
    std::string error;
    if (!client_.connect(host, static_cast<uint16_t>(serverPort_), error)) {
        UtilityFunctions::printerr("PredictedAircraft: connect failed: ",
                                   error.c_str());
    }
}

void PredictedAircraft::_physics_process(double delta) {
    if (!predicted_ || !enetInitialized_) return;
    client_.poll(0, [this](const ENetEvent& event) { handleEvent(event); });

    if (client_.isConnected() && !helloSent_) {
        net::ClientHello hello{net::kProtocolVersion};
        client_.send(net::kChannelReliable, net::serializeClientHello(hello),
                     true);
        client_.flush();
        helloSent_ = true;
    }

    if (!welcomed_) return;  // hold prediction until the handshake completes

    // One command per physics tick (120 Hz, matching net::kMaxRedundant-
    // Commands' packet-per-tick assumption - docs/increment-4-
    // specification.md, "Wire protocol changes"). Reuses FlightAircraft's
    // inherited, already-bound per-axis setters/getters: the input script
    // writes elevator_cmd etc. directly (same as flight_input.gd does for
    // plain FlightAircraft), and this reads them back each tick.
    net::ControlCommand cmd;
    cmd.elevator = net::encodeAxis(getElevatorCmd());
    cmd.aileron = net::encodeAxis(getAileronCmd());
    cmd.rudder = net::encodeAxis(getRudderCmd());
    cmd.throttle = net::encodeThrottle(getThrottleCmd());

    net::ControlInput packet = predicted_->tick(cmd);
    client_.send(net::kChannelUnreliable, net::serializeControlInput(packet),
                 false);

    lastSample_ = session_.sample();
    Transform3D target = computeAircraftTransform(
        lastSample_.lat_deg, lastSample_.lon_deg, lastSample_.alt_m,
        lastSample_.roll_deg, lastSample_.pitch_deg, lastSample_.yaw_deg,
        refLat_, refLon_);

    if (blending_) {
        blendElapsed_ += delta;
        double t = blendElapsed_ / kBlendDurationS;
        if (t >= 1.0) {
            blending_ = false;
            set_global_transform(target);
        } else {
            set_global_transform(
                blendFrom_.interpolate_with(target, static_cast<real_t>(t)));
        }
    } else {
        set_global_transform(target);
    }
}

void PredictedAircraft::_exit_tree() {
    if (!enetInitialized_) return;
    if (client_.isConnected()) {
        client_.send(net::kChannelReliable, net::serializeClientBye(), true);
        client_.flush();
        client_.disconnect();
        // Best-effort, matching NetworkClient's own _exit_tree(): give
        // ENet a brief chance to observe the disconnect before the
        // process tears the socket down.
        for (int i = 0; i < 50 && client_.isConnected(); ++i) {
            client_.poll(10, [](const ENetEvent&) {});
        }
    }
    client_.stop();
    enet_deinitialize();
    enetInitialized_ = false;
}

void PredictedAircraft::setServerHost(const String& host) {
    serverHost_ = host;
}
String PredictedAircraft::getServerHost() const { return serverHost_; }
void PredictedAircraft::setServerPort(int port) { serverPort_ = port; }
int PredictedAircraft::getServerPort() const { return serverPort_; }

bool PredictedAircraft::isConnectedToServer() const {
    return client_.isConnected();
}
int PredictedAircraft::getCorrectionCount() const { return correctionCount_; }

void PredictedAircraft::handleEvent(const ENetEvent& event) {
    if (event.type != ENET_EVENT_TYPE_RECEIVE) return;
    net::MessageTag tag;
    if (!net::peekMessageTag(event.packet->data, event.packet->dataLength,
                              tag)) {
        return;
    }
    switch (tag) {
        case net::MessageTag::kServerWelcome: {
            net::ServerWelcome welcome;
            if (net::deserializeServerWelcome(
                    event.packet->data, event.packet->dataLength, welcome)) {
                assignedPlayerId_ = welcome.assigned_player_id;
                refLat_ = welcome.origin_lat_deg;
                refLon_ = welcome.origin_lon_deg;
                predicted_->setOrigin(refLat_, refLon_);
                welcomed_ = true;
                UtilityFunctions::print(
                    "PredictedAircraft: connected, player_id=",
                    welcome.assigned_player_id,
                    " snapshot_hz=", welcome.snapshot_hz);
            }
            break;
        }
        case net::MessageTag::kServerReject: {
            net::ServerReject reject;
            if (net::deserializeServerReject(
                    event.packet->data, event.packet->dataLength, reject)) {
                UtilityFunctions::printerr(
                    "PredictedAircraft: rejected by server, reason=",
                    reject.reason_code);
            }
            break;
        }
        case net::MessageTag::kStateSnapshot: {
            net::StateSnapshot snap;
            if (net::deserializeStateSnapshot(
                    event.packet->data, event.packet->dataLength, snap)) {
                double arrivalTimeS = nowSeconds();
                for (const net::AircraftState& a : snap.aircraft) {
                    if (a.player_id != assignedPlayerId_) {
                        // Increment 5 (docs/increment-5-specification.md,
                        // "Remote-entity interpolation"): every OTHER
                        // aircraft in this chunk feeds the shared tracker.
                        // A client's own record is never split across
                        // chunks, so this loop correctly handles both this
                        // client's entry and any number of others in the
                        // same pass, regardless of which chunk they
                        // arrived in.
                        remoteTracker_.update(a.player_id, a,
                                               snap.server_tick, arrivalTimeS);
                        continue;
                    }
                    // Increment 5: ack_client_seq is now a per-aircraft
                    // field (relocated from StateSnapshot's top level,
                    // docs/increment-5-specification.md "Wire protocol
                    // changes" point 3), read off this client's own entry.
                    // Normative (review finding M1): reconciliation
                    // triggers exactly when the chunk containing this
                    // client's own player_id arrives, never gated on
                    // whether sibling chunks for the same server_tick
                    // have also arrived - which this loop already
                    // satisfies, since it acts the instant the matching
                    // entry is found in whichever chunk carries it.
                    predict::PredictedSession::ReconcileResult result =
                        predicted_->reconcile(a.ack_client_seq, a);
                    if (result.corrected) {
                        // Re-target rather than restart: blendFrom_
                        // becomes wherever the blend (or the unblended
                        // prediction) currently is, not a stale anchor
                        // (review finding m2) - correct whether or not a
                        // blend was already in progress.
                        blendFrom_ = get_global_transform();
                        blendElapsed_ = 0.0;
                        blending_ = true;
                        ++correctionCount_;
                    }
                }
            }
            break;
        }
        case net::MessageTag::kPlayerLeft: {
            net::PlayerLeft left;
            if (net::deserializePlayerLeft(event.packet->data,
                                            event.packet->dataLength, left)) {
                if (left.player_id != assignedPlayerId_) {
                    remoteTracker_.remove(left.player_id);
                }
            }
            break;
        }
        default:
            break;
    }
}

bool PredictedAircraft::hasRemote(int playerId) const {
    interp::InterpolatedState s = remoteTracker_.sample(
        static_cast<uint8_t>(playerId),
        nowSeconds() - interp::RemoteEntityTracker::kInterpolationDelayS);
    return s.valid;
}

Vector3 PredictedAircraft::getRemotePosition(int playerId) const {
    interp::InterpolatedState s = remoteTracker_.sample(
        static_cast<uint8_t>(playerId),
        nowSeconds() - interp::RemoteEntityTracker::kInterpolationDelayS);
    return Vector3(static_cast<real_t>(s.pos_local_m[0]),
                   static_cast<real_t>(s.pos_local_m[1]),
                   static_cast<real_t>(s.pos_local_m[2]));
}

Quaternion PredictedAircraft::getRemoteOrientation(int playerId) const {
    interp::InterpolatedState s = remoteTracker_.sample(
        static_cast<uint8_t>(playerId),
        nowSeconds() - interp::RemoteEntityTracker::kInterpolationDelayS);
    // Same native-quat-to-Godot-convention conversion as increment 4's
    // display path (docs/increment-4-specification.md Appendix A) -
    // interpcore deliberately stays in the wire's native representation
    // throughout (see remote_entity_tracker.h's header comment), so this
    // one-time conversion happens here, at the point closest to
    // rendering, exactly like FlightAircraft's own transform assembly.
    geo::BodyAxes axes = geo::computeBodyAxesFromQuat(s.quat[0], s.quat[1],
                                                        s.quat[2], s.quat[3]);
    Vector3 right(static_cast<real_t>(axes.right.x),
                  static_cast<real_t>(axes.right.y),
                  static_cast<real_t>(axes.right.z));
    Vector3 up(static_cast<real_t>(axes.up.x), static_cast<real_t>(axes.up.y),
               static_cast<real_t>(axes.up.z));
    Vector3 forward(static_cast<real_t>(axes.forward.x),
                     static_cast<real_t>(axes.forward.y),
                     static_cast<real_t>(axes.forward.z));
    return Basis(right, up, -forward).get_rotation_quaternion();
}

PackedInt32Array PredictedAircraft::getActiveRemotePlayerIds() const {
    PackedInt32Array out;
    for (uint8_t id : remoteTracker_.activePlayerIds()) {
        out.push_back(static_cast<int32_t>(id));
    }
    return out;
}

}  // namespace godot
