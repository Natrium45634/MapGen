#include "render/Palette.h"

#include <algorithm>
#include <cmath>

namespace mapgen {

Rgb mix(Rgb a, Rgb b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto m = [&](uint8_t x, uint8_t y) { return static_cast<uint8_t>(std::lround(x + (y - x) * t)); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b)};
}

Rgb shade(Rgb c, double f) {
    auto s = [&](uint8_t x) { return static_cast<uint8_t>(std::clamp(std::lround(x * f), 0L, 255L)); };
    return {s(c.r), s(c.g), s(c.b)};
}

Rgb ColorRamp::at(double v) const {
    if (stops_.empty()) return {0, 0, 0};
    if (v <= stops_.front().value) return stops_.front().color;
    if (v >= stops_.back().value) return stops_.back().color;
    for (size_t i = 1; i < stops_.size(); ++i) {
        if (v < stops_[i].value) {
            if (stepped_) return stops_[i - 1].color;
            double t = (v - stops_[i - 1].value) / (stops_[i].value - stops_[i - 1].value);
            return mix(stops_[i - 1].color, stops_[i].color, t);
        }
    }
    return stops_.back().color;
}

namespace {
Rgb H(uint32_t v) { return Rgb::hex(v); }
// Perceptually uniform, colour-vision-deficiency-safe sequences (viridis / cividis samples).
std::vector<ColorStop> viridis(double lo, double hi) {
    const uint32_t c[] = {0x440154, 0x482878, 0x3e4a89, 0x31688e, 0x26828e, 0x1f9e89, 0x35b779, 0x6ece58, 0xb5de2b, 0xfde725};
    std::vector<ColorStop> s;
    for (int i = 0; i < 10; ++i) s.push_back({lo + (hi - lo) * i / 9.0, H(c[i])});
    return s;
}
std::vector<ColorStop> cividis(double lo, double hi) {
    const uint32_t c[] = {0x00224e, 0x123570, 0x3b496c, 0x575d6d, 0x707173, 0x8a8678, 0xa59c74, 0xc3b369, 0xe1cc55, 0xfee838};
    std::vector<ColorStop> s;
    for (int i = 0; i < 10; ++i) s.push_back({lo + (hi - lo) * i / 9.0, H(c[i])});
    return s;
}
}  // namespace

