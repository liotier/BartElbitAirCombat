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

// Wire message types and their field-by-field (de)serialization. This is
// the normative reference for docs/increment-3-specification.md's
// "Messages" table and Appendix A - keep this file's layout in sync with
// that table; it is the single source both the server and every client
// form compile against, which is what guarantees they speak byte-
// identical wire format (see the spec's "Architecture").
//
// All multi-byte fields are little-endian, written/read explicitly byte
// by byte rather than via memcpy of a packed struct, so wire layout never
// depends on host endianness or compiler struct padding (spec review m4).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace net {

// Bumped whenever a message layout below changes; ClientHello/
// ServerWelcome exchange this so mismatched builds reject cleanly
// instead of misinterpreting each other's bytes (spec, "Wire protocol").
// Standing rule (docs/increment-6-specification.md, "Wire protocol
// changes"): any change to a message's wire layout bumps this constant.
// 2: increment 6 adds ServerWelcome.aircraft_id. 3: increment 7 adds
// ClientHello.client_flags.
constexpr uint8_t kProtocolVersion = 3;

// ENet channel assignment (spec, "Messages" table's Channel column).
constexpr uint8_t kChannelReliable = 0;
constexpr uint8_t kChannelUnreliable = 1;

// Fixed-point scale for the signed [-1,1] control axes (elevator,
// aileron, rudder) (spec Appendix A): wire value =
// round(physical_value * kControlAxisScale).
constexpr double kControlAxisScale = 32767.0;

// Fixed-point scale for throttle, [0,1] (spec Appendix A). A dedicated
// uint8 rather than reusing the signed int16 axis encoding: throttle
// never goes negative, so int16 would waste half its range (spec review
// m3), and unlike elevator/aileron/rudder - which set rotation rates,
// where visible quantization reads as jitter - throttle is a coarse,
// movement-like axis. This mirrors Quake 3's own usercmd_t split:
// forwardmove/rightmove/upmove (coarse) are a signed char; view angles
// (rotation-sensitive) get 16 bits.
constexpr double kThrottleScale = 255.0;

// Increment 4, "Wire protocol changes" / "Open questions": redundancy
// depth R for ControlInput's repeated commands. Must exceed the longest
// consecutive-packet-loss run this project's tests care about; at R=6 a
// command survives unless all 6 carrying packets drop (0.4^6 ~= 0.4% at
// 40% loss - verified, docs/increment-4-specification.md Appendix B).
constexpr uint8_t kMaxRedundantCommands = 6;

// Increment 5, "Wire protocol changes": max aircraft carried by one
// StateSnapshot chunk. Chosen with real margin below the measured/computed
// 23-aircraft fragmentation threshold at today's 58-byte AircraftState
// (docs/increment-5-specification.md Appendix B) so it survives future
// per-aircraft field growth without needing to be re-derived under time
// pressure - see the spec's "Why application-level chunking" section for
// why chunking, not ENet's own fragmentation, is the fix.
constexpr uint8_t kMaxAircraftPerChunk = 16;

enum class MessageTag : uint8_t {
    kClientHello = 1,
    kServerWelcome = 2,
    kServerReject = 3,
    kControlInput = 4,
    kStateSnapshot = 5,
    kClientBye = 6,
    kPlayerLeft = 7,
};

enum class RejectReason : uint8_t {
    kVersionMismatch = 1,
    kServerFull = 2,
};

// Increment 7 (docs/increment-7-specification.md, "One unified bot
// interface"): bit 0 of client_flags is a bot's own self-declaration - set
// identically by a local (server-forked) bot and, in a future increment, a
// remote one, so the server has exactly one "is a bot" code path. Other
// bits reserved.
constexpr uint8_t kClientFlagIsBot = 0x01;

struct ClientHello {
    uint8_t protocol_version = kProtocolVersion;
    uint8_t client_flags = 0;
};

struct ServerWelcome {
    uint8_t protocol_version = kProtocolVersion;
    uint8_t assigned_player_id = 0;
    float origin_lat_deg = 0.0f;
    float origin_lon_deg = 0.0f;
    uint16_t snapshot_hz = 0;
    // Increment 6 (docs/increment-6-specification.md, "Aircraft catalog"):
    // the server's configured aircraft type - 0=c172x, 1=camel, 2=pa28.
    uint8_t aircraft_id = 0;
};

struct ServerReject {
    uint8_t reason_code = 0;
};

// One tick's worth of control-axis input (increment 4, "Wire protocol
// changes"). Encoded fields, same fixed-point scales as increment 3.
struct ControlCommand {
    int16_t elevator = 0;
    int16_t aileron = 0;
    int16_t rudder = 0;
    uint8_t throttle = 0;
};

// Increment 4: redundant multi-command packet (review finding B2) - the
// Quake/Source pattern of resending the last few commands per packet so a
// single dropped packet loses no command. `commands` holds up to
// kMaxRedundantCommands entries for seqs newest_client_seq,
// newest_client_seq-1, ... in descending order (see protocol.cpp's
// (de)serialization for the exact wire order).
struct ControlInput {
    uint32_t newest_client_seq = 0;
    std::vector<ControlCommand> commands;
};

