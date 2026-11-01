// mapgen-cli: console front end for scripted generation, hashing, batch
// statistics, export and the self-test. Same engine as the GUI.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <chrono>
#include <cstdio>
#include <iostream>

#include "app/SelfTest.h"
#include "core/stages/Pipeline.h"
#include "core/world/CellInfo.h"
#include "io/Export.h"
#include "io/GeoWorldFile.h"
#include "mapgen/Version.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace mapgen;

namespace {

void usage() {
    std::puts(
        "mapgen-cli — генератор физико-географических карт (консоль)\n"
        "\n"
        "  mapgen-cli generate --seed S [--preset ID] [--res preview|standard|high|ultra | --width N]\n"
        "                      [--set id=value ...] [--threads N] [--out world.geoworld] [--export DIR]\n"
        "  mapgen-cli hash     --seed S [те же параметры]        печать хешей стадий\n"
        "  mapgen-cli info     world.geoworld                     манифест, проверка контрольных сумм\n"
        "  mapgen-cli probe    world.geoworld --lat X --lon Y [--month M]   значения информера\n"
        "  mapgen-cli export   world.geoworld --dir DIR           PNG всех вкладок, 16-бит, GeoTIFF, GeoJSON, CSV\n"
        "  mapgen-cli sealevel world.geoworld --level M --out new.geoworld   частичная регенерация (этапы 3–8)\n"
        "  mapgen-cli batch    --count N [--width 256] [--preset ID] [--out stats.csv]\n"
        "  mapgen-cli selftest [--dir DIR]\n"
        "  mapgen-cli presets | params\n");
}

struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::vector<std::string>> opt;
    bool has(const std::string& k) const { return opt.count(k) > 0; }
    std::string get(const std::string& k, const std::string& def = {}) const {
        auto it = opt.find(k);
        return it == opt.end() || it->second.empty() ? def : it->second.back();
    }
};

Args parse(const QStringList& list) {
    Args a;
    for (int i = 1; i < list.size(); ++i) {
        std::string s = list[i].toStdString();
        if (s.rfind("--", 0) == 0) {
            std::string k = s.substr(2);
            std::string v;
            if (i + 1 < list.size() && !list[i + 1].startsWith("--")) v = list[++i].toStdString();
            a.opt[k].push_back(v);
        } else {
            a.pos.push_back(s);
        }
    }
    return a;
}

bool buildParams(const Args& a, Params& p, std::string& err) {
    std::string preset = a.get("preset", "earthlike");
    const Preset* pr = findPreset(preset);
    if (!pr) { err = "неизвестный профиль: " + preset; return false; }
    applyPreset(p, *pr);
    if (a.has("res")) p.set("world.resolution", a.get("res"));
    if (a.has("width")) {
        p.set("world.resolution", std::string("custom"));
        p.set("world.customWidth", std::stod(a.get("width")));
    }
    if (a.opt.count("set"))
        for (auto& kv : a.opt.at("set")) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) { err = "ожидалось id=value: " + kv; return false; }
            std::string id = kv.substr(0, eq), v = kv.substr(eq + 1);
            const ParamDef* d = findParam(id);
            if (!d) { err = "неизвестный параметр: " + id; return false; }
            if (d->type == ParamType::Enum || d->type == ParamType::String) p.set(id, v);
            else p.set(id, std::stod(v));
        }
    return true;
}

ProgressFn consoleProgress() {
    return [last = -1](int stage, double f, const std::string& msg) mutable {
        int pct = static_cast<int>(f * 100);
        if (stage * 1000 + pct / 10 == last) return;
        last = stage * 1000 + pct / 10;
        std::fprintf(stderr, "\r[%d/8] %-40s %3d%%", stage, msg.c_str(), pct);
        if (f >= 1.0) std::fprintf(stderr, "\n");
    };
}

void printHashes(const WorldData& w) {
    for (auto& st : Pipeline::stages())
        std::printf("stage %d %-14s %s  %.2f s\n", st.index, st.id, w.stageHash[static_cast<size_t>(st.index)].c_str(),
                    w.reports[static_cast<size_t>(st.index)].seconds);
    std::printf("world %s\n", Pipeline::worldHash(w).c_str());
}

