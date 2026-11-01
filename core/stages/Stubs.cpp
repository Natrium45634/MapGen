// Temporary no-op stages; each is replaced by its real implementation file.
#include "core/stages/Stage.h"

namespace mapgen {
void runHydrology(StageContext&) {}
void runBiomes(StageContext&) {}
void runGeoActivity(StageContext&) {}
void runQualityAssurance(StageContext&) {}
}  // namespace mapgen
