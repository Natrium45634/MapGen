// Generation benchmark: wall time per stage, peak resident memory and layer
// memory at the requested resolutions. Writes a Markdown table to stdout.
//   mapgen_bench [--res preview,standard,high] [--seed S] [--threads N] [--repeat R]

#include <QCoreApplication>
#include <QGuiApplication>
#include <chrono>
#include <cstdio>
#include <sstream>

#include "core/stages/Pipeline.h"
#include "mapgen/Version.h"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

using namespace mapgen;

static double peakRssMb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return pmc.PeakWorkingSetSize / 1048576.0;
    return 0;
#else
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss / 1024.0;
#endif
}

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    std::string resList = "preview,standard", seed = "A7Test42";
    int threads = 0, repeat = 1;
    for (int i = 1; i + 1 < argc; ++i) {
        std::string a = argv[i];
        if (a == "--res") resList = argv[++i];
        else if (a == "--seed") seed = argv[++i];
        else if (a == "--threads") threads = std::atoi(argv[++i]);
        else if (a == "--repeat") repeat = std::atoi(argv[++i]);
    }
    ThreadPool pool(threads);
    std::printf("MapGen %s benchmark, seed %s, %d threads\n\n", kGeneratorVersion, seed.c_str(), pool.threadCount());
    std::printf("| Разрешение | Этап | Время, с |\n|---|---|---|\n");
    std::stringstream ss(resList);
    std::string res;
    while (std::getline(ss, res, ',')) {
        Params p;
        applyPreset(p, *findPreset("earthlike"));
        p.set("world.resolution", res);
        double best = 1e18;
        WorldData w;
        for (int r = 0; r < repeat; ++r) {
            CancelToken c;
            auto t0 = std::chrono::steady_clock::now();
            w = Pipeline::generate(seed, p, pool, c);
            best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        for (auto& st : Pipeline::stages())
            std::printf("| %s | %d. %s | %.2f |\n", res.c_str(), st.index, st.nameRu, w.reports[static_cast<size_t>(st.index)].seconds);
        std::printf("| %s | **всего** | **%.2f** |\n", res.c_str(), best);
        std::printf("| %s | слои в памяти, МБ | %.0f |\n", res.c_str(), w.memoryBytes() / 1048576.0);
        std::printf("| %s | пик RSS процесса, МБ | %.0f |\n", res.c_str(), peakRssMb());
        std::fflush(stdout);
    }
    return 0;
}
