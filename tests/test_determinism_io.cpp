// Seed replay (20 seeds, thread counts), partial regeneration, .geoworld and exports.

#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <catch2/catch_test_macros.hpp>

#include "core/world/CellInfo.h"
#include "io/Export.h"
#include "io/GeoWorldFile.h"
#include "io/Tiff.h"
#include "tests/TestSupport.h"

using namespace mapgen;

TEST_CASE("Seed replay: equal hashes of every layer for 20 fixed seeds and thread counts", "[determinism][slow]") {
    const char* seeds[20] = {"A7Test42", "Kepler7", "Mars3", "Zz9", "Q", "abcDEF123", "Terra", "Gaia2026", "x1y2z3", "Polar1",
                             "Archi9", "Ocean77", "LandMass", "R2D2", "Seed0", "Seed1", "OMEGA", "omega", "Lambda5", "VeryLongSeed1234567890"};
    Params p = test::smallParams(128);
    for (const char* s : seeds) {
        WorldData a = test::generate(s, p, 1);
        WorldData b = test::generate(s, p, 3);
        for (auto& info : allLayers()) {
            if (!a.has(info.id)) continue;
            REQUIRE(b.has(info.id));
            INFO("seed " << s << " layer " << info.key);
            REQUIRE(Pipeline::hashLayer(*a.layer(info.id)) == Pipeline::hashLayer(*b.layer(info.id)));
        }
        for (int st = 0; st < kStageCount; ++st) REQUIRE(a.stageHash[static_cast<size_t>(st)] == b.stageHash[static_cast<size_t>(st)]);
    }
    // Case matters: different seeds give different worlds.
    REQUIRE(Pipeline::worldHash(test::generate("OMEGA", p)) != Pipeline::worldHash(test::generate("omega", p)));
}

TEST_CASE("Partial regeneration reuses earlier stages and matches a full run", "[determinism]") {
    Params p = test::smallParams(128);
    WorldData a = test::generate("Partial1", p);
    Params q = p;
    q.set("climate.humidity", 1.3);
    WorldData b = a;
    b.params = q;
    ThreadPool pool;
    CancelToken c;
    Pipeline::run(b, firstInvalidatedStage(p, q), pool, c);
    WorldData full = test::generate("Partial1", q);
    for (int st = 0; st < kStageCount; ++st) REQUIRE(b.stageHash[static_cast<size_t>(st)] == full.stageHash[static_cast<size_t>(st)]);
}

TEST_CASE("Cancellation leaves no partial world behind", "[determinism]") {
    Params p = test::smallParams(128);
    WorldData w;
    w.seed = "Cancel1";
    w.params = p;
    CancelToken c;
    ThreadPool pool;
    int calls = 0;
    REQUIRE_THROWS_AS(Pipeline::run(w, 1, pool, c, [&](int stage, double, const std::string&) {
        if (stage == 4 && ++calls == 2) c.cancel();
    }), CancelledError);
}

TEST_CASE(".geoworld round trip keeps hashes and info card values", "[io]") {
    const WorldData& w = test::world("A7Test42", 256);
    QTemporaryDir dir;
    QString path = QDir(dir.path()).filePath(QString::fromUtf8("мир Ω проверка.geoworld"));
    std::string err;
    REQUIRE(saveGeoWorld(w, path, &err));
    WorldData r;
    LoadResult lr = loadGeoWorld(path, r);
    REQUIRE(lr.ok);
    REQUIRE(lr.hashesVerified);
    REQUIRE_FALSE(lr.versionMismatch);
    for (int st = 0; st < kStageCount; ++st) REQUIRE(w.stageHash[static_cast<size_t>(st)] == r.stageHash[static_cast<size_t>(st)]);
    REQUIRE(r.seed == w.seed);
    REQUIRE(r.params == w.params);
    for (int64_t k : {int64_t(0), w.grid.size() / 3, w.grid.size() / 2 + 17, w.grid.size() - 1})
        for (int m : {-1, 0, 6}) REQUIRE(describeCellText(w, k, m) == describeCellText(r, k, m));
    REQUIRE(r.hydro->rivers.size() == w.hydro->rivers.size());
    REQUIRE(r.geomorph->volcanoes.size() == w.geomorph->volcanoes.size());
}

