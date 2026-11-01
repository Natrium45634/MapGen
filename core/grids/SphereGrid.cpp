#include "core/grids/SphereGrid.h"

#include <queue>
#include <stdexcept>

namespace mapgen {

SphereGrid::SphereGrid(int width, int height, double radiusKm) : W_(width), H_(height), R_(radiusKm) {
    if (W_ < 8 || H_ < 4 || (W_ % 2) != 0) throw std::invalid_argument("grid: width must be even and >= 8");
    lat_.resize(static_cast<size_t>(H_));
    cosLat_.resize(static_cast<size_t>(H_));
    area_.resize(static_cast<size_t>(H_));
    lon_.resize(static_cast<size_t>(W_));
    for (int j = 0; j < H_; ++j) {
        lat_[static_cast<size_t>(j)] = latDeg(j) * kDegToRad;
        cosLat_[static_cast<size_t>(j)] = std::cos(lat_[static_cast<size_t>(j)]);
        double top = (90.0 - j * 180.0 / H_) * kDegToRad;
        double bot = (90.0 - (j + 1) * 180.0 / H_) * kDegToRad;
        area_[static_cast<size_t>(j)] = R_ * R_ * dLonRad() * (std::sin(top) - std::sin(bot));
    }
    for (int i = 0; i < W_; ++i) lon_[static_cast<size_t>(i)] = lonDeg(i) * kDegToRad;
    unit_.resize(static_cast<size_t>(size()));
    for (int j = 0; j < H_; ++j)
        for (int i = 0; i < W_; ++i) unit_[static_cast<size_t>(index(i, j))] = fromLatLon(lat_[static_cast<size_t>(j)], lon_[static_cast<size_t>(i)]);
    ndist_.resize(static_cast<size_t>(H_) * 8);
    for (int j = 0; j < H_; ++j)
        for (int d = 0; d < 8; ++d) {
            int64_t n = neighbor(W_ / 4, j, d);
            ndist_[static_cast<size_t>(j) * 8 + d] = greatCircleKm(index(W_ / 4, j), n);
        }
}

int64_t SphereGrid::neighbor(int i, int j, int dir) const {
    int ni = i + kDi[dir];
    int nj = j + kDj[dir];
    if (nj < 0) {
        nj = -nj - 1;
        ni += W_ / 2;
    } else if (nj >= H_) {
        nj = 2 * H_ - nj - 1;
        ni += W_ / 2;
    }
    return index(wrapCol(ni), nj);
}

void SphereGrid::neighbors8(int64_t idx, int64_t out[8]) const {
    int i = col(idx), j = row(idx);
    for (int d = 0; d < 8; ++d) out[d] = neighbor(i, j, d);
}

void SphereGrid::geoToGrid(double lat, double lon, double& x, double& y) const {
    x = (wrapLonDeg(lon) + 180.0) / 360.0 * W_;
    y = (90.0 - clampv(lat, -90.0, 90.0)) / 180.0 * H_;
}

int64_t SphereGrid::cellAt(double lat, double lon) const {
    double x, y;
    geoToGrid(lat, lon, x, y);
    int i = wrapCol(static_cast<int>(std::floor(x)));
    int j = clampv(static_cast<int>(std::floor(y)), 0, H_ - 1);
    return index(i, j);
}

void downsampleMean(const SphereGrid& fine, const float* src, const SphereGrid& coarse, float* dst) {
    const int fw = fine.width(), fh = fine.height(), cw = coarse.width(), ch = coarse.height();
    std::vector<double> sum(static_cast<size_t>(coarse.size()), 0.0), wsum(static_cast<size_t>(coarse.size()), 0.0);
    for (int j = 0; j < fh; ++j) {
        int cj = std::min(ch - 1, j * ch / fh);
        double w = fine.cellAreaKm2(j);
        for (int i = 0; i < fw; ++i) {
            int ci = std::min(cw - 1, i * cw / fw);
            size_t k = static_cast<size_t>(coarse.index(ci, cj));
            sum[k] += w * src[fine.index(i, j)];
            wsum[k] += w;
        }
    }
    for (size_t k = 0; k < sum.size(); ++k) dst[k] = wsum[k] > 0 ? static_cast<float>(sum[k] / wsum[k]) : 0.0f;
}

void upsampleBilinear(const SphereGrid& coarse, const float* src, const SphereGrid& fine, float* dst) {
    const double sx = static_cast<double>(coarse.width()) / fine.width();
    const double sy = static_cast<double>(coarse.height()) / fine.height();
    for (int j = 0; j < fine.height(); ++j)
        for (int i = 0; i < fine.width(); ++i)
            dst[fine.index(i, j)] = static_cast<float>(coarse.sampleBilinear(src, (i + 0.5) * sx, (j + 0.5) * sy));
}

void distanceTransform(const SphereGrid& g, const std::vector<int64_t>& sources, std::vector<float>& dist,
                       std::vector<int32_t>* nearest, double maxKm, const uint8_t* allowed, const int32_t* region) {
    const int64_t n = g.size();
    dist.assign(static_cast<size_t>(n), static_cast<float>(maxKm));
    std::vector<double> d(static_cast<size_t>(n), maxKm);
    std::vector<int32_t> src(static_cast<size_t>(n), -1);
    using Item = std::pair<double, int64_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    for (size_t s = 0; s < sources.size(); ++s) {
        int64_t c = sources[s];
        if (d[static_cast<size_t>(c)] > 0.0 || src[static_cast<size_t>(c)] < 0) {
            d[static_cast<size_t>(c)] = 0.0;
            src[static_cast<size_t>(c)] = static_cast<int32_t>(s);
            pq.push({0.0, c});
        }
    }
    while (!pq.empty()) {
        auto [dc, c] = pq.top();
        pq.pop();
        if (dc > d[static_cast<size_t>(c)]) continue;
        int i = g.col(c), j = g.row(c);
        int32_t s = src[static_cast<size_t>(c)];
        int64_t sc = sources[static_cast<size_t>(s)];
        for (int dir = 0; dir < 8; ++dir) {
            int64_t nb = g.neighbor(i, j, dir);
            if (allowed && !allowed[nb]) continue;
            if (region && region[nb] != region[sc]) continue;
            double nd = dc + g.neighborDistKm(j, dir);
            if (nd < d[static_cast<size_t>(nb)] && nd < maxKm) {
                d[static_cast<size_t>(nb)] = nd;
                src[static_cast<size_t>(nb)] = s;
                pq.push({nd, nb});
            }
        }
    }
    for (int64_t k = 0; k < n; ++k) dist[static_cast<size_t>(k)] = static_cast<float>(d[static_cast<size_t>(k)]);
    if (nearest) *nearest = std::move(src);
}

int connectedComponents(const SphereGrid& g, const uint8_t* mask, std::vector<int32_t>& labels) {
    const int64_t n = g.size();
    labels.assign(static_cast<size_t>(n), -1);
    int next = 0;
    std::vector<int64_t> stack;
    for (int64_t start = 0; start < n; ++start) {
        if (!mask[start] || labels[static_cast<size_t>(start)] >= 0) continue;
        labels[static_cast<size_t>(start)] = next;
        stack.push_back(start);
        while (!stack.empty()) {
            int64_t c = stack.back();
            stack.pop_back();
            int i = g.col(c), j = g.row(c);
            for (int dir = 0; dir < 8; ++dir) {
                int64_t nb = g.neighbor(i, j, dir);
                if (mask[nb] && labels[static_cast<size_t>(nb)] < 0) {
                    labels[static_cast<size_t>(nb)] = next;
                    stack.push_back(nb);
                }
            }
        }
        ++next;
    }
    return next;
}

}  // namespace mapgen
