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

// The increment-4 reconstruction gate (docs/increment-4-specification.md,
// "Reconstruction gate"): reconstructs a JSBSim VehicleState from
// wire-representable fields and applies it via FlightSession::setVState().
// This is the one place reconciliation can *corrupt* state rather than
// merely fail to help, so the recipe here is normative - verified during
// review to round-trip attitude to 6e-6 deg, position to 0.1 mm, velocity
// to 0, through inverted flight and 90 deg pitch (Appendix B). Do not
// improvise the frame algebra.
#pragma once

#include "test_runner.h"

namespace predict {

// lat_geod_rad/lon_rad/h_sl_m: geodetic position. qLocalWxyz: JSBSim's
// native local-attitude quaternion components in q(1..4) order (w,x,y,z) -
// the wire's `quat` field, decoded and normalized by the caller is not
// required (this function normalizes). vN_mps/vE_mps/vD_mps: local NED
// velocity. p_rps/q_rps/r_rps: body rates (JSBSim p,q,r), fed straight to
// vPQR with no rotation.
void reconstructAndApply(inc1::FlightSession& session, double lat_geod_rad,
                          double lon_rad, double h_sl_m,
                          const double qLocalWxyz[4], double vN_mps,
                          double vE_mps, double vD_mps, double p_rps,
                          double q_rps, double r_rps);

}  // namespace predict
