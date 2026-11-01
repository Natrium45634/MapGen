// Phases C–D: plates, crust, relief, bathymetry, sea level and coasts.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/world/Categories.h"
#include "tests/TestSupport.h"

using namespace mapgen;

TEST_CASE("Tectonics: global coverage, connected plates, typed boundaries", "[tectonics]") {
    const WorldData& w = test::world();
    const uint16_t* plate = w.u16(LayerId::PlateId);
    REQUIRE(plate);
    int n = static_cast<int>(w.tectonics->plates.size());
    REQUIRE(n >= w.params.integer("tectonics.plateCountMin"));
    REQUIRE(n <= w.params.integer("tectonics.plateCountMax"));
    for (int64_t k = 0; k < w.grid.size(); ++k) REQUIRE(plate[k] < n);
    for (auto& c : w.reports[1].checks) if (c.p0) REQUIRE(c.passed);
    // Continents are not plates: some plate carries both crust types, and some continent spans several plates.
    const uint8_t* crust = w.u8(LayerId::CrustType);
    std::vector<int> cont(static_cast<size_t>(n)), ocean(static_cast<size_t>(n));
    for (int64_t k = 0; k < w.grid.size(); ++k) (crust[k] ? cont : ocean)[plate[k]]++;
    int mixed = 0;
    for (int s = 0; s < n; ++s) if (cont[static_cast<size_t>(s)] > 20 && ocean[static_cast<size_t>(s)] > 20) ++mixed;
    REQUIRE(mixed >= 1);
}

TEST_CASE("Tectonics: boundary roles are geologically consistent", "[tectonics]") {
    const WorldData& w = test::world("Kepler7", 512);
    const uint8_t* side = w.u8(LayerId::TectonicSide);
    const uint8_t* crust = w.u8(LayerId::CrustType);
    const float* age = w.f32(LayerId::CrustAge);
    const float* dRidge = w.f32(LayerId::DistRidge);
    int64_t ridgeOnContinent = 0, ridgeCells = 0;
    double youngNear = 0, oldFar = 0;
    int64_t nNear = 0, nFar = 0;
    for (int64_t k = 0; k < w.grid.size(); ++k) {
        if (side[k] == kSideRidge) { ++ridgeCells; if (crust[k]) ++ridgeOnContinent; }
        if (!crust[k]) {
            if (dRidge[k] < 300) { youngNear += age[k]; ++nNear; }
            else if (dRidge[k] > 2000 && dRidge[k] < 5000) { oldFar += age[k]; ++nFar; }
        }
    }
    REQUIRE(ridgeOnContinent == 0);  // continental divergence is a rift, not a mid-ocean ridge
    if (nNear > 50 && nFar > 50) REQUIRE(youngNear / nNear < oldFar / nFar);  // ocean floor ages away from ridges
}

TEST_CASE("Relief: oceanic depth follows crust age, continents stand high", "[relief]") {
    const WorldData& w = test::world("A7Test42", 512);
    const float* z = w.f32(LayerId::Elevation);
    const uint8_t* crust = w.u8(LayerId::CrustType);
    const float* age = w.f32(LayerId::CrustAge);
    const uint8_t* lf = w.u8(LayerId::Landform);
    double youngD = 0, oldD = 0;
    int64_t ny = 0, no = 0;
    double contMean = 0, oceanMean = 0;
    int64_t nc = 0, nO = 0;
    for (int64_t k = 0; k < w.grid.size(); ++k) {
        if (crust[k]) { contMean += z[k]; ++nc; }
        else {
            oceanMean += z[k];
            ++nO;
            if (lf[k] == kLfAbyssalPlain || lf[k] == kLfRidge) {
                if (age[k] < 10) { youngD += z[k]; ++ny; }
                if (age[k] > 80) { oldD += z[k]; ++no; }
            }
        }
    }
    REQUIRE(contMean / nc > oceanMean / nO + 2000);
    if (ny > 20 && no > 20) REQUIRE(youngD / ny > oldD / no + 800);  // ridges shallower than old abyssal plains
    for (auto& c : w.reports[2].checks) if (c.p0) REQUIRE(c.passed);
}

