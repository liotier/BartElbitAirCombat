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

#include "protocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace net {

namespace {

// Appends fields little-endian, one byte at a time - deliberately not a
// struct memcpy (spec review m4), so wire layout is independent of host
// endianness (all current targets are x86-64, i.e. already little-
// endian, but the explicit form removes the assumption rather than
// relying on it) and of compiler struct padding.
class ByteWriter {
public:
    void putU8(uint8_t v) { buf_.push_back(v); }
    void putU16(uint16_t v) {
        buf_.push_back(static_cast<uint8_t>(v & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    }
    void putU32(uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            buf_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
        }
    }
    void putI16(int16_t v) { putU16(static_cast<uint16_t>(v)); }
    void putF32(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        putU32(bits);
    }
    ByteBuffer take() { return std::move(buf_); }

private:
    ByteBuffer buf_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t len) : data_(data), len_(len) {}

    bool getU8(uint8_t& out) {
        if (pos_ + 1 > len_) return false;
        out = data_[pos_];
        pos_ += 1;
        return true;
    }
    bool getU16(uint16_t& out) {
        if (pos_ + 2 > len_) return false;
        out = static_cast<uint16_t>(data_[pos_]) |
              (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
        pos_ += 2;
        return true;
    }
    bool getU32(uint32_t& out) {
        if (pos_ + 4 > len_) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            out |= static_cast<uint32_t>(data_[pos_ + i]) << (8 * i);
        }
        pos_ += 4;
        return true;
    }
    bool getI16(int16_t& out) {
        uint16_t raw;
        if (!getU16(raw)) return false;
        out = static_cast<int16_t>(raw);
        return true;
    }
    bool getF32(float& out) {
        uint32_t bits;
        if (!getU32(bits)) return false;
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }

private:
    const uint8_t* data_;
    size_t len_;
    size_t pos_ = 0;
};

bool checkTag(ByteReader& r, MessageTag expected) {
    uint8_t tag;
    if (!r.getU8(tag)) return false;
    return tag == static_cast<uint8_t>(expected);
}

}  // namespace

int16_t encodeAxis(double value) {
    double clamped = value < -1.0 ? -1.0 : (value > 1.0 ? 1.0 : value);
    return static_cast<int16_t>(std::lround(clamped * kControlAxisScale));
}

double decodeAxis(int16_t raw) {
    return static_cast<double>(raw) / kControlAxisScale;
}

uint8_t encodeThrottle(double value) {
    double clamped = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
    return static_cast<uint8_t>(std::lround(clamped * kThrottleScale));
}

double decodeThrottle(uint8_t raw) {
    return static_cast<double>(raw) / kThrottleScale;
}

ByteBuffer serializeClientHello(const ClientHello& msg) {
    ByteWriter w;
    w.putU8(static_cast<uint8_t>(MessageTag::kClientHello));
    w.putU8(msg.protocol_version);
    w.putU8(msg.client_flags);
    return w.take();
}

bool deserializeClientHello(const uint8_t* data, size_t len,
                             ClientHello& out) {
    ByteReader r(data, len);
    if (!checkTag(r, MessageTag::kClientHello)) return false;
    return r.getU8(out.protocol_version) && r.getU8(out.client_flags);
}

ByteBuffer serializeServerWelcome(const ServerWelcome& msg) {
    ByteWriter w;
    w.putU8(static_cast<uint8_t>(MessageTag::kServerWelcome));
    w.putU8(msg.protocol_version);
    w.putU8(msg.assigned_player_id);
    w.putF32(msg.origin_lat_deg);
    w.putF32(msg.origin_lon_deg);
    w.putU16(msg.snapshot_hz);
    w.putU8(msg.aircraft_id);
    return w.take();
}

bool deserializeServerWelcome(const uint8_t* data, size_t len,
                               ServerWelcome& out) {
    ByteReader r(data, len);
    if (!checkTag(r, MessageTag::kServerWelcome)) return false;
    return r.getU8(out.protocol_version) && r.getU8(out.assigned_player_id) &&
           r.getF32(out.origin_lat_deg) && r.getF32(out.origin_lon_deg) &&
           r.getU16(out.snapshot_hz) && r.getU8(out.aircraft_id);
}

ByteBuffer serializeServerReject(const ServerReject& msg) {
    ByteWriter w;
    w.putU8(static_cast<uint8_t>(MessageTag::kServerReject));
    w.putU8(msg.reason_code);
    return w.take();
}

