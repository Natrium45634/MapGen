#pragma once

// Attributes of one cell as shown by the info card and printed by `mapgen-cli probe`.
// Values that do not exist or cannot be justified are reported as "не рассчитано"
// (never as zeros); ocean cells get no land biome, land cells show height, not "depth 0".

#include <string>
#include <vector>

#include "core/world/WorldData.h"

namespace mapgen {

struct CellField {
    std::string key;      // stable id
    std::string labelRu;
    std::string valueRu;  // formatted value with units, or "не рассчитано"
    LayerId layer;        // source layer (for highlighting the active tab's value)
    bool available = true;
    double numeric = 0;   // raw physical value when numeric
};

std::vector<CellField> describeCell(const WorldData& w, int64_t cell, int month);
std::string describeCellText(const WorldData& w, int64_t cell, int month);  // "label: value" lines

extern const char* const kNotComputedRu;

}  // namespace mapgen
