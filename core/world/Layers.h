#pragma once

// Registry of all per-cell layers (structure of arrays). Each layer has a fixed
// element type, grid (full map grid or coarse climate grid), band count
// (1 or 12 months), producing stage, units and a Russian caption.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mapgen {

enum class DType : uint8_t { F32 = 0, U8 = 1, U16 = 2, I16 = 3, I32 = 4 };
enum class GridKind : uint8_t { Full = 0, Climate = 1 };

inline size_t dtypeSize(DType t) {
    switch (t) {
        case DType::F32: return 4;
        case DType::U8: return 1;
        case DType::U16: return 2;
        case DType::I16: return 2;
        case DType::I32: return 4;
    }
    return 1;
}

enum class LayerId : uint16_t {
    // Stage 1: tectonics
    PlateId, Continentality, CrustType, CrustAge, DistCOB, BoundaryType, BoundaryClass, DistBoundary,
    PlateVelE, PlateVelN, TectonicSide, TectonicIntensity, DistSubduction, DistTrench, DistCollision,
    DistRidge, DistTransform, ConvRate, DivRate,
    // Stage 2: geomorphology
    ElevationRaw, Elevation, ErosionDepth, Genesis, Lithology,
    // Stage 3: ocean & coasts
    OceanMask, WaterBody, DistCoast, Landform, CoastExposure, TidalRange,
    // Stage 4: climate (full grid)
    TempMonthly, PrecMonthly, PetMonthly, RunoffMonthly, SoilMonthly, SweMonthly,
    TempMean, TempMin, TempMax, PrecAnnual, PetAnnual, Aridity, WaterBalance, RunoffAnnual, SoilMean,
    SnowMonths, IceType, IceThickness, SeaIceMonths, LgmIce, DryMonths, PrecSeasonality, LapseRate,
    // Stage 4: climate (coarse grid)
    WindU, WindV, RelHumidity, SstAnomaly,
    // Stage 5: hydrology
    HydroSurface, FlowDir, Discharge, DrainageArea, BasinId, RiverOrder, RiverClass, LakeId, Wetland,
    // Stage 6: biomes, coasts
    Koppen, Biome, Biome2, Ecotone, AltBelt, CoastType, Karst,
    // Stage 7: geoactivity
    VolcanicZone, Geothermal, Seismic,
    // Stage 8: QA
    Provenance,
    Count
};

constexpr int kLayerCount = static_cast<int>(LayerId::Count);

struct LayerInfo {
    LayerId id;
    const char* key;      // stable identifier used in files
    DType type;
    GridKind grid;
    int bands;            // 1 or 12
    int stage;            // producing stage 1..8
    double scale;         // physical value = stored * scale (integers); 1 for floats/categories
    const char* units;    // physical units after scaling ("" for categories)
    bool categorical;
    const char* nameRu;
};

const LayerInfo& layerInfo(LayerId id);
const std::vector<LayerInfo>& allLayers();
bool layerFromKey(const std::string& key, LayerId& out);

// Type-erased layer storage.
struct LayerData {
    DType type = DType::F32;
    GridKind grid = GridKind::Full;
    int width = 0, height = 0, bands = 1;
    std::vector<uint8_t> bytes;

    int64_t cells() const { return static_cast<int64_t>(width) * height; }
    int64_t elements() const { return cells() * bands; }

    template <class T> T* as() { return reinterpret_cast<T*>(bytes.data()); }
    template <class T> const T* as() const { return reinterpret_cast<const T*>(bytes.data()); }
    template <class T> T* band(int b) { return as<T>() + static_cast<int64_t>(b) * cells(); }
    template <class T> const T* band(int b) const { return as<T>() + static_cast<int64_t>(b) * cells(); }

    // Physical value of element (any type) as double.
    double value(int64_t cell, int b = 0) const;
};

// Values that mean "not computed / not applicable" in float layers.
float notComputed();
bool isNotComputed(float v);

// Provenance bit flags (Provenance layer).
enum ProvenanceFlag : uint16_t {
    kProvLakeLevelApprox = 1u << 0,      // terminal lake level from aggregated nested depressions
    kProvClimateInterpolated = 1u << 1,  // climate interpolated from the coarse climate grid
    kProvIceIterationNotConverged = 1u << 2,
    kProvHydroFlatResolved = 1u << 3,    // flow direction on a flat resolved by flood order
    kProvAnomaly = 1u << 15,             // flagged by QA checks (see QualityReport)
};

}  // namespace mapgen
