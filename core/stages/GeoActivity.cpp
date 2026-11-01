// Stage 7: geological activity.
//
// Volcano activity follows genesis and age (hotspot chains: only the young end is active; arcs and
// rifts: active or dormant). The conditional 0–5 "activity potential" zone combines proximity to active
// volcanoes with the tectonic setting (arc on the overriding plate, rifts, ridges, young hotspots);
// transform and collision zones get seismicity but no volcanic arc. Geothermal potential = heat source ×
// groundwater availability; hot springs are placed by potential, geysers are much rarer (need high heat,
// water and siliceous volcanism). The scales are conventional — not eruption frequencies or forecasts.

#include <algorithm>
#include <cmath>

#include "core/stages/Stage.h"
#include "core/stages/StageUtil.h"
#include "core/world/Categories.h"

namespace mapgen {

void runGeoActivity(StageContext& ctx) {
    WorldData& w = ctx.world;
    const SphereGrid& g = w.grid;
    const int W = g.width(), H = g.height();
    const int64_t N = g.size();
    const double sea = w.seaLevel();
    const double R = g.radiusKm();
    const float* z = w.f32(LayerId::Elevation);
    const uint8_t* ocean = w.u8(LayerId::OceanMask);
    const uint8_t* crust = w.u8(LayerId::CrustType);
    const float* dSub = w.f32(LayerId::DistSubduction);
    const float* dTr = w.f32(LayerId::DistTrench);
    const float* dCol = w.f32(LayerId::DistCollision);
    const float* dRid = w.f32(LayerId::DistRidge);
    const float* dTrf = w.f32(LayerId::DistTransform);
    const float* conv = w.f32(LayerId::ConvRate);
    const float* aridity = w.f32(LayerId::Aridity);
    const uint8_t* lith = w.u8(LayerId::Lithology);
    const auto& volcs = w.geomorph->volcanoes;
    uint64_t key = ctx.seed.key64("geo.activity");

    auto ga = std::make_shared<GeoActivityFeatures>();
    std::vector<int64_t> activeCells, dormantCells, youngHotspotCells;
    for (auto& v : volcs) {
        VolcanoState st;
        st.volcanoId = v.id;
        double u = hashUniform(key, static_cast<uint64_t>(v.id));
        switch (v.genesis) {
            case kVgHotspotOceanic:
            case kVgHotspotContinental:
                st.activity = v.ageMyr < 1.6 ? kVaActive : (v.ageMyr < 6 ? kVaDormant : kVaExtinct);
                break;
            case kVgArcContinental:
            case kVgArcIsland:
                st.activity = u < 0.55 ? kVaActive : kVaDormant;
                break;
            default:
                st.activity = u < 0.35 ? kVaActive : (u < 0.8 ? kVaDormant : kVaExtinct);
                break;
        }
        double summit = -1e9;
        int64_t nb[8];
        g.neighbors8(v.cell, nb);
        summit = z[v.cell];
        for (int d = 0; d < 8; ++d) summit = std::max(summit, static_cast<double>(z[nb[d]]));
        st.summitM = static_cast<float>(summit - sea);
        st.submarine = summit < sea;
        st.hazardClass = st.activity == kVaActive ? ((v.morphology == kVmStrato || v.morphology == kVmCaldera) && !st.submarine ? 3 : 2)
                                                  : (st.activity == kVaDormant ? 1 : 0);
        ga->volcanoStates.push_back(st);
        if (st.activity == kVaActive) activeCells.push_back(v.cell);
        else if (st.activity == kVaDormant) dormantCells.push_back(v.cell);
        if ((v.genesis == kVgHotspotOceanic || v.genesis == kVgHotspotContinental) && v.ageMyr < 5) youngHotspotCells.push_back(v.cell);
    }
    std::vector<float> dAct, dDor, dHot;
    distanceTransform(g, activeCells, dAct, nullptr, 2000);
    distanceTransform(g, dormantCells, dDor, nullptr, 2000);
    distanceTransform(g, youngHotspotCells, dHot, nullptr, 2000);
    ctx.progress(0.3, "состояние вулканов");

    auto Lz = w.makeLayer(LayerId::VolcanicZone);
    auto Lg = w.makeLayer(LayerId::Geothermal);
    auto Ls = w.makeLayer(LayerId::Seismic);
    uint8_t* zone = Lz->as<uint8_t>();
    float* geo = Lg->as<float>();
    uint8_t* seis = Ls->as<uint8_t>();
    ctx.pool.forRows(H, [&](int j) {
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            bool cont = crust[k] == kCrustContinental;
            double cr = conv[k];
            double zv = 0;
            if (dAct[static_cast<size_t>(k)] < 60) zv = 5;
            else if (dAct[static_cast<size_t>(k)] < 160) zv = 4;
            else if (dDor[static_cast<size_t>(k)] < 80 || dAct[static_cast<size_t>(k)] < 300) zv = 3;
            if (dSub[k] > 50 && dSub[k] < 330 && cr > 1.0) zv = std::max(zv, 3.0 + (dSub[k] > 100 && dSub[k] < 250 ? 1.0 : 0.0));
            if (dRid[k] < 50) zv = std::max(zv, cont ? 3.0 : 3.0);
            else if (dRid[k] < 150) zv = std::max(zv, 2.0);
            if (dHot[static_cast<size_t>(k)] < 200) zv = std::max(zv, 4.0);
            if (dSub[k] > 330 && dSub[k] < 550 && cr > 1.0) zv = std::max(zv, 2.0);  // back-arc
            if (dDor[static_cast<size_t>(k)] < 400) zv = std::max(zv, 1.0);
            zone[k] = static_cast<uint8_t>(zv);
            // Conditional seismotectonic activity (not an earthquake forecast).
            double sv = 0;
            if (dTr[k] < 200 || dSub[k] < 300) sv = 5;
            else if (dSub[k] < 600) sv = 3;
            if (dCol[k] < 300) sv = std::max(sv, 5.0);
            else if (dCol[k] < 700) sv = std::max(sv, 3.0);
            if (dTrf[k] < 80) sv = std::max(sv, 5.0);
            else if (dTrf[k] < 250) sv = std::max(sv, 3.0);
            if (dRid[k] < 60) sv = std::max(sv, cont ? 4.0 : 3.0);
            else if (dRid[k] < 200) sv = std::max(sv, 2.0);
            if (sv == 0) sv = (dSub[k] < 1200 || dCol[k] < 1500) ? 1 : 0;
            seis[k] = static_cast<uint8_t>(sv);
            // Geothermal potential: heat × groundwater (only meaningful on land; sea floor gets heat only).
            double heat = std::max({std::exp(-dAct[static_cast<size_t>(k)] / 120.0), 0.6 * std::exp(-dDor[static_cast<size_t>(k)] / 120.0),
                                    0.8 * std::exp(-dHot[static_cast<size_t>(k)] / 150.0), (cont ? 0.7 : 0.5) * std::exp(-dRid[k] / 90.0),
                                    0.25 * std::exp(-dTrf[k] / 80.0)});
            double water = ocean[k] ? 1.0 : smoothstep(0.08, 0.8, aridity[k]);
            double perm = (lith[k] == kLithVolcanic || lith[k] == kLithCarbonate) ? 1.0 : 0.8;
            geo[k] = static_cast<float>(std::clamp(heat * (0.3 + 0.7 * water) * perm, 0.0, 1.0));
        }
    }, &ctx.cancel);
    for (size_t i = 0; i < volcs.size(); ++i) ga->volcanoStates[i].zone = zone[volcs[i].cell];
    ctx.progress(0.6, "зоны активности");

