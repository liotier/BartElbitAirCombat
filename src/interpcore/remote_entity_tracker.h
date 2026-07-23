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

// Remote-entity interpolation (docs/increment-5-specification.md, "Remote-
// entity interpolation"): buffers received AircraftState samples per
// player_id and produces a smoothly interpolated (or, past the newest
// sample, bounded-extrapolated) pose for rendering - the standard "render
// slightly in the past" pattern used by real-time multiplayer games since
// at least the Half-Life/Source engine era. Godot-free and depends only on
// netcore's wire types: unlike predictcore, there is no local FlightSession
// to simulate or reconcile, only already-received wire data to buffer and
// interpolate between, so this needs neither flightcore nor geo:: - the
// one-time conversion from the wire's native qAttitudeLocal to Godot's
// axis convention happens in the GDExtension rendering layer, once, on
// whatever pose sample() returns (mathematically equivalent to converting
// every buffered sample first: geo::computeBodyAxesFromQuat() is a fixed
// axis permutation, and slerp commutes with a fixed change of basis, so
// interpolating-then-converting and converting-then-interpolating agree -
// this just keeps that one conversion in the layer that already owns it
// rather than duplicating it here).
#pragma once

#include "netcore/protocol.h"

#include <cstdint>
#include <deque>
#include <unordered_map>

namespace interp {

// Position/orientation for one entity at one render instant. `quat` stays
// in the wire's native q(1..4) = (w,x,y,z) convention (see the header
// comment above for why the Godot-convention conversion is deliberately
// not done here). `valid` is false only if the requested player_id has
// never been seen (or was removed via PlayerLeft) - callers should not
// update a transform in that case, not render at the origin.
struct InterpolatedState {
    double pos_local_m[3] = {0.0, 0.0, 0.0};
    double quat[4] = {1.0, 0.0, 0.0, 0.0};
    bool valid = false;
};

class RemoteEntityTracker {
public:
    // Starting points (spec, "Remote-entity interpolation" / "Open
    // questions") - tunable, not load-bearing at these exact defaults,
    // the same status increment 4 gave its own correction threshold and
    // blend duration.
    static constexpr double kInterpolationDelayS = 0.100;
    static constexpr double kMaxExtrapolationS = 0.300;

    // Matches inc1::kDt (test_runner.h) without depending on flightcore -
    // this class only ever needs the tick period as a plain number to
    // convert a server_tick into a timeline position, never JSBSim itself.
    static constexpr double kTickPeriodS = 1.0 / 120.0;

    // Called once per received chunk entry (regardless of which chunk),
    // for any player_id other than the caller's own (docs/increment-5-
    // specification.md, "Wire protocol changes" point 2). `serverTick` -
    // not `arrivalTimeS` - is what anchors this sample on the
    // interpolation timeline (review finding N2): every chunk of one tick
    // shares the same server_tick, so timestamping by arrival would let
    // ordinary inter-chunk/network jitter read as motion. `arrivalTimeS`
    // (any steady, monotonic clock in seconds - std::chrono::steady_clock
    // is the natural choice) is retained only to translate a caller's
    // wall-clock `renderTimeS` (sample() below) into an equivalent
    // position on that same timeline; it never affects the interpolated
    // result directly.
    void update(uint8_t playerId, const net::AircraftState& state,
                uint32_t serverTick, double arrivalTimeS);

    // Removes a player's tracked history entirely - called on receiving
    // PlayerLeft, so a departed player's aircraft is deterministically
    // dropped rather than guessed at from a gap in snapshots.
    void remove(uint8_t playerId);

    // Renders at `renderTimeS` (same clock as `arrivalTimeS` above,
    // typically `now - kInterpolationDelayS`): interpolates between the
    // two buffered samples bracketing that instant, or, if no newer
    // sample has arrived yet (stream start, or catching up after a
    // stall), dead-reckons forward from the newest known sample using its
    // velocity/angular velocity, capped at kMaxExtrapolationS - past the
    // cap, holds the pose at exactly that capped extrapolation rather
    // than continuing to project forward indefinitely. A brand-new
    // entity (exactly one buffered sample) is handled by this same
    // extrapolation path with no special case (review finding m2): a
    // single sample already is "the newest known sample."
    InterpolatedState sample(uint8_t playerId, double renderTimeS) const;

    // The player_ids currently tracked (i.e. seen at least once via
    // update() and not since remove()'d) - lets a caller (the Godot
    // scene layer's dynamic RemoteAircraft spawner) enumerate who to
    // instantiate or free, without needing its own separate bookkeeping
    // of the same set.
    std::vector<uint8_t> activePlayerIds() const;

private:
    struct Sample {
        net::AircraftState state;
        uint32_t serverTick;
        double arrivalTimeS;
    };
    struct Entity {
        // Ascending server_tick. Capped (see remote_entity_tracker.cpp)
        // well above what kInterpolationDelayS + kMaxExtrapolationS could
        // ever need to look back across, at increment 3/4's established
        // 30 Hz snapshot rate - a handful of entries' worth of margin,
        // nothing like predictcore's ~10-second reconciliation buffer,
        // since there is no round trip to bridge here.
        std::deque<Sample> history;
        uint32_t latestServerTick = 0;
        double latestArrivalTimeS = 0.0;
    };

    std::unordered_map<uint8_t, Entity> entities_;
};

}  // namespace interp
