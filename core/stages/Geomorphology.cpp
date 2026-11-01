// Stage 2: geomorphology — structural relief and bathymetry, then controlled erosion.
//
// The height model is a sum of structural fields, each tied to a geological cause:
//   continental crust: isostatic platform level, passive/active margin profile (shelf, slope, rise);
//   oceanic crust: depth from crust age (2600 + 345·√age m, Parsons & Sclater 1977), fracture zones
//   along small circles of the plate's Euler pole, abyssal-hill texture;
//   subduction: trench and outer rise on the subducting plate, forearc, Andean belt / island arc and
//   back-arc basin on the overriding plate; continent–continent collision: broad orogen, plateau on the
//   weaker plate, foreland basin; continental rifts: graben with uplifted shoulders; transform zones:
//   linear ridges and valleys; ancient sutures: low eroded fold belts; hotspots: age-progressive volcanic
//   chains (seamounts → guyots; continental domes), arc and rift volcanoes.
// Multi-scale noise only adds texture whose amplitude is set by the structural context; it never
// decides where mountains or continents are. Erosion: implicit stream power with a preliminary runoff
// proxy, hillslope diffusion and deposition in closed lows. Young volcanic edifices are added after it.

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "core/rng/Noise.h"
#include "core/stages/Erosion.h"
#include "core/stages/Stage.h"
#include "core/stages/StageUtil.h"
#include "core/world/Categories.h"

namespace mapgen {

namespace {

// Spatial hash for minimum-distance sampling of volcano positions.
class PointHash {
public:
    explicit PointHash(double cellRad) : cell_(cellRad) {}
    bool farFrom(const Vec3& p, double minAngle) const {
        auto key = keyOf(p);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz) {
                    auto it = map_.find(pack(key[0] + dx, key[1] + dy, key[2] + dz));
                    if (it == map_.end()) continue;
                    for (auto& q : it->second)
                        if (angleBetween(p, q) < minAngle) return false;
                }
        return true;
    }
    void add(const Vec3& p) {
        auto k = keyOf(p);
        map_[pack(k[0], k[1], k[2])].push_back(p);
    }

private:
    std::array<int64_t, 3> keyOf(const Vec3& p) const {
        return {static_cast<int64_t>(std::floor(p.x / cell_)), static_cast<int64_t>(std::floor(p.y / cell_)),
                static_cast<int64_t>(std::floor(p.z / cell_))};
    }
    static int64_t pack(int64_t a, int64_t b, int64_t c) { return (a + 4096) * 16777216 + (b + 4096) * 4096 + (c + 4096); }
    double cell_;
    std::unordered_map<int64_t, std::vector<Vec3>> map_;
};

struct Edifice {
    Vec3 p;
    double heightM;
    double radiusKm;
    double flatTopM;  // guyots: maximum summit elevation (truncated), large value = none
};

}  // namespace

