#include "core/stages/Pipeline.h"

#include <chrono>
#include <cstring>

#include "core/rng/Sha256.h"

namespace mapgen {

namespace {

class ByteSink {
public:
    void f(double v) { put(&v, sizeof v); }
    void i(int64_t v) { put(&v, sizeof v); }
    void s(const std::string& v) { i(static_cast<int64_t>(v.size())); put(v.data(), v.size()); }
    void line(const Polyline& p) {
        i(static_cast<int64_t>(p.pts.size()));
        for (auto& g : p.pts) { f(g.lat); f(g.lon); }
    }
    const std::vector<uint8_t>& bytes() const { return b_; }

private:
    void put(const void* p, size_t n) {
        const uint8_t* c = static_cast<const uint8_t*>(p);
        b_.insert(b_.end(), c, c + n);
    }
    std::vector<uint8_t> b_;
};

std::string featureHash(const WorldData& w, int stage) {
    ByteSink b;
    switch (stage) {
        case 1:
            if (w.tectonics) {
                for (auto& p : w.tectonics->plates) {
                    b.i(p.id); b.f(p.poleLat); b.f(p.poleLon); b.f(p.omegaDegPerMyr); b.f(p.areaKm2);
                    b.f(p.continentalFraction); b.f(p.rigidity);
                }
                for (auto& s : w.tectonics->boundaries) { b.line(s.line); b.i(s.type); b.i(s.cls); b.f(s.convergenceCmYr); }
                b.f(w.tectonics->crustThreshold);
            }
            break;
        case 2:
            if (w.geomorph) {
                for (auto& h : w.geomorph->hotspots) { b.f(h.lat); b.f(h.lon); b.f(h.strength); b.i(h.plateId); }
                for (auto& v : w.geomorph->volcanoes) {
                    b.f(v.lat); b.f(v.lon); b.i(v.genesis); b.i(v.morphology); b.f(v.edificeHeightM); b.f(v.ageMyr);
                }
                b.f(w.geomorph->calibrationShiftM);
            }
            break;
        case 3:
            if (w.ocean) {
                b.f(w.ocean->seaLevelM); b.f(w.ocean->landFraction); b.i(w.ocean->oceanComponents);
                for (auto& c : w.ocean->coastlines) b.line(c);
            }
            break;
        case 5:
            if (w.hydro) {
                for (auto& r : w.hydro->rivers) {
                    b.line(r.line); b.i(r.order); b.f(r.dischargeEnd); b.i(r.basinId); b.i(r.downstream);
                    for (auto c : r.cells) b.i(c);
                }
                for (auto& l : w.hydro->lakes) { b.f(l.areaKm2); b.f(l.levelM); b.i(l.regime); b.i(l.genesis); }
                for (auto& s : w.hydro->basins) { b.i(s.outletCell); b.f(s.areaKm2); b.f(s.dischargeM3s); }
                for (auto& f : w.hydro->waterfalls) { b.f(f.lat); b.f(f.lon); b.f(f.dropM); }
                for (auto& d : w.hydro->deltas) { b.i(d.mouthCell); b.i(d.branches); }
            }
            break;
        case 7:
            if (w.geoActivity) {
                for (auto& v : w.geoActivity->volcanoStates) { b.i(v.volcanoId); b.i(v.activity); b.i(v.submarine); b.i(v.zone); }
                for (auto& s : w.geoActivity->springs) { b.f(s.lat); b.f(s.lon); b.i(s.geyser); }
            }
            break;
        default: break;
    }
    return hex64(hashBytes64(b.bytes().data(), b.bytes().size()));
}

}  // namespace

const std::vector<StageInfo>& Pipeline::stages() {
    static const std::vector<StageInfo> s = {
        {0, "config", "Конфигурация", runConfiguration},
        {1, "tectonics", "Тектоника", runTectonics},
        {2, "geomorphology", "Геоморфология", runGeomorphology},
        {3, "ocean", "Океан и берега", runOcean},
        {4, "climate", "Климат", runClimate},
        {5, "hydrology", "Гидрология", runHydrology},
        {6, "biomes", "Биомы и побережья", runBiomes},
        {7, "geoactivity", "Геоактивность", runGeoActivity},
        {8, "qa", "Рендер и контроль качества", runQualityAssurance},
    };
    return s;
}

std::string Pipeline::hashLayer(const LayerData& l) {
    uint64_t h = hashBytes64(l.bytes.data(), l.bytes.size(),
                             0x6D617067656Eull ^ (static_cast<uint64_t>(l.type) << 8) ^ static_cast<uint64_t>(l.bands));
    return hex64(h);
}

void Pipeline::computeStageHash(WorldData& w, int stage) {
    Sha256 h;
    h.update("stage:");
    h.updateU64(static_cast<uint64_t>(stage));
    for (auto& info : allLayers()) {
        if (info.stage != stage) continue;
        auto& lh = w.layerHash[static_cast<size_t>(info.id)];
        if (w.layers[static_cast<size_t>(info.id)] && lh.empty()) lh = hashLayer(*w.layers[static_cast<size_t>(info.id)]);
        h.update(info.key);
        h.update(lh.empty() ? std::string("-") : lh);
    }
    h.update(featureHash(w, stage));
    w.stageHash[static_cast<size_t>(stage)] = Sha256::hex(h.finish()).substr(0, 32);
}

std::string Pipeline::worldHash(const WorldData& w) {
    Sha256 h;
    for (auto& s : w.stageHash) h.update(s);
    return Sha256::hex(h.finish()).substr(0, 32);
}

void Pipeline::run(WorldData& world, int fromStage, ThreadPool& pool, const CancelToken& cancel, const ProgressFn& progress) {
    SeedKey key(world.seed, world.algorithmVersion > 0 ? world.algorithmVersion : 1);
    fromStage = std::clamp(fromStage, 1, kStageCount);
    world.clearFromStage(fromStage);
    world.storedResultOnly = false;
    world.noticeRu.clear();
    for (auto& st : stages()) {
        if (st.index != 0 && st.index < fromStage) continue;
        QualityReport report;
        report.stage = st.index;
        report.stageNameRu = st.nameRu;
        StageContext ctx{world, world.params, key, pool, cancel, report, st.index, progress};
        auto t0 = std::chrono::steady_clock::now();
        ctx.progress(0.0, st.nameRu);
        st.run(ctx);
        if (st.index == 0) key = SeedKey(world.seed, world.algorithmVersion);
        report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        world.reports[static_cast<size_t>(st.index)] = std::move(report);
        computeStageHash(world, st.index);
        world.completedStage = st.index;
        ctx.progress(1.0, st.nameRu);
    }
}

WorldData Pipeline::generate(const std::string& seed, const Params& params, ThreadPool& pool, const CancelToken& cancel,
                             const ProgressFn& progress) {
    WorldData w;
    w.seed = seed;
    w.params = params;
    run(w, 1, pool, cancel, progress);
    return w;
}

}  // namespace mapgen
