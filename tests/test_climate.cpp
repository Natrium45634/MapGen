// Phase E: climate macro-patterns.

#include <catch2/catch_test_macros.hpp>

#include "core/world/Categories.h"
#include "tests/TestSupport.h"

using namespace mapgen;

namespace {
double zonalMean(const WorldData& w, const float* f, double latLo, double latHi, int landFilter /*-1 any,0 ocean,1 land*/) {
    double s = 0, a = 0;
    const uint8_t* ocean = w.u8(LayerId::OceanMask);
    for (int j = 0; j < w.grid.height(); ++j) {
        double lat = w.grid.latDeg(j);
        if (lat < latLo || lat >= latHi) continue;
        for (int i = 0; i < w.grid.width(); ++i) {
            int64_t k = w.grid.index(i, j);
            if (landFilter == 1 && ocean[k]) continue;
            if (landFilter == 0 && !ocean[k]) continue;
            s += f[k] * w.grid.cellAreaKm2(j);
            a += w.grid.cellAreaKm2(j);
        }
    }
    return a > 0 ? s / a : 0;
}
}  // namespace

TEST_CASE("Climate: seasonality changes with latitude and hemisphere", "[climate]") {
    const WorldData& w = test::world("A7Test42", 256);
    const int16_t* tm = w.i16(LayerId::TempMonthly);
    const SphereGrid& g = w.grid;
    std::vector<float> jan(static_cast<size_t>(g.size())), jul(static_cast<size_t>(g.size())), amp(static_cast<size_t>(g.size()));
    for (int64_t k = 0; k < g.size(); ++k) {
        jan[static_cast<size_t>(k)] = tm[k] * 0.01f;
        jul[static_cast<size_t>(k)] = tm[6 * g.size() + k] * 0.01f;
        amp[static_cast<size_t>(k)] = std::fabs(jul[static_cast<size_t>(k)] - jan[static_cast<size_t>(k)]);
    }
    REQUIRE(zonalMean(w, jul.data(), 30, 70, -1) > zonalMean(w, jan.data(), 30, 70, -1) + 4);    // NH summer in July
    REQUIRE(zonalMean(w, jan.data(), -70, -30, -1) > zonalMean(w, jul.data(), -70, -30, -1) + 3);  // SH summer in January
    // Seasonal amplitude grows away from the equator.
    REQUIRE(zonalMean(w, amp.data(), 45, 70, -1) > zonalMean(w, amp.data(), -10, 10, -1) + 3);
    const float* tmean = w.f32(LayerId::TempMean);
    REQUIRE(zonalMean(w, tmean, -10, 10, -1) > zonalMean(w, tmean, 60, 90, -1) + 15);
    for (auto& c : w.reports[4].checks) if (c.p0) REQUIRE(c.passed);
}

TEST_CASE("Climate: colder with height, all else equal", "[climate]") {
    const WorldData& w = test::world("A7Test42", 256);
    const float* tmean = w.f32(LayerId::TempMean);
    const float* lapse = w.f32(LayerId::LapseRate);
    const float* z = w.f32(LayerId::Elevation);
    const uint8_t* ocean = w.u8(LayerId::OceanMask);
    // Neighbouring land cells: the higher one is colder by about the lapse rate.
    int checked = 0, ok = 0;
    for (int64_t k = 0; k < w.grid.size(); ++k) {
        if (ocean[k]) continue;
        int64_t nb[8];
        w.grid.neighbors8(k, nb);
        for (int d = 0; d < 8; d += 2) {
            int64_t q = nb[d];
            if (ocean[q] || z[q] - z[k] < 800) continue;
            ++checked;
            if (tmean[q] < tmean[k] - 0.5 * lapse[k] * (z[q] - z[k]) / 1000.0) ++ok;
        }
    }
    if (checked > 0) REQUIRE(ok >= 0.9 * checked);
}

