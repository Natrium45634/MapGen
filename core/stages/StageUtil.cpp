#include "core/stages/StageUtil.h"

#include <algorithm>
#include <deque>
#include <map>
#include <unordered_map>

namespace mapgen {

Vec3 randomUnitVector(Pcg64& rng) {
    double z = rng.uniform(-1.0, 1.0);
    double a = rng.uniform(0.0, kTwoPi);
    double r = std::sqrt(std::max(0.0, 1.0 - z * z));
    return {r * std::cos(a), r * std::sin(a), z};
}

int enforceConnectedRegions(const SphereGrid& g, std::vector<int32_t>& labels, int nLabels) {
    int reassigned = 0;
    for (int iter = 0; iter < 8; ++iter) {
        // Components of each label.
        std::vector<int32_t> comp(static_cast<size_t>(g.size()), -1);
        std::vector<int32_t> compLabel;
        std::vector<int64_t> compSize;
        std::vector<int64_t> stack;
        for (int64_t s = 0; s < g.size(); ++s) {
            if (comp[static_cast<size_t>(s)] >= 0) continue;
            int32_t lab = labels[static_cast<size_t>(s)];
            int32_t id = static_cast<int32_t>(compLabel.size());
            compLabel.push_back(lab);
            compSize.push_back(0);
            comp[static_cast<size_t>(s)] = id;
            stack.push_back(s);
            while (!stack.empty()) {
                int64_t c = stack.back();
                stack.pop_back();
                compSize[static_cast<size_t>(id)]++;
                int i = g.col(c), j = g.row(c);
                for (int d = 0; d < 8; ++d) {
                    int64_t nb = g.neighbor(i, j, d);
                    if (comp[static_cast<size_t>(nb)] < 0 && labels[static_cast<size_t>(nb)] == lab) {
                        comp[static_cast<size_t>(nb)] = id;
                        stack.push_back(nb);
                    }
                }
            }
        }
        std::vector<int32_t> largest(static_cast<size_t>(nLabels), -1);
        for (size_t c = 0; c < compLabel.size(); ++c) {
            int32_t lab = compLabel[c];
            if (lab < 0 || lab >= nLabels) continue;
            int32_t& L = largest[static_cast<size_t>(lab)];
            if (L < 0 || compSize[c] > compSize[static_cast<size_t>(L)]) L = static_cast<int32_t>(c);
        }
        // Votes of neighbouring labels for each minor component.
        std::vector<std::map<int32_t, int64_t>> votes(compLabel.size());
        bool any = false;
        for (int64_t k = 0; k < g.size(); ++k) {
            int32_t c = comp[static_cast<size_t>(k)];
            int32_t lab = compLabel[static_cast<size_t>(c)];
            if (largest[static_cast<size_t>(lab)] == c) continue;
            any = true;
            int i = g.col(k), j = g.row(k);
            for (int d = 0; d < 8; ++d) {
                int64_t nb = g.neighbor(i, j, d);
                int32_t l2 = labels[static_cast<size_t>(nb)];
                if (l2 != lab) votes[static_cast<size_t>(c)][l2]++;
            }
        }
        if (!any) break;
        std::vector<int32_t> target(compLabel.size(), -1);
        for (size_t c = 0; c < compLabel.size(); ++c) {
            int64_t best = -1;
            for (auto& [l2, n] : votes[c]) if (n > best) { best = n; target[c] = l2; }
            if (target[c] >= 0) ++reassigned;
        }
        for (int64_t k = 0; k < g.size(); ++k) {
            int32_t t = target[static_cast<size_t>(comp[static_cast<size_t>(k)])];
            if (t >= 0) labels[static_cast<size_t>(k)] = t;
        }
    }
    return reassigned;
}

double areaWeightedQuantile(const SphereGrid& g, const std::vector<float>& field, double q, const uint8_t* mask) {
    std::vector<std::pair<float, double>> v;
    v.reserve(static_cast<size_t>(g.size()));
    double total = 0;
    for (int j = 0; j < g.height(); ++j) {
        double a = g.cellAreaKm2(j);
        for (int i = 0; i < g.width(); ++i) {
            int64_t k = g.index(i, j);
            if (mask && !mask[k]) continue;
            v.emplace_back(field[static_cast<size_t>(k)], a);
            total += a;
        }
    }
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    double target = std::clamp(q, 0.0, 1.0) * total, acc = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        acc += v[i].second;
        if (acc >= target) {
            // Threshold halfway to the next distinct value so that "> threshold" splits cleanly.
            if (i + 1 < v.size()) return 0.5 * (static_cast<double>(v[i].first) + v[i + 1].first);
            return v[i].first;
        }
    }
    return v.back().first;
}

