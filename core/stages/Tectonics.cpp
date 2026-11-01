// Stage 1: tectonics.
//
// Plates: geodesic seeds on the sphere, multiplicatively weighted angular
// distance evaluated on a domain-warped position (irregular, non-circular
// boundaries), then connectivity repair. Continental crust is an independent
// spatial field (clusters of blobs + fractal noise) — a continent is not a
// plate. Each plate rotates about its own Euler pole. Boundaries are typed
// from the normal/tangential components of the relative velocity and the crust
// on both sides; ocean-floor age follows from the distance to spreading ridges.
// All large-scale fields are analytic functions of position or are computed
// on a structural grid (≤1024×512) and interpolated, so Preview and Standard
// show the same plates and continents.

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

#include "core/rng/Noise.h"
#include "core/stages/Stage.h"
#include "core/stages/StageUtil.h"
#include "core/world/Categories.h"

namespace mapgen {

namespace {

struct PlateModel {
    Vec3 seed;
    double weight;
    Vec3 pole;
    double omegaRadPerMyr;
    double rigidity;
};

// Velocity of plate (km/Myr == mm/yr) at unit position p.
Vec3 plateVelocity(const PlateModel& pm, const Vec3& p, double radiusKm) {
    return cross(pm.pole, p) * (pm.omegaRadPerMyr * radiusKm);
}

struct BoundaryCell {
    int64_t cell;     // structural grid index
    int plateA, plateB;
    Vec3 normal;      // tangent, from A towards B
    double conv = 0;  // cm/yr, >0 closing
    double shear = 0; // cm/yr
    uint8_t type = 0, cls = 0;
    bool contA = false, contB = false;
    int subducting = -1;  // plate id
};

}  // namespace

void runTectonics(StageContext& ctx) {
    WorldData& w = ctx.world;
    const Params& P = ctx.params;
    const SphereGrid& g = w.grid;
    const int W = g.width(), H = g.height();
    const double R = g.radiusKm();
    const double activity = P.num("tectonics.activity");

    // ---------------------------------------------------------------- plates
    Pcg64 rng = ctx.seed.stream("tectonics.plates");
    int nPlates = rng.rangeInt(P.integer("tectonics.plateCountMin"), P.integer("tectonics.plateCountMax"));
    nPlates = std::clamp(nPlates, 2, 60);
    std::vector<PlateModel> plates(static_cast<size_t>(nPlates));
    {
        double minSep = 0.75 * std::sqrt(4.0 * kPi / nPlates);
        for (int k = 0; k < nPlates; ++k) {
            Vec3 best;
            for (int attempt = 0;; ++attempt) {
                Vec3 c = randomUnitVector(rng);
                bool ok = true;
                for (int q = 0; q < k; ++q)
                    if (angleBetween(c, plates[static_cast<size_t>(q)].seed) < minSep) { ok = false; break; }
                if (ok || attempt > 200) { best = c; break; }
                if (attempt % 40 == 39) minSep *= 0.9;
            }
            PlateModel& pm = plates[static_cast<size_t>(k)];
            pm.seed = best;
            // A few major plates and several smaller ones (lognormal growth weights).
            pm.weight = std::exp(rng.normal(0.0, 0.33)) * (k < std::max(2, nPlates / 3) ? 1.35 : 0.85);
            pm.pole = randomUnitVector(rng);
            pm.omegaRadPerMyr = rng.uniform(0.25, 0.95) * kDegToRad * activity;
            pm.rigidity = rng.uniform(0.6, 1.4);
        }
    }
    Noise3 warpA(ctx.seed.key64("tectonics.warpA")), warpB(ctx.seed.key64("tectonics.warpB"));
    std::vector<int32_t> plateFull(static_cast<size_t>(g.size()));
    ctx.pool.forRows(H, [&](int j) {
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            const Vec3& p = g.unit(k);
            Vec3 q = normalize(p + warpA.warp(p, 1.3, 4) * 0.30 + warpB.warp(p, 4.5, 3) * 0.07);
            double best = 1e18;
            int bi = 0;
            for (int s = 0; s < nPlates; ++s) {
                double d = angleBetween(q, plates[static_cast<size_t>(s)].seed) / plates[static_cast<size_t>(s)].weight;
                if (d < best) { best = d; bi = s; }
            }
            plateFull[static_cast<size_t>(k)] = bi;
        }
    }, &ctx.cancel);
    ctx.progress(0.15, "плиты");
    int repaired = enforceConnectedRegions(g, plateFull, nPlates);
    ctx.report.log("Плит: " + std::to_string(nPlates) + "; перераспределено фрагментов: " + std::to_string(repaired));

