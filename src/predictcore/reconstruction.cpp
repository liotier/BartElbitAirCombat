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

#include "reconstruction.h"

#include "FGFDMExec.h"
#include "math/FGColumnVector3.h"
#include "math/FGLocation.h"
#include "math/FGMatrix33.h"
#include "math/FGQuaternion.h"
#include "models/FGPropagate.h"

namespace predict {

void reconstructAndApply(inc1::FlightSession& session, double lat_geod_rad,
                          double lon_rad, double h_sl_m,
                          const double qLocalWxyz[4], double vN_mps,
                          double vE_mps, double vD_mps, double p_rps,
                          double q_rps, double r_rps) {
    using JSBSim::FGColumnVector3;
    using JSBSim::FGLocation;
    using JSBSim::FGMatrix33;
    using JSBSim::FGQuaternion;

    auto P = session.fdm().GetPropagate();

    // Gotcha 2 (docs/increment-4-specification.md, "Reconstruction
    // gate"): must copy the session's existing ellipsoid-configured
    // vLocation, never a bare/default-constructed FGLocation, which has a
    // degenerate ellipsoid and gives a latitude-only position error that
    // grows with displacement.
    FGLocation loc = P->GetVState().vLocation;
    loc.SetPositionGeodetic(lon_rad, lat_geod_rad, h_sl_m / inc1::kFt2M);
    P->SetLocation(loc);  // updates location matrices + inertial position

    FGQuaternion qLocal;
    qLocal(1) = qLocalWxyz[0];
    qLocal(2) = qLocalWxyz[1];
    qLocal(3) = qLocalWxyz[2];
    qLocal(4) = qLocalWxyz[3];
    qLocal.Normalize();

    // Gotcha 1: Ti2l (ECI->local), *not* Tl2i - copied verbatim from
    // JSBSim's own FGPropagate.cpp::InitializeDerivatives().
    FGQuaternion qECI = P->GetTi2l().GetQuaternion() * qLocal;

    FGMatrix33 Tl2b = qLocal.GetT();
    FGColumnVector3 vNED(vN_mps / inc1::kFt2M, vE_mps / inc1::kFt2M,
                          vD_mps / inc1::kFt2M);
    FGColumnVector3 vUVW = Tl2b * vNED;

    auto vs = P->GetVState();  // carries the correct vLocation/
                                // vInertialPosition/epa SetLocation() set
    vs.qAttitudeECI = qECI;
    vs.vUVW = vUVW;
    vs.vPQR = FGColumnVector3(p_rps, q_rps, r_rps);
    session.setVState(vs);
}

}  // namespace predict
