# Copyright (C) 2026 The BartElbitAirCombat Project
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.20)
project(BartElbitAirCombat CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Increment 5: flight_server's first use of std::thread/std::async (the
# worker pool and async client onboarding, docs/increment-5-specification.md
# "Architecture"). A standard CMake pattern; no new fetched dependency.
find_package(Threads REQUIRED)

# Open question (increment 1 spec, "Open questions for the implementer"):
# build configuration. Chosen: Release uniformly, since there is no
# debugging need for a derisking increment and the CI time budget favours
# it.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# godot-cpp and JSBSim are both linked into the SHARED flight_gdext library
# below (increment 2 spec, "Position-independent code"), so their static
# libraries - and everything else in this build - must be PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# JSBSim's optional components default ON and are not needed here; left
# alone they pull Python/Cython/Doxygen into the configure step (increment
# 1 spec review finding F11).
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_PYTHON_MODULE OFF CACHE BOOL "" FORCE)
set(BUILD_JULIA_PACKAGE OFF CACHE BOOL "" FORCE)
set(BUILD_MATLAB_SFUNCTION OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    jsbsim
    GIT_REPOSITORY https://github.com/JSBSim-Team/jsbsim.git
    GIT_TAG        v1.3.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(jsbsim)

# increment 2 spec, "GODOTCPP_TARGET=editor is deliberate": the editor
# binary is the only Godot target this project's tests exercise, for both
# interactive play and headless CI.
set(GODOTCPP_TARGET "editor" CACHE STRING "" FORCE)

FetchContent_Declare(
    godot-cpp
    GIT_REPOSITORY https://github.com/godotengine/godot-cpp.git
    GIT_TAG        godot-4.5-stable
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(godot-cpp)

# increment 3 spec, "ENet acquisition": pinned to the newest stable tag,
# validated (pre-drafting) to configure and build with no option or
# target conflict against JSBSim or godot-cpp.
FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.18
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(enet)

# increment 1's reusable core (FlightSession, CsvLogger), shared between
# increment1_tests and the Godot GDExtension below (increment 2 spec,
# "flightcore: sharing increment 1's code"). Deliberately has no dependency
# on Godot or godot-cpp.
add_library(flightcore STATIC
    src/test_runner.cpp
    src/logging/csv_logger.cpp
    src/geo/aircraft_orientation.cpp
)
target_include_directories(flightcore PUBLIC src)
target_link_libraries(flightcore PUBLIC libJSBSim)

# The JSBSim source tree fetched above is referenced in place at runtime
# (increment 1 spec, "Aircraft configuration") rather than copied into the
# build tree.
target_compile_definitions(flightcore PUBLIC
    JSBSIM_ROOT="${jsbsim_SOURCE_DIR}")

add_executable(increment1_tests
    src/main.cpp
    src/scenarios/trim_stability.cpp
    src/scenarios/pitch_response.cpp
    src/scenarios/roll_response.cpp
    src/scenarios/power_response.cpp
)
target_link_libraries(increment1_tests PRIVATE flightcore)

# Applies to this project's own sources only - never to libJSBSim (known to
# emit at least one warning under v1.3.1, increment 1 spec review finding
# F11) or godot-cpp (its own, separately-maintained warning policy).
target_compile_options(increment1_tests PRIVATE -Wall -Wextra)
target_compile_options(flightcore PRIVATE -Wall -Wextra)

# The Godot GDExtension: FlightAircraft, wrapping FlightSession for use
# inside Godot's real-time frame loop (increment 2 spec, "FlightAircraft
# node"). Output goes straight into the Godot project's own bin/ so the
# .gdextension file in godot/bin/ can reference it with a stable relative
# path.
add_library(flight_gdext SHARED
    src/godot_ext/register_types.cpp
    src/godot_ext/flight_aircraft.cpp
    src/godot_ext/remote_aircraft.cpp
    src/godot_ext/predicted_aircraft.cpp
)
target_link_libraries(flight_gdext PRIVATE flightcore netcore predictcore interpcore godot::cpp)
target_compile_options(flight_gdext PRIVATE -Wall -Wextra)
set_target_properties(flight_gdext PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/godot/bin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/godot/bin"
)

# increment 3: netcore, a Godot-free static library depending only on
# ENet (spec, "Architecture"). Defines the wire message types and their
# serialization (protocol.h/.cpp) plus thin ENet host wrappers
# (net_server, net_client), shared unmodified by flight_server,
# flight_test_client, and flight_gdext's PredictedAircraft node so all
# three compile the identical serialization code and therefore speak
# byte-identical wire format.
add_library(netcore STATIC
    src/netcore/protocol.cpp
    src/netcore/net_server.cpp
    src/netcore/net_client.cpp
    src/netcore/snapshot_log.cpp
)
target_include_directories(netcore PUBLIC src ${enet_SOURCE_DIR}/include)
target_link_libraries(netcore PUBLIC enet)
target_compile_options(netcore PRIVATE -Wall -Wextra)

# increment 4: predictcore, a Godot-free static library depending on
# flightcore (FlightSession, geo::) and netcore (wire types, encodeAxis/
# decodeAxis) (spec, "Architecture"). Defines PredictedSession, the
# reusable predict/buffer/reconcile/replay algorithm shared unmodified by
# the Godot client (PredictedAircraft) and flight_test_client's --mode
# prediction - the same reasoning increment 3 used for netcore itself.
add_library(predictcore STATIC
    src/predictcore/reconstruction.cpp
    src/predictcore/reconciliation_math.cpp
    src/predictcore/predicted_session.cpp
)
target_include_directories(predictcore PUBLIC src)
target_link_libraries(predictcore PUBLIC flightcore netcore)
target_compile_options(predictcore PRIVATE -Wall -Wextra)

# The increment-4 reconstruction-gate regression test (spec, acceptance
# criterion 1) - Godot-free, links predictcore + flightcore only.
add_executable(predictcore_tests
    src/predictcore/gate_regression_test.cpp
)
target_link_libraries(predictcore_tests PRIVATE predictcore flightcore)
target_compile_options(predictcore_tests PRIVATE -Wall -Wextra)

# increment 5: interpcore, a Godot-free static library depending only on
# netcore (spec, "Remote-entity interpolation") - unlike predictcore, there
# is no local FlightSession to simulate or reconcile, only already-received
# wire data to buffer and interpolate between, so this needs neither
# flightcore nor geo::. Defines RemoteEntityTracker, shared unmodified by
# the Godot client (PredictedAircraft, which also owns the local player's
# own prediction) and flight_test_client's --mode multiclient.
add_library(interpcore STATIC
    src/interpcore/remote_entity_tracker.cpp
)
target_include_directories(interpcore PUBLIC src)
target_link_libraries(interpcore PUBLIC netcore)
target_compile_options(interpcore PRIVATE -Wall -Wextra)

# The interpcore unit tests (spec, "Test plan" items 1-4) - synthetic data,
# no FlightSession/JSBSim, no network. Godot-free; links interpcore +
# netcore only.
add_executable(interpcore_tests
    src/interpcore/interp_tests.cpp
)
target_link_libraries(interpcore_tests PRIVATE interpcore netcore)
target_compile_options(interpcore_tests PRIVATE -Wall -Wextra)

# The standalone authoritative server (spec, "Server (flight_server)").
# No Godot dependency - links flightcore (FlightSession), netcore, and
# Threads (the worker pool + async onboarding, increment 5's first use of
# std::thread in this project).
add_executable(flight_server
    src/server/main.cpp
)
target_link_libraries(flight_server PRIVATE flightcore netcore Threads::Threads)
target_compile_options(flight_server PRIVATE -Wall -Wextra)

# The automated network test harness (spec, "flight_test_client"):
# transport-fidelity, input-path, and protocol tests. Increment 4 adds
# --mode prediction, which runs a real PredictedSession (predictcore) and
# therefore needs flightcore too - increments 1-3's modes remain
# Godot-free/physics-free (netcore only, evaluating physics server-side).
# Increment 5 adds --mode multiclient, which also needs interpcore.
add_executable(flight_test_client
    src/net_test/main.cpp
)
target_link_libraries(flight_test_client PRIVATE netcore predictcore interpcore flightcore Threads::Threads)
target_compile_options(flight_test_client PRIVATE -Wall -Wextra)

# Userspace UDP relay proxy for latency/loss testing (spec, "Network
# impairment"). Deliberately protocol-agnostic - forwards raw datagrams
# without parsing them, so it impairs the actual byte stream ENet sees
# rather than an application-level abstraction of it; no ENet or netcore
# dependency needed.
add_executable(net_relay
    src/net_test/net_relay.cpp
)
target_compile_options(net_relay PRIVATE -Wall -Wextra)
