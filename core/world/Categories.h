#pragma once

// Category codes stored in categorical layers, with Russian captions.
// Codes are part of the file format: never renumber, only append.

#include <cstdint>
#include <string>
#include <vector>

#include "core/world/Layers.h"

namespace mapgen {

enum CrustTypeCode : uint8_t { kCrustOceanic = 0, kCrustContinental = 1 };

enum BoundaryTypeCode : uint8_t { kBndNone = 0, kBndDivergent = 1, kBndConvergent = 2, kBndTransform = 3, kBndComplex = 4 };
enum BoundaryClassCode : uint8_t { kBcNone = 0, kBcOceanOcean = 1, kBcOceanContinent = 2, kBcContinentContinent = 3 };

enum TectonicSideCode : uint8_t {
    kSideInterior = 0, kSideOverriding = 1, kSideSubducting = 2, kSideCollision = 3, kSideRift = 4,
    kSideRidge = 5, kSideTransform = 6, kSideComplex = 7
};

enum GenesisCode : uint8_t {
    kGenAbyssal = 0, kGenMidOceanRidge = 1, kGenTrench = 2, kGenContinentalMargin = 3, kGenIslandArc = 4,
    kGenVolcanicArc = 5, kGenCollisionOrogen = 6, kGenOldOrogen = 7, kGenPlateau = 8, kGenContinentalRift = 9,
    kGenPlatform = 10, kGenSedimentaryBasin = 11, kGenTransformZone = 12, kGenHotspot = 13, kGenBackArc = 14,
    kGenShield = 15
};

enum LithologyCode : uint8_t {
    kLithBasalt = 0, kLithCrystalline = 1, kLithSedimentary = 2, kLithCarbonate = 3, kLithVolcanic = 4,
    kLithAlluvium = 5, kLithMetamorphic = 6, kLithMarineSediment = 7
};

enum LandformCode : uint8_t {
    // land
    kLfMountains = 0, kLfHighlands = 1, kLfPlateau = 2, kLfTableland = 3, kLfHills = 4, kLfFoothillPlain = 5,
    kLfLowland = 6, kLfAlluvialLowland = 7, kLfDepression = 8, kLfPlatformPlain = 9, kLfCoastalPlain = 10,
    kLfRiftValley = 11, kLfVolcanic = 12, kLfGlacial = 13,
    // sea floor
    kLfShelf = 20, kLfSlope = 21, kLfRise = 22, kLfAbyssalPlain = 23, kLfRidge = 24, kLfTrench = 25,
    kLfSeamount = 26, kLfGuyot = 27, kLfArcRidge = 28, kLfCanyon = 29, kLfBackArcBasin = 30, kLfFractureZone = 31
};

enum IceTypeCode : uint8_t { kIceNone = 0, kIceGlacier = 1, kIceSheet = 2, kIceShelf = 3 };

enum RiverClassCode : uint8_t { kRiverNone = 0, kRiverPerennial = 1, kRiverSeasonal = 2, kRiverIntermittent = 3, kRiverDistributary = 4 };

enum WetlandCode : uint8_t { kWetNone = 0, kWetMarsh = 1, kWetPeat = 2, kWetFloodplain = 3, kWetSaltFlat = 4 };

enum FlowDirCode : uint8_t { kFlowOcean = 8, kFlowSink = 9 };  // 0..7 = neighbour direction

enum BiomeCode : uint8_t {
    kBiomeNone = 0, kBiomeTropicalRainforest = 1, kBiomeMonsoonForest = 2, kBiomeTropicalDryForest = 3,
    kBiomeSavanna = 4, kBiomeSteppe = 5, kBiomeHotDesert = 6, kBiomeColdDesert = 7, kBiomeSemiDesert = 8,
    kBiomeMediterranean = 9, kBiomeTemperateForest = 10, kBiomeTemperateRainforest = 11, kBiomeTaiga = 12,
    kBiomeForestTundra = 13, kBiomeTundra = 14, kBiomeAlpine = 15, kBiomeNival = 16, kBiomeIceSheet = 17,
    kBiomeFreshwaterMarsh = 18, kBiomePeatland = 19, kBiomeFloodplain = 20, kBiomeSaltFlat = 21,
    kBiomeMangrove = 22, kBiomeCoastalEcotone = 23, kBiomeLake = 24, kBiomeOcean = 25, kBiomePolarDesert = 26,
    kBiomeCount = 27
};

enum AltBeltCode : uint8_t {
    kBeltNone = 0, kBeltLowland = 1, kBeltFoothill = 2, kBeltMontane = 3, kBeltSubalpine = 4, kBeltAlpine = 5,
    kBeltNival = 6
};

enum CoastTypeCode : uint8_t {
    kCoastNone = 0, kCoastCliff = 1, kCoastAbrasion = 2, kCoastAccumulative = 3, kCoastSandy = 4, kCoastPebble = 5,
    kCoastBarrier = 6, kCoastLagoon = 7, kCoastEstuary = 8, kCoastDelta = 9, kCoastTidalFlat = 10, kCoastFjord = 11,
    kCoastRia = 12, kCoastSkerry = 13, kCoastCoral = 14, kCoastMangrove = 15, kCoastIce = 16
};

enum KarstCode : uint8_t { kKarstNone = 0, kKarstLow = 1, kKarstHigh = 2 };

// Köppen–Geiger codes 1..30 in the order of Beck et al. (2018) legend; 0 = none (ocean).
enum KoppenCode : uint8_t {
    kKgNone = 0, kKgAf, kKgAm, kKgAw, kKgBWh, kKgBWk, kKgBSh, kKgBSk, kKgCsa, kKgCsb, kKgCsc, kKgCwa, kKgCwb,
    kKgCwc, kKgCfa, kKgCfb, kKgCfc, kKgDsa, kKgDsb, kKgDsc, kKgDsd, kKgDwa, kKgDwb, kKgDwc, kKgDwd, kKgDfa,
    kKgDfb, kKgDfc, kKgDfd, kKgET, kKgEF, kKgCount
};

struct CategoryEntry {
    int code;
    std::string nameRu;
    uint32_t rgb;  // default display colour 0xRRGGBB
};

// Categories for a categorical layer (empty for continuous layers).
const std::vector<CategoryEntry>& categoriesOf(LayerId id);
std::string categoryName(LayerId id, int code);
const char* koppenCode(int code);  // "Af", "BWh", ...

}  // namespace mapgen
