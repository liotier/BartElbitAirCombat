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

#pragma once

namespace predict {

// Angle (degrees) between two unit quaternions, both in JSBSim's q(1..4)
// order (w,x,y,z). Computed as the relative rotation's angle via
// 2*atan2(|v|, |w|), which stays well-conditioned near zero - unlike
// acos(dot), which loses precision for exactly the small angles
// reconciliation needs to measure accurately (docs/increment-4-
// specification.md's "Client-side prediction algorithm" step 4, echoing
// increment 3's own m-finding about acos near small angles). Handles the
// quaternion double-cover (q and -q are the same rotation) via abs(w).
double quaternionAngleDeg(double w1, double x1, double y1, double z1,
                           double w2, double x2, double y2, double z2);

}  // namespace predict
