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

// Userspace UDP relay proxy for latency/loss testing (docs/increment-3-
// specification.md, "Network impairment"). Deliberately protocol-
// agnostic: forwards raw datagrams between a client and flight_server
// without parsing them, applying a configurable one-way delay and drop
// probability to the actual byte stream ENet sees. This is the
// mechanism the spec's resilience acceptance criteria are evaluated
// against, chosen over an above-ENet application queue specifically
// because a queue above ENet would never exercise ENet's own
// reliability/retransmission logic under real impairment (spec review
// M1) - and over tc/netem, which is unavailable in the sandbox (no
// NET_ADMIN) and, unlike this, would not be reproducible/seeded for CI.
//
// Single-threaded: one poll() loop services both the client-facing and
// server-facing sockets, since a fixed per-packet delay keeps the
// pending-delivery queue naturally FIFO-ordered (no jitter/reordering
// support - not needed until increment 4).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <vector>

namespace {

struct Config {
    uint16_t listenPort = 45301;
    std::string serverHost = "127.0.0.1";
    uint16_t serverPort = 45300;
    int delayMs = 0;
    double dropPercent = 0.0;
    unsigned seed = 12345;  // fixed default: reproducible CI runs (spec, "Network impairment")
};

Config parseArgs(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto nextVal = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
        };
        if (arg == "--listen-port") {
            cfg.listenPort = static_cast<uint16_t>(std::atoi(nextVal().c_str()));
        } else if (arg == "--server-host") {
            cfg.serverHost = nextVal();
        } else if (arg == "--server-port") {
            cfg.serverPort = static_cast<uint16_t>(std::atoi(nextVal().c_str()));
        } else if (arg == "--delay-ms") {
            cfg.delayMs = std::atoi(nextVal().c_str());
        } else if (arg == "--drop-percent") {
            cfg.dropPercent = std::atof(nextVal().c_str());
        } else if (arg == "--seed") {
            cfg.seed = static_cast<unsigned>(std::atol(nextVal().c_str()));
        }
    }
    return cfg;
}

int makeUdpSocket(uint16_t bindPort) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(bindPort);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

enum class Direction { kToServer, kToClient };

struct DelayedPacket {
    std::chrono::steady_clock::time_point dueAt;
    Direction direction;
    std::vector<uint8_t> data;
};

}  // namespace

int main(int argc, char** argv) {
    Config cfg = parseArgs(argc, argv);

    int clientSock = makeUdpSocket(cfg.listenPort);
    if (clientSock < 0) {
        std::fprintf(stderr, "error: could not bind listen port %u\n", cfg.listenPort);
        return 2;
    }
    int serverSock = makeUdpSocket(0);
    if (serverSock < 0) {
        std::fprintf(stderr, "error: could not create server-facing socket\n");
        close(clientSock);
        return 2;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(cfg.serverPort);
    if (inet_pton(AF_INET, cfg.serverHost.c_str(), &serverAddr.sin_addr) != 1) {
        std::fprintf(stderr, "error: invalid --server-host '%s'\n", cfg.serverHost.c_str());
        close(clientSock);
        close(serverSock);
        return 2;
    }

    sockaddr_in clientAddr{};
    bool haveClientAddr = false;

    std::mt19937 rng(cfg.seed);
    std::uniform_real_distribution<double> dist(0.0, 100.0);
    auto shouldDrop = [&]() {
        return cfg.dropPercent > 0.0 && dist(rng) < cfg.dropPercent;
    };

    std::deque<DelayedPacket> queue;
    std::vector<uint8_t> buf(2048);

    std::printf(
        "net_relay: listening on 0.0.0.0:%u, forwarding to %s:%u "
        "(delay=%dms drop=%.1f%% seed=%u)\n",
        cfg.listenPort, cfg.serverHost.c_str(), cfg.serverPort, cfg.delayMs,
        cfg.dropPercent, cfg.seed);
    std::fflush(stdout);

    while (true) {
        pollfd fds[2];
        fds[0].fd = clientSock;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = serverSock;
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        int waitMs = 5;  // fine enough granularity for millisecond-scale delay
        if (!queue.empty()) {
            auto now = std::chrono::steady_clock::now();
            auto untilNext = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  queue.front().dueAt - now)
                                  .count();
            waitMs = static_cast<int>(
                std::max<int64_t>(0, std::min<int64_t>(untilNext, waitMs)));
        }
        poll(fds, 2, waitMs);

        if (fds[0].revents & POLLIN) {
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            ssize_t len = recvfrom(clientSock, buf.data(), buf.size(), 0,
                                    reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (len > 0) {
                clientAddr = from;
                haveClientAddr = true;
                if (!shouldDrop()) {
                    DelayedPacket pkt;
                    pkt.dueAt = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(cfg.delayMs);
                    pkt.direction = Direction::kToServer;
                    pkt.data.assign(buf.begin(), buf.begin() + len);
                    queue.push_back(std::move(pkt));
                }
            }
        }
        if (fds[1].revents & POLLIN) {
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            ssize_t len = recvfrom(serverSock, buf.data(), buf.size(), 0,
                                    reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (len > 0 && !shouldDrop()) {
                DelayedPacket pkt;
                pkt.dueAt = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(cfg.delayMs);
                pkt.direction = Direction::kToClient;
                pkt.data.assign(buf.begin(), buf.begin() + len);
                queue.push_back(std::move(pkt));
            }
        }

        auto now = std::chrono::steady_clock::now();
        while (!queue.empty() && queue.front().dueAt <= now) {
            const DelayedPacket& pkt = queue.front();
            if (pkt.direction == Direction::kToServer) {
                sendto(serverSock, pkt.data.data(), pkt.data.size(), 0,
                       reinterpret_cast<const sockaddr*>(&serverAddr),
                       sizeof(serverAddr));
            } else if (haveClientAddr) {
                sendto(clientSock, pkt.data.data(), pkt.data.size(), 0,
                       reinterpret_cast<const sockaddr*>(&clientAddr),
                       sizeof(clientAddr));
            }
            queue.pop_front();
        }
    }
}
