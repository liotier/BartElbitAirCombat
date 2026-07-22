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

# Same keyboard map as increment 2's flight_input.gd, but sends the input
# to NetworkClient instead of driving a local FlightSession directly
# (docs/increment-3-specification.md, "Godot client"): this client owns
# no simulation, only a control surface for the server-authoritative one.
extends Node

const THROTTLE_RATE := 1.0 / 3.0  # full range in ~3s, same as increment 2

var _network_client: NetworkClient
# Starts at full power rather than 0: unlike FlightAircraft (increment 2),
# this client cannot read back the server's actual trimmed throttle
# before its first input arrives, and once any ControlInput is received
# the server applies all four axes from it (net_server, "Wire protocol"
# notes). Starting at 0 would silently cut power the instant the first
# packet lands; full power is the less surprising default and is easy to
# reduce with Page Down.
var _throttle := 1.0


func _ready() -> void:
	_network_client = get_node("../NetworkClient")


func _physics_process(delta: float) -> void:
	var pitch := 0.0
	if Input.is_key_pressed(KEY_W):
		pitch += 1.0  # forward stick, nose down
	if Input.is_key_pressed(KEY_S):
		pitch -= 1.0  # aft stick, nose up

	var roll := 0.0
	if Input.is_key_pressed(KEY_D):
		roll += 1.0  # roll right
	if Input.is_key_pressed(KEY_A):
		roll -= 1.0  # roll left

	var yaw := 0.0
	if Input.is_key_pressed(KEY_E):
		yaw += 1.0  # right rudder
	if Input.is_key_pressed(KEY_Q):
		yaw -= 1.0  # left rudder

	if Input.is_key_pressed(KEY_PAGEUP):
		_throttle += THROTTLE_RATE * delta
	if Input.is_key_pressed(KEY_PAGEDOWN):
		_throttle -= THROTTLE_RATE * delta
	_throttle = clampf(_throttle, 0.0, 1.0)

	_network_client.set_input(pitch, roll, yaw, _throttle)
