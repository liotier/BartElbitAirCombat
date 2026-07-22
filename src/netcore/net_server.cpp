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

#include "net_server.h"

namespace net {

NetServer::~NetServer() { stop(); }

bool NetServer::start(uint16_t port, size_t maxClients, std::string& error) {
    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    host_ = enet_host_create(&addr, maxClients, /*channelLimit=*/2, 0, 0);
    if (!host_) {
        error = "enet_host_create failed (port " + std::to_string(port) + ")";
        return false;
    }
    return true;
}

void NetServer::stop() {
    if (host_) {
        enet_host_destroy(host_);
        host_ = nullptr;
    }
}

void NetServer::poll(uint32_t timeoutMs, const EventHandler& onEvent) {
    if (!host_) return;
    ENetEvent event;
    while (enet_host_service(host_, &event, timeoutMs) > 0) {
        onEvent(event);
        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            enet_packet_destroy(event.packet);
        }
        // Only wait the full timeout on the first service() call per
        // poll(); subsequent drains of already-queued events should not
        // re-block.
        timeoutMs = 0;
    }
}

void NetServer::send(ENetPeer* peer, uint8_t channel,
                      const ByteBuffer& payload, bool reliable) {
    if (!peer) return;
    ENetPacket* packet =
        enet_packet_create(payload.data(), payload.size(),
                            reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    enet_peer_send(peer, channel, packet);
}

void NetServer::broadcast(uint8_t channel, const ByteBuffer& payload,
                           bool reliable) {
    if (!host_) return;
    ENetPacket* packet =
        enet_packet_create(payload.data(), payload.size(),
                            reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    enet_host_broadcast(host_, channel, packet);
}

void NetServer::disconnect(ENetPeer* peer) {
    if (peer) enet_peer_disconnect(peer, 0);
}

void NetServer::flush() {
    if (host_) enet_host_flush(host_);
}

}  // namespace net
