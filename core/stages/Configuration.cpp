// Stage 0: configuration. Validates seed and parameters, sets up the grids and
// logs every parameter value.

#include <stdexcept>

#include "core/stages/Stage.h"
#include "mapgen/Version.h"

namespace mapgen {

int climateGridWidth(int fullWidth) {
    int w = std::clamp(fullWidth / 2, 64, 512);
    return w - (w % 2);
}

void runConfiguration(StageContext& ctx) {
    WorldData& w = ctx.world;
    std::string err;
    if (!isValidSeed(w.seed, &err)) throw std::invalid_argument(err);
    auto issues = validateParams(w.params);
    for (auto& i : issues) {
        if (i.level == ValidationIssue::Error) throw std::invalid_argument(i.messageRu);
        ctx.report.warn(i.messageRu);
    }
    w.generatorVersion = kGeneratorVersion;
    w.algorithmVersion = kAlgorithmVersion;
    w.schemaVersion = kSchemaVersion;

    const int W = w.params.gridWidth();
    const double R = w.params.num("world.radiusKm");
    if (!w.grid.valid() || w.grid.width() != W || w.grid.radiusKm() != R) {
        w.grid = SphereGrid(W, W / 2, R);
        int cw = climateGridWidth(W);
        w.climateGrid = SphereGrid(cw, cw / 2, R);
    }
    ctx.report.log("Сид: " + w.seed);
    ctx.report.log("Версия генератора " + std::string(kGeneratorVersion) + ", алгоритм v" +
                   std::to_string(kAlgorithmVersion) + ", схема v" + std::to_string(kSchemaVersion));
    ctx.report.log("Сетка " + std::to_string(w.grid.width()) + "×" + std::to_string(w.grid.height()) +
                   ", климатическая сетка " + std::to_string(w.climateGrid.width()) + "×" +
                   std::to_string(w.climateGrid.height()) + ", радиус " + formatNumber(R) + " км");
    for (auto& d : paramSchema()) {
        std::string v = (d.type == ParamType::Enum || d.type == ParamType::String) ? w.params.str(d.id)
                                                                                   : formatNumber(w.params.num(d.id));
        ctx.report.log(d.id + " = " + v + (d.units.empty() ? "" : " " + d.units));
    }
    ctx.report.check("params.valid", "Параметры в допустимых диапазонах", true, 1, "без ошибок", true);
    ctx.report.metric("gridWidth", w.grid.width());
    ctx.report.metric("gridHeight", w.grid.height());
}

}  // namespace mapgen
