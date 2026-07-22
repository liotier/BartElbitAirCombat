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

#include "network_client.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <enet/enet.h>

#include "geo/aircraft_orientation.h"

namespace godot {

void NetworkClient::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_server_host", "host"),
                         &NetworkClient::setServerHost);
    ClassDB::bind_method(D_METHOD("get_server_host"),
                         &NetworkClient::getServerHost);
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "server_host"),
                 "set_server_host", "get_server_host");

    ClassDB::bind_method(D_METHOD("set_server_port", "port"),
                         &NetworkClient::setServerPort);
    ClassDB::bind_method(D_METHOD("get_server_port"),
                         &NetworkClient::getServerPort);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "server_port"), "set_server_port",
                 "get_server_port");

    ClassDB::bind_method(
        D_METHOD("set_input", "elevator", "aileron", "rudder", "throttle"),
        &NetworkClient::setInput);

    // Not named is_connected(): Object already binds that name for signal
    // connections, and shadowing it would be confusing from GDScript.
    ClassDB::bind_method(D_METHOD("is_connected_to_server"),
                         &NetworkClient::isConnected);
    ClassDB::bind_method(D_METHOD("has_snapshot"), &NetworkClient::hasSnapshot);
    ClassDB::bind_method(D_METHOD("get_remote_position"),
                         &NetworkClient::getRemotePosition);
    ClassDB::bind_method(D_METHOD("get_remote_orientation"),
                         &NetworkClient::getRemoteOrientation);
    ClassDB::bind_method(D_METHOD("get_remote_velocity"),
                         &NetworkClient::getRemoteVelocity);
}

void NetworkClient::_ready() {
    if (enet_initialize() != 0) {
        UtilityFunctions::printerr("NetworkClient: enet_initialize failed");
        return;
    }
    enetInitialized_ = true;
    std::string host(serverHost_.utf8().get_data());
    std::string error;
    if (!client_.connect(host, static_cast<uint16_t>(serverPort_), error)) {
        UtilityFunctions::printerr("NetworkClient: connect failed: ",
                                   error.c_str());
    }
}

void NetworkClient::_physics_process(double delta) {
    if (!enetInitialized_) return;
    client_.poll(0, [this](const ENetEvent& event) { handleEvent(event); });

    if (client_.isConnected() && !helloSent_) {
        net::ClientHello hello{net::kProtocolVersion};
        client_.send(net::kChannelReliable, net::serializeClientHello(hello),
                     true);
        client_.flush();
        helloSent_ = true;
    }

    if (!welcomed_) return;  // hold input until the handshake completes

    sinceLastInputSend_ += delta;
    constexpr double kInputSendPeriod = 1.0 / 60.0;  // spec, "Rates"
    if (sinceLastInputSend_ >= kInputSendPeriod) {
        sinceLastInputSend_ = 0.0;
        // No redundancy (count=1): this node does not predict, so unlike
        // PredictedAircraft it has no buffered history of past commands to
        // resend, and is not this increment's tested path (docs/
        // increment-4-specification.md - PredictedAircraft replaces this
        // node's role in networked.tscn). A single fresh command per
        // packet is still wire-valid and behaves as increment 3's did.
        net::ControlInput input;
        input.newest_client_seq = ++clientSeq_;
        net::ControlCommand cmd;
        cmd.elevator = net::encodeAxis(inputElevator_);
        cmd.aileron = net::encodeAxis(inputAileron_);
        cmd.rudder = net::encodeAxis(inputRudder_);
        cmd.throttle = net::encodeThrottle(inputThrottle_);
        input.commands.push_back(cmd);
        client_.send(net::kChannelUnreliable, net::serializeControlInput(input),
                     false);
    }
}

void NetworkClient::_exit_tree() {
    if (!enetInitialized_) return;
    if (client_.isConnected()) {
        client_.send(net::kChannelReliable, net::serializeClientBye(), true);
        client_.flush();
        client_.disconnect();
        // Best-effort: give ENet a brief chance to observe the disconnect
        // before the process tears the socket down. A human closing the
        // window abruptly is not a scenario this needs to handle
        // perfectly.
        for (int i = 0; i < 50 && client_.isConnected(); ++i) {
            client_.poll(10, [](const ENetEvent&) {});
        }
    }
    client_.stop();
    enet_deinitialize();
    enetInitialized_ = false;
}

void NetworkClient::setServerHost(const String& host) { serverHost_ = host; }
String NetworkClient::getServerHost() const { return serverHost_; }
void NetworkClient::setServerPort(int port) { serverPort_ = port; }
int NetworkClient::getServerPort() const { return serverPort_; }

void NetworkClient::setInput(float elevator, float aileron, float rudder,
                              float throttle) {
    inputElevator_ = elevator;
    inputAileron_ = aileron;
    inputRudder_ = rudder;
    inputThrottle_ = throttle;
}

bool NetworkClient::isConnected() const { return client_.isConnected(); }
bool NetworkClient::hasSnapshot() const { return haveSnapshot_; }

Vector3 NetworkClient::getRemotePosition() const {
    return Vector3(latestAircraft_.pos_local_m[0],
                   latestAircraft_.pos_local_m[1],
                   latestAircraft_.pos_local_m[2]);
}

Quaternion NetworkClient::getRemoteOrientation() const {
    // Increment 4: the wire quat is JSBSim's native qAttitudeLocal
    // (q(1..4) = w,x,y,z), not Godot's (x,y,z,w) - reinterpreting it
    // directly (increment 3's approach) would silently misinterpret the
    // rotation. geo::computeBodyAxesFromQuat() converts without ever
    // decomposing to Euler angles (gimbal-safe), verified against the
    // shipped Euler-angle path (docs/increment-4-specification.md
    // Appendix B).
    geo::BodyAxes axes = geo::computeBodyAxesFromQuat(
        latestAircraft_.quat[0], latestAircraft_.quat[1],
        latestAircraft_.quat[2], latestAircraft_.quat[3]);
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

Vector3 NetworkClient::getRemoteVelocity() const {
    return Vector3(latestAircraft_.vel_local_mps[0],
                   latestAircraft_.vel_local_mps[1],
                   latestAircraft_.vel_local_mps[2]);
}

void NetworkClient::handleEvent(const ENetEvent& event) {
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
                welcomed_ = true;
                UtilityFunctions::print(
                    "NetworkClient: connected, player_id=",
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
                    "NetworkClient: rejected by server, reason=",
                    reject.reason_code);
            }
            break;
        }
        case net::MessageTag::kStateSnapshot: {
            net::StateSnapshot snap;
            if (net::deserializeStateSnapshot(
                    event.packet->data, event.packet->dataLength, snap)) {
                for (const net::AircraftState& a : snap.aircraft) {
                    if (a.player_id == assignedPlayerId_) {
                        latestAircraft_ = a;
                        haveSnapshot_ = true;
                        break;
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

}  // namespace godot
