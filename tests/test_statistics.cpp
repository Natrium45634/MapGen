// Statistical tests over 100+ seeds (wide documented ranges, not a copy of the Earth) and golden maps.
// Reference values (Earth): land 29 %; Köppen land shares (Beck et al. 2018) A ≈ 19 %, B ≈ 30 %, C ≈ 14 %,
// D ≈ 22 %, E ≈ 15 %; hyper-arid land ≈ 7.5 % (CGIAR AI v3); endorheic land ≈ 18–20 % (HydroSHEDS);
// glacierised land ≈ 10 %; mean land elevation ≈ 840 m (ETOPO). See docs/TESTING.md.

#include <QDir>
#include <QImage>
#include <catch2/catch_test_macros.hpp>
#include <numeric>

#include "io/Export.h"
#include "tests/TestSupport.h"

using namespace mapgen;

namespace {
double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}
double mean(const std::vector<double>& v) { return v.empty() ? 0 : std::accumulate(v.begin(), v.end(), 0.0) / v.size(); }
}  // namespace

TEST_CASE("Statistics over 120 Earth-like worlds fall into documented ranges", "[stat]") {
    Params p = test::smallParams(128);
    std::vector<double> land, cont, A, B, C, D, E, hyper, endo, glacier, meanZ, temp, volcDensity, dryEndoRatio;
    int p0Failures = 0;
    for (int s = 0; s < 120; ++s) {
        WorldData w = test::generate("Stat" + std::to_string(s), p);
        for (auto& r : w.reports) if (!r.passedP0()) ++p0Failures;
        land.push_back(w.statistics["landPercent"].asNumber() / 100.0);
        cont.push_back(w.statistics["continents"].asNumber());
        A.push_back(w.reports[6].metricValue("koppenA"));
        B.push_back(w.reports[6].metricValue("koppenB"));
        C.push_back(w.reports[6].metricValue("koppenC"));
        D.push_back(w.reports[6].metricValue("koppenD"));
        E.push_back(w.reports[6].metricValue("koppenE"));
        hyper.push_back(w.reports[4].metricValue("landHyperarid"));
        endo.push_back(w.reports[5].metricValue("endorheicLandShare"));
        dryEndoRatio.push_back(w.reports[5].metricValue("endorheicShareOfDryLand") - w.reports[5].metricValue("endorheicLandShare"));
        glacier.push_back(w.statistics["glacierPercentOfLand"].asNumber() / 100.0);
        meanZ.push_back(w.statistics["meanLandElevationM"].asNumber());
        temp.push_back(w.reports[4].metricValue("globalMeanTempC"));
        volcDensity.push_back(w.statistics["volcanoesPerMillionKm2"].asNumber());
    }
    INFO("land " << mean(land) << " continents " << median(cont) << " A " << mean(A) << " B " << mean(B) << " C " << mean(C)
                 << " D " << mean(D) << " E " << mean(E) << " hyperarid " << mean(hyper) << " endorheic " << mean(endo)
                 << " glacier " << mean(glacier) << " meanZ " << mean(meanZ) << " T " << mean(temp) << " volc " << mean(volcDensity));
    REQUIRE(p0Failures == 0);
    REQUIRE(mean(land) > 0.25);
    REQUIRE(mean(land) < 0.33);
    REQUIRE(median(cont) >= 2);
    REQUIRE(median(cont) <= 9);
    REQUIRE(mean(A) > 0.05);
    REQUIRE(mean(A) < 0.35);
    REQUIRE(mean(B) > 0.10);
    REQUIRE(mean(B) < 0.45);
    REQUIRE(mean(C) > 0.05);
    REQUIRE(mean(D) > 0.03);
    REQUIRE(mean(E) < 0.35);
    REQUIRE(mean(hyper) < 0.35);
    REQUIRE(mean(endo) > 0.02);
    REQUIRE(mean(endo) < 0.40);
    REQUIRE(mean(dryEndoRatio) > 0.0);  // endorheic basins are more common on dry land
    REQUIRE(mean(glacier) < 0.30);
    REQUIRE(mean(meanZ) > 250);
    REQUIRE(mean(meanZ) < 1500);
    REQUIRE(mean(temp) > 8);
    REQUIRE(mean(temp) < 22);
    REQUIRE(mean(volcDensity) > 0.1);
    REQUIRE(mean(volcDensity) < 6);
}

TEST_CASE("Profiles produce geography, not inverted masks: continents vs archipelagos", "[stat]") {
    std::vector<double> contLargest, archLargest, contIslands, archIslands;
    for (int s = 0; s < 12; ++s) {
        WorldData c = test::generate("Prof" + std::to_string(s), test::smallParams(256, "continents"));
        WorldData a = test::generate("Prof" + std::to_string(s), test::smallParams(256, "archipelago"));
        contLargest.push_back(c.statistics["largestLandmassesKm2"][0].asNumber() / (c.statistics["landPercent"].asNumber() / 100 * c.grid.totalAreaKm2()));
        archLargest.push_back(a.statistics["largestLandmassesKm2"][0].asNumber() / (a.statistics["landPercent"].asNumber() / 100 * a.grid.totalAreaKm2()));
        contIslands.push_back(c.statistics["islandsOver1000Km2"].asNumber());
        archIslands.push_back(a.statistics["islandsOver1000Km2"].asNumber());
    }
    REQUIRE(mean(contLargest) > mean(archLargest) + 0.1);  // big masses dominate land on continental worlds
    REQUIRE(mean(archIslands) > mean(contIslands));         // many islands on archipelago worlds
}

TEST_CASE("Golden maps: visual regression at 128×64 against stored references", "[golden]") {
    QDir data(MAPGEN_TEST_DATA_DIR "/golden");
    const char* seeds[] = {"A7Test42", "Kepler7"};
    const MapTab tabs[] = {MapTab::Physical, MapTab::Biomes, MapTab::Koppen, MapTab::Plates};
    bool regenerate = qEnvironmentVariableIsSet("MAPGEN_UPDATE_GOLDEN");
    for (const char* s : seeds) {
        WorldData w = test::generate(s, test::smallParams(128));
        for (MapTab t : tabs) {
            ViewOptions v;
            QImage img = colorizeTab(w, t, v);
            QString name = QString("%1_%2.png").arg(s).arg(tabInfo(t).id);
            if (regenerate) {
                data.mkpath(".");
                REQUIRE(img.save(data.filePath(name)));
                continue;
            }
            QImage ref(data.filePath(name));
            REQUIRE(!ref.isNull());
            REQUIRE(ref.size() == img.size());
            int64_t diff = 0;
            for (int j = 0; j < img.height(); ++j)
                for (int i = 0; i < img.width(); ++i) {
                    QRgb a = img.pixel(i, j), b = ref.pixel(i, j);
                    int d = std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b));
                    if (d > 30) ++diff;
                }
            INFO(name.toStdString() << " differing pixels " << diff);
            // Bit-exact on the reference platform; small tolerance for other compilers' libm differences.
            REQUIRE(diff <= img.width() * img.height() / 12);
        }
    }
}
