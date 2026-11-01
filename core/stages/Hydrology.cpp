// Stage 5: hydrology on the final DEM.
//
//  1. Priority-flood from the ocean (Barnes et al. 2014) gives a depression-filled surface and an
//     acyclic drainage tree (steepest descent where possible, flood order on flats).
//  2. Every closed depression gets a steady water balance: inflow from its catchment (climatic runoff)
//     against net open-water evaporation of the lake surface as the level rises (hypsometry). If the
//     inflow exceeds the loss at the spill level the lake overflows (outflow lake); otherwise it is a
//     terminal (endorheic) lake at the balance level, possibly only seasonal / a salt pan.
//  3. The final receivers drain terminal basins towards their lakes; discharge is accumulated with
//     lake gains/losses (mass is conserved up to explained evaporative losses).
//  4. Rivers: cells above a discharge threshold; Strahler order, seasonality, basins, falls/rapids.
//  5. Vector rivers follow their cells (Chaikin smoothing between adjacent cell centres never leaves
//     the corridor, so a line cannot cut through a watershed); deltas are a separate distributary
//     graph at suitable mouths; wetlands / floodplains / peat / salt flats from slope and water balance.
// Nested sub-depressions of a terminal basin share its balance level (documented approximation).

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>

#include "core/stages/Erosion.h"
#include "core/stages/Stage.h"
#include "core/stages/StageUtil.h"
#include "core/world/Categories.h"

