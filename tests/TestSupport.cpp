#include "tests/TestSupport.h"

#include <QGuiApplication>
#ifdef MAPGEN_UI_TESTS
#include <QApplication>
#endif
#include <catch2/catch_session.hpp>
#include <map>
#include <mutex>

namespace mapgen::test {

ThreadPool& pool() {
    static ThreadPool p(0);
    return p;
}

Params smallParams(int width, const std::string& preset) {
    Params p;
    applyPreset(p, *findPreset(preset));
    p.set("world.resolution", std::string("custom"));
    p.set("world.customWidth", width);
    return p;
}

WorldData generate(const std::string& seed, const Params& p, int threads) {
    CancelToken c;
    if (threads > 0) {
        ThreadPool tp(threads);
        return Pipeline::generate(seed, p, tp, c);
    }
    return Pipeline::generate(seed, p, pool(), c);
}

const WorldData& world(const std::string& seed, int width, const std::string& preset) {
    static std::map<std::string, std::unique_ptr<WorldData>> cache;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    std::string key = seed + "|" + std::to_string(width) + "|" + preset;
    auto it = cache.find(key);
    if (it != cache.end()) return *it->second;
    auto w = std::make_unique<WorldData>(generate(seed, smallParams(width, preset)));
    auto& ref = *w;
    cache[key] = std::move(w);
    return ref;
}

}  // namespace mapgen::test

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef MAPGEN_UI_TESTS
    QApplication app(argc, argv);
#else
    QGuiApplication app(argc, argv);
#endif
    return Catch::Session().run(argc, argv);
}
