#include "core/world/WorldData.h"

#include <stdexcept>

namespace mapgen {

std::shared_ptr<LayerData> WorldData::makeLayer(LayerId id) const {
    const LayerInfo& info = layerInfo(id);
    const SphereGrid& g = info.grid == GridKind::Climate ? climateGrid : grid;
    if (!g.valid()) throw std::logic_error(std::string("grid not initialised for layer ") + info.key);
    auto l = std::make_shared<LayerData>();
    l->type = info.type;
    l->grid = info.grid;
    l->width = g.width();
    l->height = g.height();
    l->bands = info.bands;
    l->bytes.assign(static_cast<size_t>(l->elements()) * dtypeSize(info.type), 0);
    return l;
}

void WorldData::setLayer(LayerId id, std::shared_ptr<LayerData> data) {
    const LayerInfo& info = layerInfo(id);
    if (data && (data->type != info.type || data->bands != info.bands))
        throw std::logic_error(std::string("layer type mismatch: ") + info.key);
    layers[static_cast<size_t>(id)] = std::move(data);
    layerHash[static_cast<size_t>(id)].clear();
}

void WorldData::clearFromStage(int stage) {
    for (auto& info : allLayers())
        if (info.stage >= stage) {
            layers[static_cast<size_t>(info.id)].reset();
            layerHash[static_cast<size_t>(info.id)].clear();
        }
    if (stage <= 1) tectonics.reset();
    if (stage <= 2) geomorph.reset();
    if (stage <= 3) ocean.reset();
    if (stage <= 5) hydro.reset();
    if (stage <= 7) geoActivity.reset();
    for (int s = std::max(0, stage); s < kStageCount; ++s) {
        stageHash[static_cast<size_t>(s)].clear();
        reports[static_cast<size_t>(s)] = QualityReport{};
    }
    if (stage <= 8) statistics = Json();
    completedStage = std::min(completedStage, stage - 1);
}

double WorldData::sample(LayerId id, int64_t cell, int band) const {
    const LayerData* l = layer(id);
    if (!l) return std::numeric_limits<double>::quiet_NaN();
    const LayerInfo& info = layerInfo(id);
    if (l->grid == GridKind::Climate) {
        int i = grid.col(cell), j = grid.row(cell);
        double x = (i + 0.5) * climateGrid.width() / grid.width();
        double y = (j + 0.5) * climateGrid.height() / grid.height();
        const float* f = l->band<float>(band);
        return climateGrid.sampleBilinear(f, x, y) * info.scale;
    }
    return l->value(cell, band) * info.scale;
}

double WorldData::surfaceElevation(int64_t cell) const {
    const float* e = f32(LayerId::Elevation);
    if (!e) return 0;
    double z = e[cell];
    const float* ice = f32(LayerId::IceThickness);
    const uint8_t* it = u8(LayerId::IceType);
    if (ice && it && (it[cell] == 1 || it[cell] == 2)) z += ice[cell];
    return z;
}

size_t WorldData::memoryBytes() const {
    size_t total = 0;
    for (auto& l : layers)
        if (l) total += l->bytes.size();
    return total;
}

}  // namespace mapgen
