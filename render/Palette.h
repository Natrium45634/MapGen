#pragma once

// Colour ramps for continuous layers and helpers for categorical palettes.
// Continuous fields use purpose-built ramps (never a generic rainbow);
// categories are never interpolated.

#include <cstdint>
#include <string>
#include <vector>

namespace mapgen {

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
    uint32_t argb(uint8_t a = 255) const { return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b; }
    static Rgb hex(uint32_t v) { return {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; }
};

Rgb mix(Rgb a, Rgb b, double t);
Rgb shade(Rgb c, double factor);  // multiply brightness (0..2)

struct ColorStop {
    double value;
    Rgb color;
};

class ColorRamp {
public:
    ColorRamp() = default;
    explicit ColorRamp(std::vector<ColorStop> stops, bool stepped = false) : stops_(std::move(stops)), stepped_(stepped) {}
    Rgb at(double v) const;
    double minValue() const { return stops_.empty() ? 0 : stops_.front().value; }
    double maxValue() const { return stops_.empty() ? 1 : stops_.back().value; }
    const std::vector<ColorStop>& stops() const { return stops_; }
    bool stepped() const { return stepped_; }

private:
    std::vector<ColorStop> stops_;
    bool stepped_ = false;
};

namespace ramps {
// Hypsometric tints, metres above sea level (physical map style).
const ColorRamp& landHypsometric(bool colorblind);
// Bathymetry, metres below sea level as negative elevation.
const ColorRamp& bathymetry(bool colorblind);
// Temperature °C (diverging around 0 °C).
const ColorRamp& temperature(bool colorblind);
// Precipitation mm/year and mm/month (sequential).
const ColorRamp& precipitationAnnual(bool colorblind);
const ColorRamp& precipitationMonthly(bool colorblind);
// Aridity index P/PET with UNEP class boundaries (stepped).
const ColorRamp& aridity(bool colorblind);
const ColorRamp& relativeHumidity(bool colorblind);
const ColorRamp& soilMoisture(bool colorblind);
const ColorRamp& windSpeed(bool colorblind);
const ColorRamp& zone05(bool colorblind);       // ordinal 0..5 scales
const ColorRamp& probability(bool colorblind);  // 0..1
const ColorRamp& sequentialBlue(bool colorblind);
}  // namespace ramps

// Deterministic distinct colour for an id (plates, basins): golden-angle hue walk.
Rgb categoricalColor(int id, bool colorblind, double saturation = 0.55, double value = 0.85);
Rgb hsv(double h, double s, double v);

}  // namespace mapgen
