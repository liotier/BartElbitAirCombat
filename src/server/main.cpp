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

// The standalone authoritative server (docs/increment-5-specification.md,
// "Architecture"). No Godot dependency: links flightcore (the
// FlightSession JSBSim wrapper) and netcore (the wire protocol). Steps
// every active aircraft in lockstep across a persistent worker pool, once
// per wall-clock-paced 120 Hz tick, and broadcasts the result as one or
// more chunked StateSnapshots. Supports multiple concurrently connected
// clients (networked mode) or a single scripted-input aircraft
// (--scenario, unchanged in spirit since increment 3 - this mode never
// touches the worker pool's multi-client bookkeeping at all).
//
// Exit codes match increments 1-2's convention: 0 clean, 1 a criterion
// failed (scripted mode only), 2 an init/bind/trim execution error.
#include "aircraft_catalog.h"
#include "geo/aircraft_orientation.h"
#include "netcore/net_server.h"
#include "netcore/protocol.h"
#include "netcore/snapshot_log.h"
#include "test_runner.h"

#include "FGFDMExec.h"
#include "math/FGLocation.h"
#include "math/FGQuaternion.h"
#include "models/FGPropagate.h"

#include <enet/enet.h>

#include <sys/prctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

struct Config {
    uint16_t port = 45300;
    int snapshotHz = 30;
    std::string scenario;  // empty => networked mode
    std::string logName;   // empty => derive from mode/scenario
    // Increment 6 (docs/increment-6-specification.md, "Server-authoritative
    // type selection"): fixed for the lifetime of one server process, and
    // shared by every aircraft it manages (real clients, --stress-aircraft
    // synthetic aircraft) - a whole-session setting, not per-client.
    std::string aircraft = "c172x";
    // Increment 7 (docs/increment-7-specification.md, "Server-side
    // capacity, spawn, and CPU-leveling"): replaces increment 5's
    // --max-clients. maxBots fill the airspace when otherwise empty;
    // maxPlayers is the additive threshold of humans admitted on top of
    // bots before bots start yielding one-for-one - true human capacity is
    // maxBots+maxPlayers, not maxPlayers alone. Defaults (0 bots, 8
    // players) degenerate to increment 5's old --max-clients behaviour.
    int maxBots = 0;
    int maxPlayers = 8;
    // Increment 5 test-only flag: synthetic, unpiloted, trimmed-and-
    // flying aircraft purely to inflate the aircraft count for chunking-
    // boundary testing (spec, "Test plan" item 7) - never counted against
    // maxBots/maxPlayers, never touched by any client's input.
    int stressAircraft = 0;
};

Config parseArgs(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto nextVal = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
        };
        if (arg == "--port") {
            cfg.port = static_cast<uint16_t>(std::atoi(nextVal().c_str()));
        } else if (arg == "--snapshot-hz") {
            cfg.snapshotHz = std::atoi(nextVal().c_str());
        } else if (arg == "--scenario") {
            cfg.scenario = nextVal();
        } else if (arg == "--log-name") {
            cfg.logName = nextVal();
        } else if (arg == "--max-players") {
            cfg.maxPlayers = std::atoi(nextVal().c_str());
        } else if (arg == "--max-bots") {
            cfg.maxBots = std::atoi(nextVal().c_str());
        } else if (arg == "--stress-aircraft") {
            cfg.stressAircraft = std::atoi(nextVal().c_str());
        } else if (arg == "--aircraft") {
            cfg.aircraft = nextVal();
        }
    }
    return cfg;
}

// Hand-rolled spin-yield barrier (std::barrier is C++20; this project is
// C++17). Sized for T workers + main (T+1 parties total), called via
// arriveAndWait() exactly once per tick by EVERY party - the bulk-
// synchronous-parallel "superstep" pattern verified in
// docs/increment-5-specification.md Appendix B (probe_dynamic_pool.cpp,
// review finding B1). The discipline that makes this safe with no other
// synchronization: main mutates shared state (the aircraft roster, each
// client's command buffer via ENet polling) only between its own
// consecutive arriveAndWait() calls, complete before the next call;
// workers read that state only after their own arriveAndWait() returns,
// recomputing their chunk boundary fresh from the current roster size
// every tick.
class SpinBarrier {
public:
    explicit SpinBarrier(int count) : count_(count), waiting_(count) {}
    void arriveAndWait() {
        int gen = generation_.load(std::memory_order_relaxed);
        if (waiting_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            waiting_.store(count_, std::memory_order_relaxed);
            generation_.fetch_add(1, std::memory_order_release);
        } else {
            while (generation_.load(std::memory_order_acquire) == gen) {
                std::this_thread::yield();
            }
        }
    }

private:
    int count_;
    std::atomic<int> waiting_;
    std::atomic<int> generation_{0};
};

// Per-connection ordered command buffer (increment 4, "Wire protocol
// changes" / review finding B2), now one instance per connected client
// instead of one server-global instance. pendingCommands holds received-
// but-not-yet-applied commands keyed by client_seq; nextExpectedSeq is the
// next one the server needs; highestSeqSeen is the highest
// newest_client_seq any packet from this client has ever reported, used
// to detect a seq that can never arrive anymore.
struct CommandBuffer {
    std::map<uint32_t, net::ControlCommand> pending;
    uint32_t nextExpectedSeq = 1;
    uint32_t highestSeqSeen = 0;
    net::ControlCommand lastApplied{};
    bool hasInput = false;
};

