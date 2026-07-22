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

#include "net_client.h"

namespace net {

NetClient::~NetClient() { stop(); }

bool NetClient::connect(const std::string& host, uint16_t port,
                         std::string& error) {
    host_ = enet_host_create(nullptr, /*peerCount=*/1, /*channelLimit=*/2, 0,
                              0);
    if (!host_) {
        error = "enet_host_create (client) failed";
        return false;
    }
    ENetAddress addr;
    if (enet_address_set_host(&addr, host.c_str()) != 0) {
        error = "enet_address_set_host failed for '" + host + "'";
        enet_host_destroy(host_);
        host_ = nullptr;
        return false;
    }
    addr.port = port;
    peer_ = enet_host_connect(host_, &addr, 2, 0);
    if (!peer_) {
        error = "enet_host_connect failed";
        enet_host_destroy(host_);
        host_ = nullptr;
        return false;
    }
    return true;
}

void NetClient::stop() {
    connected_ = false;
    peer_ = nullptr;
    if (host_) {
        enet_host_destroy(host_);
        host_ = nullptr;
    }
}

void NetClient::poll(uint32_t timeoutMs, const EventHandler& onEvent) {
    if (!host_) return;
    ENetEvent event;
    while (enet_host_service(host_, &event, timeoutMs) > 0) {
        if (event.type == ENET_EVENT_TYPE_CONNECT) {
            connected_ = true;
            peer_ = event.peer;
        } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            connected_ = false;
            peer_ = nullptr;
        }
        onEvent(event);
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(event.packet);
        }
        timeoutMs = 0;
    }
}

void NetClient::send(uint8_t channel, const ByteBuffer& payload,
                      bool reliable) {
    if (!peer_) return;
    ENetPacket* packet =
        enet_packet_create(payload.data(), payload.size(),
                            reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    enet_peer_send(peer_, channel, packet);
}

void NetClient::disconnect() {
    if (peer_) enet_peer_disconnect(peer_, 0);
}

void NetClient::flush() {
    if (host_) enet_host_flush(host_);
}

}  // namespace net