void appendUnwrapped(Polyline& line, double lat, double lon) {
    if (!line.pts.empty()) {
        double prev = line.pts.back().lon;
        while (lon - prev > 180.0) lon -= 360.0;
        while (lon - prev < -180.0) lon += 360.0;
    }
    line.pts.push_back({lat, lon});
}

std::vector<BoundarySegment> traceBoundarySegments(const SphereGrid& sg, const std::vector<int32_t>& plates, size_t count,
                                                   const std::function<int64_t(size_t)>& cellOf,
                                                   const std::function<void(size_t, BoundarySegment&)>& fill) {
    std::vector<int32_t> at(static_cast<size_t>(sg.size()), -1);
    std::vector<BoundarySegment> proto(count);
    for (size_t i = 0; i < count; ++i) {
        fill(i, proto[i]);
        if (proto[i].plateA < proto[i].plateB) at[static_cast<size_t>(cellOf(i))] = static_cast<int32_t>(i);
    }
    std::vector<uint8_t> visited(count, 0);
    std::vector<BoundarySegment> out;
    static const int order[8] = {0, 2, 4, 6, 1, 3, 5, 7};  // cardinal directions first
    auto walk = [&](size_t start, std::vector<size_t>& path) {
        size_t cur = start;
        while (true) {
            int64_t c = cellOf(cur);
            int i = sg.col(c), j = sg.row(c);
            int32_t next = -1;
            for (int o : order) {
                int32_t cand = at[static_cast<size_t>(sg.neighbor(i, j, o))];
                if (cand < 0 || visited[static_cast<size_t>(cand)]) continue;
                const BoundarySegment& a = proto[cur];
                const BoundarySegment& b = proto[static_cast<size_t>(cand)];
                if (a.plateA != b.plateA || a.plateB != b.plateB || a.type != b.type) continue;
                next = cand;
                break;
            }
            if (next < 0) break;
            visited[static_cast<size_t>(next)] = 1;
            path.push_back(static_cast<size_t>(next));
            cur = static_cast<size_t>(next);
        }
    };
    for (size_t s = 0; s < count; ++s) {
        if (visited[s] || proto[s].plateA >= proto[s].plateB) continue;
        visited[s] = 1;
        std::vector<size_t> fwd, back;
        walk(s, fwd);
        walk(s, back);
        std::vector<size_t> path(back.rbegin(), back.rend());
        path.push_back(s);
        path.insert(path.end(), fwd.begin(), fwd.end());
        if (path.size() < 2) continue;
        BoundarySegment seg = proto[s];
        double conv = 0, shear = 0;
        for (size_t idx : path) {
            int64_t c = cellOf(idx);
            appendUnwrapped(seg.line, sg.latDeg(sg.row(c)), sg.lonDeg(sg.col(c)));
            conv += proto[idx].convergenceCmYr;
            shear += proto[idx].shearCmYr;
        }
        seg.convergenceCmYr = static_cast<float>(conv / path.size());
        seg.shearCmYr = static_cast<float>(shear / path.size());
        out.push_back(std::move(seg));
    }
    (void)plates;
    return out;
}

