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

// GDExtension Node3D that applies a NetworkClient's latest received
// snapshot to its own transform - raw application, no interpolation
// (docs/increment-3-specification.md, "Godot client"; smoothing is
// increment 4). Deliberately does not share a base class with
// FlightAircraft (increment 2): one steps a FlightSession every tick,
// this one only ever consumes already-resolved position/orientation, so
// the only thing in common is being a Node3D, which Node3D itself
// already provides (implementation note, resolving the spec's own
// "share a base class?" open question).
#pragma once

#include <godot_cpp/classes/node3d.hpp>

namespace godot {

class NetworkClient;

class RemoteAircraft : public Node3D {
    GDCLASS(RemoteAircraft, Node3D)

protected:
    static void _bind_methods();

public:
    void _ready() override;
    void _physics_process(double delta) override;

private:
    NetworkClient* networkClient_ = nullptr;
};

}  // namespace godot