bool deserializeServerReject(const uint8_t* data, size_t len,
                              ServerReject& out) {
    ByteReader r(data, len);
    if (!checkTag(r, MessageTag::kServerReject)) return false;
    return r.getU8(out.reason_code);
}

ByteBuffer serializeControlInput(const ControlInput& msg) {
    ByteWriter w;
    w.putU8(static_cast<uint8_t>(MessageTag::kControlInput));
    w.putU32(msg.newest_client_seq);
    uint8_t count = static_cast<uint8_t>(
        std::min<size_t>(msg.commands.size(), kMaxRedundantCommands));
    w.putU8(count);
    for (uint8_t i = 0; i < count; ++i) {
        const ControlCommand& c = msg.commands[i];
        w.putI16(c.elevator);
        w.putI16(c.aileron);
        w.putI16(c.rudder);
        w.putU8(c.throttle);
    }
    return w.take();
}

bool deserializeControlInput(const uint8_t* data, size_t len,
                              ControlInput& out) {
    ByteReader r(data, len);
    if (!checkTag(r, MessageTag::kControlInput)) return false;
    uint8_t count;
    if (!r.getU32(out.newest_client_seq) || !r.getU8(count)) return false;
    out.commands.clear();
    out.commands.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        ControlCommand c;
        if (!r.getI16(c.elevator) || !r.getI16(c.aileron) ||
            !r.getI16(c.rudder) || !r.getU8(c.throttle)) {
            return false;
        }
        out.commands.push_back(c);
    }
    return true;
}

ByteBuffer serializeStateSnapshot(const StateSnapshot& msg) {
    ByteWriter w;
    w.putU8(static_cast<uint8_t>(MessageTag::kStateSnapshot));
    w.putU32(msg.server_tick);
    w.putU8(msg.chunk_index);
    w.putU8(msg.chunk_count);
    w.putU8(static_cast<uint8_t>(msg.aircraft.size()));
    for (const AircraftState& a : msg.aircraft) {
        w.putU8(a.player_id);
        for (float v : a.pos_local_m) w.putF32(v);
        for (float v : a.quat) w.putF32(v);
        for (float v : a.vel_local_mps) w.putF32(v);
        for (float v : a.ang_vel_body_rps) w.putF32(v);
        w.putU8(a.status_flags);
        w.putU32(a.ack_client_seq);
    }
    return w.take();
}

bool deserializeStateSnapshot(const uint8_t* data, size_t len,
                               StateSnapshot& out) {
    ByteReader r(data, len);
    if (!checkTag(r, MessageTag::kStateSnapshot)) return false;
    uint8_t count;
    if (!r.getU32(out.server_tick) || !r.getU8(out.chunk_index) ||
        !r.getU8(out.chunk_count) || !r.getU8(count)) {
        return false;
    }
    out.aircraft.clear();
    out.aircraft.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        AircraftState a;
        if (!r.getU8(a.player_id)) return false;
        for (float& v : a.pos_local_m) {
            if (!r.getF32(v)) return false;
        }
        for (float& v : a.quat) {
            if (!r.getF32(v)) return false;
        }
        for (float& v : a.vel_local_mps) {
            if (!r.getF32(v)) return false;
        }
        for (float& v : a.ang_vel_body_rps) {
            if (!r.getF32(v)) return false;
        }
        if (!r.getU8(a.status_flags) || !r.getU32(a.ack_client_seq)) {
            return false;
        }
        out.aircraft.push_back(a);
    }
    return true;
}

ByteBuffer serializeClientBye() {
    ByteWriter w;
    w.putU8(static_cast<uint8_t>(MessageTag::kClientBye));
    return w.take();
}

bool deserializeClientBye(const uint8_t* data, size_t len) {
    ByteReader r(data, len);
    return checkTag(r, MessageTag::kClientBye);
}

ByteBuffer serializePlayerLeft(const PlayerLeft& msg) {
    ByteWriter w;
    w.putU8(static_cast<uint8_t>(MessageTag::kPlayerLeft));
    w.putU8(msg.player_id);
    return w.take();
}

bool deserializePlayerLeft(const uint8_t* data, size_t len, PlayerLeft& out) {
    ByteReader r(data, len);
    if (!checkTag(r, MessageTag::kPlayerLeft)) return false;
    return r.getU8(out.player_id);
}

bool peekMessageTag(const uint8_t* data, size_t len, MessageTag& tagOut) {
    if (len < 1) return false;
    tagOut = static_cast<MessageTag>(data[0]);
    return true;
}

}  // namespace net
