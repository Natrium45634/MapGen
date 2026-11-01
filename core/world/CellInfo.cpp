#include "core/world/CellInfo.h"

#include <bit>
#include <cmath>
#include <cstdio>

#include "core/world/Categories.h"

namespace mapgen {

const char* const kNotComputedRu = "не рассчитано";

namespace {

std::string num(double v, int prec) {
    if (!std::isfinite(v)) return kNotComputedRu;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", prec, v);
    std::string s(buf);
    for (auto& c : s)
        if (c == '.') c = ',';
    if (s == "-0") s = "0";
    return s;
}

const char* monthRu(int m) {
    static const char* n[] = {"янв", "фев", "мар", "апр", "май", "июн", "июл", "авг", "сен", "окт", "ноя", "дек"};
    return (m >= 0 && m < 12) ? n[m] : "год";
}

}  // namespace

std::vector<CellField> describeCell(const WorldData& w, int64_t k, int month) {
    std::vector<CellField> out;
    if (!w.grid.valid() || k < 0 || k >= w.grid.size()) return out;
    const SphereGrid& g = w.grid;
    auto add = [&](std::string key, std::string label, std::string value, LayerId layer, bool avail = true, double v = 0) {
        out.push_back({std::move(key), std::move(label), avail ? std::move(value) : std::string(kNotComputedRu), layer, avail, v});
    };
    int i = g.col(k), j = g.row(k);
    double lat = g.latDeg(j), lon = g.lonDeg(i);
    add("coords", "Координаты",
        num(std::fabs(lat), 2) + "° " + (lat >= 0 ? "с.ш." : "ю.ш.") + ", " + num(std::fabs(lon), 2) + "° " + (lon >= 0 ? "в.д." : "з.д."),
        LayerId::Count, true, lat);

    const bool haveOcean = w.has(LayerId::OceanMask);
    const bool ocean = w.isOcean(k);
    const int32_t* lakeL = w.i32(LayerId::LakeId);
    const bool lake = lakeL && lakeL[k] >= 0;
    const uint8_t* iceT = w.u8(LayerId::IceType);
    const uint16_t* seaIce = w.u16(LayerId::SeaIceMonths);

    std::string surface = kNotComputedRu;
    if (haveOcean) {
        surface = ocean ? "Океан / море" : "Суша";
        if (lake) surface = "Озеро";
        if (iceT && iceT[k] == kIceSheet) surface = "Ледяной щит";
        if (iceT && iceT[k] == kIceGlacier) surface = "Горный ледник";
        if (iceT && iceT[k] == kIceShelf) surface = "Шельфовый ледник";
        if (ocean && seaIce && iceT && iceT[k] != kIceShelf) {
            uint16_t b = seaIce[k];
            if (month >= 0 && (b & (1u << month))) surface = "Море, покрытое льдом";
            else if (month < 0 && b == 0x0fff) surface = "Многолетний морской лёд";
        }
    }
    add("surface", "Тип поверхности", surface, LayerId::OceanMask, haveOcean);

    if (w.has(LayerId::Elevation) && haveOcean) {
        double z = w.surfaceElevation(k) - w.seaLevel();
        if (ocean) add("depth", "Глубина", num(-z, 0) + " м", LayerId::Elevation, true, z);
        else {
            add("height", "Абсолютная высота", num(z, 0) + " м", LayerId::Elevation, true, z);
            const float* it = w.f32(LayerId::IceThickness);
            if (it && iceT && (iceT[k] == kIceSheet || iceT[k] == kIceGlacier))
                add("ice", "Толщина льда", num(it[k], 0) + " м (ложе " + num(w.f32(LayerId::Elevation)[k] - w.seaLevel(), 0) + " м)",
                    LayerId::IceThickness, true, it[k]);
        }
        if (lake && w.hydro && lakeL[k] < static_cast<int>(w.hydro->lakes.size())) {
            const Lake& L = w.hydro->lakes[static_cast<size_t>(lakeL[k])];
            const float* hs = w.f32(LayerId::HydroSurface);
            double depth = hs ? hs[k] - w.f32(LayerId::Elevation)[k] : 0;
            add("lakeDepth", "Глубина озера", num(depth, 0) + " м (уровень " + num(L.levelM - w.seaLevel(), 0) + " м)",
                LayerId::HydroSurface, true, depth);
        }
    } else {
        add("height", "Высота / глубина", "", LayerId::Elevation, false);
    }

    if (const uint16_t* pid = w.u16(LayerId::PlateId)) {
        std::string s = "№" + std::to_string(pid[k]);
        if (const uint8_t* ct = w.u8(LayerId::CrustType)) s += ", " + categoryName(LayerId::CrustType, ct[k]);
        add("plate", "Плита", s, LayerId::PlateId, true, pid[k]);
        const uint8_t* side = w.u8(LayerId::TectonicSide);
        const float* db = w.f32(LayerId::DistBoundary);
        std::string b = side ? categoryName(LayerId::TectonicSide, side[k]) : std::string(kNotComputedRu);
        if (db) b += "; до границы " + num(db[k], 0) + " км";
        add("boundary", "Граница / положение", b, LayerId::TectonicSide, side != nullptr);
    } else {
        add("plate", "Плита", "", LayerId::PlateId, false);
    }

    if (const uint8_t* lf = w.u8(LayerId::Landform)) {
        std::string s = categoryName(LayerId::Landform, lf[k]);
        if (const uint8_t* gn = w.u8(LayerId::Genesis)) s += " (генезис: " + categoryName(LayerId::Genesis, gn[k]) + ")";
        add("landform", "Геоморфология", s, LayerId::Landform, true, lf[k]);
    } else {
        add("landform", "Геоморфология", "", LayerId::Landform, false);
    }

    if (const float* tm = w.f32(LayerId::TempMean)) {
        std::string s = num(tm[k], 1) + " °C за год";
        if (const int16_t* tmon = w.i16(LayerId::TempMonthly)) {
            if (month >= 0) s += ", " + num(tmon[month * g.size() + k] * 0.01, 1) + " °C (" + monthRu(month) + ")";
            if (const float* mn = w.f32(LayerId::TempMin)) s += "; мин. " + num(mn[k], 1) + ", макс. " + num(w.f32(LayerId::TempMax)[k], 1);
        }
        add("temp", "Температура", s, LayerId::TempMean, true, tm[k]);
    } else {
        add("temp", "Температура", "", LayerId::TempMean, false);
    }
    if (const float* pa = w.f32(LayerId::PrecAnnual)) {
        std::string s = num(pa[k], 0) + " мм/год";
        if (month >= 0)
            if (const uint16_t* pm = w.u16(LayerId::PrecMonthly)) s += ", " + num(pm[month * g.size() + k] * 0.1, 0) + " мм (" + monthRu(month) + ")";
        add("prec", "Осадки", s, LayerId::PrecAnnual, true, pa[k]);
    } else {
        add("prec", "Осадки", "", LayerId::PrecAnnual, false);
    }
    if (const float* ai = w.f32(LayerId::Aridity)) {
        std::string s;
        if (!ocean) s = "P/PET = " + num(ai[k], 2);
        if (w.has(LayerId::RelHumidity)) {
            double rh = 0;
            if (month >= 0) rh = w.sample(LayerId::RelHumidity, k, month);
            else { for (int m = 0; m < 12; ++m) rh += w.sample(LayerId::RelHumidity, k, m); rh /= 12; }
            s += std::string(s.empty() ? "" : "; ") + "RH ≈ " + num(rh, 0) + " %";
        }
        if (!ocean)
            if (const float* sm = w.f32(LayerId::SoilMean)) s += "; почва " + num(sm[k], 0) + " мм";
        add("humidity", "Влажность", s, LayerId::Aridity, true, ai[k]);
    } else {
        add("humidity", "Влажность", "", LayerId::Aridity, false);
    }
    if (w.has(LayerId::WindU) && month >= 0) {
        double u = w.sample(LayerId::WindU, k, month), v = w.sample(LayerId::WindV, k, month);
        double s = std::hypot(u, v);
        double from = std::fmod(std::atan2(-u, -v) * kRadToDeg + 360.0, 360.0);
        static const char* dirs[] = {"С", "СВ", "В", "ЮВ", "Ю", "ЮЗ", "З", "СЗ"};
        add("wind", "Ветер (" + std::string(monthRu(month)) + ")", num(s, 1) + " м/с, с " + dirs[static_cast<int>((from + 22.5) / 45.0) % 8], LayerId::WindU, true, s);
    }
    if (const uint8_t* kg = w.u8(LayerId::Koppen)) {
        add("koppen", "Климат (Кёппен)", ocean ? std::string("— (океан)") : categoryName(LayerId::Koppen, kg[k]), LayerId::Koppen, true, kg[k]);
    } else {
        add("koppen", "Климат (Кёппен)", "", LayerId::Koppen, false);
    }
    if (const uint8_t* bi = w.u8(LayerId::Biome)) {
        std::string s = ocean ? std::string("— (океан, наземный биом не определяется)") : categoryName(LayerId::Biome, bi[k]);
        if (!ocean)
            if (const uint8_t* ab = w.u8(LayerId::AltBelt)) if (ab[k] > kBeltLowland) s += "; пояс: " + categoryName(LayerId::AltBelt, ab[k]);
        add("biome", "Биом / пояс", s, LayerId::Biome, true, bi[k]);
    } else {
        add("biome", "Биом / пояс", "", LayerId::Biome, false);
    }
    if (iceT) {
        std::string s;
        const uint8_t* sm = w.u8(LayerId::SnowMonths);
        if (ocean) {
            int n = seaIce ? std::popcount(static_cast<unsigned>(seaIce[k])) : 0;
            s = n == 0 ? "морского льда нет" : (n == 12 ? "многолетний морской лёд" : "морской лёд " + std::to_string(n) + " мес.");
        } else {
            s = sm ? "снег " + std::to_string(sm[k]) + " мес." : "";
            if (iceT[k]) s += "; " + categoryName(LayerId::IceType, iceT[k]);
        }
        add("snow", "Снег и лёд", s, LayerId::IceType, true, iceT[k]);
    } else {
        add("snow", "Снег и лёд", "", LayerId::IceType, false);
    }
    if (const int32_t* bas = w.i32(LayerId::BasinId)) {
        std::string s;
        if (ocean) s = "—";
        else {
            s = bas[k] >= 0 ? "бассейн №" + std::to_string(bas[k]) : "вне бассейнов";
            if (w.hydro && bas[k] >= 0 && bas[k] < static_cast<int>(w.hydro->basins.size()))
                s += w.hydro->basins[static_cast<size_t>(bas[k])].outlet == kBoOcean ? " (сток в океан)" : " (бессточный)";
            const uint8_t* rc = w.u8(LayerId::RiverClass);
            const float* q = w.f32(LayerId::Discharge);
            const uint8_t* ro = w.u8(LayerId::RiverOrder);
            if (rc && rc[k] && q) s += "; река: " + categoryName(LayerId::RiverClass, rc[k]) + ", " + num(q[k], q[k] < 10 ? 1 : 0) + " м³/с" +
                                       (ro ? ", порядок " + std::to_string(ro[k]) : "");
            if (lake && w.hydro && lakeL[k] < static_cast<int>(w.hydro->lakes.size())) {
                const Lake& L = w.hydro->lakes[static_cast<size_t>(lakeL[k])];
                static const char* reg[] = {"сточное", "бессточное", "временное"};
                s += "; озеро №" + std::to_string(L.id) + " (" + reg[std::min<int>(L.regime, 2)] + (L.saline ? ", солёное" : "") + ")";
            }
            if (const uint8_t* wt = w.u8(LayerId::Wetland)) if (wt[k]) s += "; " + categoryName(LayerId::Wetland, wt[k]);
        }
        add("hydro", "Бассейн / река / озеро", s, LayerId::Discharge, true);
    } else {
        add("hydro", "Бассейн / река / озеро", "", LayerId::Discharge, false);
    }
    if (const uint8_t* vz = w.u8(LayerId::VolcanicZone)) {
        std::string s = "зона " + std::to_string(vz[k]) + "/5 (усл.)";
        if (const float* gt = w.f32(LayerId::Geothermal)) s += "; геотермия " + num(gt[k], 2);
        if (const uint8_t* se = w.u8(LayerId::Seismic)) s += "; сейсмотект. " + std::to_string(se[k]) + "/5";
        if (w.geomorph) {
            double best = 1e18;
            const Volcano* nv = nullptr;
            for (auto& v : w.geomorph->volcanoes) {
                double d = g.radiusKm() * angleBetween(g.unit(k), fromLatLon(v.lat * kDegToRad, v.lon * kDegToRad));
                if (d < best) { best = d; nv = &v; }
            }
            static const char* gen[] = {"дуга окраины", "островная дуга", "рифт", "хребет", "горячая точка (океан)", "горячая точка (материк)", "задуговый"};
            if (nv && best < 300) s += "; вулкан " + num(best, 0) + " км (" + gen[std::min<int>(nv->genesis, 6)] + ")";
        }
        add("volcanic", "Вулканизм / источник тепла", s, LayerId::VolcanicZone, true, vz[k]);
    } else {
        add("volcanic", "Вулканизм / источник тепла", "", LayerId::VolcanicZone, false);
    }
    if (const uint8_t* ct = w.u8(LayerId::CoastType)) {
        add("coast", "Береговая форма", ct[k] ? categoryName(LayerId::CoastType, ct[k]) : std::string("— (не берег)"), LayerId::CoastType, true, ct[k]);
    } else {
        add("coast", "Береговая форма", "", LayerId::CoastType, false);
    }
    return out;
}

std::string describeCellText(const WorldData& w, int64_t cell, int month) {
    std::string s;
    for (auto& f : describeCell(w, cell, month)) s += f.labelRu + ": " + f.valueRu + "\n";
    return s;
}

}  // namespace mapgen
