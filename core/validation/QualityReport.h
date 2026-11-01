#pragma once

// Per-stage quality report: numeric invariants, deviations from statistical
// expectations, warnings, surrogate confidence and anomaly count.

#include <string>
#include <utility>
#include <vector>

#include "core/util/Json.h"

namespace mapgen {

struct QualityCheck {
    std::string id;
    std::string nameRu;
    bool passed = true;
    bool p0 = false;          // critical (P0) invariant
    double value = 0;
    std::string expectedRu;   // human description of the expected range
    std::string detailRu;
};

struct QualityReport {
    int stage = 0;
    std::string stageNameRu;
    std::vector<QualityCheck> checks;
    std::vector<std::string> warningsRu;
    std::vector<std::string> logRu;
    std::vector<std::pair<std::string, double>> metrics;
    double confidence = 1.0;  // surrogate 0..1, documented per stage
    double seconds = 0;
    long long anomalies = 0;

    void check(std::string id, std::string nameRu, bool passed, double value, std::string expectedRu,
               bool p0 = false, std::string detailRu = {}) {
        checks.push_back({std::move(id), std::move(nameRu), passed, p0, value, std::move(expectedRu), std::move(detailRu)});
        if (!passed) confidence = std::max(0.0, confidence - (p0 ? 0.5 : 0.1));
    }
    void metric(std::string name, double v) { metrics.emplace_back(std::move(name), v); }
    void warn(std::string w) { warningsRu.push_back(std::move(w)); }
    void log(std::string l) { logRu.push_back(std::move(l)); }

    bool passedP0() const {
        for (auto& c : checks)
            if (c.p0 && !c.passed) return false;
        return true;
    }
    bool passedAll() const {
        for (auto& c : checks)
            if (!c.passed) return false;
        return true;
    }
    double metricValue(const std::string& name, double def = 0) const {
        for (auto& m : metrics)
            if (m.first == name) return m.second;
        return def;
    }

    Json toJson() const;
    static QualityReport fromJson(const Json& j);
};

}  // namespace mapgen
