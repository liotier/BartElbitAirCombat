# Copyright (C) 2026 The BartElbitAirCombat Project
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

# Dynamically instances one RemoteAircraft per OTHER connected player
# (docs/increment-5-specification.md, "Godot integration") - entity
# orchestration (spawn/despawn) is GDScript's job per this project's own
# two-layer language split (docs/roadmap.md, "Standing design decisions"),
# not RemoteAircraft's or PredictedAircraft's. Polls
# PredictedAircraft.get_active_remote_player_ids() each tick (simpler than
# wiring up signals for a handful of players) and diffs against its own
# currently-instanced set.
extends Node

const REMOTE_AIRCRAFT_SCENE := preload("res://scenes/remote_aircraft.tscn")

var _aircraft: PredictedAircraft
var _instances: Dictionary = {}  # player_id (int) -> RemoteAircraft node


func _ready() -> void:
	_aircraft = get_node("../PredictedAircraft")


func _physics_process(_delta: float) -> void:
	var active_ids := {}
	for player_id in _aircraft.get_active_remote_player_ids():
		active_ids[player_id] = true
		if not _instances.has(player_id):
			var instance: Node3D = REMOTE_AIRCRAFT_SCENE.instantiate()
			instance.player_id = player_id
			instance.source = _aircraft
			add_child(instance)
			_instances[player_id] = instance

	for player_id in _instances.keys():
		if not active_ids.has(player_id):
			_instances[player_id].queue_free()
			_instances.erase(player_id)