namespace mapgen {

namespace {

constexpr double kSecondsPerYear = 31557600.0;
constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

int dirTo(const SphereGrid& g, int64_t from, int64_t to) {
    int i = g.col(from), j = g.row(from);
    for (int d = 0; d < 8; ++d)
        if (g.neighbor(i, j, d) == to) return d;
    return -1;
}

std::vector<int64_t> topoOrder(const std::vector<int64_t>& rec) {
    const int64_t n = static_cast<int64_t>(rec.size());
    std::vector<int64_t> start(static_cast<size_t>(n) + 1, 0), donors(static_cast<size_t>(n));
    for (int64_t k = 0; k < n; ++k)
        if (rec[static_cast<size_t>(k)] >= 0) start[static_cast<size_t>(rec[static_cast<size_t>(k)]) + 1]++;
    for (int64_t k = 0; k < n; ++k) start[static_cast<size_t>(k) + 1] += start[static_cast<size_t>(k)];
    std::vector<int64_t> pos(start.begin(), start.end() - 1);
    for (int64_t k = 0; k < n; ++k) {
        int64_t r = rec[static_cast<size_t>(k)];
        if (r >= 0) donors[static_cast<size_t>(pos[static_cast<size_t>(r)]++)] = k;
    }
    std::vector<int64_t> order;
    order.reserve(static_cast<size_t>(n));
    for (int64_t k = 0; k < n; ++k)
        if (rec[static_cast<size_t>(k)] < 0) order.push_back(k);
    for (size_t q = 0; q < order.size(); ++q) {
        int64_t c = order[q];
        for (int64_t e = start[static_cast<size_t>(c)]; e < start[static_cast<size_t>(c) + 1]; ++e) order.push_back(donors[static_cast<size_t>(e)]);
    }
    return order;  // receivers before donors; cells in cycles (none expected) would be missing
}

Polyline chaikin(const Polyline& in, int iterations) {
    Polyline cur = in;
    for (int it = 0; it < iterations && cur.pts.size() > 2; ++it) {
        Polyline out;
        out.pts.push_back(cur.pts.front());
        for (size_t i = 0; i + 1 < cur.pts.size(); ++i) {
            const GeoPoint& a = cur.pts[i];
            const GeoPoint& b = cur.pts[i + 1];
            out.pts.push_back({0.75 * a.lat + 0.25 * b.lat, 0.75 * a.lon + 0.25 * b.lon});
            out.pts.push_back({0.25 * a.lat + 0.75 * b.lat, 0.25 * a.lon + 0.75 * b.lon});
        }
        out.pts.push_back(cur.pts.back());
        cur = std::move(out);
    }
    return cur;
}

}  // namespace

void runHydrology(StageContext& ctx) {
    WorldData& w = ctx.world;
    const Params& P = ctx.params;
    const SphereGrid& g = w.grid;
    const int W = g.width(), H = g.height();
    const int64_t N = g.size();
    const double sea = w.seaLevel();
    const uint8_t* ocean = w.u8(LayerId::OceanMask);
    const float* bed = w.f32(LayerId::Elevation);
    const float* ice = w.f32(LayerId::IceThickness);
    const uint8_t* iceT = w.u8(LayerId::IceType);
    const float* runoffA = w.f32(LayerId::RunoffAnnual);
    const uint16_t* runoffM = w.u16(LayerId::RunoffMonthly);
    const float* precA = w.f32(LayerId::PrecAnnual);
    const float* petA = w.f32(LayerId::PetAnnual);
    const float* aridity = w.f32(LayerId::Aridity);
    const float* tmean = w.f32(LayerId::TempMean);
    const uint8_t* lgm = w.u8(LayerId::LgmIce);
    const uint8_t* gen = w.u8(LayerId::Genesis);
    const uint8_t* landform = w.u8(LayerId::Landform);
    const double qThreshold = P.num("hydro.riverThreshold");

    // ---- surface used for routing (ice sheets route over their surface)
    std::vector<float> z(static_cast<size_t>(N));
    std::vector<uint8_t> base(static_cast<size_t>(N));
    for (int64_t k = 0; k < N; ++k) {
        double h = bed[k];
        if (iceT && (iceT[k] == kIceSheet || iceT[k] == kIceGlacier)) h += ice[k];
        z[static_cast<size_t>(k)] = static_cast<float>(h);
        base[static_cast<size_t>(k)] = ocean[k] ? 1 : 0;
    }
    std::vector<double> area(static_cast<size_t>(N)), src(static_cast<size_t>(N)), lakeLoss(static_cast<size_t>(N));
    for (int64_t k = 0; k < N; ++k) {
        double a = g.cellAreaKm2(g.row(k));
        area[static_cast<size_t>(k)] = a;
        if (ocean[k]) continue;
        src[static_cast<size_t>(k)] = runoffA[k] / 1000.0 * a * 1e6 / kSecondsPerYear;  // m³/s
        // Net loss if this cell were open water: evaporation minus rain on the lake, plus the land runoff it no
        // longer yields (already counted in the inflow).
        double ew = 1.1 * petA[k];
        lakeLoss[static_cast<size_t>(k)] = (ew - precA[k] + runoffA[k]) / 1000.0 * a * 1e6 / kSecondsPerYear;
    }

    // ---- 1. depression-filled drainage tree
    DrainageGraph dg = buildDrainage(g, z, base, 1e-4);
    ctx.progress(0.15, "заполнение депрессий");

    // ---- 2. depressions and their water balance
    std::vector<uint8_t> inDep(static_cast<size_t>(N), 0);
    for (int64_t k = 0; k < N; ++k)
        if (!ocean[k] && dg.filled[static_cast<size_t>(k)] > z[static_cast<size_t>(k)] + 0.5f) inDep[static_cast<size_t>(k)] = 1;
    // A depression = all filled cells that leave through the same outlet along the drainage tree (so two
    // neighbouring pits with different spill levels are never merged).
    std::vector<int32_t> depId(static_cast<size_t>(N), -1);
    std::vector<int64_t> outlet;
    for (int64_t c : dg.order) {
        if (!inDep[static_cast<size_t>(c)]) continue;
        int64_t r = dg.receiver[static_cast<size_t>(c)];
        if (r >= 0 && inDep[static_cast<size_t>(r)]) depId[static_cast<size_t>(c)] = depId[static_cast<size_t>(r)];
        else {
            depId[static_cast<size_t>(c)] = static_cast<int32_t>(outlet.size());
            outlet.push_back(c);
        }
    }
    const int nDep = static_cast<int>(outlet.size());
    std::vector<std::vector<int64_t>> regionCells(static_cast<size_t>(nDep));
    for (int64_t k = 0; k < N; ++k)
        if (depId[static_cast<size_t>(k)] >= 0) regionCells[static_cast<size_t>(depId[static_cast<size_t>(k)])].push_back(k);
    // Lake decision while accumulating discharge upstream -> downstream on the filled tree.
    std::vector<double> flux(src.begin(), src.end());
    std::vector<double> lakeLevel(static_cast<size_t>(nDep), -1e30);
    std::vector<uint8_t> terminal(static_cast<size_t>(nDep), 0), isLakeDep(static_cast<size_t>(nDep), 0);
    std::vector<double> depInflow(static_cast<size_t>(nDep), 0.0), depEvap(static_cast<size_t>(nDep), 0.0);
    std::vector<int32_t> outletOf(static_cast<size_t>(N), -1);
    for (int d = 0; d < nDep; ++d) if (outlet[static_cast<size_t>(d)] >= 0) outletOf[static_cast<size_t>(outlet[static_cast<size_t>(d)])] = d;
    const double minLakeDepth = 8.0;
    for (size_t q = dg.order.size(); q-- > 0;) {
        int64_t c = dg.order[q];
        int32_t d = outletOf[static_cast<size_t>(c)];
        if (d >= 0) {
            auto& cells = regionCells[static_cast<size_t>(d)];
            // Spill level: the filled height at the outlet (flats rise by tiny epsilons inwards from it).
            double spill = dg.filled[static_cast<size_t>(c)], zmin = 1e30;
            for (int64_t k : cells) zmin = std::min(zmin, static_cast<double>(z[static_cast<size_t>(k)]));
            double depth = spill - zmin;
            double qin = flux[static_cast<size_t>(c)];
            depInflow[static_cast<size_t>(d)] = qin;
            if (depth >= minLakeDepth || cells.size() >= 3) {
                isLakeDep[static_cast<size_t>(d)] = 1;
                std::vector<int64_t> sorted(cells);
                std::sort(sorted.begin(), sorted.end(), [&](int64_t a, int64_t b) {
                    return z[static_cast<size_t>(a)] < z[static_cast<size_t>(b)] || (z[static_cast<size_t>(a)] == z[static_cast<size_t>(b)] && a < b);
                });
                double total = 0;
                for (int64_t k : sorted) total += std::max(0.0, lakeLoss[static_cast<size_t>(k)]);
                if (qin > total) {
                    lakeLevel[static_cast<size_t>(d)] = spill;
                    depEvap[static_cast<size_t>(d)] = total;
                    flux[static_cast<size_t>(c)] = qin - total;
                } else {
                    terminal[static_cast<size_t>(d)] = 1;
                    double acc = 0, level = zmin;
                    for (size_t s = 0; s < sorted.size(); ++s) {
                        acc += std::max(0.0, lakeLoss[static_cast<size_t>(sorted[s])]);
                        double next = s + 1 < sorted.size() ? z[static_cast<size_t>(sorted[s + 1])] : spill;
                        if (acc >= qin) { level = z[static_cast<size_t>(sorted[s])] + 0.5 * (next - z[static_cast<size_t>(sorted[s])]) * std::clamp(1.0 - (acc - qin) / std::max(1e-9, std::max(0.0, lakeLoss[static_cast<size_t>(sorted[s])])), 0.0, 1.0); break; }
                        level = next;
                    }
                    lakeLevel[static_cast<size_t>(d)] = std::min(level, spill - 0.01);
                    depEvap[static_cast<size_t>(d)] = std::min(qin, total);
                    flux[static_cast<size_t>(c)] = 0;
                }
            }
        }
        int64_t r = dg.receiver[static_cast<size_t>(c)];
        if (r >= 0 && !ocean[r]) flux[static_cast<size_t>(r)] += flux[static_cast<size_t>(c)];
    }
    ctx.progress(0.35, "водный баланс депрессий");

    // ---- 3. lakes, hydrologic surface, final receivers
    std::vector<uint8_t> lakeCell(static_cast<size_t>(N), 0);
    // Hydrologic surface: the depression-filled surface (strictly decreasing along the flow tree), then
    // lake levels and the re-flooded surfaces of terminal basins override it.
    std::vector<float> hs(dg.filled.begin(), dg.filled.end());
    for (int d = 0; d < nDep; ++d) {
        if (!isLakeDep[static_cast<size_t>(d)]) {
            for (int64_t k : regionCells[static_cast<size_t>(d)]) hs[static_cast<size_t>(k)] = dg.filled[static_cast<size_t>(k)];
            continue;
        }
        double L = lakeLevel[static_cast<size_t>(d)];
        // Lake = cells below the level connected to the deepest point of the depression.
        auto& cells = regionCells[static_cast<size_t>(d)];
        int64_t lo = cells.front();
        for (int64_t k : cells) if (z[static_cast<size_t>(k)] < z[static_cast<size_t>(lo)]) lo = k;
        if (z[static_cast<size_t>(lo)] >= L) continue;
        std::vector<int64_t> stack = {lo};
        lakeCell[static_cast<size_t>(lo)] = 1;
        while (!stack.empty()) {
            int64_t c = stack.back();
            stack.pop_back();
            hs[static_cast<size_t>(c)] = static_cast<float>(L);
            int i = g.col(c), j = g.row(c);
            for (int dir = 0; dir < 8; ++dir) {
                int64_t nb = g.neighbor(i, j, dir);
                if (lakeCell[static_cast<size_t>(nb)] || depId[static_cast<size_t>(nb)] != d || z[static_cast<size_t>(nb)] >= L) continue;
                lakeCell[static_cast<size_t>(nb)] = 1;
                stack.push_back(nb);
            }
        }
    }
    std::vector<int64_t> rec(dg.receiver.begin(), dg.receiver.end());
    std::vector<uint8_t> sink(static_cast<size_t>(N), 0);
    // Terminal basins: re-flood each region from its lake cells so everything drains to the lake.
    for (int d = 0; d < nDep; ++d) {
        if (!terminal[static_cast<size_t>(d)]) continue;
        auto& cells = regionCells[static_cast<size_t>(d)];
        using Item = std::pair<double, int64_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
        std::map<int64_t, uint8_t> seen;
        for (int64_t k : cells)
            if (lakeCell[static_cast<size_t>(k)]) { sink[static_cast<size_t>(k)] = 1; rec[static_cast<size_t>(k)] = -1; pq.push({hs[static_cast<size_t>(k)], k}); seen[k] = 1; }
        if (pq.empty()) {  // degenerate: deepest cell is the sink
            int64_t lo = cells.front();
            for (int64_t k : cells) if (z[static_cast<size_t>(k)] < z[static_cast<size_t>(lo)]) lo = k;
            sink[static_cast<size_t>(lo)] = 1;
            rec[static_cast<size_t>(lo)] = -1;
            pq.push({z[static_cast<size_t>(lo)], lo});
            seen[lo] = 1;
        }
        while (!pq.empty()) {
            auto [h, c] = pq.top();
            pq.pop();
            int i = g.col(c), j = g.row(c);
            for (int dir = 0; dir < 8; ++dir) {
                int64_t nb = g.neighbor(i, j, dir);
                if (depId[static_cast<size_t>(nb)] != d || seen.count(nb)) continue;
                seen[nb] = 1;
                rec[static_cast<size_t>(nb)] = c;
                double lvl = std::max(h, static_cast<double>(z[static_cast<size_t>(nb)]));
                // A nested pit above the lake level fills to its own spill and overflows towards the lake.
                hs[static_cast<size_t>(nb)] = static_cast<float>(lvl);
                pq.push({lvl, nb});
            }
        }
    }
    // Lake cells of outflow lakes keep the flood-tree receivers (flat surface towards the outlet).
    std::vector<int64_t> order = topoOrder(rec);
    ctx.report.check("hydro.acyclic", "Сеть стока ациклична (топологический порядок охватывает все ячейки)",
                     static_cast<int64_t>(order.size()) == N, static_cast<double>(N - static_cast<int64_t>(order.size())), "0 ячеек в циклах", true);

    // ---- discharge with lake gains/losses, monthly discharge, drainage area
    std::vector<double> Q(static_cast<size_t>(N), 0.0), A(static_cast<size_t>(N), 0.0), clampLoss(static_cast<size_t>(N), 0.0);
    std::vector<double> local(static_cast<size_t>(N));
    for (int64_t k = 0; k < N; ++k) {
        if (ocean[k]) continue;
        local[static_cast<size_t>(k)] = lakeCell[static_cast<size_t>(k)] ? src[static_cast<size_t>(k)] - lakeLoss[static_cast<size_t>(k)] : src[static_cast<size_t>(k)];
    }
    std::vector<std::vector<float>> Qm(12, std::vector<float>(static_cast<size_t>(N), 0.0f));
    for (size_t q = order.size(); q-- > 0;) {
        int64_t c = order[q];
        if (ocean[c]) continue;
        double v = Q[static_cast<size_t>(c)] + local[static_cast<size_t>(c)];
        if (v < 0) { clampLoss[static_cast<size_t>(c)] = -v; v = 0; }
        Q[static_cast<size_t>(c)] = v;
        A[static_cast<size_t>(c)] += area[static_cast<size_t>(c)];
        int64_t r = rec[static_cast<size_t>(c)];
        if (r >= 0 && !ocean[r]) {
            Q[static_cast<size_t>(r)] += v;
            A[static_cast<size_t>(r)] += A[static_cast<size_t>(c)];
        }
    }
    ctx.pool.forChunks(0, 12, 1, [&](int64_t m0, int64_t m1) {
        for (int64_t m = m0; m < m1; ++m) {
            auto& qm = Qm[static_cast<size_t>(m)];
            std::vector<double> acc(static_cast<size_t>(N), 0.0);
            for (size_t q = order.size(); q-- > 0;) {
                int64_t c = order[q];
                if (ocean[c]) continue;
                double r = runoffM[m * N + c] * 0.1 / 1000.0 * area[static_cast<size_t>(c)] * 1e6 / (kDays[m] * 86400.0);
                if (lakeCell[static_cast<size_t>(c)]) r = std::max(0.0, r - lakeLoss[static_cast<size_t>(c)]);
                double v = acc[static_cast<size_t>(c)] + r;
                acc[static_cast<size_t>(c)] = v;
                qm[static_cast<size_t>(c)] = static_cast<float>(v);
                int64_t rr = rec[static_cast<size_t>(c)];
                if (rr >= 0 && !ocean[rr] && !sink[static_cast<size_t>(c)]) acc[static_cast<size_t>(rr)] += v;
            }
        }
    }, &ctx.cancel);
    ctx.progress(0.55, "расходы");

    // ---- basins
    std::vector<int32_t> basin(static_cast<size_t>(N), -1);
    auto hf = std::make_shared<HydroFeatures>();
    std::vector<int32_t> lakeOfCell(static_cast<size_t>(N), -1);
    {
        std::vector<int32_t> lab;
        int nl = connectedComponents(g, lakeCell.data(), lab);
        (void)nl;
        lakeOfCell = lab;
    }
    std::map<int32_t, int32_t> terminalBasin;  // lake component -> basin id
    for (int64_t c : order) {
        if (ocean[c]) continue;
        int64_t r = rec[static_cast<size_t>(c)];
        if (sink[static_cast<size_t>(c)]) {
            int32_t lc = lakeOfCell[static_cast<size_t>(c)];
            int32_t key = lc >= 0 ? lc : -2 - static_cast<int32_t>(c);
            auto it = terminalBasin.find(key);
            if (it == terminalBasin.end()) {
                Basin b;
                b.id = static_cast<int>(hf->basins.size());
                b.outletCell = c;
                b.outlet = lc >= 0 ? kBoTerminalLake : kBoDrySink;
                hf->basins.push_back(b);
                it = terminalBasin.emplace(key, b.id).first;
            }
            basin[static_cast<size_t>(c)] = it->second;
        } else if (r < 0 || ocean[r]) {
            Basin b;
            b.id = static_cast<int>(hf->basins.size());
            b.outletCell = c;
            b.outlet = kBoOcean;
            hf->basins.push_back(b);
            basin[static_cast<size_t>(c)] = b.id;
        } else {
            basin[static_cast<size_t>(c)] = basin[static_cast<size_t>(r)];
        }
    }
    for (int64_t k = 0; k < N; ++k) {
        int32_t b = basin[static_cast<size_t>(k)];
        if (b < 0) continue;
        hf->basins[static_cast<size_t>(b)].areaKm2 += static_cast<float>(area[static_cast<size_t>(k)]);
        hf->basins[static_cast<size_t>(b)].cells++;
    }
    for (auto& b : hf->basins) b.dischargeM3s = static_cast<float>(Q[static_cast<size_t>(b.outletCell)]);

    // ---- rivers: classes, Strahler order
    std::vector<uint8_t> riverCls(static_cast<size_t>(N), 0), strahler(static_cast<size_t>(N), 0);
    const double minArea = 3.0 * area[static_cast<size_t>(g.index(0, H / 2))];
    for (int64_t k = 0; k < N; ++k) {
        if (ocean[k] || lakeCell[static_cast<size_t>(k)]) continue;
        if (Q[static_cast<size_t>(k)] < qThreshold || A[static_cast<size_t>(k)] < minArea) continue;
        double mean = 0, mn = 1e18;
        for (int m = 0; m < 12; ++m) { mean += Qm[static_cast<size_t>(m)][static_cast<size_t>(k)] / 12.0; mn = std::min(mn, static_cast<double>(Qm[static_cast<size_t>(m)][static_cast<size_t>(k)])); }
        double ratio = mean > 0 ? mn / mean : 0;
        riverCls[static_cast<size_t>(k)] = ratio >= 0.15 ? kRiverPerennial : (ratio >= 0.01 ? kRiverSeasonal : kRiverIntermittent);
    }
    {
        std::vector<uint8_t> maxIn(static_cast<size_t>(N), 0), countMax(static_cast<size_t>(N), 0);
        for (size_t q = order.size(); q-- > 0;) {
            int64_t c = order[q];
            if (!riverCls[static_cast<size_t>(c)]) continue;
            uint8_t o = maxIn[static_cast<size_t>(c)] == 0 ? 1 : (countMax[static_cast<size_t>(c)] >= 2 ? maxIn[static_cast<size_t>(c)] + 1 : maxIn[static_cast<size_t>(c)]);
            strahler[static_cast<size_t>(c)] = o;
            int64_t r = rec[static_cast<size_t>(c)];
            if (r < 0 || !riverCls[static_cast<size_t>(r)]) continue;
            if (o > maxIn[static_cast<size_t>(r)]) { maxIn[static_cast<size_t>(r)] = o; countMax[static_cast<size_t>(r)] = 1; }
            else if (o == maxIn[static_cast<size_t>(r)]) countMax[static_cast<size_t>(r)]++;
        }
    }
    ctx.progress(0.65, "реки и бассейны");

    // ---- vector river segments between sources / confluences / mouths
    {
        std::vector<int> riverDonors(static_cast<size_t>(N), 0);
        for (int64_t k = 0; k < N; ++k)
            if (riverCls[static_cast<size_t>(k)] && rec[static_cast<size_t>(k)] >= 0 && riverCls[static_cast<size_t>(rec[static_cast<size_t>(k)])])
                riverDonors[static_cast<size_t>(rec[static_cast<size_t>(k)])]++;
        std::vector<int32_t> segOfStart(static_cast<size_t>(N), -1);
        // Segment starts: sources (no river donors) and confluences (>= 2 river donors).
        std::vector<int64_t> starts;
        for (size_t q = order.size(); q-- > 0;) {
            int64_t c = order[q];
            if (riverCls[static_cast<size_t>(c)] && riverDonors[static_cast<size_t>(c)] != 1) starts.push_back(c);
        }
        std::sort(starts.begin(), starts.end());
        for (int64_t s : starts) segOfStart[static_cast<size_t>(s)] = static_cast<int32_t>(hf->rivers.size()), hf->rivers.emplace_back();
        uint64_t mkey = ctx.seed.key64("hydro.meander");
        for (size_t si = 0; si < starts.size(); ++si) {
            RiverSegment& seg = hf->rivers[si];
            seg.id = static_cast<int>(si);
            int64_t c = starts[si];
            seg.cells.push_back(c);
            int64_t next = rec[static_cast<size_t>(c)];
            while (next >= 0 && riverCls[static_cast<size_t>(next)] && riverDonors[static_cast<size_t>(next)] == 1) {
                seg.cells.push_back(next);
                next = rec[static_cast<size_t>(next)];
            }
            // Downstream link: the confluence segment, or the ocean / lake the river ends in.
            if (next >= 0 && riverCls[static_cast<size_t>(next)]) seg.downstream = segOfStart[static_cast<size_t>(next)];
            int64_t last = seg.cells.back();
            int64_t endCell = (next >= 0) ? next : last;
            Polyline raw;
            for (int64_t k : seg.cells) appendUnwrapped(raw, g.latDeg(g.row(k)), g.lonDeg(g.col(k)));
            if (next >= 0) appendUnwrapped(raw, g.latDeg(g.row(next)), g.lonDeg(g.col(next)));  // join the receiver (confluence / mouth)
            seg.line = chaikin(raw, 2);
            double len = 0;
            for (size_t i = 0; i + 1 < seg.cells.size(); ++i) len += g.greatCircleKm(seg.cells[i], seg.cells[i + 1]);
            if (next >= 0) len += g.greatCircleKm(last, endCell);
            seg.lengthKm = static_cast<float>(len);
            seg.dischargeStart = static_cast<float>(Q[static_cast<size_t>(seg.cells.front())]);
            seg.dischargeEnd = static_cast<float>(Q[static_cast<size_t>(last)]);
            seg.slope = static_cast<float>(len > 0 ? (hs[static_cast<size_t>(seg.cells.front())] - hs[static_cast<size_t>(endCell)]) / len : 0);
            seg.order = strahler[static_cast<size_t>(last)];
            seg.basinId = basin[static_cast<size_t>(last)];
            int cnt[4] = {0, 0, 0, 0};
            for (int64_t k : seg.cells) cnt[riverCls[static_cast<size_t>(k)]]++;
            seg.riverClass = cnt[kRiverIntermittent] > cnt[kRiverPerennial] + cnt[kRiverSeasonal] ? kRiverIntermittent
                             : (cnt[kRiverSeasonal] > cnt[kRiverPerennial] ? kRiverSeasonal : kRiverPerennial);
            // Lowland meanders: bounded sinusoidal displacement (≤ 0.3 cell) on gentle, large rivers.
            seg.meandering = seg.slope < 0.25 && seg.order >= 3 && seg.dischargeEnd > 3 * qThreshold && seg.line.pts.size() > 6;
            if (seg.meandering) {
                double cellDeg = 180.0 / H;
                double phase = hashUniform(mkey, static_cast<uint64_t>(si)) * kTwoPi;
                auto& pts = seg.line.pts;
                std::vector<GeoPoint> out = pts;
                for (size_t i = 1; i + 1 < pts.size(); ++i) {
                    double dx = (pts[i + 1].lon - pts[i - 1].lon) * std::cos(pts[i].lat * kDegToRad), dy = pts[i + 1].lat - pts[i - 1].lat;
                    double l = std::hypot(dx, dy);
                    if (l < 1e-9) continue;
                    double amp = 0.2 * cellDeg * std::sin(phase + i * 1.3);
                    out[i].lat += dx / l * amp;
                    out[i].lon += -dy / l * amp / std::max(0.2, std::cos(pts[i].lat * kDegToRad));
                }
                pts = out;
            }
        }
        // Corridor guarantee: every vertex of the visual line lies in one of the segment's own cells (or the
        // receiver it joins); vertices pushed outside by smoothing / meanders snap back to the cell path.
        for (auto& seg : hf->rivers) {
            std::vector<int64_t> corridor(seg.cells.begin(), seg.cells.end());
            int64_t nx = rec[static_cast<size_t>(seg.cells.back())];
            if (nx >= 0) corridor.push_back(nx);
            std::sort(corridor.begin(), corridor.end());
            for (auto& pnt : seg.line.pts) {
                int64_t c = g.cellAt(pnt.lat, wrapLonDeg(pnt.lon));
                if (std::binary_search(corridor.begin(), corridor.end(), c)) continue;
                double best = 1e18;
                GeoPoint snapped = pnt;
                for (int64_t k : corridor) {
                    double lat = g.latDeg(g.row(k)), lon = g.lonDeg(g.col(k));
                    while (lon - pnt.lon > 180) lon -= 360;
                    while (lon - pnt.lon < -180) lon += 360;
                    double d = sqr(lat - pnt.lat) + sqr((lon - pnt.lon) * std::cos(lat * kDegToRad));
                    if (d < best) { best = d; snapped = {lat, lon}; }
                }
                pnt = snapped;
            }
        }
        // Waterfalls / rapids: the sharpest gradient break of a segment, if it is a real knickpoint.
        for (auto& seg : hf->rivers) {
            double bestGrad = 0, bestDrop = 0;
            int64_t bestCell = -1;
            for (size_t i = 0; i + 1 < seg.cells.size(); ++i) {
                int64_t a = seg.cells[i], b = seg.cells[i + 1];
                double drop = z[static_cast<size_t>(a)] - z[static_cast<size_t>(b)];
                double grad = drop / g.greatCircleKm(a, b);  // m/km
                if (grad > bestGrad) { bestGrad = grad; bestDrop = drop; bestCell = a; }
            }
            double meanGrad = seg.lengthKm > 0 ? seg.slope : 0;
            if (bestCell >= 0 && bestGrad > 8.0 && bestGrad > 3.0 * std::max(0.5, meanGrad) && bestDrop > 150) {
                Waterfall f;
                f.lat = g.latDeg(g.row(bestCell));
                f.lon = g.lonDeg(g.col(bestCell));
                f.dropM = static_cast<float>(bestDrop);
                f.riverId = seg.id;
                f.rapids = bestGrad < 20.0;
                hf->waterfalls.push_back(f);
            }
        }
    }
    ctx.progress(0.75, "векторные реки");

    // ---- lakes
    {
        std::map<int32_t, int32_t> compToLake;
        uint64_t key = ctx.seed.key64("hydro.lakes");
        (void)key;
        for (int64_t k = 0; k < N; ++k) {
            if (!lakeCell[static_cast<size_t>(k)]) continue;
            int32_t comp = lakeOfCell[static_cast<size_t>(k)];
            if (!compToLake.count(comp)) {
                Lake L;
                L.id = static_cast<int>(hf->lakes.size());
                compToLake[comp] = L.id;
                hf->lakes.push_back(L);
            }
        }
        std::vector<double> latS(hf->lakes.size(), 0), lonX(hf->lakes.size(), 0), lonY(hf->lakes.size(), 0), wsum(hf->lakes.size(), 0);
        std::vector<int> rift(hf->lakes.size(), 0), glacial(hf->lakes.size(), 0), mountain(hf->lakes.size(), 0);
        std::vector<double> minz(hf->lakes.size(), 1e30), arid(hf->lakes.size(), 0);
        for (int64_t k = 0; k < N; ++k) {
            if (!lakeCell[static_cast<size_t>(k)]) continue;
            int32_t id = compToLake[lakeOfCell[static_cast<size_t>(k)]];
            Lake& L = hf->lakes[static_cast<size_t>(id)];
            double a = area[static_cast<size_t>(k)];
            L.areaKm2 += static_cast<float>(a);
            L.cells++;
            L.levelM = hs[static_cast<size_t>(k)];
            double depth = hs[static_cast<size_t>(k)] - z[static_cast<size_t>(k)];
            L.maxDepthM = std::max(L.maxDepthM, static_cast<float>(depth));
            L.volumeKm3 += static_cast<float>(depth / 1000.0 * a);
            L.evaporationM3s += static_cast<float>(std::max(0.0, lakeLoss[static_cast<size_t>(k)]));
            L.basinId = basin[static_cast<size_t>(k)];
            double lat = g.latDeg(g.row(k)), lon = g.lonDeg(g.col(k)) * kDegToRad;
            latS[static_cast<size_t>(id)] += lat * a;
            lonX[static_cast<size_t>(id)] += std::cos(lon) * a;
            lonY[static_cast<size_t>(id)] += std::sin(lon) * a;
            wsum[static_cast<size_t>(id)] += a;
            if (gen[k] == kGenContinentalRift || landform[k] == kLfRiftValley) rift[static_cast<size_t>(id)]++;
            if (lgm[k]) glacial[static_cast<size_t>(id)]++;
            if (landform[k] == kLfMountains || landform[k] == kLfHighlands) mountain[static_cast<size_t>(id)]++;
            arid[static_cast<size_t>(id)] += aridity[k] * a;
            minz[static_cast<size_t>(id)] = std::min(minz[static_cast<size_t>(id)], static_cast<double>(z[static_cast<size_t>(k)]));
        }
        // Inflow / outflow from the depression balance.
        for (int d = 0; d < nDep; ++d) {
            if (!isLakeDep[static_cast<size_t>(d)]) continue;
            int32_t id = -1;
            for (int64_t k : regionCells[static_cast<size_t>(d)])
                if (lakeCell[static_cast<size_t>(k)]) { id = compToLake[lakeOfCell[static_cast<size_t>(k)]]; break; }
            if (id < 0) continue;
            Lake& L = hf->lakes[static_cast<size_t>(id)];
            L.inflowM3s += static_cast<float>(depInflow[static_cast<size_t>(d)]);
            L.regime = terminal[static_cast<size_t>(d)] ? kLrClosed : kLrOutflow;
            L.outflowM3s = terminal[static_cast<size_t>(d)] ? 0.0f : static_cast<float>(Q[static_cast<size_t>(outlet[static_cast<size_t>(d)])]);
            L.outletCell = terminal[static_cast<size_t>(d)] ? -1 : outlet[static_cast<size_t>(d)];
        }
        std::vector<Vec3> volc;
        if (w.geomorph)
            for (auto& v : w.geomorph->volcanoes) volc.push_back(fromLatLon(v.lat * kDegToRad, v.lon * kDegToRad));
        for (size_t i = 0; i < hf->lakes.size(); ++i) {
            Lake& L = hf->lakes[i];
            L.lat = latS[i] / wsum[i];
            L.lon = std::atan2(lonY[i], lonX[i]) * kRadToDeg;
            double ai = arid[i] / wsum[i];
            Vec3 c = fromLatLon(L.lat * kDegToRad, L.lon * kDegToRad);
            bool nearVolcano = false;
            for (auto& v : volc) if (g.radiusKm() * angleBetween(c, v) < 60) { nearVolcano = true; break; }
            if (rift[i] * 2 >= L.cells) L.genesis = kLgTectonic;
            else if (nearVolcano && L.cells <= 3) L.genesis = kLgVolcanic;
            else if (glacial[i] * 2 >= L.cells) L.genesis = kLgGlacial;
            else if (mountain[i] * 2 >= L.cells) L.genesis = kLgDammed;
            else if (L.cells <= 2 && minz[i] - sea < 150) L.genesis = kLgFloodplain;
            else L.genesis = kLgBasin;
            if (L.regime == kLrClosed) {
                L.saline = ai < 0.5;
                // Small balance lake in a dry climate: seasonal / ephemeral (playa).
                if (ai < 0.2 && L.maxDepthM < 25) L.regime = kLrEphemeral;
            }
        }
        std::vector<int32_t> lakeIdLayer(static_cast<size_t>(N), -1);
        for (int64_t k = 0; k < N; ++k)
            if (lakeCell[static_cast<size_t>(k)]) lakeIdLayer[static_cast<size_t>(k)] = compToLake[lakeOfCell[static_cast<size_t>(k)]];
        lakeOfCell.swap(lakeIdLayer);
    }

    // ---- deltas: distributary graphs at suitable mouths
    {
        const float* expo = w.f32(LayerId::CoastExposure);
        const float* tide = w.f32(LayerId::TidalRange);
        for (auto& seg : hf->rivers) {
            if (seg.downstream >= 0 || seg.cells.empty()) continue;
            int64_t mouth = seg.cells.back();
            int64_t r = rec[static_cast<size_t>(mouth)];
            if (r < 0 || !ocean[r]) continue;
            double q = Q[static_cast<size_t>(mouth)];
            double ex = expo && !std::isnan(expo[mouth]) ? expo[mouth] : 0.5;
            double td = tide && !std::isnan(tide[mouth]) ? tide[mouth] : 1.0;
            // Sediment supply grows with discharge and upstream relief; energetic, macro-tidal coasts rework it.
            double relief = 0;
            for (int64_t k : seg.cells) relief = std::max(relief, static_cast<double>(z[static_cast<size_t>(k)] - sea));
            double supply = q * (1.0 + relief / 2500.0);
            bool lowCoast = z[static_cast<size_t>(mouth)] - sea < 80;
            if (supply < std::max(600.0, 20 * qThreshold) || ex > 0.62 || td > 4.0 || !lowCoast) continue;
            int64_t apex = seg.cells.size() >= 3 ? seg.cells[seg.cells.size() - 3] : seg.cells.front();
            if (z[static_cast<size_t>(apex)] - sea > 120) apex = mouth;
            // Candidate shore cells around the mouth, spread angularly.
            int ri = std::max(2, static_cast<int>(std::round(120.0 / g.dyKm())));
            int ci = g.col(apex), cj = g.row(apex);
            std::vector<std::pair<double, int64_t>> shore;
            for (int dj = -ri; dj <= ri; ++dj)
                for (int di = -ri; di <= ri; ++di) {
                    int jj = cj + dj;
                    if (jj < 0 || jj >= H) continue;
                    int64_t k = g.index(g.wrapCol(ci + di), jj);
                    if (!ocean[k]) continue;
                    bool coast = false;
                    int64_t nb[8];
                    g.neighbors8(k, nb);
                    for (int d = 0; d < 8 && !coast; ++d) coast = !ocean[nb[d]];
                    if (!coast) continue;
                    shore.push_back({std::atan2(static_cast<double>(dj), static_cast<double>(di)), k});
                }
            if (shore.size() < 2) continue;
            std::sort(shore.begin(), shore.end());
            int branches = std::clamp(static_cast<int>(2 + q / 3000.0), 2, 5);
            Delta dl;
            dl.mouthCell = mouth;
            dl.lat = g.latDeg(g.row(mouth));
            dl.lon = g.lonDeg(g.col(mouth));
            dl.dischargeM3s = static_cast<float>(q);
            dl.riverId = seg.id;
            for (int b = 0; b < branches; ++b) {
                int64_t target = shore[static_cast<size_t>((b * (shore.size() - 1)) / std::max(1, branches - 1))].second;
                // Greedy path over low land from the apex to the target shore cell.
                RiverSegment d;
                d.id = static_cast<int>(hf->rivers.size());
                d.riverClass = kRiverDistributary;
                d.order = std::max(1, seg.order - 1);
                d.basinId = seg.basinId;
                d.dischargeStart = d.dischargeEnd = static_cast<float>(q / branches);
                int64_t cur = apex;
                d.cells.push_back(cur);
                for (int step = 0; step < 4 * ri && cur != target; ++step) {
                    int i0 = g.col(cur), j0 = g.row(cur);
                    int64_t best = -1;
                    double bd = 1e18;
                    for (int dir = 0; dir < 8; ++dir) {
                        int64_t nb = g.neighbor(i0, j0, dir);
                        if (!ocean[nb] && z[static_cast<size_t>(nb)] - sea > 150) continue;
                        double dist = g.greatCircleKm(nb, target);
                        if (dist < bd) { bd = dist; best = nb; }
                    }
                    if (best < 0) break;
                    cur = best;
                    d.cells.push_back(cur);
                    if (ocean[cur]) break;
                }
                if (d.cells.size() < 2) continue;
                Polyline raw;
                for (int64_t k : d.cells) appendUnwrapped(raw, g.latDeg(g.row(k)), g.lonDeg(g.col(k)));
                d.line = chaikin(raw, 2);
                for (size_t i = 0; i + 1 < d.cells.size(); ++i) d.lengthKm += static_cast<float>(g.greatCircleKm(d.cells[i], d.cells[i + 1]));
                hf->rivers.push_back(d);
                dl.branches++;
            }
            if (dl.branches >= 2) hf->deltas.push_back(dl);
        }
    }

    // ---- wetlands
    std::vector<uint8_t> wet(static_cast<size_t>(N), 0);
    {
        std::vector<float> dRiver;
        std::vector<int64_t> big;
        for (int64_t k = 0; k < N; ++k) if (riverCls[static_cast<size_t>(k)] && Q[static_cast<size_t>(k)] > 5 * qThreshold) big.push_back(k);
        std::vector<int32_t> nearest;
        distanceTransform(g, big, dRiver, &nearest, 200);
        ctx.pool.forRows(H, [&](int j) {
            for (int i = 0; i < W; ++i) {
                int64_t k = g.index(i, j);
                if (ocean[k] || lakeCell[static_cast<size_t>(k)] || (iceT && iceT[k])) continue;
                double zc = z[static_cast<size_t>(k)], rel = 0;
                for (int d = 0; d < 8; ++d) rel = std::max(rel, std::fabs(static_cast<double>(z[static_cast<size_t>(g.neighbor(i, j, d))]) - zc));
                bool flat = rel < 40.0 * std::max(1.0, g.dyKm() / 20.0);
                if (!flat) continue;
                double balance = precA[k] - petA[k];
                bool depression = inDep[static_cast<size_t>(k)] != 0;
                bool nearLake = false;
                for (int d = 0; d < 8 && !nearLake; ++d) nearLake = lakeCell[static_cast<size_t>(g.neighbor(i, j, d))] != 0;
                int32_t nr = nearest.empty() ? -1 : nearest[static_cast<size_t>(k)];
                bool floodplain = nr >= 0 && dRiver[static_cast<size_t>(k)] < 1.6 * g.dyKm() && zc - z[static_cast<size_t>(big[static_cast<size_t>(nr)])] < 25;
                // Salt flats: terminal basins in arid climates.
                int32_t b = basin[static_cast<size_t>(k)];
                bool endorheic = b >= 0 && hf->basins[static_cast<size_t>(b)].outlet != kBoOcean;
                if (endorheic && aridity[k] < 0.2 && (depression || nearLake)) { wet[static_cast<size_t>(k)] = kWetSaltFlat; continue; }
                if (floodplain && balance > -600) { wet[static_cast<size_t>(k)] = kWetFloodplain; continue; }
                if (balance > 0 && tmean[k] > -8 && tmean[k] < 8 && (depression || nearLake || rel < 15)) { wet[static_cast<size_t>(k)] = kWetPeat; continue; }
                if (balance > 150 && tmean[k] >= 8 && (depression || nearLake)) { wet[static_cast<size_t>(k)] = kWetMarsh; continue; }
            }
        }, &ctx.cancel);
    }

    // ---- layers
    auto Lhs = w.makeLayer(LayerId::HydroSurface);
    auto Ldir = w.makeLayer(LayerId::FlowDir);
    auto Lq = w.makeLayer(LayerId::Discharge);
    auto La = w.makeLayer(LayerId::DrainageArea);
    auto Lb = w.makeLayer(LayerId::BasinId);
    auto Lo = w.makeLayer(LayerId::RiverOrder);
    auto Lc = w.makeLayer(LayerId::RiverClass);
    auto Ll = w.makeLayer(LayerId::LakeId);
    auto Lw = w.makeLayer(LayerId::Wetland);
    for (int64_t k = 0; k < N; ++k) {
        Lhs->as<float>()[k] = ocean[k] ? static_cast<float>(sea) : hs[static_cast<size_t>(k)];
        uint8_t fd;
        int64_t r = rec[static_cast<size_t>(k)];
        if (ocean[k]) fd = kFlowOcean;
        else if (sink[static_cast<size_t>(k)] || r < 0) fd = kFlowSink;
        else if (ocean[r]) fd = static_cast<uint8_t>(dirTo(g, k, r));
        else fd = static_cast<uint8_t>(dirTo(g, k, r));
        Ldir->as<uint8_t>()[k] = fd;
        Lq->as<float>()[k] = ocean[k] ? 0.0f : static_cast<float>(Q[static_cast<size_t>(k)]);
        La->as<float>()[k] = ocean[k] ? 0.0f : static_cast<float>(A[static_cast<size_t>(k)]);
        Lb->as<int32_t>()[k] = basin[static_cast<size_t>(k)];
        Lo->as<uint8_t>()[k] = strahler[static_cast<size_t>(k)];
        Lc->as<uint8_t>()[k] = riverCls[static_cast<size_t>(k)];
        Ll->as<int32_t>()[k] = lakeOfCell[static_cast<size_t>(k)];
        Lw->as<uint8_t>()[k] = wet[static_cast<size_t>(k)];
    }
    for (auto& seg : hf->rivers)
        if (seg.riverClass == kRiverDistributary)
            for (int64_t k : seg.cells) if (!ocean[k]) Lc->as<uint8_t>()[k] = kRiverDistributary;
    for (auto& b : hf->basins) {
        // Main river length: longest river path within the basin (approximate by the segment chain).
        b.mainRiverKm = 0;
    }
    for (auto& seg : hf->rivers)
        if (seg.basinId >= 0 && seg.riverClass != kRiverDistributary) hf->basins[static_cast<size_t>(seg.basinId)].mainRiverKm += seg.lengthKm * (seg.order >= 3 ? 1.0f : 0.0f);
    w.setLayer(LayerId::HydroSurface, Lhs);
    w.setLayer(LayerId::FlowDir, Ldir);
    w.setLayer(LayerId::Discharge, Lq);
    w.setLayer(LayerId::DrainageArea, La);
    w.setLayer(LayerId::BasinId, Lb);
    w.setLayer(LayerId::RiverOrder, Lo);
    w.setLayer(LayerId::RiverClass, Lc);
    w.setLayer(LayerId::LakeId, Ll);
    w.setLayer(LayerId::Wetland, Lw);
    w.hydro = hf;
    ctx.progress(0.92, "озёра, дельты, болота");

    // ---- invariants
    int64_t uphill = 0, unreached = 0;
    for (int64_t k = 0; k < N; ++k) {
        if (ocean[k]) continue;
        int64_t r = rec[static_cast<size_t>(k)];
        if (r >= 0 && !ocean[r] && hs[static_cast<size_t>(r)] > hs[static_cast<size_t>(k)] + 1e-3f && riverCls[static_cast<size_t>(k)]) ++uphill;
        // Every land cell reaches the ocean or a terminal sink: roots of the (acyclic) graph are sinks.
        if (r < 0 && !sink[static_cast<size_t>(k)]) ++unreached;
    }
    ctx.report.check("hydro.descends", "Каждый речной участок спускается по гидрологической поверхности (или по зеркалу озера)",
                     uphill == 0, static_cast<double>(uphill), "0 участков «вверх»", true);
    ctx.report.check("hydro.terminals", "Каждая ячейка суши дренируется в океан или в бессточный терминал", unreached == 0,
                     static_cast<double>(unreached), "0", true);
    // Mass balance: sources = ocean outflow + terminal absorption + explained losses.
    double sources = 0, toOcean = 0, toSinks = 0, losses = 0, lakeNeg = 0;
    for (int64_t k = 0; k < N; ++k) {
        if (ocean[k]) continue;
        if (local[static_cast<size_t>(k)] > 0) sources += local[static_cast<size_t>(k)];
        else lakeNeg += -local[static_cast<size_t>(k)];
        losses += clampLoss[static_cast<size_t>(k)];
        int64_t r = rec[static_cast<size_t>(k)];
        if (sink[static_cast<size_t>(k)]) toSinks += Q[static_cast<size_t>(k)];
        else if (r >= 0 && ocean[r]) toOcean += Q[static_cast<size_t>(k)];
    }
    double balance = sources - lakeNeg + losses - toOcean - toSinks;
    double rel = std::fabs(balance) / std::max(1.0, sources);
    ctx.report.check("hydro.mass", "Сохранение массы стока (источники = сток в океан + терминалы + объяснимые потери)", rel < 1e-6, rel,
                     "относительная погрешность < 1e-6", true);
    int64_t dryRiver = 0;
    for (int64_t k = 0; k < N; ++k) if (riverCls[static_cast<size_t>(k)] && Q[static_cast<size_t>(k)] <= 0) ++dryRiver;
    ctx.report.check("hydro.fedRivers", "Нет русел без питания (расход > 0 у каждой реки)", dryRiver == 0, static_cast<double>(dryRiver), "0", true);
    // Statistics.
    int bigBasins = 0, endorheicBasins = 0;
    double endorheicArea = 0, landArea = 0, aridArea = 0, aridEndorheic = 0;
    for (auto& b : hf->basins) {
        if (b.areaKm2 > 100000) ++bigBasins;
        if (b.outlet != kBoOcean) { ++endorheicBasins; endorheicArea += b.areaKm2; }
    }
    for (int64_t k = 0; k < N; ++k) {
        if (ocean[k]) continue;
        landArea += area[static_cast<size_t>(k)];
        if (aridity[k] < 0.5) {
            aridArea += area[static_cast<size_t>(k)];
            int32_t b = basin[static_cast<size_t>(k)];
            if (b >= 0 && hf->basins[static_cast<size_t>(b)].outlet != kBoOcean) aridEndorheic += area[static_cast<size_t>(k)];
        }
    }
    double largest = 0;
    for (auto& b : hf->basins) largest = std::max(largest, static_cast<double>(b.areaKm2));
    int nLakes = static_cast<int>(hf->lakes.size()), closedLakes = 0;
    for (auto& l : hf->lakes) if (l.regime != kLrOutflow) ++closedLakes;
    ctx.report.metric("riverSegments", static_cast<double>(hf->rivers.size()));
    ctx.report.metric("basinsOver100kKm2", bigBasins);
    ctx.report.metric("largestBasinKm2", largest);
    ctx.report.metric("endorheicLandShare", landArea > 0 ? endorheicArea / landArea : 0);
    ctx.report.metric("endorheicShareOfDryLand", aridArea > 0 ? aridEndorheic / aridArea : 0);
    ctx.report.metric("lakes", nLakes);
    ctx.report.metric("closedLakes", closedLakes);
    ctx.report.metric("deltas", static_cast<double>(hf->deltas.size()));
    ctx.report.metric("waterfalls", static_cast<double>(hf->waterfalls.size()));
    ctx.report.metric("oceanOutflowM3s", toOcean);
    ctx.report.log("Озёрно-климатическая итерация: в v1 испарение крупных озёр не возвращается в модель влагопереноса (см. ограничения).");
    ctx.progress(1.0, "гидрология");
}

}  // namespace mapgen