int cmdGenerate(const Args& a, bool hashOnly) {
    Params p;
    std::string err;
    if (!buildParams(a, p, err)) { std::fprintf(stderr, "Ошибка: %s\n", err.c_str()); return 2; }
    std::string seed = a.get("seed", "A7Test42");
    if (!isValidSeed(seed, &err)) { std::fprintf(stderr, "Ошибка сида: %s\n", err.c_str()); return 2; }
    ThreadPool pool(a.has("threads") ? std::stoi(a.get("threads")) : 0);
    CancelToken cancel;
    auto t0 = std::chrono::steady_clock::now();
    WorldData w = Pipeline::generate(seed, p, pool, cancel, hashOnly ? ProgressFn{} : consoleProgress());
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printHashes(w);
    std::printf("time %.2f s, threads %d, layers %.1f MB\n", sec, pool.threadCount(), w.memoryBytes() / 1048576.0);
    for (auto& r : w.reports)
        for (auto& c : r.checks)
            if (!c.passed) std::printf("  check FAILED%s: [%d] %s — %s (value %s)\n", c.p0 ? " (P0)" : "", r.stage, c.id.c_str(),
                                       c.nameRu.c_str(), formatNumber(c.value).c_str());
    if (!hashOnly && a.has("out")) {
        if (!saveGeoWorld(w, QString::fromStdString(a.get("out")), &err)) { std::fprintf(stderr, "Ошибка сохранения: %s\n", err.c_str()); return 1; }
        std::printf("saved %s\n", a.get("out").c_str());
    }
    if (!hashOnly && a.has("export")) {
        if (!exportAll(w, QString::fromStdString(a.get("export")), ViewOptions{}, &err)) { std::fprintf(stderr, "Ошибка экспорта: %s\n", err.c_str()); return 1; }
        std::printf("exported to %s\n", a.get("export").c_str());
    }
    for (auto& r : w.reports) if (!r.passedP0()) return 3;
    return 0;
}

bool loadOrFail(const std::string& path, WorldData& w) {
    LoadResult r = loadGeoWorld(QString::fromStdString(path), w);
    if (!r.ok) { std::fprintf(stderr, "Ошибка: %s\n", r.errorRu.c_str()); return false; }
    for (auto& m : r.warningsRu) std::fprintf(stderr, "Предупреждение: %s\n", m.c_str());
    return true;
}

int cmdInfo(const Args& a) {
    if (a.pos.size() < 2) { usage(); return 2; }
    WorldData w;
    if (!loadOrFail(a.pos[1], w)) return 1;
    std::printf("seed %s, generator %s, algorithm v%d, grid %dx%d, sea level %.1f m\n", w.seed.c_str(), w.generatorVersion.c_str(),
                w.algorithmVersion, w.grid.width(), w.grid.height(), w.seaLevel());
    std::printf("layer hashes verified: yes\n");
    printHashes(w);
    return 0;
}

int cmdProbe(const Args& a) {
    if (a.pos.size() < 2) { usage(); return 2; }
    WorldData w;
    if (!loadOrFail(a.pos[1], w)) return 1;
    double lat = std::stod(a.get("lat", "0")), lon = std::stod(a.get("lon", "0"));
    int month = std::stoi(a.get("month", "-1"));
    std::fputs(describeCellText(w, w.grid.cellAt(lat, lon), month).c_str(), stdout);
    return 0;
}

int cmdExport(const Args& a) {
    if (a.pos.size() < 2) { usage(); return 2; }
    WorldData w;
    if (!loadOrFail(a.pos[1], w)) return 1;
    std::string err;
    if (!exportAll(w, QString::fromStdString(a.get("dir", "export")), ViewOptions{}, &err)) { std::fprintf(stderr, "Ошибка: %s\n", err.c_str()); return 1; }
    std::printf("exported\n");
    return 0;
}