TEST_CASE("Relief: young mountains sit at convergent margins, not everywhere", "[relief]") {
    const WorldData& w = test::world("A7Test42", 512);
    const float* z = w.f32(LayerId::Elevation);
    const float* dSub = w.f32(LayerId::DistSubduction);
    const float* dCol = w.f32(LayerId::DistCollision);
    const float* dSut = w.f32(LayerId::TectonicIntensity);
    double sea = w.seaLevel();
    int64_t high = 0, highNearActive = 0;
    for (int64_t k = 0; k < w.grid.size(); ++k) {
        if (z[k] - sea < 3000) continue;
        ++high;
        if (dSub[k] < 700 || dCol[k] < 900 || dSut[k] > 0.15) ++highNearActive;
    }
    if (high > 0) REQUIRE(static_cast<double>(highNearActive) / high > 0.85);
}

TEST_CASE("Land/ocean: sea mask matches sea level and connectivity", "[ocean]") {
    for (const char* seed : {"A7Test42", "Kepler7", "Zz9"}) {
        const WorldData& w = test::world(seed, 256);
        const SphereGrid& g = w.grid;
        const float* z = w.f32(LayerId::Elevation);
        const uint8_t* ocean = w.u8(LayerId::OceanMask);
        double sea = w.seaLevel();
        // Recompute connectivity independently.
        std::vector<uint8_t> below(static_cast<size_t>(g.size()));
        for (int64_t k = 0; k < g.size(); ++k) below[static_cast<size_t>(k)] = z[k] < sea;
        std::vector<int32_t> lab;
        int nc = connectedComponents(g, below.data(), lab);
        std::vector<int> hasOcean(static_cast<size_t>(nc), 0);
        for (int64_t k = 0; k < g.size(); ++k) {
            if (ocean[k]) {
                REQUIRE(z[k] < sea);
                hasOcean[static_cast<size_t>(lab[static_cast<size_t>(k)])] = 1;
            }
        }
        for (int64_t k = 0; k < g.size(); ++k)
            if (below[static_cast<size_t>(k)] && hasOcean[static_cast<size_t>(lab[static_cast<size_t>(k)])]) REQUIRE(ocean[k]);
        for (auto& c : w.reports[3].checks) if (c.p0) REQUIRE(c.passed);
    }
}

TEST_CASE("Sea level modes: target fraction is reached; changing sea level keeps plates and relief", "[ocean]") {
    Params p = test::smallParams(256);
    p.set("ocean.seaLevelMode", std::string("fraction"));
    p.set("ocean.landFraction", 0.45);
    WorldData a = test::generate("A7Test42", p);
    REQUIRE(a.ocean->landFraction == Catch::Approx(0.45).margin(0.02));
    WorldData b = a;
    Params q = p;
    q.set("ocean.landFraction", 0.25);
    b.params = q;
    REQUIRE(firstInvalidatedStage(a.params, q) == 3);
    ThreadPool pool;
    CancelToken c;
    Pipeline::run(b, 3, pool, c);
    REQUIRE(b.stageHash[1] == a.stageHash[1]);
    REQUIRE(b.stageHash[2] == a.stageHash[2]);
    REQUIRE(b.stageHash[3] != a.stageHash[3]);
    REQUIRE(b.ocean->landFraction < a.ocean->landFraction);
    REQUIRE(b.layers[static_cast<size_t>(LayerId::PlateId)] == a.layers[static_cast<size_t>(LayerId::PlateId)]);  // shared, not regenerated
}

TEST_CASE("Polar scenarios are produced without artificial caps", "[ocean]") {
    Params p = test::smallParams(256, "polarContinent");
    WorldData w = test::generate("Polar1", p);
    const uint8_t* ocean = w.u8(LayerId::OceanMask);
    int land = 0;
    for (int i = 0; i < w.grid.width(); ++i) land += !ocean[w.grid.index(i, w.grid.height() - 1)];
    REQUIRE(land > w.grid.width() / 2);  // land at the south pole
    Params q = test::smallParams(256, "noPolar");
    WorldData v = test::generate("Polar1", q);
    const uint8_t* o2 = v.u8(LayerId::OceanMask);
    int sea = 0;
    for (int i = 0; i < v.grid.width(); ++i) sea += o2[v.grid.index(i, 0)] + o2[v.grid.index(i, v.grid.height() - 1)];
    REQUIRE(sea > v.grid.width());  // both poles mostly ocean
}
