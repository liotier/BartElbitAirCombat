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

#include "flight_aircraft.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>

namespace godot {

namespace {
constexpr double kDegToRad = M_PI / 180.0;
// Standard flat-earth approximation, adequate for the short-range flight
// this increment involves (see docs/increment-2-specification.md).
constexpr double kMetersPerDegLat = 111320.0;
}  // namespace

Transform3D computeAircraftTransform(double lat_deg, double lon_deg,
                                      double alt_m, double roll_deg,
                                      double pitch_deg, double yaw_deg,
                                      double refLat_deg, double refLon_deg) {
    // Position: local tangent plane relative to (refLat, refLon). Axis
    // mapping is Godot X=East, Y=Altitude (MSL), Z=-North - chosen so that
    // an aircraft pointed true north (the increment 1 initial-condition
    // convention) faces Godot's default -Z forward direction.
    double north_m = (lat_deg - refLat_deg) * kMetersPerDegLat;
    double east_m = (lon_deg - refLon_deg) * kMetersPerDegLat *
                     std::cos(refLat_deg * kDegToRad);
    Vector3 position(static_cast<real_t>(east_m), static_cast<real_t>(alt_m),
                      static_cast<real_t>(-north_m));

    // Rotation: standard aerospace 3-2-1 (yaw-pitch-roll) direction-cosine
    // matrix, giving each body axis expressed in the local NED frame.
    double phi = roll_deg * kDegToRad;
    double theta = pitch_deg * kDegToRad;
    double psi = yaw_deg * kDegToRad;
    double sphi = std::sin(phi), cphi = std::cos(phi);
    double stheta = std::sin(theta), ctheta = std::cos(theta);
    double spsi = std::sin(psi), cpsi = std::cos(psi);

    // Body forward (nose) axis in NED.
    double fwdN = ctheta * cpsi;
    double fwdE = ctheta * spsi;
    double fwdD = -stheta;

    // Body right (right wing) axis in NED.
    double rightN = sphi * stheta * cpsi - cphi * spsi;
    double rightE = sphi * stheta * spsi + cphi * cpsi;
    double rightD = sphi * ctheta;

    // Body down (belly) axis in NED.
    double downN = cphi * stheta * cpsi + sphi * spsi;
    double downE = cphi * stheta * spsi - sphi * cpsi;
    double downD = cphi * ctheta;

    auto nedToGodot = [](double n, double e, double d) {
        return Vector3(static_cast<real_t>(e), static_cast<real_t>(-d),
                        static_cast<real_t>(-n));
    };

    Vector3 forward = nedToGodot(fwdN, fwdE, fwdD);
    Vector3 right = nedToGodot(rightN, rightE, rightD);
    Vector3 down = nedToGodot(downN, downE, downD);
    Vector3 up = -down;

    // Godot's local -Z is forward and +X is right; a Basis is built from
    // where each local axis ends up (its columns).
    Basis basis(right, up, -forward);
    return Transform3D(basis, position);
}

void FlightAircraft::_bind_methods() {
    ClassDB::bind_method(D_METHOD("initialize"), &FlightAircraft::initialize);
    ClassDB::bind_method(
        D_METHOD("set_initial_condition", "alt_ft", "vc_kts", "psi_true_deg",
                 "lat_deg", "lon_deg", "gamma_deg"),
        &FlightAircraft::setInitialCondition);
    ClassDB::bind_method(D_METHOD("trim"), &FlightAircraft::trim);
    ClassDB::bind_method(D_METHOD("start_logging", "path"),
                         &FlightAircraft::startLogging);

    ClassDB::bind_method(D_METHOD("set_elevator_cmd", "value"),
                         &FlightAircraft::setElevatorCmd);
    ClassDB::bind_method(D_METHOD("get_elevator_cmd"),
                         &FlightAircraft::getElevatorCmd);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "elevator_cmd"),
                 "set_elevator_cmd", "get_elevator_cmd");

    ClassDB::bind_method(D_METHOD("set_aileron_cmd", "value"),
                         &FlightAircraft::setAileronCmd);
    ClassDB::bind_method(D_METHOD("get_aileron_cmd"),
                         &FlightAircraft::getAileronCmd);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "aileron_cmd"),
                 "set_aileron_cmd", "get_aileron_cmd");

    ClassDB::bind_method(D_METHOD("set_rudder_cmd", "value"),
                         &FlightAircraft::setRudderCmd);
    ClassDB::bind_method(D_METHOD("get_rudder_cmd"),
                         &FlightAircraft::getRudderCmd);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rudder_cmd"), "set_rudder_cmd",
                 "get_rudder_cmd");

    ClassDB::bind_method(D_METHOD("set_throttle_cmd", "value"),
                         &FlightAircraft::setThrottleCmd);
    ClassDB::bind_method(D_METHOD("get_throttle_cmd"),
                         &FlightAircraft::getThrottleCmd);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "throttle_cmd"),
                 "set_throttle_cmd", "get_throttle_cmd");

    ClassDB::bind_method(D_METHOD("get_altitude_m"),
                         &FlightAircraft::getAltitudeM);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "altitude_m"), "",
                 "get_altitude_m");

    ClassDB::bind_method(D_METHOD("get_airspeed_mps"),
                         &FlightAircraft::getAirspeedMps);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "airspeed_mps"), "",
                 "get_airspeed_mps");

    ClassDB::bind_method(D_METHOD("get_true_airspeed_mps"),
                         &FlightAircraft::getTrueAirspeedMps);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "true_airspeed_mps"), "",
                 "get_true_airspeed_mps");

    ClassDB::bind_method(D_METHOD("get_pitch_deg"),
                         &FlightAircraft::getPitchDeg);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "pitch_deg"), "",
                 "get_pitch_deg");

    ClassDB::bind_method(D_METHOD("get_bank_deg"), &FlightAircraft::getBankDeg);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bank_deg"), "", "get_bank_deg");

    ClassDB::bind_method(D_METHOD("get_alpha_deg"),
                         &FlightAircraft::getAlphaDeg);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "alpha_deg"), "",
                 "get_alpha_deg");
}

