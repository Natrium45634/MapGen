// Stage 3: sea level, ocean connectivity, coast distance, landforms, coast geometry.
//
// Sea level is an explicit parameter (relief zero / target land fraction found from the
// area-weighted height distribution / fixed metres). A cell is sea exactly when it lies below sea
// level AND belongs to a below-sea-level component connected to a world ocean on the sphere; isolated
// lows below sea level stay land (depressions) and are left to hydrology. Coast types that need
// climate (fjords, coral, mangroves, deltas) are classified later in stage 6.

#include <algorithm>
#include <cmath>

#include "core/stages/Stage.h"
#include "core/stages/StageUtil.h"
#include "core/world/Categories.h"

namespace mapgen {

namespace {

// Sliding-window min/max of a field over a radius in km (x radius widened by 1/cos(lat)).
void localRange(const SphereGrid& g, const std::vector<float>& z, double radiusKm, std::vector<float>& mn,
                std::vector<float>& mx, ThreadPool& pool) {
    const int W = g.width(), H = g.height();
    std::vector<float> rmin(static_cast<size_t>(g.size())), rmax(static_cast<size_t>(g.size()));
    pool.forRows(H, [&](int j) {
        int r = std::clamp(static_cast<int>(std::round(radiusKm / std::max(0.5, g.dxKm(j)))), 1, W / 8);
        for (int i = 0; i < W; ++i) {
            float a = 1e30f, b = -1e30f;
            for (int d = -r; d <= r; ++d) {
                float v = z[static_cast<size_t>(g.index(g.wrapCol(i + d), j))];
                a = std::min(a, v);
                b = std::max(b, v);
            }
            rmin[static_cast<size_t>(g.index(i, j))] = a;
            rmax[static_cast<size_t>(g.index(i, j))] = b;
        }
    });
    mn.resize(static_cast<size_t>(g.size()));
    mx.resize(static_cast<size_t>(g.size()));
    int ry = std::max(1, static_cast<int>(std::round(radiusKm / g.dyKm())));
    pool.forRows(H, [&](int j) {
        for (int i = 0; i < W; ++i) {
            float a = 1e30f, b = -1e30f;
            for (int d = -ry; d <= ry; ++d) {
                int jj = std::clamp(j + d, 0, H - 1);
                a = std::min(a, rmin[static_cast<size_t>(g.index(i, jj))]);
                b = std::max(b, rmax[static_cast<size_t>(g.index(i, jj))]);
            }
            mn[static_cast<size_t>(g.index(i, j))] = a;
            mx[static_cast<size_t>(g.index(i, j))] = b;
        }
    });
}

}  // namespace

void runOcean(StageContext& ctx) {
    WorldData& w = ctx.world;
    const Params& P = ctx.params;
    const SphereGrid& g = w.grid;
    const int W = g.width(), H = g.height();
    const int64_t N = g.size();
    const float* zb = w.f32(LayerId::Elevation);
    std::vector<float> z(zb, zb + N);

    // ---- sea level
    const std::string& mode = P.str("ocean.seaLevelMode");
    double sea = 0;
    if (mode == "fixed") sea = P.num("ocean.seaLevel");
    else if (mode == "fraction") sea = areaWeightedQuantile(g, z, 1.0 - P.num("ocean.landFraction"));
    ctx.report.log("Режим уровня моря: " + mode + ", уровень " + formatNumber(std::round(sea * 10) / 10) + " м от нуля проекта");

    // ---- ocean connectivity on the sphere
    std::vector<uint8_t> below(static_cast<size_t>(N));
    for (int64_t k = 0; k < N; ++k) below[static_cast<size_t>(k)] = z[static_cast<size_t>(k)] < sea;
    std::vector<int32_t> comp;
    int nc = connectedComponents(g, below.data(), comp);
    std::vector<double> carea(static_cast<size_t>(nc), 0.0);
    for (int64_t k = 0; k < N; ++k)
        if (comp[static_cast<size_t>(k)] >= 0) carea[static_cast<size_t>(comp[static_cast<size_t>(k)])] += g.cellAreaKm2(g.row(k));
    int largest = -1;
    for (int c = 0; c < nc; ++c) if (largest < 0 || carea[static_cast<size_t>(c)] > carea[static_cast<size_t>(largest)]) largest = c;
    std::vector<int32_t> oceanId(static_cast<size_t>(nc), -1);
    auto of = std::make_shared<OceanFeatures>();
    for (int c = 0; c < nc; ++c)
        if (c == largest || carea[static_cast<size_t>(c)] >= 0.01 * g.totalAreaKm2()) {
            oceanId[static_cast<size_t>(c)] = static_cast<int32_t>(of->oceanAreasKm2.size());
            of->oceanAreasKm2.push_back(carea[static_cast<size_t>(c)]);
        }
    auto Lmask = w.makeLayer(LayerId::OceanMask);
    auto Lbody = w.makeLayer(LayerId::WaterBody);
    uint8_t* ocean = Lmask->as<uint8_t>();
    int32_t* body = Lbody->as<int32_t>();
    double landArea = 0;
    int64_t depressions = 0;
    for (int64_t k = 0; k < N; ++k) {
        int32_t c = comp[static_cast<size_t>(k)];
        int32_t id = c >= 0 ? oceanId[static_cast<size_t>(c)] : -1;
        ocean[k] = id >= 0 ? 1 : 0;
        body[k] = id;
        if (!ocean[k]) landArea += g.cellAreaKm2(g.row(k));
        if (c >= 0 && id < 0) ++depressions;
    }
    of->seaLevelM = sea;
    of->landFraction = landArea / g.totalAreaKm2();
    of->oceanComponents = static_cast<int>(of->oceanAreasKm2.size());
    ctx.progress(0.25, "морская маска");

    // ---- signed distance to the coast
    std::vector<int64_t> landCoast, seaCoast;
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            bool edge = false;
            for (int d = 0; d < 8 && !edge; ++d) edge = ocean[g.neighbor(i, j, d)] != ocean[k];
            if (edge) (ocean[k] ? seaCoast : landCoast).push_back(k);
        }
    std::vector<float> dToSea, dToLand;
    distanceTransform(g, seaCoast, dToSea, nullptr, 5000);
    distanceTransform(g, landCoast, dToLand, nullptr, 5000);
    auto Ldist = w.makeLayer(LayerId::DistCoast);
    float* dc = Ldist->as<float>();
    double half = 0.5 * g.dyKm();
    for (int64_t k = 0; k < N; ++k)
        dc[k] = ocean[k] ? -static_cast<float>(dToLand[static_cast<size_t>(k)] + half) : static_cast<float>(dToSea[static_cast<size_t>(k)] + half);
    ctx.progress(0.45, "расстояние до берега");

