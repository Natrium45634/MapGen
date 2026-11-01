#pragma once

// Stage contract. Each stage reads the outputs of earlier stages from
// WorldData, writes only its own layers/features, fills its QualityReport and
// polls the cancel token regularly.

#include <functional>
#include <string>
#include <vector>

#include "core/rng/Random.h"
#include "core/util/Parallel.h"
#include "core/world/WorldData.h"

namespace mapgen {

// stage index, fraction of that stage done [0,1], message
using ProgressFn = std::function<void(int, double, const std::string&)>;

struct StageContext {
    WorldData& world;
    const Params& params;
    const SeedKey& seed;
    ThreadPool& pool;
    const CancelToken& cancel;
    QualityReport& report;
    int stage;
    ProgressFn progressFn;

    void progress(double fraction, const std::string& msg = {}) const {
        cancel.check();
        if (progressFn) progressFn(stage, fraction, msg);
    }
    void check() const { cancel.check(); }
};

using StageRunFn = void (*)(StageContext&);

struct StageInfo {
    int index;
    const char* id;
    const char* nameRu;
    StageRunFn run;
};

// Stage entry points (implemented in core/stages/*.cpp).
void runConfiguration(StageContext& ctx);
void runTectonics(StageContext& ctx);
void runGeomorphology(StageContext& ctx);
void runOcean(StageContext& ctx);
void runClimate(StageContext& ctx);
void runHydrology(StageContext& ctx);
void runBiomes(StageContext& ctx);
void runGeoActivity(StageContext& ctx);
void runQualityAssurance(StageContext& ctx);

}  // namespace mapgen