void runGeomorphology(StageContext& ctx) {
    WorldData& w = ctx.world;
    const Params& P = ctx.params;
    const SphereGrid& g = w.grid;
    const int W = g.width(), H = g.height();
    const int64_t N = g.size();
    const double R = g.radiusKm();
    const double M = P.num("relief.mountainScale");
    const double age = P.num("relief.age");
    const double cellKm = g.dyKm();

    const uint16_t* plate = w.u16(LayerId::PlateId);
    const uint8_t* crust = w.u8(LayerId::CrustType);
    const float* crustAge = w.f32(LayerId::CrustAge);
    const float* dCOB = w.f32(LayerId::DistCOB);
    const float* dSub = w.f32(LayerId::DistSubduction);
    const float* dTr = w.f32(LayerId::DistTrench);
    const float* dCol = w.f32(LayerId::DistCollision);
    const float* dRid = w.f32(LayerId::DistRidge);
    const float* dTrf = w.f32(LayerId::DistTransform);
    const float* conv = w.f32(LayerId::ConvRate);
    const float* divr = w.f32(LayerId::DivRate);
    const auto& plates = w.tectonics->plates;

    // ---- ancient sutures: distance field on a structural grid
    const int Ws = std::min(W, 1024);
    SphereGrid sg(Ws, Ws / 2, R);
    std::vector<float> dSutS;
    {
        std::vector<int64_t> src;
        for (auto& line : w.tectonics->oldSutures)
            for (size_t t = 0; t + 1 < line.pts.size(); ++t)
                for (int s = 0; s < 4; ++s) {
                    double f = s / 4.0;
                    Vec3 a = fromLatLon(line.pts[t].lat * kDegToRad, line.pts[t].lon * kDegToRad);
                    Vec3 b = fromLatLon(line.pts[t + 1].lat * kDegToRad, line.pts[t + 1].lon * kDegToRad);
                    src.push_back(sg.cellAtUnit(normalize(a * (1 - f) + b * f)));
                }
        std::sort(src.begin(), src.end());
        src.erase(std::unique(src.begin(), src.end()), src.end());
        distanceTransform(sg, src, dSutS, nullptr, 3000);
    }
    std::vector<float> dSut(static_cast<size_t>(N));
    if (Ws == W) dSut = dSutS;
    else upsampleBilinear(sg, dSutS.data(), g, dSut.data());

    Noise3 nLow(ctx.seed.key64("geo.platform")), nShelf(ctx.seed.key64("geo.shelf")), nTex(ctx.seed.key64("geo.texture")),
        nRidge(ctx.seed.key64("geo.ridged")), nArc(ctx.seed.key64("geo.arc")), nFold(ctx.seed.key64("geo.fold")),
        nTrf(ctx.seed.key64("geo.transform")), nHill(ctx.seed.key64("geo.hills")), nAbys(ctx.seed.key64("geo.abyssal"));
    std::vector<double> fzPhase(plates.size());
    {
        Pcg64 r = ctx.seed.stream("geo.fracture");
        for (auto& f : fzPhase) f = r.uniform();
    }
    const double lambdaFold = std::max(45.0, 2.6 * cellKm);

    std::vector<float> z(static_cast<size_t>(N));
    auto Lgen = w.makeLayer(LayerId::Genesis);
    auto Llith = w.makeLayer(LayerId::Lithology);
    uint8_t* gen = Lgen->as<uint8_t>();
    uint8_t* lith = Llith->as<uint8_t>();
    std::vector<float> mountainMask(static_cast<size_t>(N));

    ctx.pool.forRows(H, [&](int j) {
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            const Vec3& p = g.unit(k);
            const bool cont = crust[k] == kCrustContinental;
            const int pid = plate[k];
            const double d = dCOB[k];
            const double cf = std::clamp(conv[k] / 6.0, 0.15, 1.6);
            const double active = std::max(std::exp(-dSub[k] / 350.0), std::exp(-dTr[k] / 250.0));
            double e;
            uint8_t gn = cont ? kGenPlatform : kGenAbyssal;
            if (cont) {
                double shelfW = (30.0 + 190.0 * (1.0 - active)) * (0.35 + 1.3 * (0.5 + 0.5 * nShelf.fbm(p, 5, 4)));
                const double slopeW = 55.0;
                double platform = 120.0 + 640.0 * smoothstep(-0.6, 0.7, nLow.fbm(p, 1.6, 5)) + 160.0 * nLow.fbm(p * 1.7, 5.0, 4);
                if (d < slopeW) e = lerp(-3100.0, -140.0, smoothstep(0.0, slopeW, d));
                else if (d < slopeW + shelfW) e = lerp(-140.0, -20.0, (d - slopeW) / shelfW);
                else e = -20.0 + (platform + 20.0) * (1.0 - std::exp(-(d - slopeW - shelfW) / 420.0));
                if (d < slopeW + shelfW) gn = kGenContinentalMargin;
                else if (nLow.fbm(p * 1.3, 2.4, 3) > 0.35) gn = kGenShield;
                else if (platform < 300) gn = kGenSedimentaryBasin;
            } else {
                double a = crustAge[k];
                if (std::isnan(a)) a = 80;
                e = -std::min(6200.0, 2600.0 + 345.0 * std::sqrt(std::max(0.0, a)));
                e += 1250.0 * std::exp(d / 150.0) * (1.0 - active);  // continental rise sediment wedge (d < 0)
                e += 170.0 * nAbys.fbm(p, 40, 3);                     // abyssal hills
                // Fracture zones: small circles about the plate's Euler pole (flow lines of spreading).
                const Plate& pl = plates[static_cast<size_t>(pid)];
                Vec3 pole = fromLatLon(pl.poleLat * kDegToRad, pl.poleLon * kDegToRad);
                double colat = std::acos(std::clamp(dot(p, pole), -1.0, 1.0));
                double spacing = 520.0 / R;  // radians
                double f = colat / spacing + fzPhase[static_cast<size_t>(pid)] + 0.6 * nTrf.fbm(p, 3, 2);
                double dl = std::fabs(f - std::round(f)) * spacing * R;  // km to the nearest fracture line
                e -= 160.0 * std::exp(-sqr(dl / 14.0)) * std::exp(-dRid[k] / 2500.0);
                if (d > -140 && active < 0.3) gn = kGenContinentalMargin;
            }
            double uplift = 0;
            // --- subduction, overriding plate
            if (dSub[k] < 900) {
                double ds = dSub[k];
                if (cont) {
                    double peak = 190.0 + 40.0 * nArc.fbm(p, 4, 2);
                    double wdt = ds < peak ? 95.0 : 230.0;
                    uplift += M * 4300.0 * cf * gauss(ds - peak, wdt);
                    uplift += M * 1900.0 * smoothstep(4.5, 8.0, conv[k]) * gauss(ds - 390.0, 170.0);  // altiplano
                    e -= 1500.0 * cf * gauss(ds, 45.0);                                                 // forearc / inner trench wall
                    if (ds < 420 && uplift > 800) gn = kGenVolcanicArc;
                } else {
                    double arc = 3700.0 * cf * gauss(ds - 125.0, 48.0);
                    arc *= 0.45 + 0.55 * smoothstep(-0.25, 0.45, nArc.fbm(p, 9, 3));  // discrete islands along the arc
                    uplift += arc;
                    e -= 600.0 * gauss(ds - 330.0, 95.0);   // back-arc basin
                    e -= 2200.0 * cf * gauss(ds, 35.0);     // inner trench wall
                    if (arc > 600) gn = kGenIslandArc;
                    else if (ds > 230 && ds < 450) gn = kGenBackArc;
                }
            }
            // --- subduction, subducting plate: trench and outer rise
            if (dTr[k] < 500) {
                double dt = dTr[k];
                e -= (2600.0 + 1600.0 * std::min(cf, 1.2)) * gauss(dt - 22.0, 42.0);
                e += 280.0 * gauss(dt - 150.0, 60.0);
                if (dt < 70) gn = kGenTrench;
            }
            // --- continent–continent collision
            if (dCol[k] < 1500) {
                double dc = dCol[k];
                double wdt = 230.0 + 160.0 * std::min(cf, 1.3);
                uplift += M * (3300.0 + 3200.0 * std::min(cf, 1.4)) * gauss(dc, wdt);
                double rig = plates[static_cast<size_t>(pid)].rigidity;
                if (rig < 1.0 && conv[k] > 4.0) {
                    double wp = 300.0 + 400.0 * smoothstep(4.0, 9.0, conv[k]);
                    uplift += M * 2400.0 * (1.0 - smoothstep(0.6 * wp, wp, dc)) * smoothstep(0.0, 120.0, dc) * smoothstep(4.0, 7.0, conv[k]);
                }
                e -= 380.0 * gauss(dc - 1.7 * wdt, 130.0);  // foreland basin
                if (uplift > 1200 && dc < 900) gn = (uplift > 3500 && dc > wdt) ? kGenPlateau : kGenCollisionOrogen;
            }
            // --- divergent boundaries
            if (dRid[k] < 600) {
                double dr = dRid[k];
                if (cont) {
                    double df = std::clamp(divr[k] / 3.0, 0.2, 1.5);
                    // Graben: floors stay mostly above sea level (rift lakes form in hydrology), locally below it.
                    double floorDrop = std::max(0.0, e) * (0.45 + 0.2 * df) + 200.0 * std::max(0.0, nTrf.fbm(p, 10, 2));
                    e -= floorDrop * gauss(dr, 40.0);
                    uplift += (650.0 + 500.0 * df) * gauss(dr - 85.0, 45.0);
                    if (divr[k] > 9.0) e = lerp(e, -1600.0, gauss(dr, 60.0));  // rare: young sea in a very fast rift
                    if (dr < 120) gn = kGenContinentalRift;
                } else {
                    e -= 350.0 * gauss(dr, 12.0);  // axial valley
                    if (dr < 110) gn = kGenMidOceanRidge;
                }
            }
            // --- transform zones: linear ridges and valleys, local pull-apart basins
            if (dTrf[k] < 200) {
                double s = nTrf.fbm(p, 14, 2);
                e += (s > 0 ? 300.0 : -300.0) * gauss(dTrf[k], 22.0);
                if (dTrf[k] < 60 && gn != kGenTrench) gn = kGenTransformZone;
            }
            // --- ancient fold belts inside continents
            if (cont && dSut[static_cast<size_t>(k)] < 600) {
                double old = M * (650.0 + 1300.0 * (1.0 - age)) * gauss(dSut[static_cast<size_t>(k)], 170.0);
                old *= 0.55 + 0.45 * nRidge.ridged(p, 12, 4);
                uplift += old;
                if (old > 350 && (gn == kGenPlatform || gn == kGenShield || gn == kGenSedimentaryBasin)) gn = kGenOldOrogen;
            }
            // Fold-parallel ranges oriented along the driving boundary; ridged texture for crests and valleys.
            double mm = std::clamp(uplift / 2200.0, 0.0, 1.0);
            if (uplift > 300) {
                double dd = std::min({dSub[k], dCol[k], dSut[static_cast<size_t>(k)]});
                double fold = std::cos(kTwoPi * dd / lambdaFold + 3.0 * nFold.fbm(p, 6, 3));
                uplift *= 0.9 + 0.1 * fold;
                uplift *= 0.6 + 0.7 * nRidge.ridged(p, 20, 6, 2.1, 0.55);
            }
            // Isostatic saturation: stacked uplifts approach a crustal-thickness limit instead of adding up.
            const double uMax = 5600.0 * M;
            uplift = uMax * (1.0 - std::exp(-uplift / uMax));
            e += uplift;
            mountainMask[static_cast<size_t>(k)] = static_cast<float>(mm);
            // Texture: hills on old terrains, gentle plains elsewhere (amplitude follows context).
            if (cont) e += (1.0 - mm) * (110.0 * nTex.fbm(p, 16, 5) + 260.0 * (1.0 - 0.6 * age) * std::max(0.0, nHill.fbm(p, 8, 5)));
            // Coastal-zone detail: islands on shelves, bays and capes that follow the relief.
            if (d > -350 && d < 450) e += (1.0 - mm) * 170.0 * nShelf.fbm(p * 1.9, 24, 4) * std::exp(-std::fabs(d - 60.0) / 260.0);
            z[static_cast<size_t>(k)] = static_cast<float>(std::max(e, -10900.0));  // deepest trenches ~11 km
            gen[k] = gn;
            // Simplified lithology from the structural context.
            uint8_t li;
            if (!cont) li = kLithBasalt;
            else if (gn == kGenVolcanicArc || gn == kGenIslandArc) li = kLithVolcanic;
            else if (gn == kGenCollisionOrogen || gn == kGenOldOrogen || gn == kGenPlateau) li = kLithMetamorphic;
            else if (gn == kGenShield) li = kLithCrystalline;
            else if (gn == kGenContinentalMargin) li = kLithMarineSediment;
            else li = nLow.fbm(p * 2.1, 3.5, 3) > 0.22 ? kLithCarbonate : kLithSedimentary;
            lith[k] = li;
        }
    }, &ctx.cancel);
    ctx.progress(0.35, "структурный рельеф");

    // ---- hotspots and volcanic edifices
    auto gf = std::make_shared<GeomorphFeatures>();
    std::vector<Edifice> edifices;
    std::vector<Edifice> youngEdifices;  // added after erosion
    {
        Pcg64 hr = ctx.seed.stream("geo.hotspots");
        int nh = static_cast<int>(std::round(hr.uniform(8, 15) * P.num("relief.hotspotIntensity")));
        for (int h = 0; h < nh; ++h) {
            Vec3 hp = randomUnitVector(hr);
            int64_t hc = g.cellAtUnit(hp);
            Hotspot hs;
            hs.id = h;
            hs.lat = latOf(hp) * kRadToDeg;
            hs.lon = lonOf(hp) * kRadToDeg;
            hs.strength = hr.uniform(0.6, 1.4);
            hs.plateId = plate[hc];
            hs.oceanic = crust[hc] == kCrustOceanic;
            hs.onRidge = dRid[hc] < 180;
            gf->hotspots.push_back(hs);
            const Plate& pl = plates[static_cast<size_t>(hs.plateId)];
            Vec3 pole = fromLatLon(pl.poleLat * kDegToRad, pl.poleLon * kDegToRad);
            double omega = pl.omegaDegPerMyr * kDegToRad;
            double step = hr.uniform(2.0, 3.5);
            for (double t = 0; t <= 70.0; t += step * hr.uniform(0.7, 1.3)) {
                Vec3 p = rotateAbout(hp, pole, omega * t);  // crust that sat over the hotspot t Myr ago
                int64_t c = g.cellAtUnit(p);
                if (plate[c] != hs.plateId || dTr[c] < 60) break;  // carried into another plate / subducted
                bool oc = crust[c] == kCrustOceanic;
                Volcano v;
                v.id = static_cast<int>(gf->volcanoes.size());
                v.lat = latOf(p) * kRadToDeg;
                v.lon = lonOf(p) * kRadToDeg;
                v.cell = c;
                v.ageMyr = static_cast<float>(t);
                v.plateId = hs.plateId;
                v.hotspotId = h;
                if (oc) {
                    double hgt = (4300.0 + 1800.0 * hs.strength) * (hs.onRidge ? 1.25 : 1.0) * (1.0 - 0.3 * t / 70.0);
                    double sub = 320.0 * std::sqrt(t);  // thermal subsidence of the edifice
                    double floor = z[static_cast<size_t>(c)];
                    double summit = floor + hgt - sub;
                    v.genesis = kVgHotspotOceanic;
                    v.morphology = t < 1.5 ? kVmShield : (summit < -150 && t > 12 ? kVmGuyot : kVmSeamount);
                    v.edificeHeightM = static_cast<float>(hgt - sub);
                    double flat = v.morphology == kVmGuyot ? summit : 1e9;
                    Edifice ed{p, hgt - sub, 45.0 + 35.0 * hs.strength * (t < 5 ? 1.2 : 1.0), flat};
                    (t < 5 ? youngEdifices : edifices).push_back(ed);
                } else {
                    v.genesis = kVgHotspotContinental;
                    v.morphology = t < 1.5 ? kVmCaldera : kVmCinderField;
                    v.edificeHeightM = static_cast<float>(900.0 * hs.strength * std::exp(-t / 15.0));
                    edifices.push_back({p, 900.0 * hs.strength * std::exp(-t / 15.0), 260.0, 1e9});
                }
                gf->volcanoes.push_back(v);
            }
        }
    }
    // Arc volcanoes on the overriding plate, rift volcanoes, with minimum spacing (no volcano on every boundary cell).
    {
        PointHash ph(150.0 / R);
        for (auto& v : gf->volcanoes) ph.add(fromLatLon(v.lat * kDegToRad, v.lon * kDegToRad));
        uint64_t key = ctx.seed.key64("geo.arcVolcanoes");
        std::vector<std::pair<double, int64_t>> cand;
        const double tol = std::max(12.0, 0.75 * cellKm);
        for (int64_t k = 0; k < N; ++k) {
            bool cont = crust[k] == kCrustContinental;
            double target = cont ? 175.0 : 120.0;
            // Not next to a trench where this plate itself subducts (a plate can override one margin and sink at another).
            if (dSub[k] < 2000 && std::fabs(dSub[k] - target) < tol && conv[k] > 1.0 && dTr[k] > 200) cand.push_back({hashUniform(key, static_cast<uint64_t>(k)), k});
            else if (cont && dRid[k] > 15 && dRid[k] < 90 && divr[k] > 0.8 && hashUniform(key, static_cast<uint64_t>(k), 7) < 0.25)
                cand.push_back({hashUniform(key, static_cast<uint64_t>(k), 1) + 2.0, k});  // rift volcanoes after arcs
        }
        std::sort(cand.begin(), cand.end());
        for (auto& [order, k] : cand) {
            const Vec3& p = g.unit(k);
            bool rift = order >= 2.0;
            double spacing = (rift ? 170.0 : 85.0 + 45.0 * hashUniform(key, static_cast<uint64_t>(k), 3)) / R;
            if (!ph.farFrom(p, spacing)) continue;
            ph.add(p);
            bool cont = crust[k] == kCrustContinental;
            Volcano v;
            v.id = static_cast<int>(gf->volcanoes.size());
            v.lat = g.latDeg(g.row(k));
            v.lon = g.lonDeg(g.col(k));
            v.cell = k;
            v.plateId = plate[k];
            v.ageMyr = static_cast<float>(hashUniform(key, static_cast<uint64_t>(k), 4) * (rift ? 3.0 : 2.0));
            double u = hashUniform(key, static_cast<uint64_t>(k), 5);
            if (rift) {
                v.genesis = kVgContinentalRift;
                v.morphology = u < 0.4 ? kVmStrato : (u < 0.7 ? kVmShield : kVmCinderField);
            } else {
                v.genesis = cont ? kVgArcContinental : kVgArcIsland;
                v.morphology = u < 0.12 ? kVmCaldera : kVmStrato;
            }
            double hgt = rift ? 1400.0 + 1600.0 * u : 1600.0 + 1800.0 * hashUniform(key, static_cast<uint64_t>(k), 6);
            v.edificeHeightM = static_cast<float>(hgt);
            youngEdifices.push_back({p, hgt, std::max(16.0, 0.9 * cellKm), 1e9});
            gf->volcanoes.push_back(v);
        }
    }
    auto addEdifices = [&](const std::vector<Edifice>& list0) {
        // A cone narrower than the grid is represented by its cell-average height (no single-cell needles).
        std::vector<Edifice> list = list0;
        for (auto& ed : list) {
            double minR = 1.4 * cellKm;
            if (ed.radiusKm < minR) {
                ed.heightM *= sqr(ed.radiusKm / minR);
                ed.radiusKm = minR;
            }
        }
        for (auto& ed : list) {
            int64_t c0 = g.cellAtUnit(ed.p);
            int ci = g.col(c0), cj = g.row(c0);
            int rj = static_cast<int>(std::ceil(ed.radiusKm / g.dyKm())) + 1;
            for (int dj = -rj; dj <= rj; ++dj) {
                int jj = cj + dj;
                if (jj < 0 || jj >= H) continue;
                int ri = std::min(W / 2, static_cast<int>(std::ceil(ed.radiusKm / std::max(0.5, g.dxKm(jj)))) + 1);
                for (int di = -ri; di <= ri; ++di) {
                    int64_t k = g.index(g.wrapCol(ci + di), jj);
                    double dkm = R * angleBetween(g.unit(k), ed.p);
                    if (dkm >= ed.radiusKm) continue;
                    double shape = std::pow(1.0 - dkm / ed.radiusKm, 1.6);
                    double add = ed.heightM * shape;
                    double nz = z[static_cast<size_t>(k)] + add;
                    if (ed.flatTopM < 1e8) nz = std::min(nz, std::max(static_cast<double>(z[static_cast<size_t>(k)]), ed.flatTopM));
                    z[static_cast<size_t>(k)] = static_cast<float>(nz);
                    if (add > 300 && gen[k] != kGenTrench) gen[k] = kGenHotspot;
                }
            }
        }
    };
    addEdifices(edifices);
    // Arc volcano edifices keep the arc genesis code.
    ctx.progress(0.5, "вулканические постройки");

    // ---- calibration: put the relief land fraction at 0 m
    double landTarget = P.num("relief.landFraction");
    double q = areaWeightedQuantile(g, z, 1.0 - landTarget);
    double shift = -q;
    if (std::fabs(shift) > 1500) {
        ctx.report.warn("Для заданной доли суши требуется сдвиг рельефа на " + formatNumber(std::round(shift)) +
                        " м: параметры коры и суши плохо согласованы (см. валидатор).");
    }
    for (auto& v : z) v = static_cast<float>(v + shift);
    gf->calibrationShiftM = shift;
    // Snapshot of the structural relief (with young volcanoes) before erosion; erosion works without the
    // young edifices, which are added back afterwards (they are younger than the landscape).
    std::vector<float> zPre(z.begin(), z.end());
    addEdifices(youngEdifices);
    std::vector<float> raw(z.begin(), z.end());
    z.swap(zPre);

    // ---- erosion with a preliminary runoff proxy (zonal moisture belts + windward slopes)
    std::vector<float> precip(static_cast<size_t>(N));
    ctx.pool.forRows(H, [&](int j) {
        double lat = g.latDeg(j), al = std::fabs(lat);
        double zonal = 0.55 + 1.25 * std::exp(-sqr(lat / 11.0)) - 0.45 * std::exp(-sqr((al - 25.0) / 8.0)) + 0.45 * std::exp(-sqr((al - 52.0) / 12.0));
        // Prevailing low-level wind direction (towards which air moves), east/north components.
        double ue = al < 30 ? -1.0 : (al < 60 ? 1.0 : -0.7);
        double vn = al < 30 ? (lat > 0 ? -0.35 : 0.35) : (al < 60 ? (lat > 0 ? 0.2 : -0.2) : 0.0);
        double dx = std::max(g.dxKm(j), 0.25 * g.dyKm()) * 1000.0, dy = g.dyKm() * 1000.0;
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            double gx = (z[static_cast<size_t>(g.neighbor(i, j, 2))] - z[static_cast<size_t>(g.neighbor(i, j, 6))]) / (2 * dx);
            double gy = (z[static_cast<size_t>(g.neighbor(i, j, 0))] - z[static_cast<size_t>(g.neighbor(i, j, 4))]) / (2 * dy);
            double up = ue * gx + vn * gy;  // >0 windward (rising along the wind)
            double f = up > 0 ? 1.0 + std::min(2.0, up * 60.0) : std::max(0.35, 1.0 + up * 40.0);
            double inland = std::exp(-std::max(0.0, static_cast<double>(dCOB[k])) / 2500.0);
            precip[static_cast<size_t>(k)] = static_cast<float>(std::max(0.08, zonal) * f * (0.45 + 0.55 * inland));
        }
    }, &ctx.cancel);
    std::vector<uint8_t> base(static_cast<size_t>(N));
    for (int64_t k = 0; k < N; ++k) base[static_cast<size_t>(k)] = z[static_cast<size_t>(k)] < 0.0f;
    ErosionParams ep;
    ep.iterations = 3 + static_cast<int>(std::round(4 * age));
    ep.k = 0.02 + 0.025 * age;
    ep.diffusion = 0.05 + 0.1 * age;
    ep.deposition = 0.25 + 0.3 * age;
    ep.maxSpikeM = 1700;
    std::vector<float> erosion = erodeLandscape(g, z, base, precip, ep, ctx.pool, ctx.cancel);
    addEdifices(youngEdifices);
    ctx.progress(0.85, "эрозия и аккумуляция");
    // Deposition areas become alluvium.
    for (int64_t k = 0; k < N; ++k)
        if (erosion[static_cast<size_t>(k)] < -15.0f && crust[k] == kCrustContinental && z[static_cast<size_t>(k)] > -50) lith[k] = kLithAlluvium;

    auto put = [&](LayerId id, const std::vector<float>& v) {
        auto l = w.makeLayer(id);
        std::copy(v.begin(), v.end(), l->as<float>());
        w.setLayer(id, l);
    };
    put(LayerId::ElevationRaw, raw);
    put(LayerId::Elevation, z);
    put(LayerId::ErosionDepth, erosion);
    w.setLayer(LayerId::Genesis, Lgen);
    w.setLayer(LayerId::Lithology, Llith);
    w.geomorph = gf;

    // ---- report
    double landArea = 0, total = g.totalAreaKm2(), sumLand = 0, sumOcean = 0, oceanArea = 0, maxZ = -1e9, minZ = 1e9, spikes = 0;
    for (int j = 0; j < H; ++j) {
        double a = g.cellAreaKm2(j);
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            double v = z[static_cast<size_t>(k)];
            maxZ = std::max(maxZ, v);
            minZ = std::min(minZ, v);
            if (v >= 0) { landArea += a; sumLand += a * v; }
            else { oceanArea += a; sumOcean += a * v; }
            if (gen[k] == kGenHotspot || gen[k] == kGenVolcanicArc || gen[k] == kGenIslandArc) continue;  // edifices are real
            double mx = -1e18;
            for (int d = 0; d < 8; ++d) mx = std::max(mx, static_cast<double>(z[static_cast<size_t>(g.neighbor(i, j, d))]));
            if (v > mx + 2000) spikes++;
        }
    }
    ctx.report.metric("landFractionAtZero", landArea / total);
    ctx.report.metric("meanLandElevationM", landArea > 0 ? sumLand / landArea : 0);
    ctx.report.metric("meanOceanDepthM", oceanArea > 0 ? -sumOcean / oceanArea : 0);
    ctx.report.metric("maxElevationM", maxZ);
    ctx.report.metric("minElevationM", minZ);
    ctx.report.metric("calibrationShiftM", shift);
    ctx.report.metric("hotspots", static_cast<double>(gf->hotspots.size()));
    ctx.report.metric("volcanoes", static_cast<double>(gf->volcanoes.size()));
    ctx.report.check("relief.noSpikes", "Нет одиночных «шипов» рельефа (> 2 км над всеми соседями)", spikes == 0, spikes, "0");
    ctx.report.check("relief.range", "Высоты в физически правдоподобном диапазоне", maxZ < 11000 && minZ > -12000, maxZ,
                     "−12 000 … +11 000 м", true);
    ctx.report.check("relief.landFraction", "Доля суши у нулевой высоты соответствует цели", std::fabs(landArea / total - landTarget) < 0.04,
                     landArea / total, "цель ± 0,04");
    ctx.report.log("Глубина океана по возрасту коры: 2600 + 345·√возраст м (упрощённая термическая модель). Эрозия: " +
                   std::to_string(ep.iterations) + " итераций потоковой мощности с предварительным стоком.");
    ctx.progress(1.0, "геоморфология");
}

}  // namespace mapgen
