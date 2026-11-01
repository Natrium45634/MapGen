#include "render/Colorizer.h"

#include <bit>
#include <cmath>
#include <set>

#include "core/world/Categories.h"

namespace mapgen {

const std::vector<TabInfo>& tabs() {
    static const std::vector<TabInfo> t = {
        {MapTab::Physical, "physical", "Физическая карта", false, LayerId::Elevation, 3},
        {MapTab::Elevation, "elevation", "Высоты и батиметрия", false, LayerId::Elevation, 3},
        {MapTab::Plates, "plates", "Плиты", false, LayerId::PlateId, 1},
        {MapTab::Landforms, "landforms", "Рельеф / генезис", false, LayerId::Landform, 3},
        {MapTab::Temperature, "temperature", "Температура", true, LayerId::TempMean, 4},
        {MapTab::Precipitation, "precipitation", "Осадки", true, LayerId::PrecAnnual, 4},
        {MapTab::Humidity, "humidity", "Влажность и аридность", true, LayerId::Aridity, 4},
        {MapTab::Wind, "wind", "Ветры / сезонность", true, LayerId::WindU, 4},
        {MapTab::Rivers, "rivers", "Реки и бассейны", false, LayerId::Discharge, 5},
        {MapTab::Koppen, "koppen", "Климат Кёппена", false, LayerId::Koppen, 6},
        {MapTab::Biomes, "biomes", "Биомы / пояса", false, LayerId::Biome, 6},
        {MapTab::Volcanism, "volcanism", "Вулканизм / геотермия", false, LayerId::VolcanicZone, 7},
        {MapTab::SnowIce, "snowice", "Снег и лёд", true, LayerId::IceType, 4},
        {MapTab::Coasts, "coasts", "Побережья", false, LayerId::CoastType, 6},
    };
    return t;
}

const TabInfo& tabInfo(MapTab t) { return tabs()[static_cast<size_t>(t)]; }

Rgb notComputedColor(int x, int y) { return ((x + y) / 4) % 2 ? Rgb{205, 205, 205} : Rgb{232, 232, 232}; }

namespace {

constexpr Rgb kLakeColor{111, 168, 220};
constexpr Rgb kIceColor{240, 247, 252};

struct Ctx {
    const WorldData& w;
    const ViewOptions& o;
    int W, H;
    double sea;
};

// Hillshade factor in ~[0.55, 1.25] from surface elevation (NW light).
std::vector<float> hillshade(const WorldData& w, double exaggeration, ThreadPool* pool) {
    const SphereGrid& g = w.grid;
    const int W = g.width(), H = g.height();
    std::vector<float> out(static_cast<size_t>(g.size()), 1.0f);
    const float* e = w.f32(LayerId::Elevation);
    if (!e) return out;
    std::vector<float> z(static_cast<size_t>(g.size()));
    for (int64_t k = 0; k < g.size(); ++k) z[static_cast<size_t>(k)] = static_cast<float>(w.surfaceElevation(k));
    const double lightAz = 315.0 * kDegToRad, lightAlt = 45.0 * kDegToRad;
    auto row = [&](int j) {
        double dx = std::max(g.dxKm(j), g.dyKm() * 0.25) * 1000.0, dy = g.dyKm() * 1000.0;
        int jn = std::max(0, j - 1), js = std::min(H - 1, j + 1);
        for (int i = 0; i < W; ++i) {
            double zl = z[static_cast<size_t>(g.index(g.wrapCol(i - 1), j))], zr = z[static_cast<size_t>(g.index(g.wrapCol(i + 1), j))];
            double zn = z[static_cast<size_t>(g.index(i, jn))], zs = z[static_cast<size_t>(g.index(i, js))];
            double gx = (zr - zl) / (2 * dx) * exaggeration;
            double gy = (zn - zs) / ((js - jn) * dy) * exaggeration;
            double slope = std::atan(std::sqrt(gx * gx + gy * gy));
            double aspect = std::atan2(gy, -gx);
            double s = std::sin(lightAlt) * std::cos(slope) +
                       std::cos(lightAlt) * std::sin(slope) * std::cos(lightAz - kPi / 2 - aspect);
            out[static_cast<size_t>(g.index(i, j))] = static_cast<float>(0.45 + 0.75 * std::clamp(s, 0.0, 1.0));
        }
    };
    if (pool) pool->forRows(H, row);
    else for (int j = 0; j < H; ++j) row(j);
    return out;
}

Rgb physicalColor(const WorldData& w, int64_t k, bool cb) {
    const uint8_t* om = w.u8(LayerId::OceanMask);
    const float* e = w.f32(LayerId::Elevation);
    double sea = w.seaLevel();
    double z = e[k] - sea;
    if (om && om[k]) {
        const uint16_t* si = w.u16(LayerId::SeaIceMonths);
        const uint8_t* it = w.u8(LayerId::IceType);
        if (it && it[k] == kIceShelf) return {225, 238, 246};
        if (si && si[k] == 0x0fff) return {222, 234, 242};
        return ramps::bathymetry(cb).at(z);
    }
    const int32_t* lake = w.i32(LayerId::LakeId);
    if (lake && lake[k] >= 0) return kLakeColor;
    const uint8_t* it = w.u8(LayerId::IceType);
    if (it && (it[k] == kIceGlacier || it[k] == kIceSheet)) return kIceColor;
    return ramps::landHypsometric(cb).at(w.surfaceElevation(k) - sea);
}

Rgb catColor(LayerId id, int code) {
    for (auto& c : categoriesOf(id))
        if (c.code == code) return Rgb::hex(c.rgb);
    return {128, 128, 128};
}

}  // namespace

std::vector<float> elevationAboveSea(const WorldData& w) {
    std::vector<float> out(static_cast<size_t>(w.grid.size()), 0.0f);
    if (!w.has(LayerId::Elevation)) return out;
    double sea = w.seaLevel();
    for (int64_t k = 0; k < w.grid.size(); ++k) out[static_cast<size_t>(k)] = static_cast<float>(w.surfaceElevation(k) - sea);
    return out;
}

QImage colorizeTab(const WorldData& w, MapTab tab, const ViewOptions& o, ThreadPool* pool) {
    const SphereGrid& g = w.grid;
    if (!g.valid()) return QImage();
    const int W = g.width(), H = g.height();
    QImage img(W, H, QImage::Format_ARGB32);
    const bool cb = o.colorblind;
    const int month = o.month;

    std::vector<float> hs;
    bool wantShade = o.hillshade && w.has(LayerId::Elevation) &&
                     (tab == MapTab::Physical || tab == MapTab::Landforms || tab == MapTab::Biomes ||
                      tab == MapTab::Koppen || tab == MapTab::Volcanism || tab == MapTab::Rivers || tab == MapTab::Elevation);
    if (wantShade) hs = hillshade(w, std::clamp(W / 2048.0, 0.25, 4.0) * 25.0, pool);
    auto shadeAt = [&](int64_t k, Rgb c, double strength) {
        if (hs.empty()) return c;
        double f = 1.0 + (hs[static_cast<size_t>(k)] - 1.0) * strength;
        return shade(c, f);
    };
    const uint8_t* ocean = w.u8(LayerId::OceanMask);
    const int32_t* lake = w.i32(LayerId::LakeId);
    auto isWater = [&](int64_t k) { return (ocean && ocean[k]) || (lake && lake[k] >= 0); };

    const TabInfo& ti = tabInfo(tab);
    bool ready = w.completedStage >= ti.firstStage || w.has(ti.primaryLayer);
    if (tab == MapTab::Physical || tab == MapTab::Elevation) ready = w.has(LayerId::Elevation) && w.has(LayerId::OceanMask);

    auto rowFn = [&](int j) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(j));
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            Rgb c = notComputedColor(i, j);
            if (!ready) {
                line[i] = c.argb();
                continue;
            }
            switch (tab) {
                case MapTab::Physical: {
                    c = physicalColor(w, k, cb);
                    c = shadeAt(k, c, ocean && ocean[k] ? 0.35 : 1.0);
                    break;
                }
                case MapTab::Elevation: {
                    double z = w.surfaceElevation(k) - w.seaLevel();
                    if (o.rawElevation)
                        if (const float* raw = w.f32(LayerId::ElevationRaw)) z = raw[k] - w.seaLevel();
                    c = (ocean && ocean[k]) ? ramps::bathymetry(cb).at(z) : ramps::landHypsometric(cb).at(z);
                    c = shadeAt(k, c, 0.4);
                    if (o.contourInterval > 0) {
                        double zr = w.surfaceElevation(g.index(g.wrapCol(i + 1), j)) - w.seaLevel();
                        double zd = w.surfaceElevation(g.index(i, std::min(H - 1, j + 1))) - w.seaLevel();
                        double b0 = std::floor(z / o.contourInterval);
                        if (b0 != std::floor(zr / o.contourInterval) || b0 != std::floor(zd / o.contourInterval))
                            c = mix(c, Rgb{40, 40, 40}, (std::fmod(std::fabs(b0), 5.0) == 0) ? 0.75 : 0.45);
                    }
                    break;
                }
                case MapTab::Plates: {
                    const uint16_t* pid = w.u16(LayerId::PlateId);
                    const uint8_t* crust = w.u8(LayerId::CrustType);
                    const uint8_t* bt = w.u8(LayerId::BoundaryType);
                    if (!pid) break;
                    if (o.platesView == 2) {
                        const float* age = w.f32(LayerId::CrustAge);
                        if (age && !std::isnan(age[k])) c = ColorRamp({{0, Rgb::hex(0xd7301f)}, {20, Rgb::hex(0xfc8d59)},
                                                                       {60, Rgb::hex(0xfdcc8a)}, {100, Rgb::hex(0x91bfdb)},
                                                                       {180, Rgb::hex(0x4575b4)}}).at(age[k]);
                        else c = {200, 190, 170};
                    } else if (o.platesView == 1) {
                        const uint8_t* bc = w.u8(LayerId::BoundaryClass);
                        c = mix(categoricalColor(pid[k], cb, 0.25, 0.95), Rgb{255, 255, 255}, 0.3);
                        if (bc && bc[k]) c = catColor(LayerId::BoundaryClass, bc[k]);
                    } else {
                        c = categoricalColor(pid[k], cb);
                        if (crust && crust[k] == kCrustOceanic) c = mix(c, Rgb{40, 60, 90}, 0.35);
                        else c = mix(c, Rgb{255, 250, 235}, 0.15);
                    }
                    if (bt && bt[k] && o.platesView != 1) c = catColor(LayerId::BoundaryType, bt[k]);
                    break;
                }
                case MapTab::Landforms: {
                    LayerId id = o.landformView == 1 ? LayerId::Genesis : (o.landformView == 2 ? LayerId::Lithology : LayerId::Landform);
                    const uint8_t* v = w.u8(id);
                    if (v) c = shadeAt(k, catColor(id, v[k]), 0.6);
                    break;
                }
                case MapTab::Temperature: {
                    double t;
                    if (month < 0) { const float* tm = w.f32(LayerId::TempMean); if (!tm) break; t = tm[k]; }
                    else { const int16_t* tm = w.i16(LayerId::TempMonthly); if (!tm) break; t = tm[month * g.size() + k] * 0.01; }
                    c = ramps::temperature(cb).at(t);
                    break;
                }
                case MapTab::Precipitation: {
                    if (month < 0) {
                        const float* p = w.f32(LayerId::PrecAnnual);
                        if (!p) break;
                        c = ramps::precipitationAnnual(cb).at(p[k]);
                    } else {
                        const uint16_t* p = w.u16(LayerId::PrecMonthly);
                        if (!p) break;
                        c = ramps::precipitationMonthly(cb).at(p[month * g.size() + k] * 0.1);
                    }
                    break;
                }
                case MapTab::Humidity: {
                    if (o.humidity == HumidityMetric::RelHumidity) {
                        if (!w.has(LayerId::RelHumidity)) break;
                        double v = 0;
                        if (month < 0) { for (int m = 0; m < 12; ++m) v += w.sample(LayerId::RelHumidity, k, m); v /= 12; }
                        else v = w.sample(LayerId::RelHumidity, k, month);
                        c = ramps::relativeHumidity(cb).at(v);
                    } else if (o.humidity == HumidityMetric::SoilMoisture) {
                        if (ocean && ocean[k]) { c = {214, 226, 236}; break; }
                        if (month < 0) { const float* s = w.f32(LayerId::SoilMean); if (!s) break; c = ramps::soilMoisture(cb).at(s[k]); }
                        else { const uint16_t* s = w.u16(LayerId::SoilMonthly); if (!s) break; c = ramps::soilMoisture(cb).at(s[month * g.size() + k] * 0.1); }
                    } else {
                        if (ocean && ocean[k]) { c = {214, 226, 236}; break; }
                        double ai;
                        if (month < 0) { const float* a = w.f32(LayerId::Aridity); if (!a) break; ai = a[k]; }
                        else {
                            const uint16_t* p = w.u16(LayerId::PrecMonthly);
                            const uint16_t* e = w.u16(LayerId::PetMonthly);
                            if (!p || !e) break;
                            double pe = e[month * g.size() + k] * 0.1;
                            ai = pe > 0.5 ? p[month * g.size() + k] * 0.1 / pe : 10.0;
                        }
                        c = ramps::aridity(cb).at(ai);
                    }
                    break;
                }
                case MapTab::Wind: {
                    if (!w.has(LayerId::WindU)) break;
                    double s = 0;
                    if (month < 0) {
                        for (int m = 0; m < 12; ++m) {
                            double u = w.sample(LayerId::WindU, k, m), v = w.sample(LayerId::WindV, k, m);
                            s += std::sqrt(u * u + v * v);
                        }
                        s /= 12;
                    } else {
                        double u = w.sample(LayerId::WindU, k, month), v = w.sample(LayerId::WindV, k, month);
                        s = std::sqrt(u * u + v * v);
                    }
                    c = ramps::windSpeed(cb).at(s);
                    if (!(ocean && ocean[k])) c = mix(c, Rgb{120, 100, 70}, 0.18);
                    break;
                }
                case MapTab::Rivers: {
                    if (ocean && ocean[k]) { c = {200, 220, 236}; break; }
                    if (lake && lake[k] >= 0) {
                        const auto* hf = w.hydro.get();
                        bool closed = hf && lake[k] < static_cast<int>(hf->lakes.size()) && hf->lakes[static_cast<size_t>(lake[k])].regime != kLrOutflow;
                        c = closed ? Rgb{86, 160, 160} : Rgb{58, 120, 200};
                        break;
                    }
                    const uint8_t* wet = w.u8(LayerId::Wetland);
                    if (o.riverView == 1) {
                        const float* q = w.f32(LayerId::Discharge);
                        if (!q) break;
                        double v = std::log10(std::max(0.1, static_cast<double>(q[k])));
                        c = ramps::sequentialBlue(cb).at(std::clamp((v + 0.5) / 5.0, 0.0, 1.0));
                    } else {
                        const int32_t* b = w.i32(LayerId::BasinId);
                        if (!b) break;
                        c = categoricalColor(b[k] < 0 ? 0 : b[k], cb, 0.28, 0.93);
                        const auto* hf = w.hydro.get();
                        if (hf && b[k] >= 0 && b[k] < static_cast<int>(hf->basins.size()) && hf->basins[static_cast<size_t>(b[k])].outlet != kBoOcean)
                            c = mix(c, Rgb{230, 200, 150}, 0.45);  // endorheic basins in warm tones
                    }
                    if (wet && wet[k]) c = mix(c, catColor(LayerId::Wetland, wet[k]), 0.6);
                    c = shadeAt(k, c, 0.5);
                    break;
                }
                case MapTab::Koppen: {
                    const uint8_t* v = w.u8(LayerId::Koppen);
                    if (!v) break;
                    if (ocean && ocean[k]) { c = {222, 232, 242}; break; }
                    c = shadeAt(k, catColor(LayerId::Koppen, v[k]), 0.35);
                    break;
                }
                case MapTab::Biomes: {
                    if (o.biomeView == 1) {
                        const uint8_t* b = w.u8(LayerId::AltBelt);
                        if (!b) break;
                        if (ocean && ocean[k]) { c = {222, 232, 242}; break; }
                        c = shadeAt(k, catColor(LayerId::AltBelt, b[k]), 0.5);
                        break;
                    }
                    const uint8_t* b = w.u8(LayerId::Biome);
                    if (!b) break;
                    c = catColor(LayerId::Biome, b[k]);
                    if (o.ecotones) {
                        const uint8_t* b2 = w.u8(LayerId::Biome2);
                        const float* et = w.f32(LayerId::Ecotone);
                        // Ecotones are drawn as a dither between the two biomes, never as an interpolated colour.
                        if (b2 && et && et[k] > 0.05f && b2[k] != b[k]) {
                            double t = et[k] * 0.5;
                            uint32_t hsh = static_cast<uint32_t>(i * 73856093u) ^ static_cast<uint32_t>(j * 19349663u);
                            if ((hsh % 1000) / 1000.0 < t) c = catColor(LayerId::Biome, b2[k]);
                        }
                    }
                    if (!(ocean && ocean[k])) c = shadeAt(k, c, 0.45);
                    break;
                }
                case MapTab::Volcanism: {
                    if (o.volcanoView == 1) {
                        const float* gt = w.f32(LayerId::Geothermal);
                        if (!gt) break;
                        c = ramps::probability(cb).at(gt[k]);
                        if (ocean && ocean[k]) c = mix(c, Rgb{190, 210, 230}, 0.6);
                    } else {
                        LayerId id = o.volcanoView == 2 ? LayerId::Seismic : LayerId::VolcanicZone;
                        const uint8_t* z = w.u8(id);
                        if (!z) break;
                        c = ramps::zone05(cb).at(z[k] + 0.5);
                        if (ocean && ocean[k]) c = mix(c, Rgb{150, 180, 215}, 0.45);
                    }
                    c = shadeAt(k, c, 0.4);
                    break;
                }
                case MapTab::SnowIce: {
                    const uint8_t* it = w.u8(LayerId::IceType);
                    const uint8_t* sm = w.u8(LayerId::SnowMonths);
                    const uint16_t* si = w.u16(LayerId::SeaIceMonths);
                    const uint16_t* swe = w.u16(LayerId::SweMonthly);
                    if (!it) break;
                    if (ocean && ocean[k]) {
                        c = {70, 110, 160};
                        if (it[k] == kIceShelf) c = {170, 215, 235};
                        else if (si) {
                            uint16_t bits = si[k];
                            if (month >= 0) { if (bits & (1u << month)) c = {225, 236, 245}; }
                            else if (bits == 0x0fff) c = {235, 242, 250};
                            else if (bits) c = mix(Rgb{120, 160, 200}, Rgb{225, 236, 245}, std::popcount(static_cast<unsigned>(bits)) / 12.0);
                        }
                        break;
                    }
                    if (it[k] == kIceSheet) { c = {250, 252, 255}; break; }
                    if (it[k] == kIceGlacier) { c = {200, 230, 245}; break; }
                    c = {196, 186, 160};
                    if (lake && lake[k] >= 0) c = {120, 160, 200};
                    if (month >= 0 && swe) {
                        double s = swe[month * g.size() + k];
                        if (s > 5) c = mix(Rgb{215, 225, 235}, Rgb{255, 255, 255}, std::min(1.0, s / 200.0));
                    } else if (sm && sm[k] > 0) {
                        c = mix(c, Rgb{235, 242, 250}, sm[k] / 12.0);
                    }
                    break;
                }
                case MapTab::Coasts: {
                    const uint8_t* ct = w.u8(LayerId::CoastType);
                    if (!ocean) break;
                    if (ocean[k]) {
                        const float* e = w.f32(LayerId::Elevation);
                        c = (e && e[k] - w.seaLevel() > -200) ? Rgb{190, 222, 240} : Rgb{150, 188, 218};
                    } else {
                        c = (lake && lake[k] >= 0) ? Rgb{170, 200, 225} : Rgb{232, 226, 210};
                    }
                    if (ct) {
                        int code = ct[k];
                        if (!code) {
                            // Thicken coasts by one cell so that they stay visible at world scale.
                            int64_t nb[8];
                            g.neighbors8(k, nb);
                            for (int d = 0; d < 8 && !code; d += 2) code = ct[nb[d]];
                        }
                        if (code) c = catColor(LayerId::CoastType, code);
                    }
                    break;
                }
                default: break;
            }
            line[i] = c.argb();
        }
    };
    if (pool) pool->forRows(H, rowFn);
    else for (int j = 0; j < H; ++j) rowFn(j);
    (void)isWater;
    return img;
}