TEST_CASE("Climate: windward slopes wetter than lee (rain shadow)", "[climate][slow]") {
    // Statistical rain-shadow check over the mountain cells of several worlds: precipitation on the
    // windward side (wind blowing uphill) exceeds that on the leeward side.
    double wind = 0, lee = 0;
    int nW = 0, nL = 0;
    for (const char* seed : {"A7Test42", "Kepler7", "Mars3"}) {
        const WorldData& w = test::world(seed, 512);
        const SphereGrid& g = w.grid;
        const float* z = w.f32(LayerId::Elevation);
        const uint8_t* ocean = w.u8(LayerId::OceanMask);
        const uint16_t* pm = w.u16(LayerId::PrecMonthly);
        for (int m : {0, 6}) {
            for (int j = 2; j < g.height() - 2; ++j)
                for (int i = 0; i < g.width(); ++i) {
                    int64_t k = g.index(i, j);
                    if (ocean[k] || z[k] - w.seaLevel() < 800) continue;
                    double u = w.sample(LayerId::WindU, k, m), v = w.sample(LayerId::WindV, k, m);
                    double sp = std::hypot(u, v);
                    if (sp < 2) continue;
                    double gx = (z[g.neighbor(i, j, 2)] - z[g.neighbor(i, j, 6)]) / (2 * g.dxKm(j));
                    double gy = (z[g.neighbor(i, j, 0)] - z[g.neighbor(i, j, 4)]) / (2 * g.dyKm());
                    double up = (u * gx + v * gy) / sp;  // m/km along the wind
                    double p = pm[m * g.size() + k] * 0.1;
                    if (up > 15) { wind += p; ++nW; }
                    else if (up < -15) { lee += p; ++nL; }
                }
        }
    }
    REQUIRE(nW > 20);
    REQUIRE(nL > 20);
    REQUIRE(wind / nW > 1.2 * lee / nL);
}

TEST_CASE("Climate: humidity metrics are distinct and in range", "[climate]") {
    const WorldData& w = test::world("A7Test42", 256);
    const float* ai = w.f32(LayerId::Aridity);
    const float* soil = w.f32(LayerId::SoilMean);
    const uint8_t* ocean = w.u8(LayerId::OceanMask);
    for (int64_t k = 0; k < w.grid.size(); ++k) {
        REQUIRE(ai[k] >= 0);
        if (!ocean[k]) {
            REQUIRE(soil[k] >= 0);
            REQUIRE(soil[k] <= 220);
        }
    }
    const LayerData* rh = w.layer(LayerId::RelHumidity);
    for (int64_t c = 0; c < rh->elements(); ++c) {
        REQUIRE(rh->as<float>()[c] >= 0);
        REQUIRE(rh->as<float>()[c] <= 100.0001f);
    }
    // Aggregates agree with the monthly layers.
    const uint16_t* pm = w.u16(LayerId::PrecMonthly);
    const float* pa = w.f32(LayerId::PrecAnnual);
    for (int64_t k = 0; k < w.grid.size(); k += 97) {
        double s = 0;
        for (int m = 0; m < 12; ++m) s += pm[m * w.grid.size() + k] * 0.1;
        REQUIRE(std::fabs(s - pa[k]) < 0.05);
    }
}

TEST_CASE("Climate parameters rerun stages 4..8 only", "[climate]") {
    Params p = test::smallParams(128);
    WorldData a = test::generate("Clim1", p);
    WorldData b = a;
    b.params.set("climate.axialTilt", 35.0);
    int from = firstInvalidatedStage(a.params, b.params);
    REQUIRE(from == 4);
    ThreadPool pool;
    CancelToken c;
    Pipeline::run(b, from, pool, c);
    for (int s = 1; s <= 3; ++s) REQUIRE(a.stageHash[static_cast<size_t>(s)] == b.stageHash[static_cast<size_t>(s)]);
    REQUIRE(a.stageHash[4] != b.stageHash[4]);
}