    // ---- hot springs and geysers
    std::vector<float> dSilicic;  // distance to active/dormant volcanoes with siliceous (continental) magmas
    {
        std::vector<int64_t> src;
        for (size_t i = 0; i < volcs.size(); ++i) {
            const Volcano& v = volcs[i];
            bool silicic = v.genesis == kVgArcContinental || v.genesis == kVgHotspotContinental || v.genesis == kVgContinentalRift;
            if (silicic && ga->volcanoStates[i].activity != kVaExtinct) src.push_back(v.cell);
        }
        distanceTransform(g, src, dSilicic, nullptr, 1000);
    }
    {
        std::vector<std::pair<double, int64_t>> cand;
        for (int64_t k = 0; k < N; ++k)
            if (!ocean[k] && geo[k] > 0.35) cand.push_back({hashUniform(key, static_cast<uint64_t>(k), 11), k});
        std::sort(cand.begin(), cand.end());
        std::vector<Vec3> placed;
        const double spacing = std::max(70.0, 1.5 * g.dyKm()) / R;
        for (auto& [u, k] : cand) {
            if (hashUniform(key, static_cast<uint64_t>(k), 12) > geo[k]) continue;
            const Vec3& p = g.unit(k);
            bool ok = true;
            for (auto& q : placed) if (angleBetween(p, q) < spacing) { ok = false; break; }
            if (!ok) continue;
            placed.push_back(p);
            ThermalSpring s;
            s.lat = g.latDeg(g.row(k));
            s.lon = g.lonDeg(g.col(k));
            s.cell = k;
            s.potential = geo[k];
            // Geysers need strong heat, abundant water and siliceous (continental) volcanism: much rarer.
            s.geyser = geo[k] > 0.45 && crust[k] == kCrustContinental && dSilicic[static_cast<size_t>(k)] < 120 && aridity[k] > 0.25 &&
                       hashUniform(key, static_cast<uint64_t>(k), 13) < 0.3;
            ga->springs.push_back(s);
            if (ga->springs.size() >= 600) break;
        }
    }
    w.setLayer(LayerId::VolcanicZone, Lz);
    w.setLayer(LayerId::Geothermal, Lg);
    w.setLayer(LayerId::Seismic, Ls);
    w.geoActivity = ga;