Legend legendFor(const WorldData& w, MapTab tab, const ViewOptions& o) {
    Legend L;
    const bool cb = o.colorblind;
    auto cats = [&](LayerId id, const std::set<int>* present = nullptr) {
        for (auto& c : categoriesOf(id)) {
            if (present && !present->count(c.code)) continue;
            L.items.push_back({Rgb::hex(c.rgb), c.nameRu});
        }
    };
    auto presentCodes = [&](LayerId id) {
        std::set<int> s;
        if (const uint8_t* v = w.u8(id))
            for (int64_t k = 0; k < w.grid.size(); ++k) s.insert(v[k]);
        return s;
    };
    std::string monthName = o.month < 0 ? "год" : std::string("месяц ") + std::to_string(o.month + 1);
    switch (tab) {
        case MapTab::Physical:
        case MapTab::Elevation: {
            L.title = tab == MapTab::Physical ? "Высоты и глубины (гипсометрия)" : "Высота / глубина относительно уровня моря";
            L.units = "м";
            L.continuous = true;
            std::vector<ColorStop> s;
            for (auto& st : ramps::bathymetry(cb).stops()) s.push_back(st);
            for (auto& st : ramps::landHypsometric(cb).stops()) if (st.value > 0) s.push_back(st);
            L.ramp = ColorRamp(s);
            L.lo = -8000;
            L.hi = 6500;
            L.ticks = {{-8000, "−8000"}, {-4000, "−4000"}, {-200, "−200"}, {0, "0"}, {500, "500"}, {2000, "2000"}, {5000, "5000"}};
            L.notes.push_back("Отрицательные значения — глубины ниже уровня моря.");
            if (tab == MapTab::Physical) L.notes.push_back("Теневой рельеф: освещение с северо-запада, вертикальное преувеличение.");
            if (tab == MapTab::Elevation && o.contourInterval > 0)
                L.notes.push_back("Изолинии через " + formatNumber(o.contourInterval) + " м, утолщённые — каждая пятая.");
            if (w.has(LayerId::OceanMask)) L.notes.push_back("Уровень моря: " + formatNumber(std::round(w.seaLevel())) + " м от нуля проекта.");
            L.overlayNote = "Реки, озёра и береговая линия — векторные слои поверх растра.";
            break;
        }
        case MapTab::Plates:
            if (o.platesView == 2) {
                L.title = "Возраст океанической коры (условный)";
                L.units = "млн лет";
                L.continuous = true;
                L.ramp = ColorRamp({{0, Rgb::hex(0xd7301f)}, {20, Rgb::hex(0xfc8d59)}, {60, Rgb::hex(0xfdcc8a)},
                                    {100, Rgb::hex(0x91bfdb)}, {180, Rgb::hex(0x4575b4)}});
                L.lo = 0;
                L.hi = 180;
                L.ticks = {{0, "0"}, {50, "50"}, {100, "100"}, {180, "180"}};
                L.notes.push_back("Возраст = расстояние до хребта / полускорость спрединга; статическая реконструкция.");
                break;
            }
            L.title = o.platesView == 1 ? "Классы границ по типам коры" : "Литосферные плиты и типы границ";
            if (o.platesView == 1) cats(LayerId::BoundaryClass);
            else {
                cats(LayerId::BoundaryType);
                L.notes.push_back("Заливка — цвет плиты; приглушённый тон — океаническая кора, светлый — континентальная.");
            }
            L.notes.push_back("Стрелки — направление и условная скорость движения плиты (см/год).");
            L.notes.push_back("Континент ≠ плита: одна плита несёт и океаническую, и континентальную кору.");
            break;
        case MapTab::Landforms: {
            LayerId id = o.landformView == 1 ? LayerId::Genesis : (o.landformView == 2 ? LayerId::Lithology : LayerId::Landform);
            L.title = layerInfo(id).nameRu;
            auto s = presentCodes(id);
            cats(id, &s);
            break;
        }
        case MapTab::Temperature:
            L.title = "Температура воздуха у поверхности, " + monthName;
            L.units = "°C";
            L.continuous = true;
            L.ramp = ramps::temperature(cb);
            L.lo = -50;
            L.hi = 45;
            L.ticks = {{-40, "−40"}, {-20, "−20"}, {0, "0"}, {10, "10"}, {20, "20"}, {30, "30"}, {40, "40"}};
            L.notes.push_back("Над океаном — температура приводного воздуха с поправкой течений (упрощённая модель).");
            break;
        case MapTab::Precipitation:
            L.title = "Осадки, " + monthName;
            L.units = o.month < 0 ? "мм/год" : "мм/мес";
            L.continuous = true;
            L.ramp = o.month < 0 ? ramps::precipitationAnnual(cb) : ramps::precipitationMonthly(cb);
            L.lo = 0;
            L.hi = o.month < 0 ? 4500 : 450;
            if (o.month < 0) L.ticks = {{0, "0"}, {250, "250"}, {500, "500"}, {1000, "1000"}, {2000, "2000"}, {4000, "4000"}};
            else L.ticks = {{0, "0"}, {25, "25"}, {50, "50"}, {100, "100"}, {200, "200"}, {400, "400"}};
            if (const float* p = w.f32(LayerId::PrecAnnual)) {
                double mx = 0, mn = 1e9;
                for (int64_t k = 0; k < w.grid.size(); ++k) { mx = std::max(mx, (double)p[k]); mn = std::min(mn, (double)p[k]); }
                L.notes.push_back("Экстремумы годовой суммы: " + formatNumber(std::round(mn)) + " … " + formatNumber(std::round(mx)) + " мм.");
            }
            break;
        case MapTab::Humidity:
            L.continuous = true;
            if (o.humidity == HumidityMetric::RelHumidity) {
                L.title = "Относительная влажность воздуха (аппроксимация), " + monthName;
                L.units = "%";
                L.ramp = ramps::relativeHumidity(cb);
                L.lo = 0;
                L.hi = 100;
                L.ticks = {{0, "0"}, {25, "25"}, {50, "50"}, {75, "75"}, {100, "100"}};
                L.notes.push_back("Отношение влагосодержания столба к насыщающему; оценка модели переноса влаги.");
            } else if (o.humidity == HumidityMetric::SoilMoisture) {
                L.title = "Запас воды в почве, " + monthName;
                L.units = "мм";
                L.ramp = ramps::soilMoisture(cb);
                L.lo = 0;
                L.hi = 250;
                L.ticks = {{0, "0"}, {50, "50"}, {100, "100"}, {150, "150"}, {250, "250"}};
                L.notes.push_back("Модель «ведра» с ёмкостью по рельефу; не процент влажности воздуха.");
            } else {
                L.title = "Индекс аридности P/PET, " + monthName;
                L.units = "безразмерный";
                L.continuous = false;
                L.items = {{ramps::aridity(cb).at(0.01), "< 0,05 — гиперарид"},
                           {ramps::aridity(cb).at(0.1), "0,05–0,2 — арид"},
                           {ramps::aridity(cb).at(0.3), "0,2–0,5 — семиарид"},
                           {ramps::aridity(cb).at(0.6), "0,5–0,65 — сухой субгумид"},
                           {ramps::aridity(cb).at(1.0), "0,65–1,5 — гумид"},
                           {ramps::aridity(cb).at(2.0), "> 1,5 — избыточно влажно"}};
                L.notes.push_back("Классы ЮНЕП. Индекс — отношение осадков к испаряемости, не процент относительной влажности.");
            }
            break;
        case MapTab::Wind:
            L.title = "Приземный ветер, " + monthName;
            L.units = "м/с";
            L.continuous = true;
            L.ramp = ramps::windSpeed(cb);
            L.lo = 0;
            L.hi = 13;
            L.ticks = {{0, "0"}, {3, "3"}, {6, "6"}, {9, "9"}, {12, "12"}};
            L.notes.push_back("Стрелки — преобладающее направление. Пояса: пассаты, западный перенос, полярные восточные; муссонные аномалии от контраста суши и моря.");
            L.notes.push_back("Параметризация циркуляции, а не численная метеомодель.");
            break;
        case MapTab::Rivers:
            if (o.riverView == 1) {
                L.title = "Среднегодовой расход";
                L.units = "м³/с (лог. шкала)";
                L.continuous = true;
                L.ramp = ramps::sequentialBlue(cb);
                L.lo = 0;
                L.hi = 1;
                L.ticks = {{0, "0,3"}, {0.3, "10"}, {0.5, "100"}, {0.7, "1000"}, {0.9, "10⁴"}};
            } else {
                L.title = "Речные бассейны";
                L.items = {{{58, 120, 200}, "Сточное озеро"}, {{86, 160, 160}, "Бессточное / временное озеро"},
                           {mix(Rgb{200, 200, 200}, Rgb{230, 200, 150}, 0.45), "Бессточный (эндорейный) бассейн — тёплый тон"}};
                for (auto& c : categoriesOf(LayerId::Wetland)) if (c.code) L.items.push_back({Rgb::hex(c.rgb), c.nameRu});
            }
            L.notes.push_back("Линии: толщина — порядок Стралера; пунктир — сезонные и пересыхающие русла; бирюзовые — дельтовые рукава.");
            break;
        case MapTab::Koppen: {
            L.title = "Климатические классы Кёппена–Гейгера";
            auto s = presentCodes(LayerId::Koppen);
            s.erase(0);
            cats(LayerId::Koppen, &s);
            L.notes.push_back("Вычислены по месячным T и P формальными правилами (Beck et al., 2018/2023).");
            break;
        }
        case MapTab::Biomes: {
            LayerId id = o.biomeView == 1 ? LayerId::AltBelt : LayerId::Biome;
            L.title = o.biomeView == 1 ? "Высотные пояса" : "Биомы";
            auto s = presentCodes(id);
            cats(id, &s);
            if (o.biomeView == 0 && o.ecotones) L.notes.push_back("Экотоны показаны точечным смешением двух соседних биомов.");
            if (o.biomeView == 1) L.notes.push_back("Пояса определяются климатом конкретной широты и экспозиции, а не фиксированной высотой.");
            break;
        }
        case MapTab::Volcanism:
            L.continuous = o.volcanoView == 1;
            if (o.volcanoView == 1) {
                L.title = "Геотермальный потенциал (вероятностный)";
                L.units = "0–1";
                L.ramp = ramps::probability(cb);
                L.lo = 0;
                L.hi = 1;
                L.ticks = {{0, "0"}, {0.5, "0,5"}, {1, "1"}};
            } else {
                L.title = o.volcanoView == 2 ? "Условная сейсмотектоническая активность" : "Потенциал вулканической активности";
                L.units = "условная шкала 0–5";
                const char* labels[] = {"0 — стабильная область", "1 — слабый", "2 — умеренный", "3 — повышенный", "4 — высокий", "5 — очень высокий"};
                for (int z = 0; z <= 5; ++z) L.items.push_back({ramps::zone05(cb).at(z + 0.5), labels[z]});
            }
            L.notes.push_back("Шкала условная: не частота извержений и не прогноз риска или землетрясений.");
            L.notes.push_back("▲ активный, △ спящий, · потухший вулкан; синий контур — подводный; ★ горячая точка; ● гейзер, ○ термальный источник.");
            break;
        case MapTab::SnowIce:
            L.title = "Снег и лёд, " + monthName;
            L.items = {{{250, 252, 255}, "Ледяной щит"}, {{200, 230, 245}, "Горный ледник"}, {{170, 215, 235}, "Шельфовый ледник"},
                       {{235, 242, 250}, o.month < 0 ? "Многолетний морской лёд" : "Морской лёд"},
                       {{150, 185, 215}, "Сезонный морской лёд"}, {{230, 238, 246}, "Снежный покров (больше месяцев — светлее)"}};
            L.notes.push_back("Снег, морской лёд и ледяные щиты хранятся раздельно.");
            break;
        case MapTab::Coasts: {
            L.title = "Типы берегов";
            auto s = presentCodes(LayerId::CoastType);
            s.erase(0);
            cats(LayerId::CoastType, &s);
            L.notes.push_back("Классы назначаются только при выполнимых геоклиматических условиях; на сетке 2048×1024 узкие фьорды и рукава субпиксельны.");
            break;
        }
        default: break;
    }
    return L;
}

}  // namespace mapgen