    // ---------------------------------------------------------------- continental crust
    Pcg64 crng = ctx.seed.stream("tectonics.crust");
    const int masses = P.integer("tectonics.landmassCount");
    const double frag = P.num("tectonics.fragmentation");
    const double crustFrac = P.num("tectonics.continentalCrust");
    const std::string polar = P.str("tectonics.polarMode");
    // Anisotropic blobs (ellipses in the local tangent plane) give elongated continents and peninsulas.
    struct Blob {
        Vec3 c, a, b;
        double ra, rb, weight;
        double eval(const Vec3& q) const {
            double cz = dot(q, c);
            if (cz <= 0) return -1.0;
            double x = dot(q, a) / ra, y = dot(q, b) / rb;
            return (1.0 - std::sqrt(x * x + y * y)) * weight;
        }
    };
    auto makeBlob = [](Pcg64& r, const Vec3& c, double radius, double weight, double maxAspect) {
        Blob bl;
        bl.c = c;
        bl.a = normalize(cross(c, randomUnitVector(r)));
        bl.b = cross(c, bl.a);
        double aspect = r.uniform(1.0, maxAspect);
        bl.ra = std::sin(std::min(1.4, radius * std::sqrt(aspect)));
        bl.rb = std::sin(std::min(1.4, radius / std::sqrt(aspect)));
        bl.weight = weight;
        return bl;
    };
    std::vector<Blob> blobs;
    {
        // Area share of each landmass cluster: unequal sizes like real continents.
        std::vector<double> share(static_cast<size_t>(masses));
        double sum = 0;
        for (auto& s : share) { s = std::pow(crng.uniform(0.35, 1.6), 1.6); sum += s; }
        double clusterArea = crustFrac * (1.0 - 0.45 * frag);  // fragmentation moves area into small islands
        std::vector<Vec3> centers;
        double sep = masses > 1 ? 0.9 * std::sqrt(4.0 * kPi / masses) : 0.0;
        for (int b = 0; b < masses; ++b) {
            Vec3 c;
            bool fixedPole = (polar == "north" && b == 0) || (polar == "south" && b == 0) || (polar == "both" && b < 2);
            if (fixedPole) {
                double lat = (polar == "south" || (polar == "both" && b == 1)) ? -90.0 : 90.0;
                lat += (lat > 0 ? -1 : 1) * crng.uniform(0, 6);
                c = fromLatLon(lat * kDegToRad, crng.uniform(-kPi, kPi));
            } else {
                for (int attempt = 0;; ++attempt) {
                    c = randomUnitVector(crng);
                    double lat = latOf(c) * kRadToDeg;
                    bool ok = !(polar == "none" && std::fabs(lat) > 48) && !((polar == "north" || polar == "south" || polar == "both") && std::fabs(lat) > 62);
                    for (auto& o : centers) if (angleBetween(o, c) < sep) ok = false;
                    if (ok || attempt > 300) break;
                    if (attempt % 50 == 49) sep *= 0.9;
                }
            }
            centers.push_back(c);
            double area = 4.0 * kPi * clusterArea * share[static_cast<size_t>(b)] / sum;
            double theta = std::acos(std::clamp(1.0 - area / (2.0 * kPi), -1.0, 1.0));
            int nSub = 4 + static_cast<int>(crng.below(5)) + static_cast<int>(frag * 3);
            for (int s = 0; s < nSub; ++s) {
                Vec3 axis = normalize(cross(c, randomUnitVector(crng)));
                Vec3 sc = s == 0 ? c : rotateAbout(c, axis, theta * crng.uniform(0.2, 1.0));
                double rr = theta * (s == 0 ? crng.uniform(0.7, 0.9) : crng.uniform(0.3, 0.7));
                blobs.push_back(makeBlob(crng, sc, rr, 1.0, 2.8));
            }
        }
        // Small islands / micro-continents grow with fragmentation.
        int islands = static_cast<int>(std::round(frag * frag * 28 + frag * 6));
        for (int s = 0; s < islands; ++s) {
            Vec3 c = randomUnitVector(crng);
            if (polar == "none" && std::fabs(latOf(c)) > 55 * kDegToRad) continue;
            double area = 4.0 * kPi * crustFrac * 0.45 * frag / std::max(1, islands) * crng.uniform(0.3, 1.7);
            blobs.push_back(makeBlob(crng, c, std::acos(std::clamp(1.0 - area / (2.0 * kPi), -1.0, 1.0)) * 1.2, 0.9, 3.0));
        }
    }
    Noise3 cw(ctx.seed.key64("tectonics.crustWarp")), cn(ctx.seed.key64("tectonics.crustNoise"));
    std::vector<float> contField(static_cast<size_t>(g.size()));
    const double noiseAmp = 0.30 + 0.45 * frag;
    const double noiseFreq = 2.6 + 4.0 * frag;
    ctx.pool.forRows(H, [&](int j) {
        double latAbs = std::fabs(g.latDeg(j));
        double polarMask = polar == "none" ? 1.0 - smoothstep(58.0, 72.0, latAbs) : 1.0;
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            const Vec3& p = g.unit(k);
            Vec3 q = normalize(p + cw.warp(p, 1.8, 4) * 0.22 + cw.warp(p * 3.1, 5.0, 3) * 0.05);
            double f = -1.0;
            for (auto& b : blobs) f = std::max(f, b.eval(q));
            // Fractal edge detail: bays, peninsulas and offshore islands at many scales.
            f += noiseAmp * cn.fbm(p, noiseFreq, 7, 2.1, 0.6);
            if (polarMask < 1.0) f = f * polarMask - (1.0 - polarMask);
            contField[static_cast<size_t>(k)] = static_cast<float>(f);
        }
    }, &ctx.cancel);
    double tau = areaWeightedQuantile(g, contField, 1.0 - crustFrac);
    // Remove specks: continental fragments and oceanic holes smaller than ~3 cells / 20 000 km² are flipped
    // (they would become isolated needles of continental-slope relief).
    {
        const double minArea = std::max(20000.0 * sqr(R / 6371.0), 3.0 * g.cellAreaKm2(H / 2));
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<uint8_t> m(static_cast<size_t>(g.size()));
            for (int64_t k = 0; k < g.size(); ++k) m[static_cast<size_t>(k)] = (contField[static_cast<size_t>(k)] > tau) == (pass == 0);
            std::vector<int32_t> lab;
            int nc = connectedComponents(g, m.data(), lab);
            std::vector<double> ar(static_cast<size_t>(nc), 0.0);
            for (int64_t k = 0; k < g.size(); ++k) if (lab[static_cast<size_t>(k)] >= 0) ar[static_cast<size_t>(lab[static_cast<size_t>(k)])] += g.cellAreaKm2(g.row(k));
            for (int64_t k = 0; k < g.size(); ++k) {
                int32_t c = lab[static_cast<size_t>(k)];
                if (c >= 0 && ar[static_cast<size_t>(c)] < minArea)
                    contField[static_cast<size_t>(k)] = static_cast<float>(pass == 0 ? tau - 0.01 : tau + 0.01);
            }
        }
    }
    ctx.progress(0.3, "континентальная кора");

    // Per-plate continental fraction -> slower continental plates, rigidity.
    {
        std::vector<double> area(static_cast<size_t>(nPlates), 0.0), cont(static_cast<size_t>(nPlates), 0.0);
        for (int j = 0; j < H; ++j)
            for (int i = 0; i < W; ++i) {
                int64_t k = g.index(i, j);
                int pid = plateFull[static_cast<size_t>(k)];
                area[static_cast<size_t>(pid)] += g.cellAreaKm2(j);
                if (contField[static_cast<size_t>(k)] > tau) cont[static_cast<size_t>(pid)] += g.cellAreaKm2(j);
            }
        for (int s = 0; s < nPlates; ++s) {
            double cf = area[static_cast<size_t>(s)] > 0 ? cont[static_cast<size_t>(s)] / area[static_cast<size_t>(s)] : 0;
            plates[static_cast<size_t>(s)].omegaRadPerMyr *= (1.0 - 0.45 * cf);
            plates[static_cast<size_t>(s)].rigidity += 0.4 * cf;
        }
    }

    // ---------------------------------------------------------------- structural grid
    const int Ws = std::min(W, 1024);
    SphereGrid sg(Ws, Ws / 2, R);
    const int Hs = sg.height();
    std::vector<int32_t> ps(static_cast<size_t>(sg.size()));
    std::vector<uint8_t> contS(static_cast<size_t>(sg.size()));
    for (int j = 0; j < Hs; ++j)
        for (int i = 0; i < Ws; ++i) {
            int64_t f = g.cellAt(sg.latDeg(j), sg.lonDeg(i));
            ps[static_cast<size_t>(sg.index(i, j))] = plateFull[static_cast<size_t>(f)];
            contS[static_cast<size_t>(sg.index(i, j))] = contField[static_cast<size_t>(f)] > tau ? 1 : 0;
        }

    // Boundary cells with normals and relative motion.
    std::vector<BoundaryCell> bcells;
    std::vector<int32_t> bindex(static_cast<size_t>(sg.size()), -1);
    const int win = std::max(2, Ws / 340);
    for (int j = 0; j < Hs; ++j)
        for (int i = 0; i < Ws; ++i) {
            int64_t c = sg.index(i, j);
            int a = ps[static_cast<size_t>(c)];
            std::map<int, int> other;
            for (int d = 0; d < 8; ++d) {
                int b = ps[static_cast<size_t>(sg.neighbor(i, j, d))];
                if (b != a) other[b]++;
            }
            if (other.empty()) continue;
            int b = other.begin()->first, bc = 0;
            for (auto& [pid, cnt] : other) if (cnt > bc) { b = pid; bc = cnt; }
            const Vec3& pc = sg.unit(c);
            Vec3 n{0, 0, 0};
            int na = 0, nb = 0, ca = 0, cb = 0;
            for (int dj = -win; dj <= win; ++dj)
                for (int di = -win; di <= win; ++di) {
                    int jj = j + dj;
                    int ii = i + di;
                    if (jj < 0 || jj >= Hs) continue;
                    int64_t q = sg.index(sg.wrapCol(ii), jj);
                    int pq = ps[static_cast<size_t>(q)];
                    if (pq != a && pq != b) continue;
                    Vec3 t = sg.unit(q) - pc * dot(sg.unit(q), pc);
                    double l = length(t);
                    if (l < 1e-12) continue;
                    t = t / l;
                    if (pq == b) { n += t; ++nb; cb += contS[static_cast<size_t>(q)]; }
                    else { n -= t; ++na; ca += contS[static_cast<size_t>(q)]; }
                }
            n = n - pc * dot(n, pc);
            if (length(n) < 1e-9) n = eastAt(pc);
            n = normalize(n);
            BoundaryCell bcell;
            bcell.cell = c;
            bcell.plateA = a;
            bcell.plateB = b;
            bcell.normal = n;
            Vec3 rel = plateVelocity(plates[static_cast<size_t>(a)], pc, R) - plateVelocity(plates[static_cast<size_t>(b)], pc, R);
            rel = rel * 0.1;  // km/Myr -> cm/yr
            bcell.conv = dot(rel, n);
            bcell.shear = length(rel - n * bcell.conv);
            bcell.contA = na > 0 && 2 * ca > na;
            bcell.contB = nb > 0 && 2 * cb > nb;
            bindex[static_cast<size_t>(c)] = static_cast<int32_t>(bcells.size());
            bcells.push_back(bcell);
        }
    // Smooth motion components along the boundary (same plate pair) to remove normal-estimation noise.
    {
        std::vector<double> sc(bcells.size()), ss(bcells.size());
        const int sw = std::max(2, Ws / 256);
        for (size_t idx = 0; idx < bcells.size(); ++idx) {
            const BoundaryCell& bc = bcells[idx];
            int i = sg.col(bc.cell), j = sg.row(bc.cell);
            double sumC = 0, sumS = 0, n = 0;
            for (int dj = -sw; dj <= sw; ++dj)
                for (int di = -sw; di <= sw; ++di) {
                    int jj = j + dj;
                    if (jj < 0 || jj >= Hs) continue;
                    int32_t o = bindex[static_cast<size_t>(sg.index(sg.wrapCol(i + di), jj))];
                    if (o < 0) continue;
                    const BoundaryCell& ob = bcells[static_cast<size_t>(o)];
                    double sgn = 0;
                    if (ob.plateA == bc.plateA && ob.plateB == bc.plateB) sgn = 1;
                    else if (ob.plateA == bc.plateB && ob.plateB == bc.plateA) sgn = 1;  // same motion seen from the other side
                    if (sgn == 0) continue;
                    sumC += ob.conv;
                    sumS += ob.shear;
                    n += 1;
                }
            sc[idx] = n > 0 ? sumC / n : bc.conv;
            ss[idx] = n > 0 ? sumS / n : bc.shear;
        }
        for (size_t idx = 0; idx < bcells.size(); ++idx) { bcells[idx].conv = sc[idx]; bcells[idx].shear = ss[idx]; }
    }
    for (auto& bc : bcells) {
        double mag = std::hypot(bc.conv, bc.shear);
        double theta = std::atan2(bc.conv, bc.shear) * kRadToDeg;  // +90 pure convergence, -90 pure divergence
        if (mag < 0.4) bc.type = kBndComplex;
        else if (theta > 50) bc.type = kBndConvergent;
        else if (theta < -50) bc.type = kBndDivergent;
        else if (std::fabs(theta) < 25) bc.type = kBndTransform;
        else bc.type = kBndComplex;
        bc.cls = (bc.contA && bc.contB) ? kBcContinentContinent : ((bc.contA || bc.contB) ? kBcOceanContinent : kBcOceanOcean);
    }
    ctx.progress(0.45, "типы границ");

    // ---------------------------------------------------------------- distances on the structural grid
    std::vector<int32_t> region(ps.begin(), ps.end());
    auto transform = [&](auto pred, std::vector<float>& dist, std::vector<int32_t>& src, std::vector<int64_t>& sources) {
        sources.clear();
        std::vector<int32_t> owner;
        for (size_t idx = 0; idx < bcells.size(); ++idx)
            if (pred(bcells[idx])) { sources.push_back(bcells[idx].cell); owner.push_back(static_cast<int32_t>(idx)); }
        distanceTransform(sg, sources, dist, &src, 6000.0, nullptr, region.data());
        for (auto& s : src) s = s >= 0 ? owner[static_cast<size_t>(s)] : -1;  // map to boundary-cell index
    };
    std::vector<float> dRidge, dSub, dTrench, dColl, dTrans, dAny;
    std::vector<int32_t> sRidge, sSub, sTrench, sColl, sTrans, sAny;
    std::vector<int64_t> tmp;
    transform([](const BoundaryCell& b) { return b.type == kBndDivergent; }, dRidge, sRidge, tmp);
    // Oceanic crust age from distance to the spreading ridge and the half-spreading rate.
    std::vector<float> ageS(static_cast<size_t>(sg.size()), 0.0f);
    Noise3 an(ctx.seed.key64("tectonics.age"));
    for (int64_t k = 0; k < sg.size(); ++k) {
        double age;
        if (sRidge[static_cast<size_t>(k)] >= 0) {
            double half = std::max(0.6, -bcells[static_cast<size_t>(sRidge[static_cast<size_t>(k)])].conv / 2.0);
            age = dRidge[static_cast<size_t>(k)] / (half * 10.0);
        } else {
            age = 110 + 50 * an.fbm(sg.unit(k), 2, 3);
        }
        ageS[static_cast<size_t>(k)] = static_cast<float>(std::clamp(age, 0.0, 200.0));
    }
    // Subduction polarity.
    for (auto& bc : bcells) {
        if (bc.type != kBndConvergent && !(bc.type == kBndComplex && bc.conv > 0.5)) continue;
        if (bc.cls == kBcContinentContinent) continue;
        if (bc.cls == kBcOceanContinent) {
            bc.subducting = bc.contA ? bc.plateB : bc.plateA;
        } else {
            // Ocean–ocean: the older, denser lithosphere sinks (age sampled on both sides).
            int i = sg.col(bc.cell), j = sg.row(bc.cell);
            double ageA = 0, ageB = 0;
            int na = 0, nb = 0;
            for (int dj = -win; dj <= win; ++dj)
                for (int di = -win; di <= win; ++di) {
                    int jj = j + dj;
                    if (jj < 0 || jj >= Hs) continue;
                    int64_t q = sg.index(sg.wrapCol(i + di), jj);
                    if (ps[static_cast<size_t>(q)] == bc.plateA) { ageA += ageS[static_cast<size_t>(q)]; ++na; }
                    else if (ps[static_cast<size_t>(q)] == bc.plateB) { ageB += ageS[static_cast<size_t>(q)]; ++nb; }
                }
            ageA /= std::max(1, na);
            ageB /= std::max(1, nb);
            if (std::fabs(ageA - ageB) > 3.0) bc.subducting = ageA > ageB ? bc.plateA : bc.plateB;
            else bc.subducting = plates[static_cast<size_t>(bc.plateA)].weight < plates[static_cast<size_t>(bc.plateB)].weight ? bc.plateA : bc.plateB;
        }
    }
    // Make polarity consistent along each plate-pair boundary (majority vote in a window).
    {
        std::vector<int> fixedSub(bcells.size());
        const int sw = std::max(3, Ws / 160);
        for (size_t idx = 0; idx < bcells.size(); ++idx) {
            const BoundaryCell& bc = bcells[idx];
            fixedSub[idx] = bc.subducting;
            if (bc.subducting < 0 || bc.cls != kBcOceanOcean) continue;
            int i = sg.col(bc.cell), j = sg.row(bc.cell);
            int votesA = 0, votesB = 0;
            int lo = std::min(bc.plateA, bc.plateB), hi = std::max(bc.plateA, bc.plateB);
            for (int dj = -sw; dj <= sw; ++dj)
                for (int di = -sw; di <= sw; ++di) {
                    int jj = j + dj;
                    if (jj < 0 || jj >= Hs) continue;
                    int32_t o = bindex[static_cast<size_t>(sg.index(sg.wrapCol(i + di), jj))];
                    if (o < 0) continue;
                    const BoundaryCell& ob = bcells[static_cast<size_t>(o)];
                    if (std::min(ob.plateA, ob.plateB) != lo || std::max(ob.plateA, ob.plateB) != hi || ob.subducting < 0) continue;
                    (ob.subducting == lo ? votesA : votesB)++;
                }
            fixedSub[idx] = votesA >= votesB ? lo : hi;
        }
        for (size_t idx = 0; idx < bcells.size(); ++idx) bcells[idx].subducting = fixedSub[idx];
    }
    auto isSubduction = [](const BoundaryCell& b) { return b.subducting >= 0; };
    transform([&](const BoundaryCell& b) { return isSubduction(b) && ps[static_cast<size_t>(b.cell)] != b.subducting; }, dSub, sSub, tmp);
    transform([&](const BoundaryCell& b) { return isSubduction(b) && ps[static_cast<size_t>(b.cell)] == b.subducting; }, dTrench, sTrench, tmp);
    transform([&](const BoundaryCell& b) {
        return (b.type == kBndConvergent || (b.type == kBndComplex && b.conv > 0.5)) && b.cls == kBcContinentContinent;
    }, dColl, sColl, tmp);
    transform([](const BoundaryCell& b) { return b.type == kBndTransform || (b.type == kBndComplex && b.subducting < 0); }, dTrans, sTrans, tmp);
    transform([](const BoundaryCell&) { return true; }, dAny, sAny, tmp);
    ctx.progress(0.7, "расстояния до границ");

    // Continent–ocean crust boundary distance (signed, + inside continental crust), unrestricted by plates.
    std::vector<float> cobS(static_cast<size_t>(sg.size()));
    {
        std::vector<int64_t> srcIn, srcOut;
        for (int j = 0; j < Hs; ++j)
            for (int i = 0; i < Ws; ++i) {
                int64_t c = sg.index(i, j);
                bool edge = false;
                for (int d = 0; d < 8 && !edge; ++d) edge = contS[static_cast<size_t>(sg.neighbor(i, j, d))] != contS[static_cast<size_t>(c)];
                if (!edge) continue;
                (contS[static_cast<size_t>(c)] ? srcIn : srcOut).push_back(c);
            }
        std::vector<float> dIn, dOut;
        distanceTransform(sg, srcOut, dIn, nullptr, 8000);   // distance (for continental cells) to the nearest oceanic edge cell
        distanceTransform(sg, srcIn, dOut, nullptr, 8000);
        double half = 0.5 * sg.dyKm();
        for (int64_t k = 0; k < sg.size(); ++k)
            cobS[static_cast<size_t>(k)] = contS[static_cast<size_t>(k)] ? static_cast<float>(dIn[static_cast<size_t>(k)] - half)
                                                                         : -static_cast<float>(dOut[static_cast<size_t>(k)] - half);
    }

    // ---------------------------------------------------------------- full-grid layers
    auto up = [&](const std::vector<float>& s) {
        auto l = std::make_shared<std::vector<float>>(static_cast<size_t>(g.size()));
        if (Ws == W) std::copy(s.begin(), s.end(), l->begin());
        else upsampleBilinear(sg, s.data(), g, l->data());
        return l;
    };
    auto structOf = [&](int64_t k) {
        int i = g.col(k), j = g.row(k);
        return sg.index(std::min(Ws - 1, i * Ws / W), std::min(Hs - 1, j * Hs / H));
    };
    auto putF = [&](LayerId id, const std::vector<float>& v) {
        auto l = w.makeLayer(id);
        std::copy(v.begin(), v.end(), l->as<float>());
        w.setLayer(id, l);
    };
    auto dSubF = up(dSub), dTrenchF = up(dTrench), dCollF = up(dColl), dRidgeF = up(dRidge), dTransF = up(dTrans),
         dAnyF = up(dAny), cobF = up(cobS), ageF = up(ageS);

    auto Lplate = w.makeLayer(LayerId::PlateId);
    auto Lcont = w.makeLayer(LayerId::Continentality);
    auto Lcrust = w.makeLayer(LayerId::CrustType);
    auto Lage = w.makeLayer(LayerId::CrustAge);
    auto Lbt = w.makeLayer(LayerId::BoundaryType);
    auto Lbc = w.makeLayer(LayerId::BoundaryClass);
    auto Lve = w.makeLayer(LayerId::PlateVelE);
    auto Lvn = w.makeLayer(LayerId::PlateVelN);
    auto Lside = w.makeLayer(LayerId::TectonicSide);
    auto Lint = w.makeLayer(LayerId::TectonicIntensity);
    auto Lconv = w.makeLayer(LayerId::ConvRate);
    auto Ldiv = w.makeLayer(LayerId::DivRate);
    uint16_t* plateL = Lplate->as<uint16_t>();
    float* contL = Lcont->as<float>();
    uint8_t* crustL = Lcrust->as<uint8_t>();
    float* ageL = Lage->as<float>();
    uint8_t* btL = Lbt->as<uint8_t>();
    uint8_t* bcL = Lbc->as<uint8_t>();
    float* veL = Lve->as<float>();
    float* vnL = Lvn->as<float>();
    uint8_t* sideL = Lside->as<uint8_t>();
    float* intL = Lint->as<float>();
    float* convL = Lconv->as<float>();
    float* divL = Ldiv->as<float>();

    // Ancient sutures (old orogenic belts) inside continental crust, used by stage 2.
    auto tf = std::make_shared<TectonicsFeatures>();
    {
        Pcg64 srng = ctx.seed.stream("tectonics.sutures");
        int nSut = 2 + masses + static_cast<int>(srng.below(3));
        for (int s = 0, tries = 0; s < nSut && tries < 500; ++tries) {
            int64_t k = static_cast<int64_t>(srng.below(static_cast<uint64_t>(g.size())));
            if (contField[static_cast<size_t>(k)] <= tau + 0.05) continue;
            Vec3 c = g.unit(k);
            Vec3 axis = normalize(cross(c, randomUnitVector(srng)));
            double len = srng.uniform(12, 35) * kDegToRad;
            Polyline line;
            for (int t = 0; t <= 40; ++t) {
                Vec3 p = rotateAbout(c, axis, (t / 40.0 - 0.5) * len);
                line.pts.push_back({latOf(p) * kRadToDeg, lonOf(p) * kRadToDeg});
            }
            tf->oldSutures.push_back(line);
            ++s;
        }
    }

    ctx.pool.forRows(H, [&](int j) {
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            int pid = plateFull[static_cast<size_t>(k)];
            const Vec3& p = g.unit(k);
            plateL[k] = static_cast<uint16_t>(pid);
            double c = contField[static_cast<size_t>(k)] - tau;
            contL[k] = static_cast<float>(1.0 / (1.0 + std::exp(-c / 0.04)));
            bool continental = c > 0;
            crustL[k] = continental ? kCrustContinental : kCrustOceanic;
            ageL[k] = continental ? notComputed() : (*ageF)[static_cast<size_t>(k)];
            Vec3 v = plateVelocity(plates[static_cast<size_t>(pid)], p, R) * 0.1;
            veL[k] = static_cast<float>(dot(v, eastAt(p)));
            vnL[k] = static_cast<float>(dot(v, northAt(p)));
            // Boundary type at full resolution: cells touching another plate inherit the nearest structural boundary.
            bool isB = false;
            for (int d = 0; d < 8 && !isB; ++d) isB = plateFull[static_cast<size_t>(g.neighbor(i, j, d))] != pid;
            int64_t sc = structOf(k);
            int32_t nearestAny = sAny[static_cast<size_t>(sc)];
            if (isB && nearestAny >= 0) {
                btL[k] = bcells[static_cast<size_t>(nearestAny)].type;
                bcL[k] = bcells[static_cast<size_t>(nearestAny)].cls;
            }
            int32_t sCv = sSub[static_cast<size_t>(sc)] >= 0 ? sSub[static_cast<size_t>(sc)]
                          : (sColl[static_cast<size_t>(sc)] >= 0 ? sColl[static_cast<size_t>(sc)] : sTrench[static_cast<size_t>(sc)]);
            convL[k] = sCv >= 0 ? static_cast<float>(std::max(0.0, bcells[static_cast<size_t>(sCv)].conv)) : 0.0f;
            int32_t sDv = sRidge[static_cast<size_t>(sc)];
            divL[k] = sDv >= 0 ? static_cast<float>(std::max(0.0, -bcells[static_cast<size_t>(sDv)].conv)) : 0.0f;
            // Role relative to the nearest active margin.
            double dS = (*dSubF)[static_cast<size_t>(k)], dT = (*dTrenchF)[static_cast<size_t>(k)], dC = (*dCollF)[static_cast<size_t>(k)];
            double dR = (*dRidgeF)[static_cast<size_t>(k)], dX = (*dTransF)[static_cast<size_t>(k)];
            uint8_t side = kSideInterior;
            double best = 1e18;
            auto consider = [&](double d, double width, uint8_t s) {
                if (d < width && d / width < best) { best = d / width; side = s; }
            };
            consider(dS, 450, kSideOverriding);
            consider(dT, 160, kSideSubducting);
            consider(dC, 500, kSideCollision);
            consider(dR, continental ? 220 : 160, continental ? kSideRift : kSideRidge);
            consider(dX, 120, kSideTransform);
            sideL[k] = side;
            double cr = convL[k] / 5.0, dr = divL[k] / 5.0;
            double inten = std::max({std::exp(-dS / 350.0) * std::min(1.0, 0.3 + cr), std::exp(-dT / 120.0) * std::min(1.0, 0.3 + cr),
                                     std::exp(-dC / 450.0) * std::min(1.0, 0.4 + cr), std::exp(-dR / 150.0) * std::min(1.0, 0.3 + dr),
                                     std::exp(-dX / 100.0) * 0.6});
            intL[k] = static_cast<float>(std::clamp(inten, 0.0, 1.0));
        }
    }, &ctx.cancel);
    w.setLayer(LayerId::PlateId, Lplate);
    w.setLayer(LayerId::Continentality, Lcont);
    w.setLayer(LayerId::CrustType, Lcrust);
    w.setLayer(LayerId::CrustAge, Lage);
    w.setLayer(LayerId::BoundaryType, Lbt);
    w.setLayer(LayerId::BoundaryClass, Lbc);
    w.setLayer(LayerId::PlateVelE, Lve);
    w.setLayer(LayerId::PlateVelN, Lvn);
    w.setLayer(LayerId::TectonicSide, Lside);
    w.setLayer(LayerId::TectonicIntensity, Lint);
    w.setLayer(LayerId::ConvRate, Lconv);
    w.setLayer(LayerId::DivRate, Ldiv);
    putF(LayerId::DistSubduction, *dSubF);
    putF(LayerId::DistTrench, *dTrenchF);
    putF(LayerId::DistCollision, *dCollF);
    putF(LayerId::DistRidge, *dRidgeF);
    putF(LayerId::DistTransform, *dTransF);
    putF(LayerId::DistBoundary, *dAnyF);
    putF(LayerId::DistCOB, *cobF);
    ctx.progress(0.85, "слои");

    // ---------------------------------------------------------------- features & report
    tf->crustThreshold = tau;
    std::vector<double> area(static_cast<size_t>(nPlates), 0), cont(static_cast<size_t>(nPlates), 0);
    std::vector<Vec3> centroid(static_cast<size_t>(nPlates), Vec3{0, 0, 0});
    double contArea = 0;
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            int pid = plateL[k];
            double a = g.cellAreaKm2(j);
            area[static_cast<size_t>(pid)] += a;
            if (crustL[k]) { cont[static_cast<size_t>(pid)] += a; contArea += a; }
            centroid[static_cast<size_t>(pid)] += g.unit(k) * a;
        }
    for (int s = 0; s < nPlates; ++s) {
        const PlateModel& pm = plates[static_cast<size_t>(s)];
        Plate pl;
        pl.id = s;
        pl.seedLat = latOf(pm.seed) * kRadToDeg;
        pl.seedLon = lonOf(pm.seed) * kRadToDeg;
        pl.poleLat = latOf(pm.pole) * kRadToDeg;
        pl.poleLon = lonOf(pm.pole) * kRadToDeg;
        pl.omegaDegPerMyr = pm.omegaRadPerMyr * kRadToDeg;
        pl.weight = pm.weight;
        pl.areaKm2 = area[static_cast<size_t>(s)];
        pl.continentalFraction = area[static_cast<size_t>(s)] > 0 ? cont[static_cast<size_t>(s)] / area[static_cast<size_t>(s)] : 0;
        pl.rigidity = pm.rigidity;
        Vec3 c = normalize(centroid[static_cast<size_t>(s)]);
        pl.centroidLat = latOf(c) * kRadToDeg;
        pl.centroidLon = lonOf(c) * kRadToDeg;
        pl.meanSpeedCmYr = length(plateVelocity(pm, c, R)) * 0.1;
        tf->plates.push_back(pl);
    }
    tf->boundaries = traceBoundarySegments(sg, ps, bcells.size(),
        [&](size_t idx) { return bcells[idx].cell; },
        [&](size_t idx, BoundarySegment& seg) {
            const BoundaryCell& b = bcells[idx];
            seg.plateA = b.plateA; seg.plateB = b.plateB; seg.type = b.type; seg.cls = b.cls;
            seg.convergenceCmYr = static_cast<float>(b.conv); seg.shearCmYr = static_cast<float>(b.shear);
            seg.subductingPlate = b.subducting;
        });
    w.tectonics = tf;

    // Quality checks.
    int64_t holes = 0;
    for (int64_t k = 0; k < g.size(); ++k) if (plateL[k] >= nPlates) ++holes;
    ctx.report.check("plates.coverage", "Глобальное покрытие плитами без дыр", holes == 0, static_cast<double>(holes), "0 непокрытых ячеек", true);
    {
        std::vector<int32_t> lab;
        int disconnected = 0;
        for (int s = 0; s < nPlates; ++s) {
            std::vector<uint8_t> m(static_cast<size_t>(g.size()));
            for (int64_t k = 0; k < g.size(); ++k) m[static_cast<size_t>(k)] = plateL[k] == s;
            if (connectedComponents(g, m.data(), lab) > 1) ++disconnected;
        }
        ctx.report.check("plates.connected", "Каждая плита связна (с учётом шва и полюсов)", disconnected == 0, disconnected, "0 несвязных плит", true);
    }
    double frac = contArea / g.totalAreaKm2();
    ctx.report.check("crust.fraction", "Доля континентальной коры близка к заданной", std::fabs(frac - crustFrac) < 0.03, frac,
                     "заданная ± 0,03");
    int counts[5] = {0, 0, 0, 0, 0};
    for (auto& b : bcells) counts[b.type]++;
    double nb = std::max<size_t>(1, bcells.size());
    ctx.report.metric("plates", nPlates);
    ctx.report.metric("continentalCrustFraction", frac);
    ctx.report.metric("boundaryDivergentShare", counts[kBndDivergent] / nb);
    ctx.report.metric("boundaryConvergentShare", counts[kBndConvergent] / nb);
    ctx.report.metric("boundaryTransformShare", counts[kBndTransform] / nb);
    ctx.report.metric("boundaryComplexShare", counts[kBndComplex] / nb);
    ctx.report.check("boundaries.mixed", "Есть и дивергентные, и конвергентные границы",
                     counts[kBndDivergent] > 0 && counts[kBndConvergent] > 0, counts[kBndDivergent] * 1000.0 + counts[kBndConvergent],
                     "оба типа присутствуют");
    double maxSpeed = 0;
    for (auto& pl : tf->plates) maxSpeed = std::max(maxSpeed, pl.meanSpeedCmYr);
    ctx.report.metric("maxPlateSpeedCmYr", maxSpeed);
    ctx.report.log("Скорости плит условные (см/год), статическая реконструкция — не расчёт долгосрочной эволюции.");
    ctx.progress(1.0, "тектоника");
}

}  // namespace mapgen