// Increment 7 (docs/increment-7-specification.md, "Marking bots as
// non-human"): bit 0 of AircraftState.status_flags - set by the server for
// any peer whose ClientHello declared it a bot (kClientFlagIsBot), clear
// for humans. The first defined bit of a field increment 5 reserved.
constexpr uint8_t kStatusFlagBot = 0x01;

// One aircraft's rigid-body state within a StateSnapshot (spec, "Messages"
// table's StateSnapshot row). status_flags bit 0: see kStatusFlagBot;
// other bits still reserved for future increments (e.g. crashed).
struct AircraftState {
    uint8_t player_id = 0;
    float pos_local_m[3] = {0.0f, 0.0f, 0.0f};  // East, Up, -North
    // Increment 4 (docs/increment-4-specification.md Appendix A): JSBSim's
    // native qAttitudeLocal (body->NED) components in q(1..4) order, i.e.
    // (w,x,y,z) - *not* Godot's (x,y,z,w), and *not* increment 3's
    // Godot-convention quaternion. Chosen so reconciliation's VehicleState
    // reconstruction needs no conversion; display code must convert via
    // geo::computeBodyAxesFromQuat() (gimbal-safe, no Euler decomposition).
    float quat[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float vel_local_mps[3] = {0.0f, 0.0f, 0.0f};
    // Increment 4: body-frame roll/pitch/yaw rate (JSBSim p,q,r), rad/s -
    // fed straight to VehicleState::vPQR with no rotation.
    float ang_vel_body_rps[3] = {0.0f, 0.0f, 0.0f};
    uint8_t status_flags = 0;
    // Increment 5 (docs/increment-5-specification.md, "Wire protocol
    // changes" point 3): relocated from StateSnapshot's top level. Highest
    // client_seq the server has applied for THIS aircraft's owning
    // connection - meaningful only on the entry matching the reading
    // client's own assigned_player_id; every other entry's value is
    // ignored by everyone but that aircraft's owning client. A single
    // top-level scalar could carry only one client's ack per broadcast
    // packet, which stopped being sufficient once a broadcast serves more
    // than one client.
    uint32_t ack_client_seq = 0;
};

struct StateSnapshot {
    uint32_t server_tick = 0;
    // Increment 5: a tick's full aircraft list may be split across
    // multiple StateSnapshot packets (see kMaxAircraftPerChunk) - every
    // chunk of one tick shares the same server_tick and chunk_count;
    // chunk_index is this packet's 0-based position among them. A client
    // processes each chunk independently as it arrives (spec, "Wire
    // protocol changes" point 2) - these fields are framing, not a
    // reassembly requirement.
    uint8_t chunk_index = 0;
    uint8_t chunk_count = 1;
    std::vector<AircraftState> aircraft;
};

// Increment 5: broadcast reliably when the server detects a client
// disconnect, so remote-entity tracking (interpcore::RemoteEntityTracker)
// can remove that player_id deterministically rather than guessing from a
// gap in snapshots.
struct PlayerLeft {
    uint8_t player_id = 0;
};

using ByteBuffer = std::vector<uint8_t>;

// value clamped to [-1, 1] before scaling; elevator/aileron/rudder.
int16_t encodeAxis(double value);
double decodeAxis(int16_t raw);

// value clamped to [0, 1] before scaling; throttle only.
uint8_t encodeThrottle(double value);
double decodeThrottle(uint8_t raw);

// Each serialize function returns a complete wire packet: the 1-byte tag
// followed by the message's fields in the order tabulated in the spec.
ByteBuffer serializeClientHello(const ClientHello& msg);
ByteBuffer serializeServerWelcome(const ServerWelcome& msg);
ByteBuffer serializeServerReject(const ServerReject& msg);
ByteBuffer serializeControlInput(const ControlInput& msg);
ByteBuffer serializeStateSnapshot(const StateSnapshot& msg);
ByteBuffer serializeClientBye();
ByteBuffer serializePlayerLeft(const PlayerLeft& msg);

// Each deserialize function verifies the leading tag byte and that the
// buffer is at least as long as the fixed-size fields require, then
// fills `out`. Returns false (and leaves `out` unspecified) on a tag
// mismatch or short buffer - both treated as a malformed/foreign packet,
// never as a crash.
bool deserializeClientHello(const uint8_t* data, size_t len, ClientHello& out);
bool deserializeServerWelcome(const uint8_t* data, size_t len,
                               ServerWelcome& out);
bool deserializeServerReject(const uint8_t* data, size_t len,
                              ServerReject& out);
bool deserializeControlInput(const uint8_t* data, size_t len,
                              ControlInput& out);
bool deserializeStateSnapshot(const uint8_t* data, size_t len,
                               StateSnapshot& out);
bool deserializeClientBye(const uint8_t* data, size_t len);
bool deserializePlayerLeft(const uint8_t* data, size_t len, PlayerLeft& out);

// Reads just the leading tag byte, for dispatch before picking which
// deserialize function to call. False if `len` is 0.
bool peekMessageTag(const uint8_t* data, size_t len, MessageTag& tagOut);

}  // namespace net