    // ---- landforms
    std::vector<float> mn, mx;
    localRange(g, z, std::max(45.0, 2.0 * g.dyKm()), mn, mx, ctx.pool);
    std::vector<int64_t> mountainCells;
    const uint8_t* gen = w.u8(LayerId::Genesis);
    const uint8_t* lith = w.u8(LayerId::Lithology);
    auto Llf = w.makeLayer(LayerId::Landform);
    uint8_t* lf = Llf->as<uint8_t>();
    for (int64_t k = 0; k < N; ++k) {
        double h = z[static_cast<size_t>(k)] - sea, rel = mx[static_cast<size_t>(k)] - mn[static_cast<size_t>(k)];
        if (!ocean[k] && (rel > 1500 || (h > 2500 && rel > 900))) mountainCells.push_back(k);
    }
    std::vector<float> dMount;
    distanceTransform(g, mountainCells, dMount, nullptr, 400);
    std::vector<uint8_t> guyotCell(static_cast<size_t>(N), 0);
    if (w.geomorph)
        for (auto& v : w.geomorph->volcanoes)
            if (v.morphology == kVmGuyot || v.morphology == kVmSeamount) guyotCell[static_cast<size_t>(v.cell)] = v.morphology == kVmGuyot ? 2 : 1;
    ctx.pool.forRows(H, [&](int j) {
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            double h = z[static_cast<size_t>(k)] - sea;
            double rel = mx[static_cast<size_t>(k)] - mn[static_cast<size_t>(k)];
            uint8_t c;
            if (!ocean[k]) {
                if (h < 0) c = kLfDepression;
                else if (gen[k] == kGenContinentalRift && h < 1800) c = kLfRiftValley;
                else if (gen[k] == kGenHotspot && rel > 400) c = kLfVolcanic;
                else if (rel > 1500 || (h > 2500 && rel > 900)) c = kLfMountains;
                else if (h > 1500 && rel < 600) c = kLfPlateau;
                else if (h > 1000 && rel >= 600) c = kLfHighlands;
                else if (h > 600 && rel < 300) c = kLfTableland;
                else if (rel > 250 && h < 1500) c = kLfHills;
                else if (dMount[static_cast<size_t>(k)] < 250 && h < 700) c = kLfFoothillPlain;
                else if (h < 200 && lith[k] == kLithAlluvium) c = kLfAlluvialLowland;
                else if (h < 120 && dc[k] < 150) c = kLfCoastalPlain;
                else if (h < 200) c = kLfLowland;
                else if (gen[k] == kGenPlatform || gen[k] == kGenShield || gen[k] == kGenSedimentaryBasin) c = kLfPlatformPlain;
                else c = kLfLowland;
            } else {
                double D = -h;
                if (guyotCell[static_cast<size_t>(k)]) c = guyotCell[static_cast<size_t>(k)] == 2 ? kLfGuyot : kLfSeamount;
                else if (gen[k] == kGenTrench && D > 4500) c = kLfTrench;
                else if (D <= 200 && dc[k] > -600) c = kLfShelf;
                else if (gen[k] == kGenHotspot) c = kLfSeamount;
                else if (gen[k] == kGenIslandArc && D < 3500) c = kLfArcRidge;
                else if (gen[k] == kGenMidOceanRidge) c = kLfRidge;
                else if (gen[k] == kGenBackArc) c = kLfBackArcBasin;
                else if (gen[k] == kGenTransformZone) c = kLfFractureZone;
                else if (D < 3000 && dc[k] > -450) c = kLfSlope;
                else if (D < 4600 && dc[k] > -900 && gen[k] != kGenMidOceanRidge) c = kLfRise;
                else c = kLfAbyssalPlain;
            }
            lf[k] = c;
        }
    }, &ctx.cancel);
    // Submarine canyons: slope cells in front of large preliminary drainage outlets (deep valleys reaching the shelf edge).
    {
        const float* ero = w.f32(LayerId::ErosionDepth);
        for (int64_t k = 0; k < N; ++k) {
            if (lf[k] != kLfSlope) continue;
            int i = g.col(k), j = g.row(k);
            int strongNeighbours = 0;
            for (int d = 0; d < 8; ++d) {
                int64_t nb = g.neighbor(i, j, d);
                if ((lf[nb] == kLfShelf || !ocean[nb]) && ero[nb] > 250) ++strongNeighbours;
            }
            if (strongNeighbours >= 2) lf[k] = kLfCanyon;
        }
    }
    ctx.progress(0.65, "формы рельефа");

    // ---- coast exposure (open-water fetch) and parameterised tidal range (P2)
    auto Lexp = w.makeLayer(LayerId::CoastExposure);
    auto Ltide = w.makeLayer(LayerId::TidalRange);
    float* ex = Lexp->as<float>();
    float* tide = Ltide->as<float>();
    const double tidalScale = P.num("ocean.tidalScale");
    const int maxSteps = std::max(8, static_cast<int>(1500.0 / g.dyKm()));
    ctx.pool.forRows(H, [&](int j) {
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            ex[k] = notComputed();
            tide[k] = notComputed();
            if (std::fabs(dc[k]) > 1.6 * g.dyKm() + 1) continue;
            double sum = 0;
            for (int d = 0; d < 8; ++d) {
                int ci = i, cj = j;
                double dist = 0;
                for (int s = 0; s < maxSteps; ++s) {
                    int64_t nb = g.neighbor(ci, cj, d);
                    int ni = g.col(nb), nj = g.row(nb);
                    if (!ocean[nb]) break;
                    dist += g.neighborDistKm(cj, d);
                    ci = ni;
                    cj = nj;
                }
                sum += std::min(1.0, dist / 1500.0);
            }
            ex[k] = static_cast<float>(sum / 8.0);
            // Tides amplify on wide, shallow shelves and in embayments (low exposure).
            int shelf = 0, total = 0;
            int r = std::max(1, static_cast<int>(250.0 / g.dyKm()));
            for (int dj = -r; dj <= r; dj += std::max(1, r / 4))
                for (int di = -r; di <= r; di += std::max(1, r / 4)) {
                    int jj = std::clamp(j + dj, 0, H - 1);
                    int64_t q = g.index(g.wrapCol(i + di), jj);
                    if (!ocean[q]) continue;
                    ++total;
                    if (z[static_cast<size_t>(q)] - sea > -200) ++shelf;
                }
            double shelfShare = total ? static_cast<double>(shelf) / total : 0;
            tide[k] = static_cast<float>(tidalScale * (0.6 + 4.5 * shelfShare * shelfShare + 1.5 * (1.0 - ex[k]) * shelfShare));
        }
    }, &ctx.cancel);

    // ---- coastline vectors
    of->coastlines = maskContours(g, ocean, 3);
    ctx.progress(0.9, "береговая линия");

    w.setLayer(LayerId::OceanMask, Lmask);
    w.setLayer(LayerId::WaterBody, Lbody);
    w.setLayer(LayerId::DistCoast, Ldist);
    w.setLayer(LayerId::Landform, Llf);
    w.setLayer(LayerId::CoastExposure, Lexp);
    w.setLayer(LayerId::TidalRange, Ltide);
    w.ocean = of;

    // ---- invariants
    int64_t bad = 0;
    for (int64_t k = 0; k < N; ++k) {
        if (ocean[k] && !(z[static_cast<size_t>(k)] < sea)) ++bad;
        // Below sea level and adjacent to the ocean must itself be ocean (connectivity).
        if (!ocean[k] && z[static_cast<size_t>(k)] < sea) {
            int64_t nb[8];
            g.neighbors8(k, nb);
            for (int d = 0; d < 8; ++d) if (ocean[nb[d]]) { ++bad; break; }
        }
    }
    ctx.report.check("ocean.mask", "Морская маска точно соответствует уровню моря и связности", bad == 0, static_cast<double>(bad),
                     "0 несоответствий", true);
    ctx.report.metric("seaLevelM", sea);
    ctx.report.metric("landFraction", of->landFraction);
    ctx.report.metric("oceans", of->oceanComponents);
    ctx.report.metric("belowSeaLevelLandCells", static_cast<double>(depressions));
    ctx.report.metric("coastlinePolylines", static_cast<double>(of->coastlines.size()));
    if (mode == "fraction")
        ctx.report.check("ocean.fraction", "Доля суши соответствует целевой", std::fabs(of->landFraction - P.num("ocean.landFraction")) < 0.02,
                         of->landFraction, "цель ± 0,02");
    ctx.progress(1.0, "океан");
}

}  // namespace mapgen
