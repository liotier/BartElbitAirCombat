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

#include "aircraft_catalog.h"

namespace aircraft {

namespace {

// docs/increment-6-specification.md, "Aircraft catalog" table.
const std::vector<CatalogEntry> kCatalog = {
    {"c172x", "c172x", 0, 5000.0, 100.0},
    {"camel", "Camel", 1, 5000.0, 65.0},
    {"pa28", "pa28", 2, 1000.0, 100.0},
};

}  // namespace

const CatalogEntry* findByToken(const std::string& token) {
    for (const CatalogEntry& e : kCatalog) {
        if (e.token == token) return &e;
    }
    return nullptr;
}

const CatalogEntry* findById(uint8_t id) {
    for (const CatalogEntry& e : kCatalog) {
        if (e.aircraft_id == id) return &e;
    }
    return nullptr;
}

const std::vector<CatalogEntry>& all() { return kCatalog; }

}  // namespace aircraft