std::vector<Polyline> maskContours(const SphereGrid& g, const uint8_t* mask, int minPoints) {
    const int W = g.width(), H = g.height();
    // Edge ids: horizontal edge (i,j)-(i+1,j) = 2*(j*W+i); vertical edge (i,j)-(i,j+1) = 2*(j*W+i)+1.
    auto hEdge = [&](int i, int j) { return 2 * (static_cast<int64_t>(j) * W + g.wrapCol(i)); };
    auto vEdge = [&](int i, int j) { return 2 * (static_cast<int64_t>(j) * W + g.wrapCol(i)) + 1; };
    std::vector<std::pair<int64_t, int64_t>> segs;
    for (int j = 0; j + 1 < H; ++j)
        for (int i = 0; i < W; ++i) {
            int a = mask[g.index(i, j)] ? 1 : 0, b = mask[g.index(g.wrapCol(i + 1), j)] ? 1 : 0;
            int c = mask[g.index(g.wrapCol(i + 1), j + 1)] ? 1 : 0, d = mask[g.index(i, j + 1)] ? 1 : 0;
            int code = a | (b << 1) | (c << 2) | (d << 3);
            if (code == 0 || code == 15) continue;
            int64_t top = hEdge(i, j), right = vEdge(i + 1, j), bottom = hEdge(i, j + 1), left = vEdge(i, j);
            switch (code) {
                case 1: case 14: segs.emplace_back(left, top); break;
                case 2: case 13: segs.emplace_back(top, right); break;
                case 3: case 12: segs.emplace_back(left, right); break;
                case 4: case 11: segs.emplace_back(right, bottom); break;
                case 6: case 9: segs.emplace_back(top, bottom); break;
                case 7: case 8: segs.emplace_back(left, bottom); break;
                case 5: segs.emplace_back(left, top); segs.emplace_back(right, bottom); break;
                case 10: segs.emplace_back(top, right); segs.emplace_back(left, bottom); break;
                default: break;
            }
        }
    std::unordered_map<int64_t, std::vector<size_t>> byEdge;
    byEdge.reserve(segs.size() * 2);
    for (size_t s = 0; s < segs.size(); ++s) {
        byEdge[segs[s].first].push_back(s);
        byEdge[segs[s].second].push_back(s);
    }
    auto edgePoint = [&](int64_t e, double& lat, double& lon) {
        int64_t cell = e / 2;
        int i = static_cast<int>(cell % W), j = static_cast<int>(cell / W);
        if (e % 2 == 0) { lat = g.latDeg(j); lon = g.lonDeg(i) + 180.0 / W; }
        else { lat = 0.5 * (g.latDeg(j) + g.latDeg(j + 1)); lon = g.lonDeg(i); }
    };
    std::vector<uint8_t> used(segs.size(), 0);
    std::vector<Polyline> out;
    for (size_t s0 = 0; s0 < segs.size(); ++s0) {
        if (used[s0]) continue;
        used[s0] = 1;
        std::deque<int64_t> chain = {segs[s0].first, segs[s0].second};
        for (int dir = 0; dir < 2; ++dir) {
            while (true) {
                int64_t endE = dir == 0 ? chain.back() : chain.front();
                size_t next = SIZE_MAX;
                for (size_t cand : byEdge[endE]) if (!used[cand]) { next = cand; break; }
                if (next == SIZE_MAX) break;
                used[next] = 1;
                int64_t other = segs[next].first == endE ? segs[next].second : segs[next].first;
                if (dir == 0) chain.push_back(other);
                else chain.push_front(other);
            }
        }
        if (static_cast<int>(chain.size()) < minPoints) continue;
        Polyline line;
        for (int64_t e : chain) {
            double lat, lon;
            edgePoint(e, lat, lon);
            appendUnwrapped(line, lat, lon);
        }
        out.push_back(std::move(line));
    }
    return out;
}

}  // namespace mapgen
