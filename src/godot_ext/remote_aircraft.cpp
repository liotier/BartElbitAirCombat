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

#include "network_client.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

void RemoteAircraft::_bind_methods() {}

void RemoteAircraft::_ready() {
    // Sibling lookup, matching flight_input.gd's own hardcoded
    // get_node("../FlightAircraft") convention (increment 2) rather than
    // an exported NodePath property this project has no other precedent
    // for.
    Node* n = get_node_or_null(NodePath("../NetworkClient"));
    networkClient_ = Object::cast_to<NetworkClient>(n);
    if (!networkClient_) {
        UtilityFunctions::printerr(
            "RemoteAircraft: sibling NetworkClient not found at "
            "../NetworkClient");
    }
}

void RemoteAircraft::_physics_process(double delta) {
    (void)delta;
    if (!networkClient_ || !networkClient_->hasSnapshot()) return;
    // Wire fields are already in Godot's local East/Up/-North frame and
    // already a rotation quaternion in Godot's own convention (both
    // computed server-side by geo::computeLocalOffset()/
    // computeOrientationQuat() - see docs/increment-3-specification.md's
    // implementation notes), so this is direct application, not a call
    // through computeAircraftTransform() (which converts from geodetic
    // lat/lon and Euler angles - a different, upstream representation).
    set_global_transform(
        Transform3D(Basis(networkClient_->getRemoteOrientation()),
                    networkClient_->getRemotePosition()));
}

}  // namespace godot
