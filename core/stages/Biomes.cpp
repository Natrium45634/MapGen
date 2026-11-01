// Stage 6: Köppen–Geiger climate classes, biomes, altitudinal belts, ecotones, coast types, karst.
//
// Köppen–Geiger: formal rules on monthly T and P (Beck et al. 2018, Peel et al. 2007; C/D boundary 0 °C,
// summer = warmer of AMJJAS / ONDJFM).
// Biomes: an ordered rule table (first matching rule wins) over temperature, water availability
// (P/PET), seasonality, growing degree days, altitudinal belt, snow/ice, wetness and coastal salinity /
// tides. The same absolute height maps to different belts at the equator and near the poles because the
// belts follow the local warmest-month temperature (treeline ~10 °C), not a fixed elevation.
// Ecotones: the classification is re-evaluated with perturbed climate (±1.5 °C, P/PET ×1.25/0.8); if it
// changes, the cell is an ecotone towards the alternative biome (display: dither, categories stay stable).
// Coasts: classes are assigned only where their geomorphic / climatic conditions hold.

#include <algorithm>
#include <cmath>
#include <functional>

#include "core/stages/Stage.h"
#include "core/stages/StageUtil.h"
#include "core/world/Categories.h"

namespace mapgen {

namespace {

constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

struct ClimateSummary {
    double mat, map, tcold, thot, gdd5, ai;
    int tmon10, dryMonths;
    double pdry, psdry, pwdry, pswet, pwwet, pSummerShare;
    double seasonality;
};

ClimateSummary summarize(const double* T, const double* P, const double* E, bool northern) {
    ClimateSummary s{};
    s.tcold = 1e9;
    s.thot = -1e9;
    s.pdry = 1e9;
    double pet = 0;
    for (int m = 0; m < 12; ++m) {
        s.mat += T[m] / 12;
        s.map += P[m];
        pet += E[m];
        s.tcold = std::min(s.tcold, T[m]);
        s.thot = std::max(s.thot, T[m]);
        s.pdry = std::min(s.pdry, P[m]);
        if (T[m] > 10) s.tmon10++;
        s.gdd5 += std::max(0.0, T[m] - 5.0) * kDays[m];
        if (P[m] < 0.5 * E[m]) s.dryMonths++;
    }
    s.ai = pet > 1 ? s.map / pet : 10.0;
    // Summer half-year = the warmer of AMJJAS and ONDJFM.
    double tA = 0, tO = 0;
    for (int m = 3; m <= 8; ++m) tA += T[m];
    for (int m : {9, 10, 11, 0, 1, 2}) tO += T[m];
    bool amjjasSummer = tA >= tO;
    (void)northern;
    s.psdry = 1e9; s.pwdry = 1e9; s.pswet = 0; s.pwwet = 0;
    double pSummer = 0;
    for (int m = 0; m < 12; ++m) {
        bool summer = (m >= 3 && m <= 8) == amjjasSummer;
        if (summer) { s.psdry = std::min(s.psdry, P[m]); s.pswet = std::max(s.pswet, P[m]); pSummer += P[m]; }
        else { s.pwdry = std::min(s.pwdry, P[m]); s.pwwet = std::max(s.pwwet, P[m]); }
    }
    s.pSummerShare = s.map > 0 ? pSummer / s.map : 0.5;
    double si = 0;
    for (int m = 0; m < 12; ++m) si += std::fabs(P[m] - s.map / 12);
    s.seasonality = s.map > 1 ? std::min(1.0, si / s.map / 1.83) : 0;
    return s;
}

uint8_t koppen(const ClimateSummary& s) {
    if (s.thot < 10) return s.thot > 0 ? kKgET : kKgEF;
    double pth = 2 * s.mat + (s.pSummerShare >= 0.7 ? 28 : (s.pSummerShare <= 0.3 ? 0 : 14));
    if (s.map < 10 * pth) {
        bool hot = s.mat >= 18;
        if (s.map < 5 * pth) return hot ? kKgBWh : kKgBWk;
        return hot ? kKgBSh : kKgBSk;
    }
    if (s.tcold >= 18) {
        if (s.pdry >= 60) return kKgAf;
        if (s.pdry >= 100 - s.map / 25) return kKgAm;
        return kKgAw;
    }
    bool summerDry = s.psdry < 40 && s.psdry < s.pwwet / 3;
    bool winterDry = s.pwdry < s.pswet / 10;
    char second = summerDry ? 's' : (winterDry ? 'w' : 'f');
    char third;
    if (s.thot >= 22) third = 'a';
    else if (s.tmon10 >= 4) third = 'b';
    else if (s.tcold <= -38) third = 'd';
    else third = 'c';
    if (s.tcold > 0) {
        if (third == 'd') third = 'c';
        if (second == 's') return third == 'a' ? kKgCsa : (third == 'b' ? kKgCsb : kKgCsc);
        if (second == 'w') return third == 'a' ? kKgCwa : (third == 'b' ? kKgCwb : kKgCwc);
        return third == 'a' ? kKgCfa : (third == 'b' ? kKgCfb : kKgCfc);
    }
    if (second == 's') return third == 'a' ? kKgDsa : (third == 'b' ? kKgDsb : (third == 'c' ? kKgDsc : kKgDsd));
    if (second == 'w') return third == 'a' ? kKgDwa : (third == 'b' ? kKgDwb : (third == 'c' ? kKgDwc : kKgDwd));
    return third == 'a' ? kKgDfa : (third == 'b' ? kKgDfb : (third == 'c' ? kKgDfc : kKgDfd));
}

struct BiomeInput {
    ClimateSummary c;
    bool mountain;     // relief context for belts
    uint8_t belt;
    uint8_t wetland;
    bool mangroveSite;
    bool summerDry;
};

// Ordered rule table. `soft` widens thresholds of rare biomes (biome.extendRare).
struct BiomeRule {
    const char* descriptionRu;
    std::function<bool(const BiomeInput&, double soft)> when;
    uint8_t biome;
};

const std::vector<BiomeRule>& biomeRules() {
    static const std::vector<BiomeRule> rules = {
        {"Нивальный пояс: самый тёплый месяц < 1,5 °C в горах", [](const BiomeInput& b, double) { return b.belt == kBeltNival; }, kBiomeNival},
        {"Мангры: приливный защищённый низкий берег, без заморозков (холодный месяц ≥ 15 °C)",
         [](const BiomeInput& b, double soft) { return b.mangroveSite && b.c.tcold >= 15 - 3 * soft && b.c.mat >= 20 - 2 * soft; }, kBiomeMangrove},
        {"Солончаки: бессточные понижения сухого климата", [](const BiomeInput& b, double) { return b.wetland == kWetSaltFlat; }, kBiomeSaltFlat},
        {"Торфяники: прохладный климат с избытком влаги на плоском рельефе", [](const BiomeInput& b, double) { return b.wetland == kWetPeat; }, kBiomePeatland},
        {"Пресноводные болота: тёплый влажный плоский рельеф у вод", [](const BiomeInput& b, double) { return b.wetland == kWetMarsh; }, kBiomeFreshwaterMarsh},
        {"Сезонно затапливаемые поймы крупных рек", [](const BiomeInput& b, double) { return b.wetland == kWetFloodplain; }, kBiomeFloodplain},
        {"Альпийские луга / горная тундра: выше верхней границы леса", [](const BiomeInput& b, double) { return b.belt == kBeltAlpine; }, kBiomeAlpine},
        {"Полярная пустыня: самый тёплый месяц < 0 °C без ледника", [](const BiomeInput& b, double) { return b.c.thot < 0; }, kBiomePolarDesert},
        {"Тундра: самый тёплый месяц < 10 °C", [](const BiomeInput& b, double) { return b.c.thot < 10; }, kBiomeTundra},
        {"Лесотундра: у полярной границы леса (самый тёплый месяц 10–12,5 °C)",
         [](const BiomeInput& b, double) { return b.c.thot < 12.5 && b.c.tcold < -3; }, kBiomeForestTundra},
        {"Пустыня: P/PET < 0,05", [](const BiomeInput& b, double) { return b.c.ai < 0.05 && b.c.mat >= 18; }, kBiomeHotDesert},
        {"Холодная пустыня: P/PET < 0,05, среднегодовая < 18 °C", [](const BiomeInput& b, double) { return b.c.ai < 0.05; }, kBiomeColdDesert},
        {"Полупустыня: P/PET 0,05–0,2", [](const BiomeInput& b, double) { return b.c.ai < 0.2; }, kBiomeSemiDesert},
        {"Влажный тропический лес: без заморозков, P/PET ≥ 1, ≤ 2 сухих месяцев",
         [](const BiomeInput& b, double soft) { return b.c.tcold >= 18 - 2 * soft && b.c.ai >= 1.0 - 0.15 * soft && b.c.dryMonths <= 2 + soft; }, kBiomeTropicalRainforest},
        {"Муссонный лес: большая сезонная амплитуда осадков при обильном влажном сезоне",
         [](const BiomeInput& b, double soft) {
             return b.c.tcold >= 16 - 2 * soft && b.c.ai >= 0.75 - 0.1 * soft && b.c.seasonality >= 0.35 - 0.1 * soft && b.c.map >= 1300 - 300 * soft && b.c.dryMonths <= 6;
         }, kBiomeMonsoonForest},
        {"Сухой тропический лес: P/PET ≥ 0,55, сухой сезон ≤ 7 мес",
         [](const BiomeInput& b, double) { return b.c.tcold >= 16 && b.c.ai >= 0.55 && b.c.dryMonths <= 7; }, kBiomeTropicalDryForest},
        {"Саванна: тропики, P/PET 0,2–0,55 или длинный сухой сезон", [](const BiomeInput& b, double) { return b.c.tcold >= 16; }, kBiomeSavanna},
        {"Средиземноморская жестколистная: сухое жаркое лето, мягкая зима",
         [](const BiomeInput& b, double) { return b.summerDry && b.c.tcold > -3 && b.c.thot >= 15 && b.c.ai < 1.1; }, kBiomeMediterranean},
        {"Умеренный дождевой лес: очень влажно, ровные сезоны, прохладное лето",
         [](const BiomeInput& b, double soft) {
             return b.c.tcold > -3 && b.c.ai >= 1.3 - 0.2 * soft && b.c.map >= 1400 - 200 * soft && b.c.thot < 22 && b.c.mat >= 4 && b.c.seasonality < 0.4;
         }, kBiomeTemperateRainforest},
        {"Умеренный широколиственный/смешанный лес: P/PET ≥ 0,6, зима мягкая", [](const BiomeInput& b, double) { return b.c.tcold > -3 && b.c.ai >= 0.6; }, kBiomeTemperateForest},
        {"Степь/прерия: P/PET 0,2–0,6", [](const BiomeInput& b, double) { return b.c.ai < 0.6 && b.c.ai >= 0.2 && (b.c.tcold > -3 || b.c.ai < 0.45); }, kBiomeSteppe},
        {"Смешанный лес: холодная зима, длинный вегетационный период (GDD5 ≥ 1300)",
         [](const BiomeInput& b, double) { return b.c.gdd5 >= 1300 && b.c.ai >= 0.55; }, kBiomeTemperateForest},
        {"Хвойная тайга: холодная зима, короткое лето", [](const BiomeInput&, double) { return true; }, kBiomeTaiga},
    };
    return rules;
}

uint8_t classifyBiome(const BiomeInput& b, double soft) {
    for (auto& r : biomeRules())
        if (r.when(b, soft)) return r.biome;
    return kBiomeTaiga;
}

}  // namespace

std::vector<std::string> biomeRuleDescriptions() {
    std::vector<std::string> v;
    for (auto& r : biomeRules()) v.push_back(r.descriptionRu);
    return v;
}

void runBiomes(StageContext& ctx) {
    WorldData& w = ctx.world;
    const Params& P = ctx.params;
    const SphereGrid& g = w.grid;
    const int W = g.width(), H = g.height();
    const int64_t N = g.size();
    const double sea = w.seaLevel();
    const double soft = P.flag("biome.extendRare") ? 1.0 : 0.0;
    const uint8_t* ocean = w.u8(LayerId::OceanMask);
    const float* z = w.f32(LayerId::Elevation);
    const int16_t* tm = w.i16(LayerId::TempMonthly);
    const uint16_t* pm = w.u16(LayerId::PrecMonthly);
    const uint16_t* em = w.u16(LayerId::PetMonthly);
    const uint8_t* iceT = w.u8(LayerId::IceType);
    const uint8_t* lgm = w.u8(LayerId::LgmIce);
    const int32_t* lake = w.i32(LayerId::LakeId);
    const uint8_t* wet = w.u8(LayerId::Wetland);
    const uint8_t* landform = w.u8(LayerId::Landform);
    const uint8_t* lith = w.u8(LayerId::Lithology);
    const float* dCoast = w.f32(LayerId::DistCoast);
    const float* expo = w.f32(LayerId::CoastExposure);
    const float* tide = w.f32(LayerId::TidalRange);
    const float* q = w.f32(LayerId::Discharge);
    const uint8_t* rc = w.u8(LayerId::RiverClass);
    const float* dSub = w.f32(LayerId::DistSubduction);

    auto Lkg = w.makeLayer(LayerId::Koppen);
    auto Lb = w.makeLayer(LayerId::Biome);
    auto Lb2 = w.makeLayer(LayerId::Biome2);
    auto Le = w.makeLayer(LayerId::Ecotone);
    auto Lbelt = w.makeLayer(LayerId::AltBelt);
    auto Lcoast = w.makeLayer(LayerId::CoastType);
    auto Lk = w.makeLayer(LayerId::Karst);
    uint8_t* kg = Lkg->as<uint8_t>();
    uint8_t* biome = Lb->as<uint8_t>();
    uint8_t* biome2 = Lb2->as<uint8_t>();
    float* eco = Le->as<float>();
    uint8_t* belt = Lbelt->as<uint8_t>();
    uint8_t* coast = Lcoast->as<uint8_t>();
    uint8_t* karst = Lk->as<uint8_t>();

    // Delta mouths and large river mouths (for mangrove sites, coral turbidity, estuaries).
    std::vector<uint8_t> deltaNear(static_cast<size_t>(N), 0), mouthNear(static_cast<size_t>(N), 0);
    {
        std::vector<int64_t> deltaSrc, mouthSrc;
        if (w.hydro) {
            for (auto& d : w.hydro->deltas) deltaSrc.push_back(d.mouthCell);
            for (auto& r : w.hydro->rivers)
                if (r.downstream < 0 && !r.cells.empty() && r.riverClass != kRiverDistributary && r.dischargeEnd > 300) mouthSrc.push_back(r.cells.back());
        }
        std::vector<float> dd, dm;
        distanceTransform(g, deltaSrc, dd, nullptr, 400);
        distanceTransform(g, mouthSrc, dm, nullptr, 400);
        for (int64_t k = 0; k < N; ++k) {
            deltaNear[static_cast<size_t>(k)] = dd[static_cast<size_t>(k)] < std::max(60.0, 2.2 * g.dyKm());
            mouthNear[static_cast<size_t>(k)] = dm[static_cast<size_t>(k)] < std::max(80.0, 3.0 * g.dyKm());
        }
    }

    // ---- per-cell climate classes, belts and biomes
    std::vector<ClimateSummary> unused;
    ctx.pool.forRows(H, [&](int j) {
        bool north = g.latDeg(j) >= 0;
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            double T[12], Pp[12], E[12];
            for (int m = 0; m < 12; ++m) {
                T[m] = tm[m * N + k] * 0.01;
                Pp[m] = pm[m * N + k] * 0.1;
                E[m] = em[m * N + k] * 0.1;
            }
            ClimateSummary c = summarize(T, Pp, E, north);
            if (ocean[k]) {
                kg[k] = kKgNone;
                biome[k] = biome2[k] = kBiomeOcean;
                eco[k] = 0;
                belt[k] = kBeltNone;
                karst[k] = kKarstNone;
                continue;
            }
            kg[k] = koppen(c);
            double h = z[k] - sea;
            bool mountain = landform[k] == kLfMountains || landform[k] == kLfHighlands || landform[k] == kLfPlateau || landform[k] == kLfVolcanic || h > 1500;
            uint8_t bl = kBeltLowland;
            if (mountain || h > 700) {
                if (c.thot < 1.5) bl = kBeltNival;
                else if (c.thot < 8.5) bl = kBeltAlpine;
                else if (c.thot < 11.0) bl = kBeltSubalpine;
                else if (mountain) bl = kBeltMontane;
                else bl = kBeltFoothill;
            } else if (landform[k] == kLfFoothillPlain || landform[k] == kLfHills) {
                bl = kBeltFoothill;
            }
            bool iceCovered = iceT && (iceT[k] == kIceSheet || iceT[k] == kIceGlacier);
            bool isLake = lake && lake[k] >= 0;
            belt[k] = (iceCovered || isLake) ? kBeltNone : bl;
            BiomeInput in;
            in.c = c;
            in.mountain = mountain;
            in.belt = bl;
            in.wetland = wet ? wet[k] : 0;
            double ex = expo && !std::isnan(expo[k]) ? expo[k] : 1.0;
            double td = tide && !std::isnan(tide[k]) ? tide[k] : 0.0;
            bool coastal = dCoast[k] < 1.6 * g.dyKm() + 1;
            in.mangroveSite = coastal && h < 60 && (ex < 0.55 + 0.1 * soft || deltaNear[static_cast<size_t>(k)]) && td >= 0.4 - 0.2 * soft;
            in.summerDry = c.psdry < 40 && c.psdry < c.pwwet / 3;
            uint8_t b;
            if (isLake) b = kBiomeLake;
            else if (iceCovered) b = kBiomeIceSheet;
            else b = classifyBiome(in, soft);
            biome[k] = b;
            // Ecotone: re-evaluate with perturbed climate.
            uint8_t alt = b;
            int changes = 0;
            if (!isLake && !iceCovered) {
                const double dT[4] = {1.5, -1.5, 0, 0};
                const double fA[4] = {1, 1, 1.25, 0.8};
                for (int t = 0; t < 4; ++t) {
                    BiomeInput p = in;
                    p.c.mat += dT[t]; p.c.tcold += dT[t]; p.c.thot += dT[t];
                    p.c.gdd5 = std::max(0.0, p.c.gdd5 + dT[t] * 180);
                    p.c.ai *= fA[t];
                    p.c.map *= fA[t];
                    uint8_t o = classifyBiome(p, soft);
                    if (o != b) { ++changes; alt = o; }
                }
            }
            biome2[k] = alt;
            eco[k] = static_cast<float>(changes / 4.0);
            // Karst potential (P2): carbonate rocks with water.
            karst[k] = (lith[k] == kLithCarbonate && c.ai > 0.5 && c.mat > 0) ? (c.map > 1000 ? kKarstHigh : kKarstLow) : kKarstNone;
        }
    }, &ctx.cancel);
    ctx.progress(0.5, "климатические классы и биомы");

