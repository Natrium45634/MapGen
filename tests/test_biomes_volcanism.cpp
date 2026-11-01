// Phases G–H: Köppen, biomes, belts, coasts, volcanism.

#include <catch2/catch_test_macros.hpp>

#include "core/world/Categories.h"
#include "core/world/CellInfo.h"
#include "tests/TestSupport.h"

using namespace mapgen;

TEST_CASE("Biomes: mangroves not in cold or inland places, no forest under ice, ocean has no land biome", "[biomes]") {
    for (const char* seed : {"A7Test42", "Kepler7", "Mars3"}) {
        const WorldData& w = test::world(seed, 512);
        const uint8_t* biome = w.u8(LayerId::Biome);
        const uint8_t* ocean = w.u8(LayerId::OceanMask);
        const uint8_t* ice = w.u8(LayerId::IceType);
        const float* tmin = w.f32(LayerId::TempMin);
        const float* dc = w.f32(LayerId::DistCoast);
        for (int64_t k = 0; k < w.grid.size(); ++k) {
            if (ocean[k]) { REQUIRE(biome[k] == kBiomeOcean); continue; }
            REQUIRE(biome[k] != kBiomeNone);
            if (biome[k] == kBiomeMangrove) {
                REQUIRE(tmin[k] >= 10.0f);
                REQUIRE(dc[k] < 3.0f * w.grid.dyKm());
            }
            if (ice[k] == kIceSheet || ice[k] == kIceGlacier) {
                REQUIRE(biome[k] != kBiomeTropicalRainforest);
                REQUIRE(biome[k] != kBiomeTaiga);
                REQUIRE(biome[k] != kBiomeTemperateForest);
            }
        }
        for (auto& c : w.reports[6].checks) if (c.p0) REQUIRE(c.passed);
    }
}

TEST_CASE("Altitudinal belts follow local climate, not a fixed world height", "[biomes]") {
    double loH = 0, hiH = 0;
    int nLo = 0, nHi = 0;
    for (const char* seed : {"A7Test42", "Kepler7", "Mars3"}) {
        const WorldData& w = test::world(seed, 512);
        const uint8_t* belt = w.u8(LayerId::AltBelt);
        const float* z = w.f32(LayerId::Elevation);
        for (int j = 0; j < w.grid.height(); ++j) {
            double alat = std::fabs(w.grid.latDeg(j));
            for (int i = 0; i < w.grid.width(); ++i) {
                int64_t k = w.grid.index(i, j);
                if (belt[k] != kBeltAlpine) continue;
                double h = z[k] - w.seaLevel();
                if (alat < 30) { loH += h; ++nLo; }
                else if (alat > 50) { hiH += h; ++nHi; }
            }
        }
    }
    REQUIRE(nLo > 3);
    REQUIRE(nHi > 3);
    REQUIRE(loH / nLo > hiH / nHi + 800);  // tropical alpine belt lies much higher
}

TEST_CASE("Climate classes appear in plausible shares", "[biomes]") {
    const WorldData& w = test::world("A7Test42", 512);
    double a = w.reports[6].metricValue("koppenA"), b = w.reports[6].metricValue("koppenB");
    double c = w.reports[6].metricValue("koppenC"), d = w.reports[6].metricValue("koppenD"), e = w.reports[6].metricValue("koppenE");
    REQUIRE(a + b + c + d + e > 0.99);
    REQUIRE(b > 0.05);  // dry climates exist
    REQUIRE(a > 0.03);  // tropical climates exist on an Earth-like planet
}

TEST_CASE("Volcanism: arcs on the overriding plate, hotspots inside plates, no transform volcanic rows", "[volcanism]") {
    for (const char* seed : {"A7Test42", "Kepler7", "Mars3"}) {
        const WorldData& w = test::world(seed, 512);
        for (auto& c : w.reports[7].checks) if (c.p0) REQUIRE(c.passed);
        const float* dSub = w.f32(LayerId::DistSubduction);
        const float* dTr = w.f32(LayerId::DistTrench);
        const float* dTrf = w.f32(LayerId::DistTransform);
        const float* dRid = w.f32(LayerId::DistRidge);
        int arcs = 0, onTransform = 0;
        for (auto& v : w.geomorph->volcanoes) {
            if (v.genesis == kVgArcContinental || v.genesis == kVgArcIsland) {
                ++arcs;
                REQUIRE(dSub[v.cell] < 450);  // overriding side, close to the subduction zone
                REQUIRE(dTr[v.cell] > 150);   // not on the subducting plate's trench side
            }
            if (dTrf[v.cell] < 50 && dSub[v.cell] > 400 && dRid[v.cell] > 250 && v.hotspotId < 0) ++onTransform;
        }
        REQUIRE(onTransform * 20 <= static_cast<int>(w.geomorph->volcanoes.size()) + 1);
        // Older hotspot volcanoes are not equally active.
        for (size_t i = 0; i < w.geomorph->volcanoes.size(); ++i) {
            const Volcano& v = w.geomorph->volcanoes[i];
            if (v.hotspotId >= 0 && v.ageMyr > 10) REQUIRE(w.geoActivity->volcanoStates[i].activity == kVaExtinct);
        }
        int geysers = 0;
        for (auto& s : w.geoActivity->springs) geysers += s.geyser;
        REQUIRE(geysers * 4 <= static_cast<int>(w.geoActivity->springs.size()) + 1);
        (void)arcs;
    }
}

TEST_CASE("Info card shows 'не рассчитано' instead of zeros and no land biome at sea", "[ui-data]") {
    const WorldData& w = test::world("A7Test42", 256);
    const uint8_t* ocean = w.u8(LayerId::OceanMask);
    int64_t sea = -1, land = -1;
    for (int64_t k = 0; k < w.grid.size() && (sea < 0 || land < 0); ++k) (ocean[k] ? sea : land) = k;
    auto fs = describeCell(w, sea, 6);
    for (auto& f : fs) {
        if (f.key == "biome") REQUIRE(f.valueRu.find("океан") != std::string::npos);
        if (f.key == "height") FAIL("ocean cell must show depth, not height");
    }
    auto fl = describeCell(w, land, 6);
    bool hasHeight = false;
    for (auto& f : fl) {
        if (f.key == "depth") FAIL("land cell must show height, not depth");
        hasHeight |= f.key == "height";
    }
    REQUIRE(hasHeight);
    // A world without later stages reports missing values as "не рассчитано", never as zeros.
    WorldData partial;
    partial.seed = "Part1";
    partial.params = test::smallParams(128);
    ThreadPool pool;
    CancelToken c;
    Pipeline::run(partial, 1, pool, c);
    partial.clearFromStage(4);
    for (auto& f : describeCell(partial, 100, 3))
        if (f.key == "temp" || f.key == "prec" || f.key == "biome" || f.key == "koppen") {
            REQUIRE_FALSE(f.available);
            REQUIRE(f.valueRu == kNotComputedRu);
        }
}