TEST_CASE(".geoworld detects corruption and truncation", "[io]") {
    const WorldData& w = test::world("A7Test42", 256);
    QTemporaryDir dir;
    QString path = QDir(dir.path()).filePath("c.geoworld");
    REQUIRE(saveGeoWorld(w, path));
    QFile f(path);
    REQUIRE(f.open(QIODevice::ReadOnly));
    QByteArray bytes = f.readAll();
    f.close();
    // Flip one byte inside a compressed layer blob.
    QByteArray bad = bytes;
    bad[bad.size() / 2] = static_cast<char>(bad[bad.size() / 2] ^ 0x5A);
    QString badPath = QDir(dir.path()).filePath("bad.geoworld");
    QFile b(badPath);
    REQUIRE(b.open(QIODevice::WriteOnly));
    b.write(bad);
    b.close();
    WorldData r;
    REQUIRE_FALSE(loadGeoWorld(badPath, r).ok);
    // Truncated file (interrupted write without the atomic rename).
    QString truncPath = QDir(dir.path()).filePath("trunc.geoworld");
    QFile t(truncPath);
    REQUIRE(t.open(QIODevice::WriteOnly));
    t.write(bytes.left(bytes.size() / 3));
    t.close();
    LoadResult lr = loadGeoWorld(truncPath, r);
    REQUIRE_FALSE(lr.ok);
    REQUIRE_FALSE(lr.errorRu.empty());
}

TEST_CASE("Exports: GeoTIFF float32, 16-bit heightmap with mapping, GeoJSON, CSV", "[io]") {
    const WorldData& w = test::world("A7Test42", 256);
    QTemporaryDir dir;
    std::string err;
    QString tif = QDir(dir.path()).filePath("e.tif");
    REQUIRE(exportGeoTiff(w, LayerId::Elevation, 0, tif, &err));
    TiffRaster r;
    REQUIRE(readGeoTiffFloat(tif, r, &err));
    REQUIRE(r.width == w.grid.width());
    REQUIRE(r.height == w.grid.height());
    REQUIRE(r.sphereRadiusM == w.grid.radiusKm() * 1000.0);
    for (int64_t k = 0; k < w.grid.size(); k += 31)
        REQUIRE(std::fabs(r.data[static_cast<size_t>(k)] - (w.surfaceElevation(k) - w.seaLevel())) < 1e-2);
    QString h16 = QDir(dir.path()).filePath("h.png");
    REQUIRE(exportHeightmap16(w, h16, &err));
    QImage img(h16);
    REQUIRE(img.format() == QImage::Format_Grayscale16);
    QFile side(QDir(dir.path()).filePath("h.json"));
    REQUIRE(side.open(QIODevice::ReadOnly));
    Json sj = Json::parse(side.readAll().toStdString());
    double mn = sj["minM"].asNumber(), mx = sj["maxM"].asNumber();
    for (int j = 0; j < img.height(); j += 13)
        for (int i = 0; i < img.width(); i += 17) {
            double v = reinterpret_cast<const uint16_t*>(img.constScanLine(j))[i];
            double h = mn + v / 65535.0 * (mx - mn);
            REQUIRE(std::fabs(h - (w.surfaceElevation(w.grid.index(i, j)) - w.seaLevel())) <= (mx - mn) / 65535.0 + 0.01);
        }
    QString gj = QDir(dir.path()).filePath("f.geojson");
    REQUIRE(exportGeoJson(w, gj, "all", &err));
    QFile gf(gj);
    REQUIRE(gf.open(QIODevice::ReadOnly));
    Json g = Json::parse(gf.readAll().toStdString());
    REQUIRE(g["type"].asString() == "FeatureCollection");
    REQUIRE(g["features"].size() > 10);
    REQUIRE(g["mapgen"]["datum"]["type"].asString() == "sphere");
    for (auto& f : g["features"].items()) {
        const Json& geom = f["geometry"];
        auto check = [](const Json& c) {
            REQUIRE(c[0].asNumber() >= -180.0);
            REQUIRE(c[0].asNumber() <= 180.0);
            REQUIRE(c[1].asNumber() >= -90.0);
            REQUIRE(c[1].asNumber() <= 90.0);
        };
        if (geom["type"].asString() == "Point") check(geom["coordinates"]);
        else for (auto& c : geom["coordinates"].items()) check(c);
    }
    for (const char* t : {"statistics", "volcanoes", "lakes", "basins", "rivers"})
        REQUIRE(exportCsv(w, QDir(dir.path()).filePath(QString("%1.csv").arg(t)), t, &err));
    REQUIRE(exportLayerPng(w, MapTab::Biomes, ViewOptions{}, true, QDir(dir.path()).filePath("b.png"), &err));
}

TEST_CASE("Opening a file from another algorithm version is flagged as a stored result", "[io]") {
    WorldData w = test::world("A7Test42", 128);
    w.algorithmVersion = 999;
    QTemporaryDir dir;
    QString path = QDir(dir.path()).filePath("old.geoworld");
    REQUIRE(saveGeoWorld(w, path));
    WorldData r;
    LoadResult lr = loadGeoWorld(path, r);
    REQUIRE(lr.ok);
    REQUIRE(lr.versionMismatch);
    REQUIRE(r.storedResultOnly);
    REQUIRE_FALSE(r.noticeRu.empty());
}
