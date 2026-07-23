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

// GDExtension Node3D representing ONE other connected player's aircraft
// (docs/increment-5-specification.md, "Godot integration"). Increment 5
// rework: previously a single scene-authored node consuming one
// NetworkClient singleton's one latestAircraft_ (raw snapshot application,
// no interpolation); now dynamically instanced - one per other player_id,
// spawned/freed by a GDScript sibling script watching
// PredictedAircraft::get_active_remote_player_ids() (entity orchestration
// is GDScript's job per this project's two-layer language split,
// docs/roadmap.md's "Standing design decisions" - this class itself does
// not decide when to exist). `source` is set once by the spawner right
// after instancing, before add_child() - a direct reference rather than a
// NodePath lookup, since a dynamically-instanced node has no fixed
// position in the scene tree to look one up from.
#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>

namespace godot {

class RemoteAircraft : public Node3D {
    GDCLASS(RemoteAircraft, Node3D)

protected:
    static void _bind_methods();

public:
    void _physics_process(double delta) override;

    void setPlayerId(int id);
    int getPlayerId() const;
    void setSource(Node* source);
    Node* getSource() const;

private:
    int playerId_ = 0;
    Node* source_ = nullptr;
};

}  // namespace godot
