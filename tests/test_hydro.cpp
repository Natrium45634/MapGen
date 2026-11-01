// Phase F: strict hydrology checks (Hydro 1, Hydro 2, endorheic basins, vector rivers).

#include <catch2/catch_test_macros.hpp>
#include <set>

#include "core/world/Categories.h"
#include "tests/TestSupport.h"

using namespace mapgen;

namespace {
int64_t receiverOf(const WorldData& w, int64_t k) {
    const uint8_t* fd = w.u8(LayerId::FlowDir);
    if (fd[k] >= 8) return -1;
    return w.grid.neighbor(w.grid.col(k), w.grid.row(k), fd[k]);
}
}  // namespace

TEST_CASE("Hydro 1: rivers descend on the hydrologic surface; no infinite cycles", "[hydro]") {
    for (const char* seed : {"A7Test42", "Kepler7", "Mars3"}) {
        const WorldData& w = test::world(seed, 512);
        const float* hs = w.f32(LayerId::HydroSurface);
        const uint8_t* rc = w.u8(LayerId::RiverClass);
        const uint8_t* ocean = w.u8(LayerId::OceanMask);
        const int64_t N = w.grid.size();
        // Kahn's algorithm on the flow graph: every cell must be ordered (no cycle).
        std::vector<int> indeg(static_cast<size_t>(N), 0);
        for (int64_t k = 0; k < N; ++k) {
            int64_t r = receiverOf(w, k);
            if (r >= 0 && !ocean[k]) indeg[static_cast<size_t>(r)]++;
        }
        std::vector<int64_t> q;
        for (int64_t k = 0; k < N; ++k) if (indeg[static_cast<size_t>(k)] == 0) q.push_back(k);
        size_t seen = 0;
        while (!q.empty()) {
            int64_t c = q.back();
            q.pop_back();
            ++seen;
            int64_t r = ocean[c] ? -1 : receiverOf(w, c);
            if (r >= 0 && --indeg[static_cast<size_t>(r)] == 0) q.push_back(r);
        }
        REQUIRE(static_cast<int64_t>(seen) == N);
        for (int64_t k = 0; k < N; ++k) {
            if (!rc[k] || ocean[k]) continue;
            int64_t r = receiverOf(w, k);
            if (r < 0 || ocean[r]) continue;
            REQUIRE(hs[r] <= hs[k] + 1e-3f);
        }
        for (auto& c : w.reports[5].checks) if (c.p0) REQUIRE(c.passed);
    }
}

TEST_CASE("Hydro 2: basins connected, confluences add up, terminals consistent", "[hydro]") {
    const WorldData& w = test::world("A7Test42", 512);
    const SphereGrid& g = w.grid;
    const int32_t* basin = w.i32(LayerId::BasinId);
    const float* q = w.f32(LayerId::Discharge);
    const int32_t* lake = w.i32(LayerId::LakeId);
    const uint8_t* ocean = w.u8(LayerId::OceanMask);
    const uint8_t* fd = w.u8(LayerId::FlowDir);
    // Every basin is one connected region.
    std::vector<uint8_t> visited(static_cast<size_t>(g.size()), 0);
    std::set<int32_t> started;
    for (int64_t s = 0; s < g.size(); ++s) {
        if (ocean[s] || visited[static_cast<size_t>(s)]) continue;
        int32_t b = basin[s];
        REQUIRE(b >= 0);
        REQUIRE(!started.count(b));  // a second component of the same basin would fail here
        started.insert(b);
        std::vector<int64_t> st = {s};
        visited[static_cast<size_t>(s)] = 1;
        while (!st.empty()) {
            int64_t c = st.back();
            st.pop_back();
            int64_t nb[8];
            g.neighbors8(c, nb);
            for (int d = 0; d < 8; ++d)
                if (!ocean[nb[d]] && !visited[static_cast<size_t>(nb[d])] && basin[nb[d]] == b) {
                    visited[static_cast<size_t>(nb[d])] = 1;
                    st.push_back(nb[d]);
                }
        }
    }
    // Downstream discharge never drops on land outside lakes (no unexplained losses).
    for (int64_t k = 0; k < g.size(); ++k) {
        if (ocean[k]) continue;
        int64_t r = receiverOf(w, k);
        if (r < 0 || ocean[r] || lake[r] >= 0 || lake[k] >= 0) continue;
        REQUIRE(q[r] >= q[k] - 1e-6f * std::max(1.0f, q[k]));
    }
    // Terminal sinks are terminal lakes or dry sinks of endorheic basins.
    for (int64_t k = 0; k < g.size(); ++k)
        if (!ocean[k] && fd[k] == kFlowSink) REQUIRE(w.hydro->basins[static_cast<size_t>(basin[k])].outlet != kBoOcean);
}

TEST_CASE("Endorheic basins concentrate in dry climates; humid basins drain to the sea", "[hydro]") {
    double aridEndo = 0, aridAll = 0, humidEndo = 0, humidAll = 0;
    for (const char* seed : {"A7Test42", "Kepler7", "Mars3"}) {
        const WorldData& w = test::world(seed, 512);
        const float* ai = w.f32(LayerId::Aridity);
        const int32_t* basin = w.i32(LayerId::BasinId);
        const uint8_t* ocean = w.u8(LayerId::OceanMask);
        for (int64_t k = 0; k < w.grid.size(); ++k) {
            if (ocean[k]) continue;
            double a = w.grid.cellAreaKm2(w.grid.row(k));
            bool endo = w.hydro->basins[static_cast<size_t>(basin[k])].outlet != kBoOcean;
            if (ai[k] < 0.3) { aridAll += a; if (endo) aridEndo += a; }
            if (ai[k] > 1.0) { humidAll += a; if (endo) humidEndo += a; }
        }
    }
    REQUIRE(aridAll > 0);
    REQUIRE(humidAll > 0);
    REQUIRE(aridEndo / aridAll > humidEndo / humidAll);
}

TEST_CASE("Vector rivers stay inside their basin corridor; desert rivers are fed from upstream", "[hydro]") {
    const WorldData& w = test::world("Kepler7", 512);
    const SphereGrid& g = w.grid;
    const int32_t* basin = w.i32(LayerId::BasinId);
    const uint8_t* ocean = w.u8(LayerId::OceanMask);
    const int32_t* lake = w.i32(LayerId::LakeId);
    for (auto& r : w.hydro->rivers) {
        if (r.riverClass == kRiverDistributary) continue;
        std::set<int64_t> corridor(r.cells.begin(), r.cells.end());
        int64_t next = receiverOf(w, r.cells.back());
        if (next >= 0) corridor.insert(next);
        for (auto& p : r.line.pts) {
            int64_t c = g.cellAt(p.lat, wrapLonDeg(p.lon));
            if (corridor.count(c) || ocean[c] || lake[c] >= 0) continue;
            // Smoothing may touch a neighbouring cell, but never one of another basin.
            REQUIRE(basin[c] == r.basinId);
        }
    }
    const float* ai = w.f32(LayerId::Aridity);
    const float* area = w.f32(LayerId::DrainageArea);
    const uint8_t* rc = w.u8(LayerId::RiverClass);
    for (int64_t k = 0; k < g.size(); ++k)
        if (rc[k] && rc[k] != kRiverDistributary && ai[k] < 0.05) REQUIRE(area[k] > 3 * g.cellAreaKm2(g.row(k)));  // exotic rivers come from wetter areas
}
