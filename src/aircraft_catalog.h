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

// The increment-6 aircraft catalog (docs/increment-6-specification.md,
// "Aircraft catalog"): the one place aircraft-specific identity and trim
// IC live. Three fields are kept explicitly distinct and never derived
// from one another (review finding B1) - a `token` (lowercase, user-
// facing: --aircraft, AIRCRAFT env), a `load_model` (JSBSim's exact-case
// LoadModel string - Camel's directory is "Camel/", so LoadModel("camel")
// fails on a case-sensitive filesystem, measured, Appendix B), and an
// `aircraft_id` (the wire encoding, ServerWelcome.aircraft_id).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aircraft {

struct CatalogEntry {
    std::string token;
    std::string load_model;
    uint8_t aircraft_id = 0;
    double canonical_alt_ft = 0.0;
    double canonical_vc_kts = 0.0;
};

// Looks up by the lowercase user-facing token (e.g. "camel"). Returns
// nullptr if `token` names no known aircraft.
const CatalogEntry* findByToken(const std::string& token);

// Looks up by wire aircraft_id. Returns nullptr if `id` names no known
// aircraft.
const CatalogEntry* findById(uint8_t id);

// Every catalog entry, in table order - for exhaustive iteration (e.g.
// the increment-6 standalone load+trim regression test), so that test
// stays correct if a future increment grows the catalog rather than
// needing its own hardcoded token list kept in sync by hand.
const std::vector<CatalogEntry>& all();

}  // namespace aircraft
