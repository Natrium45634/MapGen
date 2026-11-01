#pragma once

// Helpers shared by the stages.

#include <functional>
#include <vector>

#include "core/grids/SphereGrid.h"
#include "core/rng/Random.h"
#include "core/world/Features.h"

namespace mapgen {

Vec3 randomUnitVector(Pcg64& rng);

// Keeps the largest connected component of every label; smaller pieces are given to
// the most frequent neighbouring label. Returns the number of reassigned pieces.
int enforceConnectedRegions(const SphereGrid& g, std::vector<int32_t>& labels, int nLabels);

// Value v such that the area-weighted fraction of cells with field <= v equals q.
double areaWeightedQuantile(const SphereGrid& g, const std::vector<float>& field, double q,
                            const uint8_t* mask = nullptr);

// Polylines along plate boundaries traced through boundary cells of a structural grid.
std::vector<BoundarySegment> traceBoundarySegments(const SphereGrid& sg, const std::vector<int32_t>& plates, size_t count,
                                                   const std::function<int64_t(size_t)>& cellOf,
                                                   const std::function<void(size_t, BoundarySegment&)>& fill);

// Appends lat/lon of a cell to a polyline keeping longitudes continuous (unwrapped).
void appendUnwrapped(Polyline& line, double lat, double lon);

// Contours of a binary mask (marching squares on cell centres) as unwrapped polylines.
std::vector<Polyline> maskContours(const SphereGrid& g, const uint8_t* mask, int minPoints = 3);

}  // namespace mapgen
