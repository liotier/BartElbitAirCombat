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

// Thin ENet server host wrapper (spec, "Architecture": netcore's
// server-side half). Owns host lifetime and the send/receive mechanics;
// deliberately does not know about player IDs, sessions, or which
// message tags mean what - that is flight_server's job, using
// protocol.h. The caller must call enet_initialize() once before
// constructing a NetServer and enet_deinitialize() once after the last
// one is destroyed (matches the pre-drafting validation probes,
// Appendix B).
#pragma once

#include "protocol.h"

#include <enet/enet.h>

#include <cstdint>
#include <functional>
#include <string>

namespace net {

class NetServer {
public:
    NetServer() = default;
    ~NetServer();

    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    // Binds to 0.0.0.0:port with room for maxClients peers and 2
    // channels (kChannelReliable, kChannelUnreliable). Returns false and
    // fills `error` on failure.
    bool start(uint16_t port, size_t maxClients, std::string& error);
    void stop();

    using EventHandler = std::function<void(const ENetEvent&)>;
    // Services the host for up to timeoutMs milliseconds, invoking
    // onEvent for every CONNECT/RECEIVE/DISCONNECT event observed.
    // RECEIVE packets are destroyed automatically right after onEvent
    // returns - callers must copy any data they need out of
    // event.packet during the callback, not retain the pointer.
    void poll(uint32_t timeoutMs, const EventHandler& onEvent);

    void send(ENetPeer* peer, uint8_t channel, const ByteBuffer& payload,
              bool reliable);
    void broadcast(uint8_t channel, const ByteBuffer& payload, bool reliable);
    void disconnect(ENetPeer* peer);
    void flush();

private:
    ENetHost* host_ = nullptr;
};

}  // namespace net