enum class AircraftKind { kScripted, kClient, kStress };

// One active, currently-stepping aircraft. Owned exclusively by the main
// thread's roster (`aircraft` in main()) and mutated only between barrier
// calls (see SpinBarrier's comment) - workers read/step their assigned
// slice each tick but never resize or reorder the roster itself.
//
// Increment 7 (docs/increment-7-specification.md, "One unified bot
// interface"): a bot is just a kClient whose ClientHello declared it one -
// zero new AircraftKind, zero new branching in the command-buffer/
// worker-pool stepping code below, matching "no server-side special-
// casing beyond the fork/track bookkeeping" (acceptance criterion 2).
// isBot/botPid/beingDisplaced are meaningful only when kind == kClient
// && isBot; a *local* bot's botPid is the forked child process this
// server itself owns and must signal/reap (spec, "Spawn/despawn
// mechanism") - a future remote bot would leave botPid at -1.
struct Aircraft {
    AircraftKind kind;
    uint8_t playerId = 0;
    std::unique_ptr<inc1::FlightSession> session;
    ENetPeer* peer = nullptr;  // kClient only
    CommandBuffer cmdBuf;      // kClient only
    bool isBot = false;
    pid_t botPid = -1;
    // Set the moment this bot is chosen to be displaced (SIGTERM sent) so
    // it is never chosen again while its disconnect is still in flight
    // (docs/increment-7-specification.md, "Displacement sequencing").
    bool beingDisplaced = false;
};

// A client whose FlightSession is being constructed off the tick-stepping
// path (docs/increment-5-specification.md, "Client onboarding" - the
// ~12 ms initialize()+trim() cost measured in Appendix B is more than one
// full tick budget). `cancelled` is set if the peer disconnects while
// still in flight; the future is still always consumed (never abandoned)
// since a std::async future's destructor blocks until the task completes.
struct PendingOnboard {
    ENetPeer* peer = nullptr;
    uint8_t playerId = 0;
    bool isBot = false;
    bool cancelled = false;
    // Paired from pendingBotPids at ClientHello-observation time (isBot
    // only) - see startOnboarding's own comment for why this must not wait
    // until the onboarding future below resolves.
    pid_t botPid = -1;
    std::future<std::unique_ptr<inc1::FlightSession>> future;
};

// Shifts `session`'s position by a local (east_m, north_m) offset from
// (originLatDeg, originLonDeg), leaving velocity/attitude/body-rates
// untouched (a freshly-trimmed session's are already correct in isolation
// - only the location needs to move). A strict subset of predictcore's
// reconstructAndApply() recipe (docs/increment-4-specification.md,
// "Reconstruction gate"): same FGLocation-copy-then-SetPositionGeodetic()-
// then-SetLocation() core, without touching qAttitudeECI/vUVW/vPQR, which
// this use case never needs to change.
void offsetSessionPosition(inc1::FlightSession& session, double eastM,
                            double northM, double originLatDeg,
                            double originLonDeg) {
    using JSBSim::FGLocation;
    geo::GeodeticPos pos =
        geo::invertLocalOffset(eastM, northM, originLatDeg, originLonDeg);
    auto P = session.fdm().GetPropagate();
    double altFt = P->GetVState().vLocation.GetGeodAltitude();
    FGLocation loc = P->GetVState().vLocation;  // copies the ellipsoid setup
    loc.SetPositionGeodetic(pos.lon_deg * (M_PI / 180.0),
                             pos.lat_deg * (M_PI / 180.0), altFt);
    P->SetLocation(loc);
}

// Runs entirely on an onboarding thread (std::async), off the main/
// tick-stepping path: initialize()+trim() at the shared IC every increment
// has used, then a small per-player_id east offset so simultaneously-
// spawning aircraft don't land exactly coincident (docs/increment-5-
// specification.md, "Shared world origin and spawn placement" - a
// deliberately simple placement rule, not real spawn-point design).
// Returns nullptr on failure (init/trim non-convergence - not observed in
// this project's history for this fixed IC, but handled rather than
// assumed away).
std::unique_ptr<inc1::FlightSession> onboardNewAircraft(
    uint8_t playerId, double originLat, double originLon,
    const aircraft::CatalogEntry& entry) {
    auto s = std::make_unique<inc1::FlightSession>();
    std::string error;
    if (!s->initialize(error, entry.load_model)) {
        std::fprintf(stderr, "error: onboarding init failed: %s\n",
                     error.c_str());
        return nullptr;
    }
    s->setInitialCondition(entry.canonical_alt_ft, entry.canonical_vc_kts, 0.0,
                            0.0, 0.0, 0.0);
    if (!s->trim(error)) {
        std::fprintf(stderr, "error: onboarding trim failed: %s\n",
                     error.c_str());
        return nullptr;
    }
    double eastOffsetM = 50.0 * static_cast<double>(playerId - 1);
    offsetSessionPosition(*s, eastOffsetM, 0.0, originLat, originLon);
    return s;
}

