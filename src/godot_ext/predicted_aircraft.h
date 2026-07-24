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

// GDExtension Node3D for the local player's own aircraft (docs/increment-
// 4-specification.md, "Architecture"). Subclasses FlightAircraft (proven
// workable, Appendix B) to inherit its FlightSession ownership and
// control/telemetry surface unchanged, adding: a PredictedSession-driven
// input/state ring buffer, its own net::NetClient connection (reusing the
// same wrapper NetworkClient used before it was superseded - this node
// owns the connection entirely), and a smoothed rendered transform across
// reconciliation corrections. Replaced RemoteAircraft's role for the
// local player in networked.tscn.
//
// Increment 5 (docs/increment-5-specification.md, "Remote-entity
// interpolation" / "Godot integration"): also owns the single
// interp::RemoteEntityTracker for *other* connected clients' aircraft -
// this node is the only live network connection a game client has, so it
// is the natural place to feed chunks for player_ids other than its own.
// Dynamically-instanced RemoteAircraft nodes (one per other player,
// spawned/freed by a GDScript sibling watching getActiveRemotePlayerIds())
// query this node's getRemotePosition()/getRemoteOrientation() each tick -
// entity orchestration (spawn/despawn) is GDScript's job per this
// project's own two-layer language split (docs/roadmap.md, "Standing
// design decisions"), not this node's.
#pragma once

#include "flight_aircraft.h"
#include "interpcore/remote_entity_tracker.h"
#include "netcore/net_client.h"
#include "predictcore/predicted_session.h"

#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <memory>

namespace godot {

class PredictedAircraft : public FlightAircraft {
    GDCLASS(PredictedAircraft, FlightAircraft)

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

    // Increment 6 (docs/increment-6-specification.md, "Server-authoritative
    // type selection"): the aircraft type this client trims locally -
    // must match the server's own --aircraft for prediction/reconciliation
    // to make sense (see handleEvent's ServerWelcome case).
    void setAircraftType(const String& type);
    String getAircraftType() const;
    // "Continue flying only if a deliberate override flag is set" - for a
    // tester who explicitly wants to observe the mismatch behaviour rather
    // than the default warn-and-disconnect.
    void setAllowAircraftMismatch(bool allow);
    bool getAllowAircraftMismatch() const;

    bool isConnectedToServer() const;
    int getCorrectionCount() const;

    // Increment 5: the remote-entity data path. hasRemote()/
    // getRemotePosition()/getRemoteOrientation() are plain C++ calls (not
    // Godot-bound - RemoteAircraft calls them directly, both classes
    // living in the same GDExtension library); getActiveRemotePlayerIds()
    // is bound for the GDScript spawner script to poll.
    bool hasRemote(int playerId) const;
    Vector3 getRemotePosition(int playerId) const;
    Quaternion getRemoteOrientation(int playerId) const;
    PackedInt32Array getActiveRemotePlayerIds() const;

private:
    net::NetClient client_;
    bool enetInitialized_ = false;
    bool helloSent_ = false;
    bool welcomed_ = false;
    uint8_t assignedPlayerId_ = 0;

    String serverHost_ = "127.0.0.1";
    int serverPort_ = 45300;
    String aircraftType_ = "c172x";
    bool allowAircraftMismatch_ = false;

    // Constructed in _ready() once trim() has succeeded (session_ exists
    // as soon as the base FlightAircraft subobject does, but there is no
    // point predicting before there is a valid trimmed state to predict
    // from) - null before then and doubles as the "fully set up" guard.
    std::unique_ptr<predict::PredictedSession> predicted_;
    int correctionCount_ = 0;

    // Increment 5: fed from every received chunk entry whose player_id is
    // not this client's own (handleEvent), queried by RemoteAircraft
    // instances via getRemotePosition()/getRemoteOrientation().
    interp::RemoteEntityTracker remoteTracker_;

    // Smooth error correction (visual only, docs/increment-4-
    // specification.md): blends the rendered transform from wherever it
    // was toward the (possibly still-evolving) local prediction over
    // kBlendDurationS, re-targeting from the current interpolated pose -
    // never restarting from a stale anchor - if a new correction arrives
    // before a blend completes (review finding m2).
    bool blending_ = false;
    double blendElapsed_ = 0.0;
    Transform3D blendFrom_;
    static constexpr double kBlendDurationS = 0.150;

    void handleEvent(const ENetEvent& event);
};

}  // namespace godot