int cmdSeaLevel(const Args& a) {
    if (a.pos.size() < 2) { usage(); return 2; }
    WorldData w;
    if (!loadOrFail(a.pos[1], w)) return 1;
    WorldData n = w;
    n.params.set("ocean.seaLevelMode", std::string("fixed"));
    n.params.set("ocean.seaLevel", std::stod(a.get("level", "0")));
    int from = firstInvalidatedStage(w.params, n.params);
    ThreadPool pool;
    CancelToken cancel;
    Pipeline::run(n, from, pool, cancel, consoleProgress());
    std::printf("rerun from stage %d\n", from);
    for (int s = 0; s < kStageCount; ++s)
        std::printf("stage %d %s %s\n", s, w.stageHash[static_cast<size_t>(s)] == n.stageHash[static_cast<size_t>(s)] ? "same   " : "changed",
                    n.stageHash[static_cast<size_t>(s)].c_str());
    if (a.has("out")) {
        std::string err;
        if (!saveGeoWorld(n, QString::fromStdString(a.get("out")), &err)) { std::fprintf(stderr, "Ошибка: %s\n", err.c_str()); return 1; }
    }
    return 0;
}

int cmdBatch(const Args& a) {
    int count = std::stoi(a.get("count", "100"));
    Params p;
    std::string err;
    Args b = a;
    if (!b.has("width") && !b.has("res")) b.opt["width"] = {"256"};
    if (!buildParams(b, p, err)) { std::fprintf(stderr, "Ошибка: %s\n", err.c_str()); return 2; }
    ThreadPool pool(a.has("threads") ? std::stoi(a.get("threads")) : 0);
    CancelToken cancel;
    std::string csv;
    bool header = false;
    int failures = 0;
    for (int i = 0; i < count; ++i) {
        std::string seed = "Batch" + std::to_string(i);
        WorldData w = Pipeline::generate(seed, p, pool, cancel);
        if (!header) {
            csv += "seed";
            for (auto& r : w.reports) for (auto& m : r.metrics) csv += "," + std::to_string(r.stage) + ":" + m.first;
            csv += ",p0\n";
            header = true;
        }
        csv += seed;
        bool p0 = true;
        for (auto& r : w.reports) {
            for (auto& m : r.metrics) csv += "," + formatNumber(m.second);
            p0 = p0 && r.passedP0();
        }
        csv += p0 ? ",1\n" : ",0\n";
        if (!p0) ++failures;
        std::fprintf(stderr, "\r%d/%d", i + 1, count);
    }
    std::fprintf(stderr, "\n");
    if (a.has("out")) {
        QFile f(QString::fromStdString(a.get("out")));
        if (f.open(QIODevice::WriteOnly)) f.write(csv.data(), static_cast<qint64>(csv.size()));
    } else {
        std::fputs(csv.c_str(), stdout);
    }
    std::printf("worlds %d, P0 failures %d\n", count, failures);
    return failures ? 3 : 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    // Offscreen platform: text rendering for PNG legends works without a display.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    Args a = parse(app.arguments());
    if (a.pos.empty() || a.has("help")) { usage(); return a.pos.empty() ? 2 : 0; }
    const std::string& cmd = a.pos[0];
    try {
        if (cmd == "generate") return cmdGenerate(a, false);
        if (cmd == "hash") return cmdGenerate(a, true);
        if (cmd == "info") return cmdInfo(a);
        if (cmd == "probe") return cmdProbe(a);
        if (cmd == "export") return cmdExport(a);
        if (cmd == "sealevel") return cmdSeaLevel(a);
        if (cmd == "batch") return cmdBatch(a);
        if (cmd == "selftest") {
            std::string log;
            int rc = runSelfTest(QString::fromStdString(a.get("dir", QDir::tempPath().toStdString())), log);
            std::fputs(log.c_str(), stdout);
            return rc;
        }
        if (cmd == "presets") {
            for (auto& p : presets()) std::printf("%-15s %s — %s\n", p.id.c_str(), p.nameRu.c_str(), p.descriptionRu.c_str());
            return 0;
        }
        if (cmd == "params") {
            for (auto& d : paramSchema())
                std::printf("%-28s stage %d  %s [%s]\n", d.id.c_str(), d.stageInvalidated, d.nameRu.c_str(), d.units.c_str());
            return 0;
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "Ошибка: %s\n", ex.what());
        return 1;
    }
    usage();
    return 2;
}
