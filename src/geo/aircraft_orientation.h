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

// Godot-free core of the aerospace-Euler-to-engine-axis convention
// docs/increment-2-specification.md established and validated for
// FlightAircraft's transform. Factored out here (rather than left inside
// godot_ext/flight_aircraft.cpp) so docs/increment-3-specification.md's
// server - which must not depend on Godot - can compute the identical
// rotation for the wire quaternion, instead of an independently-derived
// formula that could silently disagree with the Godot client on axis
// convention or rotation sequence. flight_aircraft.cpp's
// computeAircraftTransform() calls computeBodyAxes() for exactly this
// reason; see that file for the Godot-side Vector3/Basis assembly.
#pragma once

namespace geo {

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// A rotation's right/up/forward axes, expressed in Godot's engine
// convention (X=East, Y=Up, Z=-North; local -Z is "forward"). Together
// these three vectors are a rotation matrix's columns (right, up,
// -forward) - see docs/increment-2-specification.md for the derivation.
struct BodyAxes {
    Vec3d right;
    Vec3d up;
    Vec3d forward;
};

// roll/pitch/yaw are degrees, standard aerospace 3-2-1 (yaw-pitch-roll)
// Euler angles in the local NED frame.
BodyAxes computeBodyAxes(double roll_deg, double pitch_deg, double yaw_deg);

struct Quatf {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

// The rotation quaternion (x, y, z, w) equivalent to the Basis whose
// columns are (right, up, -forward) from computeBodyAxes() at the same
// angles - i.e. exactly the rotation FlightAircraft's transform would
// show, in the compact form docs/increment-3-specification.md's
// StateSnapshot carries over the wire. Uses the standard four-case
// (Shepperd) matrix-to-quaternion method, numerically safe regardless of
// which diagonal term is largest; see
// docs/increment-3-specification.md's implementation notes for the
// empirical cross-check against Godot's own Basis::get_quaternion().
Quatf computeOrientationQuat(double roll_deg, double pitch_deg, double yaw_deg);

// Local tangent-plane offset (east_m, north_m) of (lat_deg, lon_deg) from
// (ref_lat_deg, ref_lon_deg), using the same flat-earth approximation as
// computeAircraftTransform() - adequate for the short-range flight this
// project involves (docs/increment-2-specification.md).
struct LocalOffset {
    double east_m = 0.0;
    double north_m = 0.0;
};
LocalOffset computeLocalOffset(double lat_deg, double lon_deg,
                                double ref_lat_deg, double ref_lon_deg);

}  // namespace geo
