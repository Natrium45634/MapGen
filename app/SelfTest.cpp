#include "app/SelfTest.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <chrono>

#include "core/stages/Pipeline.h"
#include "core/world/CellInfo.h"
#include "io/Export.h"
#include "io/GeoWorldFile.h"
#include "mapgen/Version.h"

namespace mapgen {

int runSelfTest(const QString& workDir, std::string& log) {
    auto line = [&](const std::string& s) { log += s + "\n"; };
    line(std::string("MapGen ") + kGeneratorVersion + " selftest (algorithm v" + std::to_string(kAlgorithmVersion) + ")");
    try {
        Params p;
        applyPreset(p, *findPreset("earthlike"));
        p.set("world.resolution", std::string("custom"));
        p.set("world.customWidth", 256);
        CancelToken cancel;
        auto t0 = std::chrono::steady_clock::now();
        ThreadPool pool1(1), poolN(0);
        WorldData a = Pipeline::generate("A7Test42", p, pool1, cancel);
        WorldData b = Pipeline::generate("A7Test42", p, poolN, cancel);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        line("generated 2 worlds 256x128 in " + std::to_string(sec) + " s (threads 1 and " + std::to_string(poolN.threadCount()) + ")");
        for (int s = 0; s < kStageCount; ++s) {
            if (a.stageHash[static_cast<size_t>(s)] != b.stageHash[static_cast<size_t>(s)]) {
                line("FAIL: stage " + std::to_string(s) + " hash differs between thread counts");
                return 1;
            }
            const QualityReport& r = a.reports[static_cast<size_t>(s)];
            for (auto& c : r.checks)
                if (c.p0 && !c.passed) {
                    line("FAIL: P0 check " + c.id + " (" + c.nameRu + ") value " + formatNumber(c.value));
                    return 1;
                }
            line("stage " + std::to_string(s) + " " + r.stageNameRu + ": hash " + a.stageHash[static_cast<size_t>(s)] + ", checks " +
                 std::to_string(r.checks.size()));
        }
        QDir dir(workDir);
        if (!dir.exists()) dir.mkpath(".");
        QString file = dir.filePath(QString::fromUtf8("проверка_selftest.geoworld"));  // Unicode path on purpose
        std::string err;
        if (!saveGeoWorld(a, file, &err)) { line("FAIL: save: " + err); return 1; }
        WorldData c;
        LoadResult lr = loadGeoWorld(file, c);
        if (!lr.ok) { line("FAIL: load: " + lr.errorRu); return 1; }
        for (int s = 0; s < kStageCount; ++s)
            if (a.stageHash[static_cast<size_t>(s)] != c.stageHash[static_cast<size_t>(s)]) { line("FAIL: hash after reload"); return 1; }
        int64_t probe = a.grid.cellAt(10.0, 20.0);
        if (describeCellText(a, probe, 6) != describeCellText(c, probe, 6)) { line("FAIL: info card differs after reload"); return 1; }
        line("save/load: OK (" + std::to_string(QFile(file).size()) + " bytes)");
        ViewOptions view;
        if (!exportLayerPng(a, MapTab::Physical, view, true, dir.filePath("selftest_physical.png"), &err)) { line("FAIL: png: " + err); return 1; }
        if (!exportHeightmap16(a, dir.filePath("selftest_height16.png"), &err)) { line("FAIL: heightmap: " + err); return 1; }
        if (!exportGeoTiff(a, LayerId::Elevation, 0, dir.filePath("selftest_elevation.tif"), &err)) { line("FAIL: geotiff: " + err); return 1; }
        if (!exportGeoJson(a, dir.filePath("selftest.geojson"), "all", &err)) { line("FAIL: geojson: " + err); return 1; }
        line("exports: OK");
        line("world hash " + Pipeline::worldHash(a));
        line("SELFTEST: OK");
        return 0;
    } catch (const std::exception& ex) {
        line(std::string("FAIL: exception: ") + ex.what());
        return 1;
    }
}

}  // namespace mapgen
