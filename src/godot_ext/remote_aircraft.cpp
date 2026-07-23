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

#include "remote_aircraft.h"

#include "predicted_aircraft.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/transform3d.hpp>

namespace godot {

void RemoteAircraft::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_player_id", "id"),
                         &RemoteAircraft::setPlayerId);
    ClassDB::bind_method(D_METHOD("get_player_id"),
                         &RemoteAircraft::getPlayerId);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "player_id"), "set_player_id",
                 "get_player_id");

    ClassDB::bind_method(D_METHOD("set_source", "source"),
                         &RemoteAircraft::setSource);
    ClassDB::bind_method(D_METHOD("get_source"), &RemoteAircraft::getSource);
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "source", PROPERTY_HINT_NODE_TYPE,
                              "PredictedAircraft"),
                 "set_source", "get_source");
}

void RemoteAircraft::_physics_process(double delta) {
    (void)delta;
    PredictedAircraft* source = Object::cast_to<PredictedAircraft>(source_);
    if (!source || !source->hasRemote(playerId_)) return;
    // Both already in Godot's local East/Up/-North frame and Godot's own
    // rotation convention (PredictedAircraft::getRemotePosition()/
    // getRemoteOrientation() do the native-to-Godot conversion) - direct
    // application, no blending: unlike PredictedAircraft's own aircraft,
    // there is no "correction" event to smooth over here, only a
    // continuously-updated interpolated/extrapolated pose (docs/
    // increment-5-specification.md, "Godot integration").
    set_global_transform(
        Transform3D(Basis(source->getRemoteOrientation(playerId_)),
                    source->getRemotePosition(playerId_)));
}

void RemoteAircraft::setPlayerId(int id) { playerId_ = id; }
int RemoteAircraft::getPlayerId() const { return playerId_; }
void RemoteAircraft::setSource(Node* source) { source_ = source; }
Node* RemoteAircraft::getSource() const { return source_; }

}  // namespace godot
