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

#include "reconciliation_math.h"

#include <cmath>

namespace predict {

double quaternionAngleDeg(double w1, double x1, double y1, double z1,
                           double w2, double x2, double y2, double z2) {
    // Relative rotation q1^-1 * q2 (unit quaternions, so inverse ==
    // conjugate): standard Hamilton product of conj(q1) and q2.
    double rw = w1 * w2 + x1 * x2 + y1 * y2 + z1 * z2;
    double rx = w1 * x2 - x1 * w2 - y1 * z2 + z1 * y2;
    double ry = w1 * y2 + x1 * z2 - y1 * w2 - z1 * x2;
    double rz = w1 * z2 - x1 * y2 + y1 * x2 - z1 * w2;
    double vnorm = std::sqrt(rx * rx + ry * ry + rz * rz);
    double angleRad = 2.0 * std::atan2(vnorm, std::fabs(rw));
    return angleRad * (180.0 / M_PI);
}

}  // namespace predict
