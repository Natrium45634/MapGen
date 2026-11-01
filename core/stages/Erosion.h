#pragma once

// Landscape evolution helpers: priority-flood depression filling, drainage
// ordering, implicit stream-power incision (Braun & Willett 2013, n = 1),
// hillslope diffusion, sediment deposition in closed lows, spike removal.

#include <vector>

#include "core/grids/SphereGrid.h"
#include "core/util/Parallel.h"

namespace mapgen {

struct DrainageGraph {
    std::vector<int64_t> order;     // topological order: every receiver precedes its donors
    std::vector<int64_t> receiver;  // -1 for base-level cells
    std::vector<float> filled;      // depression-filled surface (strictly decreasing to base level on land)
};

// Base-level cells are those with base[k] != 0. Ties are broken by cell index, so the
// result is deterministic. Filled values get +eps steps along flats so flow is defined.
DrainageGraph buildDrainage(const SphereGrid& g, const std::vector<float>& z, const std::vector<uint8_t>& base,
                            double epsilon = 1e-3);

// Upstream accumulation of a per-cell source term (e.g. area × runoff) along the graph.
std::vector<double> accumulate(const SphereGrid& g, const DrainageGraph& d, const std::vector<double>& local);

struct ErosionParams {
    int iterations = 4;
    double k = 0.03;              // stream-power coefficient per iteration (dimensionless with sqrt(area)/length)
    double m = 0.5;
    double diffusion = 0.08;      // hillslope diffusion per iteration (fraction of Laplacian)
    double deposition = 0.35;     // fraction of each closed depression filled with sediment per iteration
    double maxSpikeM = 1600;      // spikes above all neighbours by more than this are flattened
};

// Erodes land cells (base == 0) in place; returns per-cell erosion depth (+) / deposition (−).
std::vector<float> erodeLandscape(const SphereGrid& g, std::vector<float>& z, const std::vector<uint8_t>& base,
                                  const std::vector<float>& precipFactor, const ErosionParams& p, ThreadPool& pool,
                                  const CancelToken& cancel);

}  // namespace mapgen