    // ---- checks
    const uint8_t* side = w.u8(LayerId::TectonicSide);
    const float* dBnd = w.f32(LayerId::DistBoundary);
    int arcs = 0, arcsWrongSide = 0, hot0 = 0, hot0Interior = 0, nearTransformOnly = 0;
    for (auto& v : volcs) {
        if (v.genesis == kVgArcContinental || v.genesis == kVgArcIsland) {
            ++arcs;
            if ((side[v.cell] == kSideSubducting && dTr[v.cell] < 150) || dSub[v.cell] > 450) ++arcsWrongSide;
        }
        if ((v.genesis == kVgHotspotOceanic || v.genesis == kVgHotspotContinental) && v.ageMyr < 0.01) {
            ++hot0;
            if (dBnd[v.cell] > 100 || dRid[v.cell] < 200) ++hot0Interior;
        }
        if (dTrf[v.cell] < 60 && dSub[v.cell] > 400 && dRid[v.cell] > 250 && v.genesis != kVgHotspotOceanic && v.genesis != kVgHotspotContinental)
            ++nearTransformOnly;
    }
    ctx.report.check("volc.arcSide", "Дуговые вулканы — на верхней плите зоны субдукции", arcsWrongSide == 0, arcsWrongSide, "0", true);
    if (hot0 > 0)
        ctx.report.check("volc.hotspotInterior", "Активные горячие точки внутри плит (или на хребте)", hot0Interior * 10 >= hot0 * 7,
                         static_cast<double>(hot0Interior) / hot0, "≥ 70 %");
    ctx.report.check("volc.noTransformArcs", "Нет вулканических рядов вдоль трансформных границ", nearTransformOnly * 20 <= static_cast<int>(volcs.size()) + 1,
                     nearTransformOnly, "≤ 5 % вулканов");
    int active = 0, geysers = 0;
    for (auto& s : ga->volcanoStates) active += s.activity == kVaActive;
    for (auto& s : ga->springs) geysers += s.geyser;
    ctx.report.metric("volcanoes", static_cast<double>(volcs.size()));
    ctx.report.metric("activeVolcanoes", active);
    ctx.report.metric("arcVolcanoes", arcs);
    ctx.report.metric("thermalSprings", static_cast<double>(ga->springs.size()));
    ctx.report.metric("geysers", geysers);
    ctx.report.check("volc.geysersRare", "Гейзеры существенно реже горячих источников", geysers * 4 <= static_cast<int>(ga->springs.size()) + 1,
                     geysers, "≤ 25 % источников");
    ctx.report.log("Шкала 0–5 — условный потенциал активности (не частота извержений и не прогноз риска).");
    ctx.progress(1.0, "геоактивность");
}

}  // namespace mapgen
