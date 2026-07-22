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
constexpr uint8_t kProtocolVersion = 1;

// ENet channel assignment (spec, "Messages" table's Channel column).
constexpr uint8_t kChannelReliable = 0;
constexpr uint8_t kChannelUnreliable = 1;

// Fixed-point scale for control axes (spec Appendix A): wire value =
// round(physical_value * kControlAxisScale).
constexpr double kControlAxisScale = 32767.0;

enum class MessageTag : uint8_t {
    kClientHello = 1,
    kServerWelcome = 2,
    kServerReject = 3,
    kControlInput = 4,
    kStateSnapshot = 5,
    kClientBye = 6,
};

enum class RejectReason : uint8_t {
    kVersionMismatch = 1,
    kServerFull = 2,
};

struct ClientHello {
    uint8_t protocol_version = kProtocolVersion;
};

struct ServerWelcome {
    uint8_t protocol_version = kProtocolVersion;
    uint8_t assigned_player_id = 0;
    float origin_lat_deg = 0.0f;
    float origin_lon_deg = 0.0f;
    uint16_t snapshot_hz = 0;
};

struct ServerReject {
    uint8_t reason_code = 0;
};

struct ControlInput {
    uint32_t client_seq = 0;
    int16_t elevator = 0;
    int16_t aileron = 0;
    int16_t rudder = 0;
    int16_t throttle = 0;
};

// One aircraft's rigid-body state within a StateSnapshot (spec, "Messages"
// table's StateSnapshot row). status_flags is reserved for future
// increments (e.g. crashed/on-ground); always 0 in increment 3.
struct AircraftState {
    uint8_t player_id = 0;
    float pos_local_m[3] = {0.0f, 0.0f, 0.0f};  // East, Up, -North
    float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};   // x, y, z, w
    float vel_local_mps[3] = {0.0f, 0.0f, 0.0f};
    uint8_t status_flags = 0;
};

struct StateSnapshot {
    uint32_t server_tick = 0;
    std::vector<AircraftState> aircraft;
};

using ByteBuffer = std::vector<uint8_t>;

// value clamped to [-1, 1] before scaling; used for both control axes
// ([-1,1]) and throttle ([0,1], a subset of the same clamp range).
int16_t encodeAxis(double value);
double decodeAxis(int16_t raw);

// Each serialize function returns a complete wire packet: the 1-byte tag
// followed by the message's fields in the order tabulated in the spec.
ByteBuffer serializeClientHello(const ClientHello& msg);
ByteBuffer serializeServerWelcome(const ServerWelcome& msg);
ByteBuffer serializeServerReject(const ServerReject& msg);
ByteBuffer serializeControlInput(const ControlInput& msg);
ByteBuffer serializeStateSnapshot(const StateSnapshot& msg);
ByteBuffer serializeClientBye();

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

// Reads just the leading tag byte, for dispatch before picking which
// deserialize function to call. False if `len` is 0.
bool peekMessageTag(const uint8_t* data, size_t len, MessageTag& tagOut);

}  // namespace net
