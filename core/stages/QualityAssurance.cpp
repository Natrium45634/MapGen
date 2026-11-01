// Stage 8: render & QA — global statistics, cross-stage consistency checks, provenance / anomaly map.
//
// Continents: connected land masses ≥ 5·10⁶ km² scaled by (R / 6371 km)² (Australia, ~7.7·10⁶ km², is a
// continent; Greenland, ~2.2·10⁶ km², is an island). All areas are true spherical cell areas.

#include <algorithm>
#include <cmath>

#include "core/stages/Stage.h"
#include "core/stages/StageUtil.h"
#include "core/world/Categories.h"

namespace mapgen {

void runQualityAssurance(StageContext& ctx) {
    WorldData& w = ctx.world;
    const SphereGrid& g = w.grid;
    const int64_t N = g.size();
    const uint8_t* ocean = w.u8(LayerId::OceanMask);
    const uint8_t* biome = w.u8(LayerId::Biome);
    const uint8_t* kg = w.u8(LayerId::Koppen);
    const uint8_t* iceT = w.u8(LayerId::IceType);
    const int32_t* lake = w.i32(LayerId::LakeId);
    const float* z = w.f32(LayerId::Elevation);
    const double sea = w.seaLevel();
    Json st = Json::object();

    double land = 0, total = g.totalAreaKm2(), desert = 0, glacier = 0, sumZ = 0;
    std::vector<float> landZ;
    std::vector<uint8_t> landMask(static_cast<size_t>(N));
    for (int64_t k = 0; k < N; ++k) {
        landMask[static_cast<size_t>(k)] = !ocean[k];
        if (ocean[k]) continue;
        double a = g.cellAreaKm2(g.row(k));
        land += a;
        sumZ += a * (z[k] - sea);
        if (kg[k] == kKgBWh || kg[k] == kKgBWk) desert += a;
        if (iceT[k] == kIceSheet || iceT[k] == kIceGlacier) glacier += a;
    }
    std::vector<int32_t> lab;
    int nc = connectedComponents(g, landMask.data(), lab);
    std::vector<double> carea(static_cast<size_t>(nc), 0.0);
    for (int64_t k = 0; k < N; ++k) if (lab[static_cast<size_t>(k)] >= 0) carea[static_cast<size_t>(lab[static_cast<size_t>(k)])] += g.cellAreaKm2(g.row(k));
    const double scale = sqr(g.radiusKm() / 6371.0);
    int continents = 0, islands = 0;
    for (double a : carea) {
        if (a >= 5.0e6 * scale) ++continents;
        else if (a >= 1000.0 * scale) ++islands;
    }
    std::sort(carea.rbegin(), carea.rend());
    st["landPercent"] = 100.0 * land / total;
    st["continents"] = continents;
    st["continentRuleKm2"] = 5.0e6 * scale;
    st["islandsOver1000Km2"] = islands;
    Json masses = Json::array();
    for (size_t i = 0; i < std::min<size_t>(8, carea.size()); ++i) masses.push(carea[i]);
    st["largestLandmassesKm2"] = masses;
    st["meanLandElevationM"] = land > 0 ? sumZ / land : 0;
    st["desertPercentOfLand"] = land > 0 ? 100.0 * desert / land : 0;
    st["glacierPercentOfLand"] = land > 0 ? 100.0 * glacier / land : 0;
    st["volcanoes"] = w.geomorph ? static_cast<int>(w.geomorph->volcanoes.size()) : 0;
    st["volcanoesPerMillionKm2"] = w.geomorph ? w.geomorph->volcanoes.size() / (total / 1e6) : 0;
    // Hypsometry (area-weighted percentiles of surface height relative to sea level).
    {
        std::vector<std::pair<float, double>> v;
        v.reserve(static_cast<size_t>(N));
        for (int64_t k = 0; k < N; ++k) v.emplace_back(static_cast<float>(w.surfaceElevation(k) - sea), g.cellAreaKm2(g.row(k)));
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        Json hyp = Json::object();
        double acc = 0;
        size_t idx = 0;
        for (int pc : {1, 5, 10, 25, 50, 75, 90, 95, 99}) {
            double target = pc / 100.0 * total;
            while (idx < v.size() && acc + v[idx].second < target) acc += v[idx++].second;
            hyp[std::to_string(pc)] = idx < v.size() ? v[idx].first : v.back().first;
        }
        st["hypsometryM"] = hyp;
    }
    // Biome areas (true areas, i.e. latitude-corrected).
    {
        std::vector<double> ba(kBiomeCount, 0.0);
        for (int64_t k = 0; k < N; ++k) if (!ocean[k]) ba[biome[k]] += g.cellAreaKm2(g.row(k));
        Json b = Json::object();
        for (auto& c : categoriesOf(LayerId::Biome))
            if (c.code > 0 && c.code < kBiomeCount && ba[static_cast<size_t>(c.code)] > 0) b[c.nameRu] = 100.0 * ba[static_cast<size_t>(c.code)] / land;
        st["biomePercentOfLand"] = b;
    }
    if (w.hydro) {
        std::vector<std::pair<double, int>> bs;
        for (auto& b : w.hydro->basins) bs.push_back({b.areaKm2, b.id});
        std::sort(bs.rbegin(), bs.rend());
        Json lb = Json::array();
        for (size_t i = 0; i < std::min<size_t>(5, bs.size()); ++i) {
            Json o = Json::object();
            auto& b = w.hydro->basins[static_cast<size_t>(bs[i].second)];
            o["id"] = b.id;
            o["areaKm2"] = b.areaKm2;
            o["dischargeM3s"] = b.dischargeM3s;
            o["endorheic"] = b.outlet != kBoOcean;
            lb.push(o);
        }
        st["largestBasins"] = lb;
        st["lakes"] = static_cast<int>(w.hydro->lakes.size());
        st["riverSegments"] = static_cast<int>(w.hydro->rivers.size());
    }
    // ---- cross-stage consistency
    int64_t landNoBiome = 0, oceanLandBiome = 0, lakeMismatch = 0;
    for (int64_t k = 0; k < N; ++k) {
        if (ocean[k] && biome[k] != kBiomeOcean) ++oceanLandBiome;
        if (!ocean[k] && biome[k] == kBiomeNone) ++landNoBiome;
        if (lake[k] >= 0 && ocean[k]) ++lakeMismatch;
    }
    ctx.report.check("qa.biomeCoverage", "Каждая ячейка суши имеет биом; в океане нет наземного биома", landNoBiome == 0 && oceanLandBiome == 0,
                     static_cast<double>(landNoBiome + oceanLandBiome), "0", true);
    ctx.report.check("qa.lakesOnLand", "Озёра только на суше", lakeMismatch == 0, static_cast<double>(lakeMismatch), "0", true);
    int failedP0 = 0;
    for (int s = 0; s < 8; ++s)
        for (auto& c : w.reports[static_cast<size_t>(s)].checks) if (c.p0 && !c.passed) ++failedP0;
    ctx.report.check("qa.allP0", "Все P0-инварианты предыдущих этапов выполнены", failedP0 == 0, failedP0, "0", true);

    // ---- provenance / anomaly map
    auto Lp = w.makeLayer(LayerId::Provenance);
    uint16_t* prov = Lp->as<uint16_t>();
    int64_t anomalies = 0;
    std::vector<uint8_t> terminalLake(w.hydro ? w.hydro->lakes.size() : 0, 0);
    if (w.hydro) for (auto& l : w.hydro->lakes) terminalLake[static_cast<size_t>(l.id)] = l.regime != kLrOutflow;
    const uint8_t* gen = w.u8(LayerId::Genesis);
    for (int64_t k = 0; k < N; ++k) {
        uint16_t f = kProvClimateInterpolated;
        if (lake[k] >= 0 && terminalLake[static_cast<size_t>(lake[k])]) f |= kProvLakeLevelApprox;
        // Anomaly: relief needle not explained by a volcanic edifice.
        int64_t nb[8];
        g.neighbors8(k, nb);
        double mx = -1e18;
        for (int d = 0; d < 8; ++d) mx = std::max(mx, static_cast<double>(z[nb[d]]));
        if (z[k] > mx + 2000 && gen[k] != kGenHotspot && gen[k] != kGenVolcanicArc && gen[k] != kGenIslandArc) { f |= kProvAnomaly; ++anomalies; }
        prov[k] = f;
    }
    w.setLayer(LayerId::Provenance, Lp);
    ctx.report.anomalies = anomalies;
    for (auto& m : st.members())
        if (m.second.isNumber()) ctx.report.metric(m.first, m.second.asNumber());
    w.statistics = st;
    ctx.report.log("Материк — связный массив суши ≥ " + formatNumber(5.0e6 * scale) + " км² (правило по площади, масштабируется радиусом).");
    ctx.progress(1.0, "контроль качества");
}

}  // namespace mapgen
