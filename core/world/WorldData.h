#pragma once

// WorldData: the single source of truth shared by all tabs, the info card and
// exports. Layers are immutable once a stage has produced them and are held by
// shared_ptr, so a partially regenerated world shares unchanged layers with its
// predecessor instead of copying full float arrays.

#include <array>
#include <memory>
#include <string>

#include "core/grids/SphereGrid.h"
#include "core/rng/Random.h"
#include "core/validation/QualityReport.h"
#include "core/world/Features.h"
#include "core/world/Layers.h"
#include "core/world/Params.h"

namespace mapgen {

inline constexpr int kStageCount = 9;  // 0 configuration ... 8 render & QA

class WorldData {
public:
    std::string seed;
    Params params;
    SphereGrid grid;          // full map grid
    SphereGrid climateGrid;   // coarse grid of the circulation / moisture model
    std::string generatorVersion;
    int algorithmVersion = 0;
    int schemaVersion = 0;

    std::array<std::shared_ptr<const LayerData>, kLayerCount> layers{};
    std::array<std::string, kLayerCount> layerHash{};

    std::shared_ptr<const TectonicsFeatures> tectonics;
    std::shared_ptr<const GeomorphFeatures> geomorph;
    std::shared_ptr<const OceanFeatures> ocean;
    std::shared_ptr<const HydroFeatures> hydro;
    std::shared_ptr<const GeoActivityFeatures> geoActivity;

    std::array<std::string, kStageCount> stageHash{};
    std::array<QualityReport, kStageCount> reports{};
    Json statistics;          // stage 8 summary
    int completedStage = -1;  // highest stage that finished
    bool storedResultOnly = false;  // loaded from a file of another algorithm version
    std::string noticeRu;           // user-facing notice about the provenance of the data

    bool has(LayerId id) const { return layers[static_cast<size_t>(id)] != nullptr; }
    const LayerData* layer(LayerId id) const { return layers[static_cast<size_t>(id)].get(); }
    const float* f32(LayerId id) const { auto* l = layer(id); return l ? l->as<float>() : nullptr; }
    const uint8_t* u8(LayerId id) const { auto* l = layer(id); return l ? l->as<uint8_t>() : nullptr; }
    const uint16_t* u16(LayerId id) const { auto* l = layer(id); return l ? l->as<uint16_t>() : nullptr; }
    const int16_t* i16(LayerId id) const { auto* l = layer(id); return l ? l->as<int16_t>() : nullptr; }
    const int32_t* i32(LayerId id) const { auto* l = layer(id); return l ? l->as<int32_t>() : nullptr; }

    // Allocates a new zero-initialised layer with the registry's type/grid/bands.
    std::shared_ptr<LayerData> makeLayer(LayerId id) const;
    void setLayer(LayerId id, std::shared_ptr<LayerData> data);

    // Drops all layers and features produced by stages >= stage.
    void clearFromStage(int stage);

    // Physical value of a layer at a full-grid cell (climate-grid layers are bilinearly sampled).
    double sample(LayerId id, int64_t cell, int band = 0) const;

    // Surface elevation (bedrock + land ice) at a cell in metres.
    double surfaceElevation(int64_t cell) const;
    double seaLevel() const { return ocean ? ocean->seaLevelM : 0.0; }
    bool isOcean(int64_t cell) const { auto* m = u8(LayerId::OceanMask); return m && m[cell] != 0; }

    size_t memoryBytes() const;
};

}  // namespace mapgen
