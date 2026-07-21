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

// GDExtension Node3D wrapping increment 1's FlightSession for use inside
// Godot's real-time frame loop. See docs/increment-2-specification.md,
// "FlightAircraft node" and Appendix A.
#pragma once

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include "test_runner.h"

namespace godot {

// Converts a geodetic position (relative to a local reference point) and
// aerospace Euler attitude into a Godot Transform3D. Factored out of
// FlightAircraft so it can be exercised independently of JSBSim/Godot's
// scene tree - see docs/increment-2-specification.md for the axis mapping
// this implements (Godot X=East, Y=Altitude, Z=-North) and the standard
// aerospace direction-cosine-matrix formulas used to derive the body axes.
Transform3D computeAircraftTransform(double lat_deg, double lon_deg,
                                      double alt_m, double roll_deg,
                                      double pitch_deg, double yaw_deg,
                                      double refLat_deg, double refLon_deg);

class FlightAircraft : public Node3D {
    GDCLASS(FlightAircraft, Node3D)

protected:
    static void _bind_methods();

public:
    void _physics_process(double delta) override;

    bool initialize();
    void setInitialCondition(float alt_ft, float vc_kts, float psi_true_deg,
                              float lat_deg, float lon_deg, float gamma_deg);
    bool trim();
    bool startLogging(const String& path);

    void setElevatorCmd(float value);
    float getElevatorCmd() const;
    void setAileronCmd(float value);
    float getAileronCmd() const;
    void setRudderCmd(float value);
    float getRudderCmd() const;
    void setThrottleCmd(float value);
    float getThrottleCmd() const;

    float getAltitudeM() const;
    float getAirspeedMps() const;
    float getTrueAirspeedMps() const;
    float getPitchDeg() const;
    float getBankDeg() const;
    float getAlphaDeg() const;

private:
    inc1::FlightSession session_;
    inc1::CsvLogger logger_;
    inc1::FlightSample lastSample_;
    bool initialized_ = false;
    bool logging_ = false;
    double refLat_ = 0.0;
    double refLon_ = 0.0;
};

}  // namespace godot
