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

# increment 1's reusable core (FlightSession, CsvLogger), shared between
# increment1_tests and the Godot GDExtension below (increment 2 spec,
# "flightcore: sharing increment 1's code"). Deliberately has no dependency
# on Godot or godot-cpp.
add_library(flightcore STATIC
    src/test_runner.cpp
    src/logging/csv_logger.cpp
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
)
target_link_libraries(flight_gdext PRIVATE flightcore godot::cpp)
target_compile_options(flight_gdext PRIVATE -Wall -Wextra)
set_target_properties(flight_gdext PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/godot/bin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/godot/bin"
)
