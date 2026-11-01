#pragma once

// Global equirectangular grid with spherical topology.
//
// Cell (i, j): column i in [0, W) eastwards from -180°, row j in [0, H) southwards
// from +90°. Longitude wraps (i = -1 is i = W-1). Crossing a pole continues on
// the same row on the opposite meridian (i + W/2), so there is no seam and no
// artificial pole cap. Geographic coordinates, great-circle distances and true
// cell areas are stored separately from the raster indexing.

#include <array>
#include <cstdint>
#include <vector>

#include "core/util/Math.h"

namespace mapgen {

class SphereGrid {
public:
    SphereGrid() = default;
    SphereGrid(int width, int height, double radiusKm = 6371.0);

    int width() const { return W_; }
    int height() const { return H_; }
    int64_t size() const { return static_cast<int64_t>(W_) * H_; }
    double radiusKm() const { return R_; }
    bool valid() const { return W_ > 0 && H_ > 0; }

    int64_t index(int i, int j) const { return static_cast<int64_t>(j) * W_ + i; }
    int col(int64_t idx) const { return static_cast<int>(idx % W_); }
    int row(int64_t idx) const { return static_cast<int>(idx / W_); }
    int wrapCol(int i) const { int r = i % W_; return r < 0 ? r + W_ : r; }

    // Cell-centre coordinates.
    double latDeg(int j) const { return 90.0 - (j + 0.5) * 180.0 / H_; }
    double lonDeg(int i) const { return -180.0 + (i + 0.5) * 360.0 / W_; }
    double latRad(int j) const { return lat_[static_cast<size_t>(j)]; }
    double lonRad(int i) const { return lon_[static_cast<size_t>(i)]; }
    double cosLat(int j) const { return cosLat_[static_cast<size_t>(j)]; }
    const Vec3& unit(int64_t idx) const { return unit_[static_cast<size_t>(idx)]; }
    Vec3 unitAt(int i, int j) const { return unit_[static_cast<size_t>(index(i, j))]; }

    // Exact spherical area of a cell in row j (km²) and of the whole sphere.
    double cellAreaKm2(int j) const { return area_[static_cast<size_t>(j)]; }
    double totalAreaKm2() const { return 4.0 * kPi * R_ * R_; }
    double dLatRad() const { return kPi / H_; }
    double dLonRad() const { return kTwoPi / W_; }
    // Physical size of a cell (km): east-west at row j, north-south.
    double dxKm(int j) const { return R_ * cosLat(j) * dLonRad(); }
    double dyKm() const { return R_ * dLatRad(); }

    // Fractional raster coordinates of a geographic point (cell centres at .5 offsets removed).
    void geoToGrid(double latDeg, double lonDeg, double& x, double& y) const;
    // Cell containing a geographic point.
    int64_t cellAt(double latDeg, double lonDeg) const;
    int64_t cellAtUnit(const Vec3& p) const { return cellAt(latOf(p) * kRadToDeg, lonOf(p) * kRadToDeg); }

    // 8-neighbourhood with longitude wrap and pole crossing. Order: N, NE, E, SE, S, SW, W, NW.
    static constexpr int kDi[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    static constexpr int kDj[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    int64_t neighbor(int i, int j, int dir) const;
    void neighbors8(int64_t idx, int64_t out[8]) const;
    // Great-circle distance from a cell in row j to its neighbour in direction dir (km).
    double neighborDistKm(int j, int dir) const { return ndist_[static_cast<size_t>(j) * 8 + dir]; }

    double greatCircleKm(int64_t a, int64_t b) const { return R_ * angleBetween(unit(a), unit(b)); }

    // Bilinear sample of a cell-centred field at fractional grid coordinates (x wraps, y clamps).
    template <class T>
    double sampleBilinear(const T* field, double x, double y) const;

private:
    int W_ = 0, H_ = 0;
    double R_ = 6371.0;
    std::vector<double> lat_, lon_, cosLat_, area_, ndist_;
    std::vector<Vec3> unit_;
};

template <class T>
double SphereGrid::sampleBilinear(const T* field, double x, double y) const {
    double fx = x - 0.5, fy = y - 0.5;
    if (fy < 0) fy = 0;
    if (fy > H_ - 1) fy = H_ - 1;
    double x0f = std::floor(fx), y0f = std::floor(fy);
    int x0 = static_cast<int>(x0f), y0 = static_cast<int>(y0f);
    double tx = fx - x0f, ty = fy - y0f;
    int x1 = x0 + 1, y1 = std::min(y0 + 1, H_ - 1);
    x0 = wrapCol(x0);
    x1 = wrapCol(x1);
    double a = static_cast<double>(field[index(x0, y0)]), b = static_cast<double>(field[index(x1, y0)]);
    double c = static_cast<double>(field[index(x0, y1)]), d = static_cast<double>(field[index(x1, y1)]);
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
}

// Resampling helpers between two global grids.
// Area-weighted average from a fine grid to a coarse grid (integer factor not required).
void downsampleMean(const SphereGrid& fine, const float* src, const SphereGrid& coarse, float* dst);
// Bilinear interpolation from a coarse grid to a fine grid.
void upsampleBilinear(const SphereGrid& coarse, const float* src, const SphereGrid& fine, float* dst);

// Multi-source geodesic distance transform (Dijkstra on the 8-neighbour graph with
// true great-circle edge lengths). dist = 0 at sources; cells with allowed==0 are not
// traversed (pass nullptr for all). Returns the source index reaching each cell in nearest.
// Distances beyond maxKm are left at maxKm. If region is given, the front only spreads
// into cells whose region equals the region of the source that reached them.
void distanceTransform(const SphereGrid& g, const std::vector<int64_t>& sources, std::vector<float>& dist,
                       std::vector<int32_t>* nearest, double maxKm, const uint8_t* allowed = nullptr,
                       const int32_t* region = nullptr);

// Connected components (8-connectivity, spherical topology) of cells where mask != 0.
// Returns the number of components; labels: -1 for masked-out cells, component id otherwise.
// Components are numbered in order of their first cell index (deterministic).
int connectedComponents(const SphereGrid& g, const uint8_t* mask, std::vector<int32_t>& labels);

}  // namespace mapgen
