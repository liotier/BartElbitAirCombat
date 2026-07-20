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
project(BartElbitAirCombat_Increment1 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Open question (spec, "Open questions for the implementer"): build
# configuration. Chosen: Release uniformly for JSBSim and this project's own
# code, since there is no debugging need for a derisking increment and the
# CI time budget favours it.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# JSBSim's optional components default ON and are not needed here; left
# alone they pull Python/Cython/Doxygen into the configure step (spec
# review finding F11).
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

add_executable(increment1_tests
    src/main.cpp
    src/test_runner.cpp
    src/logging/csv_logger.cpp
    src/scenarios/trim_stability.cpp
    src/scenarios/pitch_response.cpp
    src/scenarios/roll_response.cpp
    src/scenarios/power_response.cpp
)

target_include_directories(increment1_tests PRIVATE src)
target_link_libraries(increment1_tests PRIVATE libJSBSim)

# The JSBSim source tree fetched above is referenced in place at runtime
# (spec, "Aircraft configuration") rather than copied into the build tree.
target_compile_definitions(increment1_tests PRIVATE
    JSBSIM_ROOT="${jsbsim_SOURCE_DIR}")

# Applies to this project's own sources only - never to the libJSBSim
# target, which is known to emit at least one warning under v1.3.1 (spec
# review finding F11).
target_compile_options(increment1_tests PRIVATE -Wall -Wextra)