    // Coastal ecotone strip on low accumulative coasts (assigned after coasts below).
    // ---- coast types (land and sea cells on both sides of the shoreline)
    std::vector<float> relief(static_cast<size_t>(N));
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            double mx = z[k];
            int r = std::max(1, static_cast<int>(std::round(40.0 / g.dyKm())));
            for (int dj = -r; dj <= r; ++dj)
                for (int di = -r; di <= r; ++di) {
                    int jj = std::clamp(j + dj, 0, H - 1);
                    int64_t q2 = g.index(g.wrapCol(i + di), jj);
                    if (!ocean[q2]) mx = std::max(mx, static_cast<double>(z[q2]));
                }
            relief[static_cast<size_t>(k)] = static_cast<float>(mx - sea);
        }
    const int16_t* tmColdSrc = tm;
    auto coldestMonth = [&](int64_t k) {
        double t = 1e9;
        for (int m = 0; m < 12; ++m) t = std::min(t, tmColdSrc[m * N + k] * 0.01);
        return t;
    };
    ctx.pool.forRows(H, [&](int j) {
        double alat = std::fabs(g.latDeg(j));
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            int64_t nb[8];
            g.neighbors8(k, nb);
            bool shore = false;
            for (int d = 0; d < 8 && !shore; ++d) shore = ocean[nb[d]] != ocean[k];
            coast[k] = kCoastNone;
            if (!shore) continue;
            // Land-side context: the land cell itself or the land neighbour with the largest relief.
            int64_t landCell = k;
            if (ocean[k])
                for (int d = 0; d < 8; ++d) if (!ocean[nb[d]] && (ocean[landCell] || relief[static_cast<size_t>(nb[d])] > relief[static_cast<size_t>(landCell)])) landCell = nb[d];
            double rel = relief[static_cast<size_t>(landCell)];
            double h = z[landCell] - sea;
            double ex = expo && !std::isnan(expo[k]) ? expo[k] : 0.5;
            double td = tide && !std::isnan(tide[k]) ? tide[k] : 1.0;
            bool glaciated = lgm && lgm[landCell];
            bool riverMouth = false;
            for (int d = 0; d < 8 && !riverMouth; ++d) riverMouth = rc[nb[d]] && !ocean[nb[d]] && q[nb[d]] > 40;
            if (rc[landCell] && q[landCell] > 40) riverMouth = true;
            bool active = dSub[landCell] < 400;
            bool shelfWide = false;
            {
                int sh = 0, tot = 0;
                int r = std::max(1, static_cast<int>(120.0 / g.dyKm()));
                for (int dj = -r; dj <= r; dj += std::max(1, r / 3))
                    for (int di = -r; di <= r; di += std::max(1, r / 3)) {
                        int64_t q2 = g.index(g.wrapCol(i + di), std::clamp(j + dj, 0, H - 1));
                        if (!ocean[q2]) continue;
                        ++tot;
                        if (z[q2] - sea > -120) ++sh;
                    }
                shelfWide = tot > 0 && sh * 2 > tot;
            }
            uint8_t c;
            if (iceT && (iceT[landCell] == kIceSheet || iceT[k] == kIceShelf)) c = kCoastIce;
            else if (deltaNear[static_cast<size_t>(k)]) c = kCoastDelta;
            else if (biome[landCell] == kBiomeMangrove) c = kCoastMangrove;
            else if (ocean[k] && coldestMonth(k) >= 18.5 && alat < 32 && z[k] - sea > -200 && !mouthNear[static_cast<size_t>(k)] && ex > 0.15) c = kCoastCoral;
            else if (glaciated && rel > 600) c = kCoastFjord;
            else if (glaciated && rel < 350 && (lith[landCell] == kLithCrystalline || lith[landCell] == kLithMetamorphic)) c = kCoastSkerry;
            else if (riverMouth && mouthNear[static_cast<size_t>(k)] && (td >= 2.0 || ex > 0.5)) c = kCoastEstuary;
            else if (riverMouth && rel > 250 && rel < 1500 && !glaciated) c = kCoastRia;
            else if (td >= 3.5 && h < 25 && shelfWide) c = kCoastTidalFlat;
            else if (rel > 500 && ex >= 0.35) c = kCoastCliff;
            else if (rel > 150 && ex >= 0.5) c = kCoastAbrasion;
            else if (h < 30 && shelfWide && !active && td < 2.5 && ex > 0.3 && ex < 0.85) c = ex < 0.45 ? kCoastLagoon : kCoastBarrier;
            else if (rel > 60 && ex >= 0.45 && (alat > 40 || rel > 300)) c = kCoastPebble;
            else if (h < 40 && ex >= 0.25) c = kCoastSandy;
            else c = kCoastAccumulative;
            coast[k] = c;
        }
    }, &ctx.cancel);
    // Coastal ecotone: narrow strip of low accumulative coasts (dunes, salt meadows), unless a stronger biome applies.
    for (int64_t k = 0; k < N; ++k) {
        if (ocean[k] || !coast[k]) continue;
        uint8_t c = coast[k];
        bool soft = c == kCoastSandy || c == kCoastBarrier || c == kCoastLagoon || c == kCoastAccumulative || c == kCoastTidalFlat;
        uint8_t b = biome[k];
        bool replaceable = b != kBiomeMangrove && b != kBiomeIceSheet && b != kBiomeLake && b != kBiomeNival && b != kBiomeSaltFlat &&
                           b != kBiomeFreshwaterMarsh && b != kBiomePolarDesert;
        if (soft && replaceable && z[k] - sea < 25) {
            biome2[k] = b;
            biome[k] = kBiomeCoastalEcotone;
            eco[k] = std::max(eco[k], 0.5f);
        }
    }
    ctx.progress(0.85, "побережья");

    w.setLayer(LayerId::Koppen, Lkg);
    w.setLayer(LayerId::Biome, Lb);
    w.setLayer(LayerId::Biome2, Lb2);
    w.setLayer(LayerId::Ecotone, Le);
    w.setLayer(LayerId::AltBelt, Lbelt);
    w.setLayer(LayerId::CoastType, Lcoast);
    w.setLayer(LayerId::Karst, Lk);

    // ---- checks and statistics (area-weighted)
    int64_t mangroveCold = 0, forestOnIce = 0;
    std::vector<double> biomeArea(kBiomeCount, 0.0), kgArea(kKgCount, 0.0);
    double landArea = 0;
    double alpineLatLo = 0, alpineLatHi = 0, nLo = 0, nHi = 0;
    for (int j = 0; j < H; ++j) {
        double a = g.cellAreaKm2(j);
        double alat = std::fabs(g.latDeg(j));
        for (int i = 0; i < W; ++i) {
            int64_t k = g.index(i, j);
            if (ocean[k]) continue;
            landArea += a;
            biomeArea[biome[k]] += a;
            kgArea[kg[k]] += a;
            if (biome[k] == kBiomeMangrove && (dCoast[k] > 3 * g.dyKm() || coldestMonth(k) < 10)) ++mangroveCold;
            bool forest = biome[k] == kBiomeTropicalRainforest || biome[k] == kBiomeMonsoonForest || biome[k] == kBiomeTemperateForest ||
                          biome[k] == kBiomeTaiga || biome[k] == kBiomeTemperateRainforest || biome[k] == kBiomeTropicalDryForest;
            if (forest && iceT && (iceT[k] == kIceSheet || iceT[k] == kIceGlacier)) ++forestOnIce;
            if (belt[k] == kBeltAlpine) {
                double h = z[k] - sea;
                if (alat < 25) { alpineLatLo += h; ++nLo; }
                else if (alat > 45) { alpineLatHi += h; ++nHi; }
            }
        }
    }
    ctx.report.check("biome.mangroves", "Мангры только на тёплых побережьях", mangroveCold == 0, static_cast<double>(mangroveCold), "0", true);
    ctx.report.check("biome.noForestOnIce", "Нет леса на ледниках и ледяных щитах", forestOnIce == 0, static_cast<double>(forestOnIce), "0", true);
    if (nLo > 5 && nHi > 5)
        ctx.report.check("biome.alpineByLatitude", "Альпийский пояс в тропиках выше, чем в высоких широтах", alpineLatLo / nLo > alpineLatHi / nHi + 500,
                         alpineLatLo / nLo - alpineLatHi / nHi, "> 500 м");
    for (auto& cat : categoriesOf(LayerId::Biome))
        if (cat.code > 0 && cat.code < kBiomeCount && landArea > 0 && biomeArea[static_cast<size_t>(cat.code)] > 0)
            ctx.report.metric(std::string("biome:") + std::to_string(cat.code), biomeArea[static_cast<size_t>(cat.code)] / landArea);
    double grp[5] = {0, 0, 0, 0, 0};
    for (int c = 1; c < kKgCount; ++c) {
        int gidx = c <= kKgAw ? 0 : (c <= kKgBSk ? 1 : (c <= kKgCfc ? 2 : (c <= kKgDfd ? 3 : 4)));
        grp[gidx] += kgArea[static_cast<size_t>(c)];
    }
    const char* gname[5] = {"koppenA", "koppenB", "koppenC", "koppenD", "koppenE"};
    for (int gi = 0; gi < 5; ++gi) ctx.report.metric(gname[gi], landArea > 0 ? grp[gi] / landArea : 0);
    if (soft > 0) ctx.report.warn("Включено «расширить условия редких биомов»: пороги мангров, муссонных и дождевых лесов смягчены (отклонение от реализма).");
    ctx.progress(1.0, "биомы");
}

}  // namespace mapgen
