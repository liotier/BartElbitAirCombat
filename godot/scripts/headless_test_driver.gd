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

# Drives one of the five automated scenarios from
# docs/increment-2-specification.md ("Test scenarios"), selected via the
# TEST_SCENARIO environment variable. Tests 1-4 reuse increment 1's exact
# initial conditions, control-input schedules and pass thresholds
# (docs/increment-1-specification.md); test 5 (realtime_pacing) is new,
# checking that the 120Hz physics configuration actually holds against
# wall-clock time.
extends Node

const KT_TO_MPS := 1852.0 / 3600.0
const G0 := 9.80665

var aircraft: FlightAircraft
var scenario: String
var tick: int = 0
var max_ticks: int = 0

var t0_alt: float = 0.0
var t0_ias: float = 0.0
var t0_pitch: float = 0.0
var t0_tas: float = 0.0

var max_abs_bank: float = 0.0
var max_pitch_5_10: float = -INF
var max_alpha: float = -INF
var min_ias: float = INF
var max_bank_5_8: float = -INF
var crossing_tick: int = -1
var min_bank_pre_crossing: float = INF
var min_ias_power: float = INF
var max_ias_power: float = -INF
var any_nan: bool = false

var pace_first_usec: int = -1
var pace_last_usec: int = 0

var _overall_pass: bool = true


func _ready() -> void:
	aircraft = get_node("FlightAircraft")
	aircraft.initialize()

	scenario = OS.get_environment("TEST_SCENARIO")
	match scenario:
		"power_response":
			aircraft.set_initial_condition(5000.0, 70.0, 0.0, 0.0, 0.0, 0.0)
			max_ticks = 7200
		"pitch_response":
			aircraft.set_initial_condition(5000.0, 100.0, 0.0, 0.0, 0.0, 0.0)
			max_ticks = 3600
		"roll_response":
			aircraft.set_initial_condition(5000.0, 100.0, 0.0, 0.0, 0.0, 0.0)
			max_ticks = 1800
		"realtime_pacing":
			aircraft.set_initial_condition(5000.0, 100.0, 0.0, 0.0, 0.0, 0.0)
			max_ticks = 600
		_:
			scenario = "trim_stability"
			aircraft.set_initial_condition(5000.0, 100.0, 0.0, 0.0, 0.0, 0.0)
			max_ticks = 7200

	aircraft.trim()
	# Godot's --path flag makes the project directory (godot/) the effective
	# cwd for plain (non-res://) relative file I/O, so this needs to go up
	# one level to land in the repo root's results/ alongside increment 1's
	# own CSVs, not create a second results/ inside godot/.
	aircraft.start_logging("../results/godot_%s.csv" % scenario)

	t0_alt = aircraft.altitude_m
	t0_ias = aircraft.airspeed_mps
	t0_pitch = aircraft.pitch_deg
	t0_tas = aircraft.true_airspeed_mps


func _physics_process(_delta: float) -> void:
	var t_sec := float(tick) / 120.0

	match scenario:
		"pitch_response":
			if t_sec >= 5.0:
				aircraft.elevator_cmd = -1.0
		"roll_response":
			if t_sec >= 5.0:
				aircraft.aileron_cmd = 1.0
		"power_response":
			if t_sec >= 5.0:
				aircraft.throttle_cmd = 1.0

	_check_nan()

	var bank := aircraft.bank_deg
	max_abs_bank = max(max_abs_bank, absf(bank))
	max_alpha = max(max_alpha, aircraft.alpha_deg)
	min_ias = min(min_ias, aircraft.airspeed_mps)

	if scenario == "pitch_response" and t_sec >= 5.0 and t_sec <= 10.0:
		max_pitch_5_10 = max(max_pitch_5_10, aircraft.pitch_deg)

	if scenario == "roll_response":
		if t_sec >= 5.0 and t_sec <= 8.0:
			max_bank_5_8 = max(max_bank_5_8, bank)
		if t_sec >= 5.0 and crossing_tick < 0:
			min_bank_pre_crossing = min(min_bank_pre_crossing, bank)
			if bank >= 60.0:
				crossing_tick = tick

	if scenario == "power_response":
		min_ias_power = min(min_ias_power, aircraft.airspeed_mps)
		max_ias_power = max(max_ias_power, aircraft.airspeed_mps)

	if scenario == "realtime_pacing":
		var now := Time.get_ticks_usec()
		if pace_first_usec < 0:
			pace_first_usec = now
		pace_last_usec = now

	tick += 1
	if tick >= max_ticks:
		_finish()


func _check_nan() -> void:
	var vals := [
		aircraft.altitude_m, aircraft.airspeed_mps, aircraft.true_airspeed_mps,
		aircraft.pitch_deg, aircraft.bank_deg, aircraft.alpha_deg
	]
	for v in vals:
		if is_nan(v) or is_inf(v):
			any_nan = true


func _check(name: String, actual: float, limit: float, cmp: String) -> void:
	var ok: bool = actual <= limit if cmp == "<=" else actual >= limit
	if not ok:
		_overall_pass = false
	print("%s: actual=%.4f %s limit=%.4f -> %s" % [
		name, actual, cmp, limit, "PASS" if ok else "FAIL"
	])


func _finish() -> void:
	match scenario:
		"trim_stability":
			_check("altitude_drift_abs", absf(aircraft.altitude_m - t0_alt), 61.0, "<=")
			_check("max_bank_abs", max_abs_bank, 2.0, "<=")
			_check("ias_drift_abs", absf(aircraft.airspeed_mps - t0_ias), 5.0 * KT_TO_MPS, "<=")
			_check("pitch_drift_abs", absf(aircraft.pitch_deg - t0_pitch), 5.0, "<=")
		"pitch_response":
			_check("max_pitch_5_10", max_pitch_5_10, 30.0, ">=")
			_check("max_alpha", max_alpha, 12.0, ">=")
			_check("min_ias", min_ias, 60.0 * KT_TO_MPS, "<=")
		"roll_response":
			_check("max_bank_5_8", max_bank_5_8, 60.0, ">=")
			_check("min_bank_pre_crossing", min_bank_pre_crossing, -10.0, ">=")
		"power_response":
			var eh_end: float = aircraft.altitude_m + (aircraft.true_airspeed_mps ** 2) / (2.0 * G0)
			var eh_0: float = t0_alt + (t0_tas ** 2) / (2.0 * G0)
			_check("energy_height_gain", eh_end - eh_0, 100.0, ">=")
			_check("altitude_gain", aircraft.altitude_m - t0_alt, 0.0, ">=")
			_check("ias_floor", min_ias_power, 50.0 * KT_TO_MPS, ">=")
			_check("ias_ceiling", max_ias_power, 140.0 * KT_TO_MPS, "<=")
		"realtime_pacing":
			var elapsed_sec: float = float(pace_last_usec - pace_first_usec) / 1_000_000.0
			var measured_hz: float = float(max_ticks - 1) / elapsed_sec
			_check("physics_hz_floor", measured_hz, 114.0, ">=")
			_check("physics_hz_ceiling", measured_hz, 126.0, "<=")

	_check("no_nan", 1.0 if any_nan else 0.0, 0.0, "<=")

	print("%s: %s" % [scenario, "PASS" if _overall_pass else "FAIL"])
	get_tree().quit(0 if _overall_pass else 1)
