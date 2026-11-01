#pragma once

// Turns WorldData layers into display images for each map tab, and describes
// the matching legend. All tabs read the same WorldData; switching a tab never
// regenerates the world.

#include <QImage>
#include <string>
#include <vector>

#include "core/util/Parallel.h"
#include "core/world/WorldData.h"
#include "render/Palette.h"

namespace mapgen {

enum class MapTab : int {
    Physical = 0, Elevation, Plates, Landforms, Temperature, Precipitation, Humidity, Wind, Rivers, Koppen, Biomes,
    Volcanism, SnowIce, Coasts, Count
};
constexpr int kTabCount = static_cast<int>(MapTab::Count);

struct TabInfo {
    MapTab tab;
    const char* id;
    const char* nameRu;
    bool monthly;           // month selector applies
    LayerId primaryLayer;   // highlighted in the info card
    int firstStage;         // stage that must have run for the tab to have data
};
const std::vector<TabInfo>& tabs();
const TabInfo& tabInfo(MapTab t);

enum class HumidityMetric : int { Aridity = 0, RelHumidity = 1, SoilMoisture = 2 };

struct ViewOptions {
    int month = -1;                 // -1 = annual / all months
    HumidityMetric humidity = HumidityMetric::Aridity;
    int landformView = 0;           // 0 landform, 1 structural genesis, 2 lithology
    int biomeView = 0;              // 0 biome, 1 altitudinal belt
    int platesView = 0;             // 0 plates + crust, 1 boundary class, 2 crust age
    int volcanoView = 0;            // 0 activity zone, 1 geothermal, 2 seismotectonic
    int riverView = 0;              // 0 basins, 1 discharge
    bool colorblind = false;
    bool highContrast = false;
    bool hillshade = true;
    bool ecotones = true;
    double contourInterval = 0;     // metres; baked contours for software view / export
    bool rawElevation = false;      // Elevation tab: structural relief before erosion (stage 2 snapshot)
};

QImage colorizeTab(const WorldData& w, MapTab tab, const ViewOptions& opt, ThreadPool* pool = nullptr);

// Single-channel float image of surface elevation relative to sea level (for GPU contours).
std::vector<float> elevationAboveSea(const WorldData& w);

struct LegendItem {
    Rgb color;
    std::string label;
};

struct Legend {
    std::string title;
    std::string units;
    bool continuous = false;
    ColorRamp ramp;                                  // continuous / stepped scale
    double lo = 0, hi = 1;                           // displayed range
    std::vector<std::pair<double, std::string>> ticks;
    std::vector<LegendItem> items;                   // categorical entries
    std::vector<std::string> notes;                  // meaning, units, model uncertainty
    std::string overlayNote;
};

Legend legendFor(const WorldData& w, MapTab tab, const ViewOptions& opt);

// Physical colour of the "not computed" hatch.
Rgb notComputedColor(int x, int y);

}  // namespace mapgen
