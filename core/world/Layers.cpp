#include "core/world/Layers.h"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace mapgen {

namespace {
using L = LayerId;
constexpr auto F = DType::F32;
constexpr auto U8 = DType::U8;
constexpr auto U16 = DType::U16;
constexpr auto I16 = DType::I16;
constexpr auto I32 = DType::I32;
constexpr auto FG = GridKind::Full;
constexpr auto CG = GridKind::Climate;

const std::vector<LayerInfo> kLayers = {
    // id, key, type, grid, bands, stage, scale, units, categorical, name
    {L::PlateId, "plateId", U16, FG, 1, 1, 1, "", true, "Идентификатор литосферной плиты"},
    {L::Continentality, "continentality", F, FG, 1, 1, 1, "0–1", false, "Поле континентальной коры"},
    {L::CrustType, "crustType", U8, FG, 1, 1, 1, "", true, "Тип коры"},
    {L::CrustAge, "crustAge", F, FG, 1, 1, 1, "млн лет", false, "Возраст океанической коры (условный)"},
    {L::DistCOB, "distCOB", F, FG, 1, 1, 1, "км", false, "Расстояние до границы океанической и континентальной коры"},
    {L::BoundaryType, "boundaryType", U8, FG, 1, 1, 1, "", true, "Тип границы плит"},
    {L::BoundaryClass, "boundaryClass", U8, FG, 1, 1, 1, "", true, "Класс границы по типам коры"},
    {L::DistBoundary, "distBoundary", F, FG, 1, 1, 1, "км", false, "Расстояние до границы плиты"},
    {L::PlateVelE, "plateVelE", F, FG, 1, 1, 1, "см/год (усл.)", false, "Скорость плиты, восточная компонента"},
    {L::PlateVelN, "plateVelN", F, FG, 1, 1, 1, "см/год (усл.)", false, "Скорость плиты, северная компонента"},
    {L::TectonicSide, "tectonicSide", U8, FG, 1, 1, 1, "", true, "Положение относительно ближайшей активной границы"},
    {L::TectonicIntensity, "tectonicIntensity", F, FG, 1, 1, 1, "0–1", false, "Интенсивность тектонического влияния"},
    {L::DistSubduction, "distSubduction", F, FG, 1, 1, 1, "км", false, "Расстояние до зоны субдукции (верхняя плита)"},
    {L::DistTrench, "distTrench", F, FG, 1, 1, 1, "км", false, "Расстояние до зоны субдукции (погружающаяся плита)"},
    {L::DistCollision, "distCollision", F, FG, 1, 1, 1, "км", false, "Расстояние до зоны коллизии"},
    {L::DistRidge, "distRidge", F, FG, 1, 1, 1, "км", false, "Расстояние до дивергентной границы"},
    {L::DistTransform, "distTransform", F, FG, 1, 1, 1, "км", false, "Расстояние до трансформной границы"},
    {L::ConvRate, "convRate", F, FG, 1, 1, 1, "см/год (усл.)", false, "Скорость сближения на ближайшей конвергентной границе"},
    {L::DivRate, "divRate", F, FG, 1, 1, 1, "см/год (усл.)", false, "Скорость раздвижения на ближайшей дивергентной границе"},

    {L::ElevationRaw, "elevationRaw", F, FG, 1, 2, 1, "м", false, "Структурная высота до эрозии"},
    {L::Elevation, "elevation", F, FG, 1, 2, 1, "м", false, "Высота / глубина коренного рельефа"},
    {L::ErosionDepth, "erosionDepth", F, FG, 1, 2, 1, "м", false, "Эрозионный срез (+) / аккумуляция (−)"},
    {L::Genesis, "genesis", U8, FG, 1, 2, 1, "", true, "Структурный генезис рельефа"},
    {L::Lithology, "lithology", U8, FG, 1, 2, 1, "", true, "Литология (упрощённая)"},

    {L::OceanMask, "oceanMask", U8, FG, 1, 3, 1, "", true, "Морская маска"},
    {L::WaterBody, "waterBody", I32, FG, 1, 3, 1, "", true, "Океаническая связная компонента"},
    {L::DistCoast, "distCoast", F, FG, 1, 3, 1, "км", false, "Расстояние до берега (+ суша, − море)"},
    {L::Landform, "landform", U8, FG, 1, 3, 1, "", true, "Форма рельефа / дна"},
    {L::CoastExposure, "coastExposure", F, FG, 1, 3, 1, "0–1", false, "Открытость берега волнению"},
    {L::TidalRange, "tidalRange", F, FG, 1, 3, 1, "м (усл.)", false, "Приливная амплитуда (параметризация)"},

    {L::TempMonthly, "tempMonthly", I16, FG, 12, 4, 0.01, "°C", false, "Среднемесячная температура"},
    {L::PrecMonthly, "precMonthly", U16, FG, 12, 4, 0.1, "мм/мес", false, "Месячные осадки"},
    {L::PetMonthly, "petMonthly", U16, FG, 12, 4, 0.1, "мм/мес", false, "Потенциальная испаряемость (PET)"},
    {L::RunoffMonthly, "runoffMonthly", U16, FG, 12, 4, 0.1, "мм/мес", false, "Климатический сток"},
    {L::SoilMonthly, "soilMonthly", U16, FG, 12, 4, 0.1, "мм", false, "Запас воды в почве"},
    {L::SweMonthly, "sweMonthly", U16, FG, 12, 4, 1, "мм в.э.", false, "Снегозапас (водный эквивалент)"},
    {L::TempMean, "tempMean", F, FG, 1, 4, 1, "°C", false, "Среднегодовая температура"},
    {L::TempMin, "tempMin", F, FG, 1, 4, 1, "°C", false, "Температура самого холодного месяца"},
    {L::TempMax, "tempMax", F, FG, 1, 4, 1, "°C", false, "Температура самого тёплого месяца"},
    {L::PrecAnnual, "precAnnual", F, FG, 1, 4, 1, "мм/год", false, "Годовая сумма осадков"},
    {L::PetAnnual, "petAnnual", F, FG, 1, 4, 1, "мм/год", false, "Годовая потенциальная испаряемость"},
    {L::Aridity, "aridity", F, FG, 1, 4, 1, "P/PET", false, "Индекс аридности P/PET (безразмерный)"},
    {L::WaterBalance, "waterBalance", F, FG, 1, 4, 1, "мм/год", false, "Водный баланс P − PET"},
    {L::RunoffAnnual, "runoffAnnual", F, FG, 1, 4, 1, "мм/год", false, "Годовой климатический сток"},
    {L::SoilMean, "soilMean", F, FG, 1, 4, 1, "мм", false, "Средний запас воды в почве"},
    {L::SnowMonths, "snowMonths", U8, FG, 1, 4, 1, "мес", false, "Число месяцев со снежным покровом"},
    {L::IceType, "iceType", U8, FG, 1, 4, 1, "", true, "Наземный лёд"},
    {L::IceThickness, "iceThickness", F, FG, 1, 4, 1, "м", false, "Толщина ледника / ледяного щита"},
    {L::SeaIceMonths, "seaIceMonths", U16, FG, 1, 4, 1, "", true, "Морской лёд по месяцам (битовая маска)"},
    {L::LgmIce, "lgmIce", U8, FG, 1, 4, 1, "", true, "Оледенение в холодную эпоху (палеосценарий)"},
    {L::DryMonths, "dryMonths", U8, FG, 1, 4, 1, "мес", false, "Число сухих месяцев (P < PET/2)"},
    {L::PrecSeasonality, "precSeasonality", F, FG, 1, 4, 1, "0–1", false, "Сезонность осадков"},
    {L::LapseRate, "lapseRate", F, FG, 1, 4, 1, "°C/км", false, "Вертикальный температурный градиент"},
    {L::WindU, "windU", F, CG, 12, 4, 1, "м/с", false, "Ветер, восточная компонента"},
    {L::WindV, "windV", F, CG, 12, 4, 1, "м/с", false, "Ветер, северная компонента"},
    {L::RelHumidity, "relHumidity", F, CG, 12, 4, 1, "%", false, "Относительная влажность (аппроксимация)"},
    {L::SstAnomaly, "sstAnomaly", F, CG, 1, 4, 1, "°C", false, "Аномалия ТПО от течений (упрощённая)"},

    {L::HydroSurface, "hydroSurface", F, FG, 1, 5, 1, "м", false, "Гидрологическая поверхность (с уровнями озёр)"},
    {L::FlowDir, "flowDir", U8, FG, 1, 5, 1, "", true, "Направление стока D8"},
    {L::Discharge, "discharge", F, FG, 1, 5, 1, "м³/с", false, "Среднегодовой расход"},
    {L::DrainageArea, "drainageArea", F, FG, 1, 5, 1, "км²", false, "Площадь водосбора"},
    {L::BasinId, "basinId", I32, FG, 1, 5, 1, "", true, "Речной бассейн"},
    {L::RiverOrder, "riverOrder", U8, FG, 1, 5, 1, "", false, "Порядок реки (Стралер)"},
    {L::RiverClass, "riverClass", U8, FG, 1, 5, 1, "", true, "Режим водотока"},
    {L::LakeId, "lakeId", I32, FG, 1, 5, 1, "", true, "Озеро"},
    {L::Wetland, "wetland", U8, FG, 1, 5, 1, "", true, "Болота и поймы"},

    {L::Koppen, "koppen", U8, FG, 1, 6, 1, "", true, "Климат по Кёппену–Гейгеру"},
    {L::Biome, "biome", U8, FG, 1, 6, 1, "", true, "Биом"},
    {L::Biome2, "biome2", U8, FG, 1, 6, 1, "", true, "Соседний биом экотона"},
    {L::Ecotone, "ecotone", F, FG, 1, 6, 1, "0–1", false, "Сила экотона"},
    {L::AltBelt, "altBelt", U8, FG, 1, 6, 1, "", true, "Высотный пояс"},
    {L::CoastType, "coastType", U8, FG, 1, 6, 1, "", true, "Тип берега"},
    {L::Karst, "karst", U8, FG, 1, 6, 1, "", true, "Карст (потенциал)"},

    {L::VolcanicZone, "volcanicZone", U8, FG, 1, 7, 1, "0–5 (усл.)", false, "Потенциал вулканической активности (условная шкала)"},
    {L::Geothermal, "geothermal", F, FG, 1, 7, 1, "0–1", false, "Геотермальный потенциал (вероятностный)"},
    {L::Seismic, "seismic", U8, FG, 1, 7, 1, "0–5 (усл.)", false, "Условная сейсмотектоническая активность"},

    {L::Provenance, "provenance", U16, FG, 1, 8, 1, "", true, "Происхождение и флаги качества"},
};
}  // namespace

