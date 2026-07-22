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

# Same keyboard map as flight_input.gd/networked_input.gd, but drives
# PredictedAircraft directly via its inherited (from FlightAircraft)
# elevator_cmd/aileron_cmd/rudder_cmd/throttle_cmd properties - increment 4
# (docs/increment-4-specification.md): PredictedAircraft owns its own
# FlightSession and network connection, so there is no separate
# NetworkClient node to go through.
extends Node

const THROTTLE_RATE := 1.0 / 3.0  # full range in ~3s, per spec

var _aircraft: PredictedAircraft


func _ready() -> void:
	_aircraft = get_node("../PredictedAircraft")


func _physics_process(delta: float) -> void:
	var pitch := 0.0
	if Input.is_key_pressed(KEY_W):
		pitch += 1.0  # forward stick, nose down
	if Input.is_key_pressed(KEY_S):
		pitch -= 1.0  # aft stick, nose up
	_aircraft.elevator_cmd = pitch

	var roll := 0.0
	if Input.is_key_pressed(KEY_D):
		roll += 1.0  # roll right
	if Input.is_key_pressed(KEY_A):
		roll -= 1.0  # roll left
	_aircraft.aileron_cmd = roll

	var yaw := 0.0
	if Input.is_key_pressed(KEY_E):
		yaw += 1.0  # right rudder
	if Input.is_key_pressed(KEY_Q):
		yaw -= 1.0  # left rudder
	_aircraft.rudder_cmd = yaw

	var throttle := _aircraft.throttle_cmd
	if Input.is_key_pressed(KEY_PAGEUP):
		throttle += THROTTLE_RATE * delta
	if Input.is_key_pressed(KEY_PAGEDOWN):
		throttle -= THROTTLE_RATE * delta
	_aircraft.throttle_cmd = clampf(throttle, 0.0, 1.0)
