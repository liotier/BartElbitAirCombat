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

#include "test_runner.h"

#include "FGFDMExec.h"
#include "initialization/FGTrim.h"
#include "models/FGPropulsion.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>

#ifndef JSBSIM_ROOT
#error "JSBSIM_ROOT must be defined by the build (see CMakeLists.txt)"
#endif

namespace inc1 {

FlightSession::FlightSession() : fdm_(std::make_unique<JSBSim::FGFDMExec>()) {}

FlightSession::~FlightSession() = default;

bool FlightSession::initialize(std::string& error) {
    fdm_->Setdt(kDt);
    fdm_->SetRootDir(SGPath(JSBSIM_ROOT));
    fdm_->SetAircraftPath(SGPath("aircraft"));
    fdm_->SetEnginePath(SGPath("engine"));
    fdm_->SetSystemsPath(SGPath("systems"));
    if (!fdm_->LoadModel("c172x")) {
        error = "JSBSim LoadModel(\"c172x\") failed";
        return false;
    }
    // The c172x model defines its own CSV/socket output blocks; suppress
    // them so a test run does not write stray files (spec review F7).
    fdm_->DisableOutput();
    return true;
}

void FlightSession::setInitialCondition(double alt_ft, double vc_kts,
                                         double psi_true_deg, double lat_deg,
                                         double lon_deg, double gamma_deg) {
    fdm_->SetPropertyValue("ic/h-sl-ft", alt_ft);
    fdm_->SetPropertyValue("ic/vc-kts", vc_kts);
    fdm_->SetPropertyValue("ic/psi-true-deg", psi_true_deg);
    fdm_->SetPropertyValue("ic/lat-gc-deg", lat_deg);
    fdm_->SetPropertyValue("ic/long-gc-deg", lon_deg);
    fdm_->SetPropertyValue("ic/gamma-deg", gamma_deg);
}

bool FlightSession::trim(std::string& error) {
    fdm_->RunIC();
    // JSBSim loads models with engines off; without this, tests 1-3 would
    // trim a powered glider and test 4's throttle input would do nothing
    // (spec review F6).
    fdm_->GetPropulsion()->InitRunning(-1);
    JSBSim::FGTrim trimmer(fdm_.get(), JSBSim::tFull);
    if (!trimmer.DoTrim()) {
        error = "JSBSim trim (tFull) did not converge";
        return false;
    }
    return true;
}

void FlightSession::setProperty(const std::string& name, double value) {
    fdm_->SetPropertyValue(name, value);
}

double FlightSession::property(const std::string& name) const {
    return fdm_->GetPropertyValue(name);
}

void FlightSession::step() { fdm_->Run(); }

FlightSample FlightSession::sample() const {
    FlightSample s;
    s.time_s = property("simulation/sim-time-sec");
    s.lat_deg = property("position/lat-geod-deg");
    s.lon_deg = property("position/long-gc-deg");
    s.alt_m = property("position/h-sl-ft") * kFt2M;
    s.vel_north_mps = property("velocities/v-north-fps") * kFt2M;
    s.vel_east_mps = property("velocities/v-east-fps") * kFt2M;
    s.vel_down_mps = property("velocities/v-down-fps") * kFt2M;
    s.roll_deg = property("attitude/phi-deg");
    s.pitch_deg = property("attitude/theta-deg");
    s.yaw_deg = property("attitude/psi-deg");
    s.alpha_deg = property("aero/alpha-deg");
    s.beta_deg = property("aero/beta-deg");
    s.ias_mps = property("velocities/vc-kts") * kKt2Mps;
    s.tas_mps = property("velocities/vt-fps") * kFt2M;
    s.elevator_norm = property("fcs/elevator-cmd-norm");
    s.aileron_norm = property("fcs/aileron-cmd-norm");
    s.rudder_norm = property("fcs/rudder-cmd-norm");
    // JSBSim's trim writes its pitch solution here, not to
    // fcs/elevator-cmd-norm, which stays 0 after a trim (spec review F8).
    s.pitch_trim_norm = property("fcs/pitch-trim-cmd-norm");
    s.throttle_norm = property("fcs/throttle-cmd-norm");
    return s;
}

Criterion makeCriterion(std::string name, double actual, double limit,
                         std::string comparison, std::string unit) {
    bool passed = (comparison == "<=") ? (actual <= limit) : (actual >= limit);
    return Criterion{std::move(name), passed, actual, limit,
                      std::move(comparison), std::move(unit)};
}

std::vector<FlightSample> runLoop(
    FlightSession& session, double duration_s,
    const std::function<void(double, FlightSession&)>& controlInput,
    CsvLogger& logger) {
    const long nsteps = std::lround(duration_s / kDt);
    std::vector<FlightSample> samples;
    samples.reserve(static_cast<size_t>(nsteps) + 1);

    FlightSample s0 = session.sample();
    samples.push_back(s0);
    logger.writeRow(s0);

    for (long i = 1; i <= nsteps; ++i) {
        controlInput(session.property("simulation/sim-time-sec"), session);
        session.step();
        FlightSample s = session.sample();
        samples.push_back(s);
        // 10 Hz logging at 120 Hz simulation == every 12th step (spec,
        // "Logging frequency").
        if (i % 12 == 0) {
            logger.writeRow(s);
        }
    }
    return samples;
}

Criterion checkFinite(const std::vector<FlightSample>& samples) {
    for (const auto& s : samples) {
        const double values[] = {
            s.time_s,  s.lat_deg,  s.lon_deg,        s.alt_m,
            s.vel_north_mps, s.vel_east_mps, s.vel_down_mps,
            s.roll_deg, s.pitch_deg, s.yaw_deg,
            s.alpha_deg, s.beta_deg, s.ias_mps, s.tas_mps,
            s.elevator_norm, s.aileron_norm, s.rudder_norm,
            s.pitch_trim_norm, s.throttle_norm};
        for (double v : values) {
            if (!std::isfinite(v)) {
                return makeCriterion("no_nan", 1.0, 0.0, "<=", "bool");
            }
        }
    }
    return makeCriterion("no_nan", 0.0, 0.0, "<=", "bool");
}

namespace {

const char* statusName(TestStatus status) {
    switch (status) {
        case TestStatus::kPassed:
            return "passed";
        case TestStatus::kFailed:
            return "failed";
        case TestStatus::kError:
            return "error";
    }
    return "error";
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

bool allPassed(const std::vector<TestResult>& results) {
    for (const auto& r : results) {
        if (r.status != TestStatus::kPassed) return false;
    }
    return true;
}

}  // namespace

void printSummaryTable(const std::vector<TestResult>& results) {
    std::printf("%-18s %s\n", "TEST", "STATUS");
    std::printf("%-18s %s\n", "----", "------");
    for (const auto& r : results) {
        std::printf("%-18s %s\n", r.name.c_str(), statusName(r.status));
        if (r.status == TestStatus::kError) {
            std::printf("  error: %s\n", r.message.c_str());
            continue;
        }
        for (const auto& c : r.criteria) {
            if (!c.passed) {
                std::printf("  FAIL %s: actual=%.6g %s limit=%.6g %s\n",
                            c.name.c_str(), c.actual, c.comparison.c_str(),
                            c.limit, c.unit.c_str());
            }
        }
    }
    std::printf("\noverall: %s\n", allPassed(results) ? "PASS" : "FAIL");
}

void writeSummaryJson(const std::vector<TestResult>& results,
                      const std::string& path) {
    std::ofstream out(path);
    out << std::fixed << std::setprecision(6);
    out << "{\n";
    out << "  \"increment\": 1,\n";
    out << "  \"jsbsim_tag\": \"v1.3.1\",\n";
    out << "  \"tests\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const TestResult& r = results[i];
        out << "    {\n";
        out << "      \"name\": \"" << jsonEscape(r.name) << "\",\n";
        out << "      \"status\": \"" << statusName(r.status) << "\"";
        if (r.status == TestStatus::kError) {
            out << ",\n      \"message\": \"" << jsonEscape(r.message) << "\"";
        }
        out << ",\n      \"criteria\": [\n";
        for (size_t j = 0; j < r.criteria.size(); ++j) {
            const Criterion& c = r.criteria[j];
            out << "        {\"name\": \"" << jsonEscape(c.name) << "\", "
                << "\"passed\": " << (c.passed ? "true" : "false") << ", "
                << "\"actual\": " << c.actual << ", "
                << "\"limit\": " << c.limit << ", "
                << "\"comparison\": \"" << c.comparison << "\", "
                << "\"unit\": \"" << jsonEscape(c.unit) << "\"}"
                << (j + 1 < r.criteria.size() ? ",\n" : "\n");
        }
        out << "      ]\n";
        out << "    }" << (i + 1 < results.size() ? ",\n" : "\n");
    }
    out << "  ],\n";
    out << "  \"all_passed\": " << (allPassed(results) ? "true" : "false")
        << "\n";
    out << "}\n";
}

}  // namespace inc1
