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

#include "aircraft_orientation.h"

#include <cmath>

namespace geo {

namespace {
constexpr double kDegToRad = M_PI / 180.0;
}  // namespace

Vec3d nedToGodot(Vec3d ned) { return Vec3d{ned.y, -ned.z, -ned.x}; }

BodyAxes computeBodyAxes(double roll_deg, double pitch_deg, double yaw_deg) {
    // Identical arithmetic to godot_ext/flight_aircraft.cpp's original
    // inline derivation: standard aerospace 3-2-1 direction-cosine-matrix
    // giving each body axis in the local NED frame, then remapped into
    // Godot's X=East, Y=Up, Z=-North convention.
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

    Vec3d forward = nedToGodot(Vec3d{fwdN, fwdE, fwdD});
    Vec3d right = nedToGodot(Vec3d{rightN, rightE, rightD});
    Vec3d down = nedToGodot(Vec3d{downN, downE, downD});
    Vec3d up{-down.x, -down.y, -down.z};

    return BodyAxes{right, up, forward};
}

BodyAxes computeBodyAxesFromQuat(double q0, double q1, double q2,
                                  double q3) {
    // Transcribed verbatim from JSBSim's FGQuaternion::
    // ComputeDerivedUnconditional() (Stevens & Lewis Eqn. 1.3-32): the
    // Local(NED)-to-Body direction cosine matrix from quaternion
    // components. q0 is the scalar part.
    double q0q0 = q0 * q0, q1q1 = q1 * q1, q2q2 = q2 * q2, q3q3 = q3 * q3;
    double q0q1 = q0 * q1, q0q2 = q0 * q2, q0q3 = q0 * q3;
    double q1q2 = q1 * q2, q1q3 = q1 * q3, q2q3 = q2 * q3;

    // Rows of Tl2b (NED->body) are the body axes expressed in NED.
    Vec3d fwdNED{q0q0 + q1q1 - q2q2 - q3q3, 2.0 * (q1q2 + q0q3),
                 2.0 * (q1q3 - q0q2)};
    Vec3d rightNED{2.0 * (q1q2 - q0q3), q0q0 - q1q1 + q2q2 - q3q3,
                   2.0 * (q2q3 + q0q1)};
    Vec3d downNED{2.0 * (q1q3 + q0q2), 2.0 * (q2q3 - q0q1),
                  q0q0 - q1q1 - q2q2 + q3q3};

    Vec3d forward = nedToGodot(fwdNED);
    Vec3d right = nedToGodot(rightNED);
    Vec3d down = nedToGodot(downNED);
    Vec3d up{-down.x, -down.y, -down.z};

    return BodyAxes{right, up, forward};
}

Quatf computeOrientationQuat(double roll_deg, double pitch_deg,
                             double yaw_deg) {
    BodyAxes axes = computeBodyAxes(roll_deg, pitch_deg, yaw_deg);

    // The rotation matrix whose columns are (right, up, -forward) - the
    // same matrix godot::Basis(right, up, -forward) would hold. Row-major
    // element names below match the standard matrix-to-quaternion
    // derivation (e.g. Shepperd 1978): mRC is row R, column C.
    double m00 = axes.right.x, m10 = axes.right.y, m20 = axes.right.z;
    double m01 = axes.up.x, m11 = axes.up.y, m21 = axes.up.z;
    double m02 = -axes.forward.x, m12 = -axes.forward.y, m22 = -axes.forward.z;

    // Four-case method: picks whichever of (trace, m00, m11, m22) is
    // largest as the division pivot, so the sqrt argument is always
    // safely bounded away from zero for any proper rotation matrix -
    // unlike the single-case trace formula, which loses precision (or
    // divides by ~0) near 180 degree rotations.
    double trace = m00 + m11 + m22;
    double qx, qy, qz, qw;
    if (trace > 0.0) {
        double s = std::sqrt(trace + 1.0) * 2.0;  // s = 4*qw
        qw = 0.25 * s;
        qx = (m21 - m12) / s;
        qy = (m02 - m20) / s;
        qz = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;  // s = 4*qx
        qw = (m21 - m12) / s;
        qx = 0.25 * s;
        qy = (m01 + m10) / s;
        qz = (m02 + m20) / s;
    } else if (m11 > m22) {
        double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;  // s = 4*qy
        qw = (m02 - m20) / s;
        qx = (m01 + m10) / s;
        qy = 0.25 * s;
        qz = (m12 + m21) / s;
    } else {
        double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;  // s = 4*qz
        qw = (m10 - m01) / s;
        qx = (m02 + m20) / s;
        qy = (m12 + m21) / s;
        qz = 0.25 * s;
    }

    return Quatf{static_cast<float>(qx), static_cast<float>(qy),
                 static_cast<float>(qz), static_cast<float>(qw)};
}

LocalOffset computeLocalOffset(double lat_deg, double lon_deg,
                                double ref_lat_deg, double ref_lon_deg) {
    // Same flat-earth constant as flight_aircraft.cpp's original inline
    // computation.
    constexpr double kMetersPerDegLat = 111320.0;
    double north_m = (lat_deg - ref_lat_deg) * kMetersPerDegLat;
    double east_m = (lon_deg - ref_lon_deg) * kMetersPerDegLat *
                     std::cos(ref_lat_deg * kDegToRad);
    return LocalOffset{east_m, north_m};
}

GeodeticPos invertLocalOffset(double east_m, double north_m,
                               double ref_lat_deg, double ref_lon_deg) {
    constexpr double kMetersPerDegLat = 111320.0;
    double lat_deg = ref_lat_deg + north_m / kMetersPerDegLat;
    double lon_deg = ref_lon_deg + east_m / (kMetersPerDegLat *
                                              std::cos(ref_lat_deg * kDegToRad));
    return GeodeticPos{lat_deg, lon_deg};
}

}  // namespace geo