const std::vector<LayerInfo>& allLayers() {
    static bool checked = [] {
        if (static_cast<int>(kLayers.size()) != kLayerCount) throw std::logic_error("layer registry size mismatch");
        for (int i = 0; i < kLayerCount; ++i)
            if (static_cast<int>(kLayers[static_cast<size_t>(i)].id) != i) throw std::logic_error("layer registry order mismatch");
        return true;
    }();
    (void)checked;
    return kLayers;
}

const LayerInfo& layerInfo(LayerId id) { return allLayers()[static_cast<size_t>(id)]; }

bool layerFromKey(const std::string& key, LayerId& out) {
    for (auto& l : allLayers())
        if (key == l.key) {
            out = l.id;
            return true;
        }
    return false;
}

float notComputed() { return std::numeric_limits<float>::quiet_NaN(); }
bool isNotComputed(float v) { return std::isnan(v); }

double LayerData::value(int64_t cell, int b) const {
    int64_t k = static_cast<int64_t>(b) * cells() + cell;
    switch (type) {
        case DType::F32: return as<float>()[k];
        case DType::U8: return as<uint8_t>()[k];
        case DType::U16: return as<uint16_t>()[k];
        case DType::I16: return as<int16_t>()[k];
        case DType::I32: return as<int32_t>()[k];
    }
    return 0;
}

}  // namespace mapgen