bool FlightAircraft::initialize() {
    std::string error;
    if (!session_.initialize(error)) {
        UtilityFunctions::printerr("FlightAircraft::initialize failed: ",
                                   error.c_str());
        return false;
    }
    initialized_ = true;
    return true;
}

void FlightAircraft::setInitialCondition(float alt_ft, float vc_kts,
                                          float psi_true_deg, float lat_deg,
                                          float lon_deg, float gamma_deg) {
    session_.setInitialCondition(alt_ft, vc_kts, psi_true_deg, lat_deg,
                                  lon_deg, gamma_deg);
}

bool FlightAircraft::trim() {
    std::string error;
    if (!session_.trim(error)) {
        UtilityFunctions::printerr("FlightAircraft::trim failed: ",
                                   error.c_str());
        return false;
    }
    // The trimmed starting position becomes the local tangent-plane origin
    // for the transform computed in _physics_process.
    lastSample_ = session_.sample();
    refLat_ = lastSample_.lat_deg;
    refLon_ = lastSample_.lon_deg;
    return true;
}

bool FlightAircraft::startLogging(const String& path) {
    std::string error;
    std::string pathStd(path.utf8().get_data());
    if (!logger_.open(pathStd, error)) {
        UtilityFunctions::printerr("FlightAircraft::start_logging failed: ",
                                   error.c_str());
        return false;
    }
    logging_ = true;
    // Matches increment 1's convention: the first row is the t=0 sample
    // taken before any integration step, not the state after the first
    // _physics_process tick.
    logger_.writeRow(lastSample_);
    return true;
}

void FlightAircraft::setElevatorCmd(float value) {
    if (!initialized_) return;
    session_.setProperty("fcs/elevator-cmd-norm", value);
}
float FlightAircraft::getElevatorCmd() const {
    if (!initialized_) return 0.0f;
    return static_cast<float>(session_.property("fcs/elevator-cmd-norm"));
}

void FlightAircraft::setAileronCmd(float value) {
    if (!initialized_) return;
    session_.setProperty("fcs/aileron-cmd-norm", value);
}
float FlightAircraft::getAileronCmd() const {
    if (!initialized_) return 0.0f;
    return static_cast<float>(session_.property("fcs/aileron-cmd-norm"));
}

void FlightAircraft::setRudderCmd(float value) {
    if (!initialized_) return;
    session_.setProperty("fcs/rudder-cmd-norm", value);
}
float FlightAircraft::getRudderCmd() const {
    if (!initialized_) return 0.0f;
    return static_cast<float>(session_.property("fcs/rudder-cmd-norm"));
}

void FlightAircraft::setThrottleCmd(float value) {
    if (!initialized_) return;
    session_.setProperty("fcs/throttle-cmd-norm", value);
}
float FlightAircraft::getThrottleCmd() const {
    if (!initialized_) return 0.0f;
    return static_cast<float>(session_.property("fcs/throttle-cmd-norm"));
}

float FlightAircraft::getAltitudeM() const {
    return static_cast<float>(lastSample_.alt_m);
}
float FlightAircraft::getAirspeedMps() const {
    return static_cast<float>(lastSample_.ias_mps);
}
float FlightAircraft::getTrueAirspeedMps() const {
    return static_cast<float>(lastSample_.tas_mps);
}
float FlightAircraft::getPitchDeg() const {
    return static_cast<float>(lastSample_.pitch_deg);
}
float FlightAircraft::getBankDeg() const {
    return static_cast<float>(lastSample_.roll_deg);
}
float FlightAircraft::getAlphaDeg() const {
    return static_cast<float>(lastSample_.alpha_deg);
}

void FlightAircraft::_physics_process(double delta) {
    (void)delta;
    if (!initialized_) return;
    session_.step();
    lastSample_ = session_.sample();

    set_global_transform(computeAircraftTransform(
        lastSample_.lat_deg, lastSample_.lon_deg, lastSample_.alt_m,
        lastSample_.roll_deg, lastSample_.pitch_deg, lastSample_.yaw_deg,
        refLat_, refLon_));

    if (logging_) {
        logger_.writeRow(lastSample_);
    }
}

}  // namespace godot
