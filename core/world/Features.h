#pragma once

// Vector features kept separately from rasters: plates, hotspots, volcanoes,
// coastlines, rivers, lakes, basins, waterfalls, thermal springs.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace mapgen {

struct GeoPoint {
    double lat = 0, lon = 0;  // degrees; lon may be unwrapped (beyond ±180) inside polylines
};

struct Polyline {
    std::vector<GeoPoint> pts;
};

struct Plate {
    int id = 0;
    double seedLat = 0, seedLon = 0;
    double poleLat = 0, poleLon = 0;  // Euler pole
    double omegaDegPerMyr = 0;        // angular speed (conventional)
    double weight = 1;                // growth weight used for the shape
    double areaKm2 = 0;
    double continentalFraction = 0;
    double rigidity = 1;
    double centroidLat = 0, centroidLon = 0;
    double meanSpeedCmYr = 0;
};

struct BoundarySegment {
    Polyline line;
    int plateA = 0, plateB = 0;
    uint8_t type = 0, cls = 0;  // BoundaryTypeCode, BoundaryClassCode
    float convergenceCmYr = 0, shearCmYr = 0;
    int subductingPlate = -1;
};

struct TectonicsFeatures {
    std::vector<Plate> plates;
    std::vector<BoundarySegment> boundaries;
    double crustThreshold = 0;  // continentality threshold of continental crust
    std::vector<Polyline> oldSutures;  // ancient orogenic belts (great-circle arcs)
};

enum VolcanoGenesis : uint8_t {
    kVgArcContinental = 0, kVgArcIsland = 1, kVgContinentalRift = 2, kVgRidge = 3, kVgHotspotOceanic = 4,
    kVgHotspotContinental = 5, kVgBackArc = 6
};
enum VolcanoMorphology : uint8_t {
    kVmStrato = 0, kVmShield = 1, kVmCaldera = 2, kVmCinderField = 3, kVmSeamount = 4, kVmGuyot = 5, kVmFissure = 6
};

struct Volcano {
    int id = 0;
    double lat = 0, lon = 0;
    int64_t cell = 0;
    uint8_t genesis = 0, morphology = 0;
    float edificeHeightM = 0;  // above surrounding terrain / sea floor
    float ageMyr = 0;          // age of the edifice (hotspot chains); 0 = active front
    int plateId = 0;
    int hotspotId = -1;
};

struct Hotspot {
    int id = 0;
    double lat = 0, lon = 0;  // mantle-fixed position
    double strength = 1;
    int plateId = 0;
    bool oceanic = true;
    bool onRidge = false;
};

struct GeomorphFeatures {
    std::vector<Hotspot> hotspots;
    std::vector<Volcano> volcanoes;
    double calibrationShiftM = 0;  // shift applied to put the relief land fraction at 0 m
};

struct OceanFeatures {
    double seaLevelM = 0;
    double landFraction = 0;
    int oceanComponents = 0;
    std::vector<Polyline> coastlines;
    std::vector<double> oceanAreasKm2;
};

struct RiverSegment {
    int id = 0;
    Polyline line;                 // smoothed visual geometry, never leaves its cells
    std::vector<int64_t> cells;    // computational path, upstream to downstream
    int order = 1;                 // Strahler
    float dischargeStart = 0, dischargeEnd = 0;  // m³/s
    float lengthKm = 0;
    float slope = 0;               // mean gradient (m/km)
    int basinId = -1;
    int downstream = -1;           // next segment id (-1 = mouth / terminal)
    uint8_t riverClass = 1;        // RiverClassCode
    bool meandering = false;
};

enum LakeGenesis : uint8_t { kLgTectonic = 0, kLgGlacial = 1, kLgVolcanic = 2, kLgFloodplain = 3, kLgDammed = 4, kLgBasin = 5 };
enum LakeRegime : uint8_t { kLrOutflow = 0, kLrClosed = 1, kLrEphemeral = 2 };

struct Lake {
    int id = 0;
    double lat = 0, lon = 0;
    float areaKm2 = 0, levelM = 0, maxDepthM = 0, volumeKm3 = 0;
    float inflowM3s = 0, evaporationM3s = 0, outflowM3s = 0;
    uint8_t genesis = kLgBasin, regime = kLrOutflow;
    bool saline = false;
    int basinId = -1;
    int64_t outletCell = -1;
    int cells = 0;
};

enum BasinOutlet : uint8_t { kBoOcean = 0, kBoTerminalLake = 1, kBoDrySink = 2 };

struct Basin {
    int id = 0;
    int64_t outletCell = -1;
    uint8_t outlet = kBoOcean;
    float areaKm2 = 0;
    float dischargeM3s = 0;
    float mainRiverKm = 0;
    int cells = 0;
};

struct Waterfall {
    double lat = 0, lon = 0;
    float dropM = 0;
    int riverId = -1;
    bool rapids = false;
};

struct Delta {
    int64_t mouthCell = -1;
    double lat = 0, lon = 0;
    int branches = 0;
    float dischargeM3s = 0;
    int riverId = -1;
};

struct HydroFeatures {
    std::vector<RiverSegment> rivers;
    std::vector<Lake> lakes;
    std::vector<Basin> basins;
    std::vector<Waterfall> waterfalls;
    std::vector<Delta> deltas;
    int climateLakeIterations = 0;
};

enum VolcanoActivity : uint8_t { kVaExtinct = 0, kVaDormant = 1, kVaActive = 2 };

struct VolcanoState {
    int volcanoId = 0;
    uint8_t activity = 0;
    bool submarine = false;
    float summitM = 0;      // relative to sea level
    uint8_t hazardClass = 0;  // conditional 0..3
    uint8_t zone = 0;         // 0..5
};

struct ThermalSpring {
    double lat = 0, lon = 0;
    bool geyser = false;
    float potential = 0;
    int64_t cell = 0;
};

struct GeoActivityFeatures {
    std::vector<VolcanoState> volcanoStates;  // parallel to GeomorphFeatures::volcanoes
    std::vector<ThermalSpring> springs;
};

}  // namespace mapgen