namespace ramps {

const ColorRamp& landHypsometric(bool cb) {
    static const ColorRamp normal({{0, H(0x5e9e5b)}, {100, H(0x7fb36a)}, {300, H(0xb4c886)}, {600, H(0xdcd59b)},
                                   {1000, H(0xd8b77f)}, {1500, H(0xc19463)}, {2200, H(0xa6744d)}, {3000, H(0x8e6a55)},
                                   {4000, H(0xa79b94)}, {5000, H(0xd9d6d4)}, {6500, H(0xffffff)}});
    static const ColorRamp safe(cividis(0, 6000));
    return cb ? safe : normal;
}

const ColorRamp& bathymetry(bool cb) {
    static const ColorRamp normal({{-11000, H(0x061a3a)}, {-7000, H(0x0b2a5c)}, {-5000, H(0x153f7a)},
                                   {-3500, H(0x22599a)}, {-2000, H(0x3473b4)}, {-1000, H(0x4f90c8)},
                                   {-200, H(0x7cb6de)}, {-50, H(0xa9d3ee)}, {0, H(0xc9e6f6)}});
    static const ColorRamp safe({{-11000, H(0x08101f)}, {-6000, H(0x1d2f57)}, {-3500, H(0x2d4f8a)},
                                 {-1500, H(0x4a7ab8)}, {-200, H(0x8fb5de)}, {0, H(0xd6e6f5)}});
    return cb ? safe : normal;
}

const ColorRamp& temperature(bool cb) {
    static const ColorRamp normal({{-50, H(0x3b0f70)}, {-35, H(0x2c3f99)}, {-20, H(0x2f73c2)}, {-10, H(0x5aa5dc)},
                                   {-2, H(0xa9d6ee)}, {0, H(0xf2f2f2)}, {5, H(0xfde3a7)}, {12, H(0xf9bb6a)},
                                   {20, H(0xee8a3c)}, {27, H(0xd8452e)}, {35, H(0xa3151f)}, {45, H(0x5c0a17)}});
    static const ColorRamp safe({{-50, H(0x053061)}, {-25, H(0x2166ac)}, {-10, H(0x67a9cf)}, {0, H(0xf7f7f7)},
                                 {10, H(0xfdb863)}, {25, H(0xe08214)}, {45, H(0x7f3b08)}});
    return cb ? safe : normal;
}

const ColorRamp& precipitationAnnual(bool cb) {
    static const ColorRamp normal({{0, H(0xfdf6e3)}, {100, H(0xf2e3b3)}, {250, H(0xdfd08a)}, {500, H(0xb5d27a)},
                                   {1000, H(0x63b95e)}, {1500, H(0x2c9a6b)}, {2000, H(0x1f7f8c)}, {3000, H(0x2a5ea8)},
                                   {4500, H(0x3a2f8f)}, {6000, H(0x5c1a6e)}});
    static const ColorRamp safe(viridis(0, 4000));
    return cb ? safe : normal;
}

const ColorRamp& precipitationMonthly(bool cb) {
    static const ColorRamp normal({{0, H(0xfdf6e3)}, {10, H(0xf2e3b3)}, {25, H(0xdfd08a)}, {50, H(0xb5d27a)},
                                   {100, H(0x63b95e)}, {150, H(0x2c9a6b)}, {200, H(0x1f7f8c)}, {300, H(0x2a5ea8)},
                                   {450, H(0x3a2f8f)}, {700, H(0x5c1a6e)}});
    static const ColorRamp safe(viridis(0, 400));
    return cb ? safe : normal;
}

const ColorRamp& aridity(bool cb) {
    // UNEP classes: hyper-arid <0.05, arid <0.2, semi-arid <0.5, dry sub-humid <0.65, humid >=0.65 (+ wet >=1.5)
    static const ColorRamp normal({{0, H(0xb2182b)}, {0.05, H(0xef8a62)}, {0.2, H(0xfddbc7)}, {0.5, H(0xd1e5f0)},
                                   {0.65, H(0x67a9cf)}, {1.5, H(0x2166ac)}, {100, H(0x2166ac)}},
                                  true);
    static const ColorRamp safe({{0, H(0x7f3b08)}, {0.05, H(0xe08214)}, {0.2, H(0xfee0b6)}, {0.5, H(0xd8daeb)},
                                 {0.65, H(0x8073ac)}, {1.5, H(0x2d004b)}, {100, H(0x2d004b)}},
                                true);
    return cb ? safe : normal;
}

const ColorRamp& relativeHumidity(bool cb) {
    static const ColorRamp normal({{0, H(0xf7e9c3)}, {30, H(0xe9d49a)}, {50, H(0xbfd8a9)}, {70, H(0x6fb7b6)},
                                   {85, H(0x2f7fb4)}, {100, H(0x1b3f86)}});
    static const ColorRamp safe(viridis(0, 100));
    return cb ? safe : normal;
}

const ColorRamp& soilMoisture(bool cb) {
    static const ColorRamp normal({{0, H(0xf3e2c0)}, {25, H(0xd9c08a)}, {60, H(0xa6b86d)}, {100, H(0x5c9c5b)},
                                   {150, H(0x2f7a64)}, {250, H(0x1d4f6b)}});
    static const ColorRamp safe(cividis(0, 250));
    return cb ? safe : normal;
}

const ColorRamp& windSpeed(bool cb) {
    static const ColorRamp normal({{0, H(0xf7fbff)}, {3, H(0xc6dbef)}, {6, H(0x6baed6)}, {9, H(0x2171b5)},
                                   {13, H(0x08306b)}});
    static const ColorRamp safe(cividis(0, 13));
    return cb ? safe : normal;
}

const ColorRamp& zone05(bool cb) {
    static const ColorRamp normal({{0, H(0xf4f1e8)}, {1, H(0xfde0b0)}, {2, H(0xf9b26b)}, {3, H(0xee7a3a)},
                                   {4, H(0xcc3b2b)}, {5, H(0x8c1020)}, {6, H(0x8c1020)}},
                                  true);
    static const ColorRamp safe({{0, H(0xf7f7f7)}, {1, H(0xd9d9d9)}, {2, H(0xbdbdbd)}, {3, H(0x969696)},
                                 {4, H(0x525252)}, {5, H(0x000000)}, {6, H(0x000000)}},
                                true);
    return cb ? safe : normal;
}

const ColorRamp& probability(bool cb) {
    static const ColorRamp normal({{0, H(0xf7f4f9)}, {0.25, H(0xd4b9da)}, {0.5, H(0xdf65b0)}, {0.75, H(0xce1256)},
                                   {1, H(0x67001f)}});
    static const ColorRamp safe(viridis(0, 1));
    return cb ? safe : normal;
}

const ColorRamp& sequentialBlue(bool cb) {
    static const ColorRamp normal({{0, H(0xf7fbff)}, {0.5, H(0x6baed6)}, {1, H(0x08306b)}});
    static const ColorRamp safe(cividis(0, 1));
    return cb ? safe : normal;
}

}  // namespace ramps

Rgb hsv(double h, double s, double v) {
    h = std::fmod(h, 1.0);
    if (h < 0) h += 1.0;
    double r, g, b;
    int i = static_cast<int>(h * 6.0);
    double f = h * 6.0 - i;
    double p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch (i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return {static_cast<uint8_t>(std::lround(r * 255)), static_cast<uint8_t>(std::lround(g * 255)),
            static_cast<uint8_t>(std::lround(b * 255))};
}

Rgb categoricalColor(int id, bool cb, double sat, double val) {
    double h = std::fmod(0.13 + id * 0.6180339887498949, 1.0);
    // Alternate value so that neighbouring ids also differ in lightness (helps colour-blind users).
    double v = val - ((id % 3) * (cb ? 0.18 : 0.08));
    double s = cb ? sat * 0.8 : sat;
    return hsv(h, s, std::clamp(v, 0.3, 1.0));
}

}  // namespace mapgen
