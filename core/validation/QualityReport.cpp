#include "core/validation/QualityReport.h"

namespace mapgen {

Json QualityReport::toJson() const {
    Json j = Json::object();
    j["stage"] = stage;
    j["stageName"] = stageNameRu;
    j["confidence"] = confidence;
    j["seconds"] = seconds;
    j["anomalies"] = static_cast<double>(anomalies);
    Json cs = Json::array();
    for (auto& c : checks) {
        Json cj = Json::object();
        cj["id"] = c.id;
        cj["name"] = c.nameRu;
        cj["passed"] = c.passed;
        cj["p0"] = c.p0;
        cj["value"] = c.value;
        cj["expected"] = c.expectedRu;
        if (!c.detailRu.empty()) cj["detail"] = c.detailRu;
        cs.push(cj);
    }
    j["checks"] = cs;
    Json ws = Json::array();
    for (auto& w : warningsRu) ws.push(w);
    j["warnings"] = ws;
    Json ls = Json::array();
    for (auto& l : logRu) ls.push(l);
    j["log"] = ls;
    Json ms = Json::object();
    for (auto& m : metrics) ms[m.first] = m.second;
    j["metrics"] = ms;
    return j;
}

QualityReport QualityReport::fromJson(const Json& j) {
    QualityReport r;
    r.stage = j["stage"].asInt();
    r.stageNameRu = j["stageName"].asString(std::string());
    r.confidence = j["confidence"].asNumber(1.0);
    r.seconds = j["seconds"].asNumber();
    r.anomalies = static_cast<long long>(j["anomalies"].asNumber());
    for (auto& c : j["checks"].items()) {
        QualityCheck q;
        q.id = c["id"].asString(std::string());
        q.nameRu = c["name"].asString(std::string());
        q.passed = c["passed"].asBool(true);
        q.p0 = c["p0"].asBool(false);
        q.value = c["value"].asNumber();
        q.expectedRu = c["expected"].asString(std::string());
        q.detailRu = c["detail"].asString(std::string());
        r.checks.push_back(q);
    }
    for (auto& w : j["warnings"].items()) r.warningsRu.push_back(w.asString(std::string()));
    for (auto& l : j["log"].items()) r.logRu.push_back(l.asString(std::string()));
    for (auto& m : j["metrics"].members()) r.metrics.emplace_back(m.first, m.second.asNumber());
    return r;
}

}  // namespace mapgen
