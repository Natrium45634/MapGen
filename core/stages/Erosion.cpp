#include "core/stages/Erosion.h"

#include <cmath>
#include <queue>

namespace mapgen {

DrainageGraph buildDrainage(const SphereGrid& g, const std::vector<float>& z, const std::vector<uint8_t>& base, double eps) {
    const int64_t n = g.size();
    DrainageGraph d;
    d.filled.assign(z.begin(), z.end());
    d.receiver.assign(static_cast<size_t>(n), -1);
    d.order.reserve(static_cast<size_t>(n));
    std::vector<uint8_t> done(static_cast<size_t>(n), 0);
    using Item = std::pair<double, int64_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    for (int64_t k = 0; k < n; ++k)
        if (base[static_cast<size_t>(k)]) {
            pq.push({z[static_cast<size_t>(k)], k});
            done[static_cast<size_t>(k)] = 1;
        }
    while (!pq.empty()) {
        auto [h, c] = pq.top();
        pq.pop();
        d.order.push_back(c);
        int i = g.col(c), j = g.row(c);
        for (int dir = 0; dir < 8; ++dir) {
            int64_t nb = g.neighbor(i, j, dir);
            if (done[static_cast<size_t>(nb)]) continue;
            done[static_cast<size_t>(nb)] = 1;
            double nz = std::max(static_cast<double>(z[static_cast<size_t>(nb)]), h + eps);
            d.filled[static_cast<size_t>(nb)] = static_cast<float>(nz);
            d.receiver[static_cast<size_t>(nb)] = c;  // flood parent: valid fallback on flats
            pq.push({nz, nb});
        }
    }
    // Steepest descent on the filled surface where it is strictly lower (keeps acyclicity: receivers are lower).
    for (int64_t k = 0; k < n; ++k) {
        if (base[static_cast<size_t>(k)]) continue;
        int i = g.col(k), j = g.row(k);
        double hk = d.filled[static_cast<size_t>(k)];
        double best = 0;
        int64_t br = -1;
        for (int dir = 0; dir < 8; ++dir) {
            int64_t nb = g.neighbor(i, j, dir);
            double drop = hk - d.filled[static_cast<size_t>(nb)];
            if (drop <= 0) continue;
            double s = drop / g.neighborDistKm(j, dir);
            if (s > best) { best = s; br = nb; }
        }
        if (br >= 0) d.receiver[static_cast<size_t>(k)] = br;
    }
    // Recompute a topological order for the final receivers (receivers before donors).
    std::vector<int32_t> indeg(static_cast<size_t>(n), 0);
    for (int64_t k = 0; k < n; ++k)
        if (d.receiver[static_cast<size_t>(k)] >= 0) indeg[static_cast<size_t>(d.receiver[static_cast<size_t>(k)])]++;
    // Donor lists (CSR).
    std::vector<int64_t> start(static_cast<size_t>(n) + 1, 0), donors(static_cast<size_t>(n));
    for (int64_t k = 0; k < n; ++k)
        if (d.receiver[static_cast<size_t>(k)] >= 0) start[static_cast<size_t>(d.receiver[static_cast<size_t>(k)]) + 1]++;
    for (int64_t k = 0; k < n; ++k) start[static_cast<size_t>(k) + 1] += start[static_cast<size_t>(k)];
    std::vector<int64_t> fillPos(start.begin(), start.end() - 1);
    for (int64_t k = 0; k < n; ++k) {
        int64_t r = d.receiver[static_cast<size_t>(k)];
        if (r >= 0) donors[static_cast<size_t>(fillPos[static_cast<size_t>(r)]++)] = k;
    }
    d.order.clear();
    for (int64_t k = 0; k < n; ++k)
        if (d.receiver[static_cast<size_t>(k)] < 0) d.order.push_back(k);
    for (size_t q = 0; q < d.order.size(); ++q) {
        int64_t c = d.order[q];
        for (int64_t e = start[static_cast<size_t>(c)]; e < start[static_cast<size_t>(c) + 1]; ++e) d.order.push_back(donors[static_cast<size_t>(e)]);
    }
    return d;
}

std::vector<double> accumulate(const SphereGrid& g, const DrainageGraph& d, const std::vector<double>& local) {
    std::vector<double> acc(local.begin(), local.end());
    for (size_t q = d.order.size(); q-- > 0;) {
        int64_t c = d.order[q];
        int64_t r = d.receiver[static_cast<size_t>(c)];
        if (r >= 0) acc[static_cast<size_t>(r)] += acc[static_cast<size_t>(c)];
    }
    (void)g;
    return acc;
}

std::vector<float> erodeLandscape(const SphereGrid& g, std::vector<float>& z, const std::vector<uint8_t>& base,
                                  const std::vector<float>& precip, const ErosionParams& p, ThreadPool& pool,
                                  const CancelToken& cancel) {
    const int W = g.width(), H = g.height();
    const int64_t n = g.size();
    std::vector<float> z0(z.begin(), z.end());
    const double refKm = 20.0;  // reference cell size for resolution-consistent incision
    for (int it = 0; it < p.iterations; ++it) {
        cancel.check();
        DrainageGraph d = buildDrainage(g, z, base);
        std::vector<double> local(static_cast<size_t>(n));
        for (int64_t k = 0; k < n; ++k) local[static_cast<size_t>(k)] = g.cellAreaKm2(g.row(k)) * precip[static_cast<size_t>(k)];
        std::vector<double> area = accumulate(g, d, local);
        // Implicit stream-power incision, receivers first.
        for (int64_t c : d.order) {
            int64_t r = d.receiver[static_cast<size_t>(c)];
            if (r < 0 || base[static_cast<size_t>(c)]) continue;
            double hr = z[static_cast<size_t>(r)];
            double hc = z[static_cast<size_t>(c)];
            if (hc <= hr) continue;  // inside a closed low: handled by deposition
            int j = g.row(c);
            double distKm = g.greatCircleKm(c, r);
            double F = p.k * std::pow(area[static_cast<size_t>(c)] / (refKm * refKm), p.m) * (refKm / std::max(1e-3, distKm));
            z[static_cast<size_t>(c)] = static_cast<float>((hc + F * hr) / (1.0 + F));
            (void)j;
        }
        // Sediment deposition in closed depressions (partial fill toward the spill level).
        for (int64_t k = 0; k < n; ++k)
            if (!base[static_cast<size_t>(k)] && d.filled[static_cast<size_t>(k)] > z[static_cast<size_t>(k)] + 0.5f)
                z[static_cast<size_t>(k)] += static_cast<float>(p.deposition * (d.filled[static_cast<size_t>(k)] - z[static_cast<size_t>(k)]));
        // Hillslope diffusion (anisotropic cell sizes handled per row).
        std::vector<float> zn(z.begin(), z.end());
        pool.forRows(H, [&](int j) {
            double dx = std::max(g.dxKm(j), 0.2 * g.dyKm()), dy = g.dyKm();
            double wx = 1.0 / (dx * dx), wy = 1.0 / (dy * dy);
            double norm = 2 * wx + 2 * wy;
            for (int i = 0; i < W; ++i) {
                int64_t k = g.index(i, j);
                if (base[static_cast<size_t>(k)]) continue;
                double c = z[static_cast<size_t>(k)];
                double lap = wx * (z[static_cast<size_t>(g.neighbor(i, j, 2))] + z[static_cast<size_t>(g.neighbor(i, j, 6))] - 2 * c) +
                             wy * (z[static_cast<size_t>(g.neighbor(i, j, 0))] + z[static_cast<size_t>(g.neighbor(i, j, 4))] - 2 * c);
                zn[static_cast<size_t>(k)] = static_cast<float>(c + p.diffusion * lap / norm);
            }
        }, &cancel);
        z.swap(zn);
    }
    // Spike removal (no isolated needles from noise).
    for (int64_t k = 0; k < n; ++k) {
        if (base[static_cast<size_t>(k)]) continue;
        int i = g.col(k), j = g.row(k);
        double mx = -1e18;
        for (int dir = 0; dir < 8; ++dir) mx = std::max(mx, static_cast<double>(z[static_cast<size_t>(g.neighbor(i, j, dir))]));
        if (z[static_cast<size_t>(k)] > mx + p.maxSpikeM) z[static_cast<size_t>(k)] = static_cast<float>(mx + 0.5 * p.maxSpikeM);
    }
    std::vector<float> depth(static_cast<size_t>(n));
    for (int64_t k = 0; k < n; ++k) depth[static_cast<size_t>(k)] = z0[static_cast<size_t>(k)] - z[static_cast<size_t>(k)];
    return depth;
}

}  // namespace mapgen