// Increment 7 (docs/increment-7-specification.md, "Spawn/despawn
// mechanism"): flight_bot is a sibling binary in the same build output
// directory. Resolved via /proc/self/exe (Linux-specific, matching this
// project's own "Linux-first" vision, docs/roadmap.md) rather than
// argv[0], which may be relative to a cwd this process no longer has by
// the time a bot needs spawning.
std::string resolveBotBinaryPath() {
    std::error_code ec;
    std::filesystem::path exePath =
        std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return "flight_bot";  // best-effort fallback: rely on PATH/cwd
    return (exePath.parent_path() / "flight_bot").string();
}

// Blocking wait with a bounded SIGKILL fallback (mirrors scripts/
// server.sh's own SIGTERM-then-SIGKILL-after-timeout idiom, applied at
// the process level): used only at server shutdown, so one hung bot
// child can never block the server from actually exiting.
void waitForExitOrKill(pid_t pid) {
    for (int i = 0; i < 50; ++i) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

}  // namespace

int main(int argc, char** argv) {
    Config config = parseArgs(argc, argv);
    bool scripted = !config.scenario.empty();

    const aircraft::CatalogEntry* catalogEntry =
        aircraft::findByToken(config.aircraft);
    if (!catalogEntry) {
        std::fprintf(stderr,
                      "error: unknown --aircraft '%s' (expected one of "
                      "c172x, camel, pa28)\n",
                      config.aircraft.c_str());
        return 2;
    }
    // docs/increment-6-specification.md, "Aircraft catalog": scripted mode
    // *is* the c172x ground truth (increment 1's regression scenarios) - a
    // non-c172x --aircraft combined with --scenario is rejected outright
    // rather than silently run through c172x-tuned pass thresholds (review
    // finding m1).
    if (scripted && config.aircraft != "c172x") {
        std::fprintf(stderr,
                      "error: --scenario requires --aircraft c172x (or no "
                      "--aircraft flag); got '%s'\n",
                      config.aircraft.c_str());
        return 2;
    }

    // Increment 1's stray-output-file issue (c172x's own <output> block
    // opens a CSV during LoadModel(), before DisableOutput() runs) applies
    // here identically; contained the same way (src/main.cpp): run from
    // within results/.
    std::error_code ec;
    std::filesystem::create_directories("results", ec);
    std::filesystem::current_path("results", ec);
    if (ec) {
        std::fprintf(stderr, "error: could not enter results/ directory: %s\n",
                     ec.message().c_str());
        return 2;
    }

    if (scripted && config.scenario != "pitch_response") {
        std::fprintf(stderr,
                      "error: unsupported --scenario '%s' (only "
                      "pitch_response is wired up)\n",
                      config.scenario.c_str());
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // A one-time reference trim purely to establish the shared world
    // origin (docs/increment-5-specification.md, "Shared world origin and
    // spawn placement"): every aircraft (scripted, client, or stress) is
    // trimmed at the identical IC, which increment 4 already confirmed
    // converges bit-identically across separate invocations, so this
    // reference point is valid for all of them regardless of whether this
    // exact session becomes an active aircraft (scripted mode) or is
    // discarded once its origin is read (networked mode).
    auto originSession = std::make_unique<inc1::FlightSession>();
    std::string error;
    if (!originSession->initialize(error, catalogEntry->load_model)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 2;
    }
    originSession->setInitialCondition(catalogEntry->canonical_alt_ft,
                                        catalogEntry->canonical_vc_kts, 0.0,
                                        0.0, 0.0, 0.0);
    if (!originSession->trim(error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 2;
    }
    inc1::FlightSample originSample = originSession->sample();
    double originLat = originSample.lat_deg;
    double originLon = originSample.lon_deg;

    std::string logName =
        !config.logName.empty() ? config.logName
        : scripted              ? config.scenario
                                 : std::string("networked");
    std::ofstream snapLog("server_" + logName + ".csv");
    if (!snapLog) {
        std::fprintf(stderr, "error: could not open server_%s.csv\n",
                      logName.c_str());
        return 2;
    }
    net::writeSnapshotCsvHeader(snapLog);

    if (enet_initialize() != 0) {
        std::fprintf(stderr, "error: enet_initialize failed\n");
        return 2;
    }
    net::NetServer server;
    // A small margin above the real, application-level maxClients so an
    // over-capacity connection attempt still reaches our own
    // ServerReject(server_full) logic below, rather than being silently
    // refused by ENet itself before any message exchange can occur
    // (increment 3 finding, unchanged reasoning).
    if (!server.start(config.port,
                       static_cast<size_t>(config.maxBots + config.maxPlayers) + 4,
                       error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        enet_deinitialize();
        return 2;
    }

    // The active roster: owned and mutated only by the main thread, only
    // between ticks (see SpinBarrier's comment). A plain std::vector
    // rebuilt as clients join/leave rather than in-place reusable slots -
    // cheap enough at any realistic client count that a slot-reuse scheme
    // is not worth the added complexity (docs/increment-5-specification.md,
    // "Server-side concurrency").
    std::vector<std::unique_ptr<Aircraft>> aircraft;

    // player_id allocation pool (networked mode only): index 0 unused
    // (0 is reserved as "no client" elsewhere in the wire protocol),
    // [1, maxBots+maxPlayers] is the real pool - shared by humans and
    // bots alike (a bot is just a kClient, see Aircraft's own comment).
    // Stress aircraft use a separate range starting at 200 (review
    // finding M2), so they never interact with this pool or the capacity
    // check below.
    std::vector<bool> playerIdInUse(
        static_cast<size_t>(config.maxBots + config.maxPlayers) + 1, false);
    std::vector<PendingOnboard> pendingOnboards;

    if (scripted) {
        auto a = std::make_unique<Aircraft>();
        a->kind = AircraftKind::kScripted;
        a->playerId = 1;
        a->session = std::move(originSession);
        aircraft.push_back(std::move(a));
    } else {
        originSession.reset();  // origin established; not a live aircraft
        uint8_t nextStressId = 200;
        for (int i = 0; i < config.stressAircraft; ++i) {
            auto a = std::make_unique<Aircraft>();
            a->kind = AircraftKind::kStress;
            a->playerId = nextStressId++;
            a->session = onboardNewAircraft(a->playerId, originLat, originLon,
                                             *catalogEntry);
            if (!a->session) {
                std::fprintf(stderr, "error: stress aircraft init/trim failed\n");
                enet_deinitialize();
                return 2;
            }
            aircraft.push_back(std::move(a));
        }
    }

    // Increment 7 bot capacity/CPU-leveling state (docs/increment-7-
    // specification.md, "Server-side capacity, spawn, and CPU-leveling").
    // pendingBotPids: forked, not yet matched to an onboarded Aircraft (the
    // window between fork() and this server observing that bot's own
    // ClientHello) - counted toward the target so a connect burst cannot
    // over-spawn (spec: "counts toward the target the moment it is
    // fork()ed"). waitingHumanPeers: humans admitted past the additive
    // threshold, parked until a bot's *actual* disconnect is processed -
    // never onboarded merely behind the earlier SIGTERM (review finding
    // M1).
    std::deque<pid_t> pendingBotPids;
    std::deque<ENetPeer*> waitingHumanPeers;
    const std::string botBinaryPath = resolveBotBinaryPath();

    auto currentHumanCount = [&]() -> int {
        int count = static_cast<int>(waitingHumanPeers.size());
        for (const auto& po : pendingOnboards) {
            if (!po.isBot) ++count;
        }
        for (const auto& a : aircraft) {
            if (a->kind == AircraftKind::kClient && !a->isBot) ++count;
        }
        return count;
    };

    auto spawnBot = [&]() {
        pid_t pid = fork();
        if (pid < 0) {
            std::fprintf(stderr, "error: fork() for bot spawn failed: %s\n",
                         std::strerror(errno));
            return;
        }
        if (pid == 0) {
            // Child: defence in depth for a crashed parent (spec,
            // "Spawn/despawn mechanism") - the primary crash-safety
            // mechanism is flight_bot's own exit-on-connection-loss; this
            // just makes the common case faster.
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            std::string portStr = std::to_string(config.port);
            execl(botBinaryPath.c_str(), botBinaryPath.c_str(), "--host",
                  "127.0.0.1", "--port", portStr.c_str(), "--aircraft",
                  config.aircraft.c_str(), static_cast<char*>(nullptr));
            std::fprintf(stderr, "error: execl('%s') failed: %s\n",
                         botBinaryPath.c_str(), std::strerror(errno));
            _exit(127);
        }
        pendingBotPids.push_back(pid);
    };

    // Moves the live bot count toward the owner's load-model target (spec:
    // "the airspace fills to B bots when empty... humans beyond P each
    // displace one bot"): desiredBots = clamp(B+P-humanCount, 0, B) covers
    // both the fill-to-B and the displace-past-P regimes with one formula,
    // and naturally refills a bot when a human disconnect raises it back
    // up (test-plan item 4). Called after every admission decision and
    // every disconnect (spec: "driven toward this target on each actual
    // ENet connect/disconnect event").
    auto reconcileBotCount = [&]() {
        int humanCount = currentHumanCount();
        int desiredBots = std::clamp(
            config.maxBots + config.maxPlayers - humanCount, 0, config.maxBots);
        int totalBotCount = static_cast<int>(pendingBotPids.size());
        for (const auto& po : pendingOnboards) {
            if (po.isBot) ++totalBotCount;
        }
        int beingDisplacedCount = 0;
        std::vector<Aircraft*> displaceable;
        for (auto& a : aircraft) {
            if (a->kind == AircraftKind::kClient && a->isBot) {
                ++totalBotCount;
                if (a->beingDisplaced) {
                    ++beingDisplacedCount;
                } else {
                    displaceable.push_back(a.get());
                }
            }
        }
        if (totalBotCount > desiredBots) {
            int needToSignal = (totalBotCount - desiredBots) - beingDisplacedCount;
            for (int i = 0; i < needToSignal &&
                            i < static_cast<int>(displaceable.size());
                 ++i) {
                displaceable[i]->beingDisplaced = true;
                kill(displaceable[i]->botPid, SIGTERM);
            }
        } else if (totalBotCount < desiredBots && pendingBotPids.empty()) {
            // One at a time, not the full shortfall at once: fork() order
            // between two sibling bots is not guaranteed to match the
            // order their connections/ClientHellos actually arrive in
            // (each one independently races through execl, dynamic
            // linking, and its own ENet connect handshake) - two
            // simultaneously in-flight forks would leave pendingBotPids
            // ambiguous about which real PID belongs to which peer.
            // Keeping at most one unclaimed fork outstanding makes
            // startOnboarding's pairing unambiguous by construction; the
            // rest of the shortfall is picked up by this same call being
            // repeated every tick (see the call site after the
            // pendingOnboards flush) once this one is claimed.
            spawnBot();
        }
    };

    // Reaps any exited bot child (despawned, crashed, or a failed spawn) so
    // it never becomes a zombie; called once per tick. Deliberately
    // decoupled from any specific ENet disconnect event - a bot's OS
    // process can exit slightly before or after its ENet DISCONNECT is
    // observed, and this needs no correlation between the two, just "reap
    // whatever has exited."
    auto reapExitedChildren = [&]() {
        int status = 0;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            pendingBotPids.erase(
                std::remove(pendingBotPids.begin(), pendingBotPids.end(), pid),
                pendingBotPids.end());
        }
    };

    auto allocatePlayerId = [&]() -> uint8_t {
        for (uint8_t candidate = 1;
             candidate <= static_cast<uint8_t>(config.maxBots + config.maxPlayers);
             ++candidate) {
            if (!playerIdInUse[candidate]) return candidate;
        }
        return 0;  // exhausted - should not happen given the capacity checks
    };

    auto startOnboarding = [&](ENetPeer* peer, uint8_t pid, bool isBot) {
        playerIdInUse[pid] = true;
        PendingOnboard po;
        po.peer = peer;
        po.playerId = pid;
        po.isBot = isBot;
        // Pair this bot's real forked PID to its peer right now, at
        // ClientHello-observation time, rather than later when the
        // onboarding future below resolves: two sibling bots' async
        // FlightSession-init calls (onboardNewAircraft) can finish in
        // either order regardless of fork order, especially under CPU
        // contention, so matching by future-completion order let a
        // displacement decision (which Aircraft is marked beingDisplaced)
        // and the SIGTERM meant to carry it out land on two different
        // bots - permanently stranding the marked Aircraft, since its
        // real process was never signalled, and starving every human
        // waiting behind it.
        if (isBot && !pendingBotPids.empty()) {
            po.botPid = pendingBotPids.front();
            pendingBotPids.pop_front();
        }
        po.future = std::async(std::launch::async, onboardNewAircraft, pid,
                                originLat, originLon, std::cref(*catalogEntry));
        pendingOnboards.push_back(std::move(po));
    };

    if (!scripted) {
        reconcileBotCount();  // spawns the initial --max-bots fleet
    }

    auto onEvent = [&](const ENetEvent& event) {
        if (scripted) return;  // scripted mode accepts no clients at all

        if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            for (size_t i = 0; i < aircraft.size(); ++i) {
                if (aircraft[i]->kind == AircraftKind::kClient &&
                    aircraft[i]->peer == event.peer) {
                    uint8_t pid = aircraft[i]->playerId;
                    playerIdInUse[pid] = false;
                    net::PlayerLeft left{pid};
                    server.broadcast(net::kChannelReliable,
                                      net::serializePlayerLeft(left), true);
                    bool wasBot = aircraft[i]->isBot;
                    aircraft.erase(aircraft.begin() + i);
                    // Increment 7 (review finding M1): a freed bot slot
                    // goes to the longest-waiting displaced human FIRST -
                    // its onboarding starts only now, never merely behind
                    // the earlier SIGTERM.
                    if (wasBot && !waitingHumanPeers.empty()) {
                        ENetPeer* waitingPeer = waitingHumanPeers.front();
                        waitingHumanPeers.pop_front();
                        startOnboarding(waitingPeer, allocatePlayerId(),
                                        /*isBot=*/false);
                    }
                    // humanCount may also have just dropped (a human
                    // disconnected) - this refills a bot toward B
                    // (test-plan item 4).
                    reconcileBotCount();
                    break;
                }
            }
            for (auto& p : pendingOnboards) {
                if (p.peer == event.peer) p.cancelled = true;
            }
            waitingHumanPeers.erase(
                std::remove(waitingHumanPeers.begin(), waitingHumanPeers.end(),
                            event.peer),
                waitingHumanPeers.end());
            return;
        }
        if (event.type != ENET_EVENT_TYPE_RECEIVE) return;

        net::MessageTag tag;
        if (!net::peekMessageTag(event.packet->data, event.packet->dataLength,
                                  tag)) {
            return;
        }
        switch (tag) {
            case net::MessageTag::kClientHello: {
                net::ClientHello hello;
                if (!net::deserializeClientHello(
                        event.packet->data, event.packet->dataLength, hello)) {
                    return;
                }
                if (hello.protocol_version != net::kProtocolVersion) {
                    net::ServerReject reject{
                        static_cast<uint8_t>(net::RejectReason::kVersionMismatch)};
                    server.send(event.peer, net::kChannelReliable,
                                net::serializeServerReject(reject), true);
                    server.flush();
                    // Deliberately does not disconnect the peer here (increment
                    // 3 finding): an immediate enet_peer_disconnect() right
                    // after queuing a reliable send races that packet's actual
                    // delivery.
                    return;
                }
                bool isBot = (hello.client_flags & net::kClientFlagIsBot) != 0;
                // Increment 7 (docs/increment-7-specification.md, "One
                // unified bot interface"): a bot is only ever this
                // server's own forked child in this increment (remote-bot
                // admission policy is explicitly deferred) -
                // reconcileBotCount() already forks exactly as many bots
                // as the capacity model wants, so a bot's own ClientHello
                // is admitted unconditionally rather than re-checked
                // against the human ceiling below.
                if (!isBot) {
                    int humanCount = currentHumanCount();
                    if (humanCount >= config.maxBots + config.maxPlayers) {
                        // Terminal case (spec, "Server-side capacity...":
                        // B+P humans already accounted for, no bot left
                        // to displace.
                        net::ServerReject reject{static_cast<uint8_t>(
                            net::RejectReason::kServerFull)};
                        server.send(event.peer, net::kChannelReliable,
                                    net::serializeServerReject(reject), true);
                        server.flush();
                        return;
                    }
                    if (humanCount >= config.maxPlayers) {
                        // Beyond the additive threshold: a bot must yield
                        // first (finding M1) - parked, not onboarded,
                        // until that actually happens.
                        waitingHumanPeers.push_back(event.peer);
                        reconcileBotCount();
                        break;
                    }
                }
                startOnboarding(event.peer, allocatePlayerId(), isBot);
                if (!isBot) reconcileBotCount();  // may lower desiredBots
                break;
            }
            case net::MessageTag::kControlInput: {
                net::ControlInput input;
                if (!net::deserializeControlInput(event.packet->data,
                                                   event.packet->dataLength,
                                                   input)) {
                    return;
                }
                for (auto& a : aircraft) {
                    if (a->kind != AircraftKind::kClient ||
                        a->peer != event.peer) {
                        continue;
                    }
                    CommandBuffer& cb = a->cmdBuf;
                    // Redundant multi-command packet (increment 4, "Wire
                    // protocol changes" / review finding B2): merge every
                    // command not already decided into the ordered buffer.
                    for (size_t i = 0; i < input.commands.size() &&
                                       i <= input.newest_client_seq;
                         ++i) {
                        uint32_t seq = input.newest_client_seq -
                                       static_cast<uint32_t>(i);
                        if (seq >= cb.nextExpectedSeq) {
                            cb.pending[seq] = input.commands[i];
                        }
                    }
                    cb.highestSeqSeen =
                        std::max(cb.highestSeqSeen, input.newest_client_seq);
                    cb.hasInput = true;
                    break;
                }
                break;
            }
            case net::MessageTag::kClientBye:
                server.disconnect(event.peer);
                break;
            default:
                break;
        }
    };

    // ---- Worker pool (docs/increment-5-specification.md, "Server-side
    // concurrency" - verified protocol, Appendix B / probe_dynamic_pool.cpp)
    unsigned hwConcurrency = std::thread::hardware_concurrency();
    int numWorkers = std::max(1, hwConcurrency > 1
                                      ? static_cast<int>(hwConcurrency) - 1
                                      : 1);
    SpinBarrier tickBarrier(numWorkers + 1);
    std::atomic<bool> poolStop{false};
    std::vector<std::thread> workers;
    workers.reserve(numWorkers);

    auto applyClientCommand = [](Aircraft& a) {
        CommandBuffer& cb = a.cmdBuf;
        if (!cb.hasInput) return;  // leave trim values alone (increment 3)
        auto it = cb.pending.find(cb.nextExpectedSeq);
        if (it != cb.pending.end()) {
            cb.lastApplied = it->second;
            ++cb.nextExpectedSeq;
        } else if (cb.highestSeqSeen >=
                   cb.nextExpectedSeq + net::kMaxRedundantCommands) {
            ++cb.nextExpectedSeq;
        }
        cb.pending.erase(cb.pending.begin(),
                          cb.pending.lower_bound(cb.nextExpectedSeq));
        a.session->setProperty("fcs/elevator-cmd-norm",
                                net::decodeAxis(cb.lastApplied.elevator));
        a.session->setProperty("fcs/aileron-cmd-norm",
                                net::decodeAxis(cb.lastApplied.aileron));
        a.session->setProperty("fcs/rudder-cmd-norm",
                                net::decodeAxis(cb.lastApplied.rudder));
        a.session->setProperty("fcs/throttle-cmd-norm",
                                net::decodeThrottle(cb.lastApplied.throttle));
    };

    for (int w = 0; w < numWorkers; ++w) {
        workers.emplace_back([&, w]() {
            while (true) {
                tickBarrier.arriveAndWait();  // wait for main's "go"
                if (poolStop.load(std::memory_order_relaxed)) return;
                int n = static_cast<int>(aircraft.size());
                int chunk = (n + numWorkers - 1) / numWorkers;
                int lo = std::min(n, w * chunk);
                int hi = std::min(n, lo + chunk);
                for (int i = lo; i < hi; ++i) {
                    Aircraft& a = *aircraft[i];
                    if (a.kind == AircraftKind::kClient) {
                        applyClientCommand(a);
                    } else if (a.kind == AircraftKind::kScripted) {
                        // Reproduces src/scenarios/pitch_response.cpp's
                        // exact schedule so the server-side trajectory
                        // matches increment 1's validated one.
                        double t = a.session->property("simulation/sim-time-sec");
                        if (t >= 5.0) {
                            a.session->setProperty("fcs/elevator-cmd-norm", -1.0);
                        }
                    }
                    // kStress: no input change - frozen at whatever trim
                    // left the controls (unpiloted, per the spec).
                    a.session->step();
                }
                tickBarrier.arriveAndWait();  // signal done
            }
        });
    }

    std::vector<inc1::FlightSample> samples;
    const long scriptedTicks =
        scripted ? std::lround(30.0 / inc1::kDt) : -1;
    if (scripted) samples.push_back(aircraft[0]->session->sample());

    const int snapshotInterval =
        std::max(1, static_cast<int>(std::lround(120.0 / config.snapshotHz)));
    uint32_t tick = 0;

    auto stepOneTick = [&]() {
        // Increment 7: reap any bot child that exited (despawned,
        // crashed, or a failed spawn) before it can become a zombie -
        // decoupled from any specific ENet event, see reapExitedChildren's
        // own comment.
        reapExitedChildren();

        // Main's between-tick work: drain any completed onboarding
        // (splicing the new aircraft into the roster and welcoming its
        // peer), then release the workers for this tick. All of this
        // happens strictly before tickBarrier.arriveAndWait() below, so
        // it is safely visible to workers once they return from theirs
        // (docs/increment-5-specification.md, "Server-side concurrency").
        for (size_t i = 0; i < pendingOnboards.size();) {
            PendingOnboard& po = pendingOnboards[i];
            if (po.future.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
                std::unique_ptr<inc1::FlightSession> session = po.future.get();
                if (!po.cancelled && session) {
                    auto a = std::make_unique<Aircraft>();
                    a->kind = AircraftKind::kClient;
                    a->playerId = po.playerId;
                    a->session = std::move(session);
                    a->peer = po.peer;
                    a->isBot = po.isBot;
                    a->botPid = po.botPid;
                    aircraft.push_back(std::move(a));
                    net::ServerWelcome welcome{
                        net::kProtocolVersion, po.playerId,
                        static_cast<float>(originLat),
                        static_cast<float>(originLon),
                        static_cast<uint16_t>(config.snapshotHz),
                        catalogEntry->aircraft_id};
                    server.send(po.peer, net::kChannelReliable,
                                net::serializeServerWelcome(welcome), true);
                } else {
                    playerIdInUse[po.playerId] = false;
                }
                pendingOnboards.erase(pendingOnboards.begin() + i);
            } else {
                ++i;
            }
        }
        // A prior reconcileBotCount() call can find a spawn/displacement
        // need it cannot fully act on yet (e.g. the bots it would displace
        // are still mid-onboard, not yet in `aircraft`) - nothing else
        // retries that need once circumstances change, since reconcile is
        // otherwise only driven by admission/disconnect events. Re-running
        // it once per tick, right after onboarding completions are
        // spliced in above, guarantees it eventually converges instead of
        // leaving a human stranded in waitingHumanPeers indefinitely.
        if (!scripted) reconcileBotCount();

        tickBarrier.arriveAndWait();  // release workers for this tick
        tickBarrier.arriveAndWait();  // wait for them to finish

        ++tick;
        if (scripted) samples.push_back(aircraft[0]->session->sample());

        if (tick % static_cast<uint32_t>(snapshotInterval) == 0 &&
            !aircraft.empty()) {
            std::vector<net::AircraftState> allStates;
            allStates.reserve(aircraft.size());
            for (auto& a : aircraft) {
                inc1::FlightSample sample = a->session->sample();
                net::AircraftState as;
                as.player_id = a->playerId;
                geo::LocalOffset off = geo::computeLocalOffset(
                    sample.lat_deg, sample.lon_deg, originLat, originLon);
                as.pos_local_m[0] = static_cast<float>(off.east_m);
                as.pos_local_m[1] = static_cast<float>(sample.alt_m);
                as.pos_local_m[2] = static_cast<float>(-off.north_m);
                JSBSim::FGQuaternion qLocal =
                    a->session->getVState().qAttitudeLocal;
                as.quat[0] = static_cast<float>(qLocal(1));
                as.quat[1] = static_cast<float>(qLocal(2));
                as.quat[2] = static_cast<float>(qLocal(3));
                as.quat[3] = static_cast<float>(qLocal(4));
                as.vel_local_mps[0] = static_cast<float>(sample.vel_east_mps);
                as.vel_local_mps[1] = static_cast<float>(-sample.vel_down_mps);
                as.vel_local_mps[2] = static_cast<float>(-sample.vel_north_mps);
                as.ang_vel_body_rps[0] = static_cast<float>(
                    a->session->property("velocities/p-rad_sec"));
                as.ang_vel_body_rps[1] = static_cast<float>(
                    a->session->property("velocities/q-rad_sec"));
                as.ang_vel_body_rps[2] = static_cast<float>(
                    a->session->property("velocities/r-rad_sec"));
                // Increment 7 (docs/increment-7-specification.md, "Marking
                // bots as non-human"): set from the peer's own ClientHello
                // self-declaration, uniform for local and (future) remote
                // bots - never from fork-knowledge.
                as.status_flags =
                    (a->kind == AircraftKind::kClient && a->isBot)
                        ? net::kStatusFlagBot
                        : 0;
                // Increment 5 (review finding, "Wire protocol changes"
                // point 3): ack_client_seq is now per-aircraft, meaningful
                // only to this aircraft's own owning client; 0 for
                // scripted/stress aircraft, which own no connection.
                as.ack_client_seq = (a->kind == AircraftKind::kClient)
                                        ? (a->cmdBuf.nextExpectedSeq - 1)
                                        : 0;
                allStates.push_back(as);
            }

            size_t chunkCount =
                (allStates.size() + net::kMaxAircraftPerChunk - 1) /
                net::kMaxAircraftPerChunk;
            for (size_t c = 0; c < chunkCount; ++c) {
                net::StateSnapshot snap;
                snap.server_tick = tick;
                snap.chunk_index = static_cast<uint8_t>(c);
                snap.chunk_count = static_cast<uint8_t>(chunkCount);
                size_t lo = c * net::kMaxAircraftPerChunk;
                size_t hi = std::min(allStates.size(),
                                      lo + net::kMaxAircraftPerChunk);
                snap.aircraft.assign(allStates.begin() + lo,
                                      allStates.begin() + hi);
                net::writeSnapshotCsvRow(snapLog, snap);
                server.broadcast(net::kChannelUnreliable,
                                  net::serializeStateSnapshot(snap), false);
            }
            snapLog.flush();  // a concurrently-running test client reads this file live
            server.flush();
        }
    };

    auto nextTick = std::chrono::steady_clock::now();
    constexpr int kMaxCatchUpTicks = 8;

    bool finished = false;
    while (!g_stop && !finished) {
        auto now = std::chrono::steady_clock::now();
        if (now < nextTick) {
            auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              nextTick - now)
                              .count();
            server.poll(static_cast<uint32_t>(std::max<int64_t>(waitMs, 0)),
                        onEvent);
            continue;
        }
        int caughtUp = 0;
        while (std::chrono::steady_clock::now() >= nextTick &&
               caughtUp < kMaxCatchUpTicks) {
            stepOneTick();
            nextTick += std::chrono::duration_cast<
                std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(inc1::kDt));
            ++caughtUp;
            if (scripted && static_cast<long>(tick) >= scriptedTicks) {
                finished = true;
                break;
            }
        }
        if (caughtUp == kMaxCatchUpTicks) {
            nextTick = std::chrono::steady_clock::now();
        }
        server.poll(0, onEvent);
    }

    poolStop.store(true, std::memory_order_relaxed);
    tickBarrier.arriveAndWait();  // release workers so they observe the stop
    for (auto& w : workers) w.join();

    // Increment 7 (test-plan item 5): a clean server shutdown terminates
    // every bot child with no orphans - both onboarded ones and any still
    // mid-onboard/mid-connect. SIGTERM first for all of them (flight_bot
    // handles it as a clean disconnect), then wait for each to actually
    // exit; a hung child gets SIGKILLed rather than blocking shutdown
    // forever (waitForExitOrKill).
    {
        std::vector<pid_t> allBotPids(pendingBotPids.begin(), pendingBotPids.end());
        for (const auto& po : pendingOnboards) {
            if (po.isBot) allBotPids.push_back(po.botPid);
        }
        for (auto& a : aircraft) {
            if (a->kind == AircraftKind::kClient && a->isBot) {
                allBotPids.push_back(a->botPid);
            }
        }
        for (pid_t pid : allBotPids) kill(pid, SIGTERM);
        for (pid_t pid : allBotPids) waitForExitOrKill(pid);
    }

    int exitCode = 0;
    if (scripted) {
        inc1::TestResult result;
        result.name = "server_" + config.scenario;
        double maxPitch5to10 = -std::numeric_limits<double>::infinity();
        double maxAlpha = -std::numeric_limits<double>::infinity();
        double minIas = std::numeric_limits<double>::infinity();
        for (const inc1::FlightSample& s : samples) {
            if (s.time_s >= 5.0 && s.time_s <= 10.0) {
                maxPitch5to10 = std::max(maxPitch5to10, s.pitch_deg);
            }
            maxAlpha = std::max(maxAlpha, s.alpha_deg);
            minIas = std::min(minIas, s.ias_mps);
        }
        result.criteria.push_back(
            inc1::makeCriterion("max_pitch_5_10", maxPitch5to10, 30.0, ">=", "deg"));
        result.criteria.push_back(
            inc1::makeCriterion("max_alpha", maxAlpha, 12.0, ">=", "deg"));
        result.criteria.push_back(inc1::makeCriterion(
            "min_ias", minIas, 60.0 * inc1::kKt2Mps, "<=", "m/s"));
        result.criteria.push_back(inc1::checkFinite(samples));
        result.status =
            std::all_of(result.criteria.begin(), result.criteria.end(),
                        [](const inc1::Criterion& c) { return c.passed; })
                ? inc1::TestStatus::kPassed
                : inc1::TestStatus::kFailed;

        std::vector<inc1::TestResult> results{result};
        inc1::printSummaryTable(results);
        inc1::writeSummaryJson(results, "server_summary.json");
        exitCode = (result.status == inc1::TestStatus::kPassed) ? 0 : 1;
    }

    server.stop();
    enet_deinitialize();
    return exitCode;
}
