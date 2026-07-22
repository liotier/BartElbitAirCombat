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

// Thin ENet client host wrapper, the counterpart to net_server.h. Shared
// unchanged by flight_test_client and the Godot NetworkClient node (spec,
// "Architecture") - both link netcore, so both drive the identical
// connect/send/receive mechanics and therefore the identical wire
// protocol. The caller must call enet_initialize() once before
// constructing a NetClient and enet_deinitialize() once after the last
// one is destroyed.
#pragma once

#include "protocol.h"

#include <enet/enet.h>

#include <cstdint>
#include <functional>
#include <string>

namespace net {

class NetClient {
public:
    NetClient() = default;
    ~NetClient();

    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    // Creates the local (unbound) host and begins connecting to
    // host:port with 2 channels. Connection completion arrives later as
    // an ENET_EVENT_TYPE_CONNECT from poll(). Returns false and fills
    // `error` only on a local failure (host creation or address
    // resolution) - a refused/unreachable remote is reported by poll()
    // never observing a CONNECT event.
    bool connect(const std::string& host, uint16_t port, std::string& error);
    void stop();

    using EventHandler = std::function<void(const ENetEvent&)>;
    // Services the host for up to timeoutMs milliseconds, invoking
    // onEvent for every event observed. RECEIVE packets are destroyed
    // automatically right after onEvent returns.
    void poll(uint32_t timeoutMs, const EventHandler& onEvent);

    void send(uint8_t channel, const ByteBuffer& payload, bool reliable);
    void disconnect();
    void flush();

    bool isConnected() const { return connected_; }

private:
    ENetHost* host_ = nullptr;
    ENetPeer* peer_ = nullptr;
    bool connected_ = false;
};

}  // namespace net
