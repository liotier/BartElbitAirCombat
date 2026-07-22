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

// GDExtension Node owning the ENet client host for the human-facing
// Godot client (docs/increment-3-specification.md, "Godot client").
// Connects on _ready(), sends the local control input at the spec's 60
// Hz default, and receives StateSnapshots. Links netcore - the same
// wire-format code flight_test_client compiles - so this node's
// transport path is exactly what that binary's automated tests already
// exercise (spec, "Architecture").
#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "netcore/net_client.h"
#include "netcore/protocol.h"

namespace godot {

class NetworkClient : public Node {
    GDCLASS(NetworkClient, Node)

protected:
    static void _bind_methods();

public:
    void _ready() override;
    void _physics_process(double delta) override;
    void _exit_tree() override;

    void setServerHost(const String& host);
    String getServerHost() const;
    void setServerPort(int port);
    int getServerPort() const;

    // elevator/aileron/rudder in [-1,1], throttle in [0,1] - same
    // convention as FlightAircraft's per-axis setters (increment 2).
    void setInput(float elevator, float aileron, float rudder, float throttle);

    bool isConnected() const;
    bool hasSnapshot() const;
    Vector3 getRemotePosition() const;
    Quaternion getRemoteOrientation() const;
    Vector3 getRemoteVelocity() const;

private:
    net::NetClient client_;
    bool enetInitialized_ = false;
    bool helloSent_ = false;
    bool welcomed_ = false;
    uint8_t assignedPlayerId_ = 0;

    String serverHost_ = "127.0.0.1";
    int serverPort_ = 45300;

    float inputElevator_ = 0.0f;
    float inputAileron_ = 0.0f;
    float inputRudder_ = 0.0f;
    float inputThrottle_ = 0.0f;
    uint32_t clientSeq_ = 0;
    double sinceLastInputSend_ = 0.0;

    bool haveSnapshot_ = false;
    net::AircraftState latestAircraft_{};

    void handleEvent(const ENetEvent& event);
};

}  // namespace godot
