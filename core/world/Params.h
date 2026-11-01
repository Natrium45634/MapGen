#pragma once

// Declarative parameter schema (id, type, range, default, units, dependencies,
// invalidated stage, realism notes, serialization version), presets and the
// realism/conflict validator.

#include <map>
#include <string>
#include <vector>

#include "core/util/Json.h"

namespace mapgen {

enum class ParamType { Int, Real, Bool, Enum, String };

struct EnumOption {
    std::string id;
    std::string nameRu;
};

struct ParamDef {
    std::string id;
    ParamType type = ParamType::Real;
    double minValue = 0, maxValue = 1, defValue = 0;
    std::string defString;
    std::vector<EnumOption> options;  // for Enum
    std::string units;
    std::string nameRu;
    std::string descriptionRu;
    std::string group;                // UI group caption (ru)
    int stageInvalidated = 1;         // first stage to rerun when this parameter changes (0 = UI only)
    std::vector<std::string> dependencies;
    std::string realismNotes;
    int serializationVersion = 1;
    double step = 0;                  // UI step hint
};

const std::vector<ParamDef>& paramSchema();
const ParamDef* findParam(const std::string& id);

struct Resolution {
    std::string id;
    int width, height;
    std::string nameRu;
};
const std::vector<Resolution>& resolutions();

class Params {
public:
    Params();  // schema defaults

    double num(const std::string& id) const;
    int integer(const std::string& id) const { return static_cast<int>(num(id) + (num(id) >= 0 ? 0.5 : -0.5)); }
    bool flag(const std::string& id) const { return num(id) != 0.0; }
    const std::string& str(const std::string& id) const;  // Enum / String
    void set(const std::string& id, double v);
    void set(const std::string& id, const std::string& v);
    bool has(const std::string& id) const;

    // Grid size implied by world.resolution (+ world.customWidth for "custom").
    int gridWidth() const;
    int gridHeight() const { return gridWidth() / 2; }

    Json toJson() const;
    // Unknown fields are preserved in `unknown` and reported in warnings.
    static Params fromJson(const Json& j, std::vector<std::string>* warningsRu = nullptr);

    bool operator==(const Params& o) const { return nums_ == o.nums_ && strs_ == o.strs_; }
    bool operator!=(const Params& o) const { return !(*this == o); }

    const std::map<std::string, double>& numbers() const { return nums_; }
    const std::map<std::string, std::string>& strings() const { return strs_; }
    Json unknown;  // unknown fields from a newer file, kept for round trip

private:
    std::map<std::string, double> nums_;
    std::map<std::string, std::string> strs_;
};

// First stage that must rerun when moving from `a` to `b` (9 = nothing to rerun, 0 = UI only).
int firstInvalidatedStage(const Params& a, const Params& b);

struct ParamRange {
    double lo, hi;
};

struct Preset {
    std::string id;
    std::string nameRu;
    std::string descriptionRu;
    std::map<std::string, ParamRange> ranges;     // numeric ranges; value = midpoint unless `values` overrides
    std::map<std::string, double> values;         // explicit defaults inside the ranges
    std::map<std::string, std::string> strings;   // enum values
};

const std::vector<Preset>& presets();
const Preset* findPreset(const std::string& id);
// Applies preset defaults (keeps world.resolution and seed-unrelated UI params).
void applyPreset(Params& p, const Preset& preset);

struct ValidationIssue {
    enum Level { Info, Warning, Conflict, Error } level;
    std::string messageRu;
    std::vector<std::string> params;
};

// Range checks always; physical-plausibility conflicts when realism mode is on.
std::vector<ValidationIssue> validateParams(const Params& p);

}  // namespace mapgen
