#include "core/world/Categories.h"

#include <map>

namespace mapgen {

namespace {

using V = std::vector<CategoryEntry>;

const V kCrust = {{kCrustOceanic, "Океаническая кора", 0x3b6ea8}, {kCrustContinental, "Континентальная кора", 0xc8a16a}};

const V kBoundaryType = {{kBndNone, "Внутриплитная область", 0xe8e8e8},
                         {kBndDivergent, "Дивергентная (раздвижение)", 0xe4572e},
                         {kBndConvergent, "Конвергентная (сближение)", 0x2e86ab},
                         {kBndTransform, "Трансформная (сдвиг)", 0x76b041},
                         {kBndComplex, "Сложная / косая", 0xa35bb0}};

const V kBoundaryClass = {{kBcNone, "—", 0xe8e8e8},
                          {kBcOceanOcean, "Океан – океан", 0x2a7fc1},
                          {kBcOceanContinent, "Океан – континент", 0xd08b2c},
                          {kBcContinentContinent, "Континент – континент", 0x8c4a2f}};

const V kSide = {{kSideInterior, "Внутренняя часть плиты", 0xdddddd},
                 {kSideOverriding, "Верхняя плита зоны субдукции", 0xd1495b},
                 {kSideSubducting, "Погружающаяся плита (желоб)", 0x00798c},
                 {kSideCollision, "Зона коллизии", 0x8f2d56},
                 {kSideRift, "Континентальный рифт", 0xedae49},
                 {kSideRidge, "Срединно-океанический хребет", 0xf26419},
                 {kSideTransform, "Трансформная зона", 0x66a182},
                 {kSideComplex, "Сложная граница", 0x9d79bc}};

const V kGenesis = {{kGenAbyssal, "Абиссальная котловина", 0x1d3f72},
                    {kGenMidOceanRidge, "Срединно-океанический хребет", 0xe76f51},
                    {kGenTrench, "Глубоководный желоб", 0x0b1d3a},
                    {kGenContinentalMargin, "Континентальная окраина", 0x5fa8d3},
                    {kGenIslandArc, "Островная дуга", 0xf4a261},
                    {kGenVolcanicArc, "Вулканический пояс окраины", 0xc0392b},
                    {kGenCollisionOrogen, "Коллизионный ороген", 0x7b2d26},
                    {kGenOldOrogen, "Древняя складчатая область", 0xa98467},
                    {kGenPlateau, "Плато / нагорье", 0xd4a373},
                    {kGenContinentalRift, "Континентальный рифт", 0xe9c46a},
                    {kGenPlatform, "Древняя платформа", 0xb7c68b},
                    {kGenSedimentaryBasin, "Осадочный бассейн", 0x90be6d},
                    {kGenTransformZone, "Трансформная зона", 0x43aa8b},
                    {kGenHotspot, "Горячая точка", 0xff6b35},
                    {kGenBackArc, "Задуговый бассейн", 0x3d5a80},
                    {kGenShield, "Кристаллический щит", 0xc9ada7}};

const V kLithology = {{kLithBasalt, "Базальты океанической коры", 0x355070},
                      {kLithCrystalline, "Кристаллические породы щитов", 0xb56576},
                      {kLithSedimentary, "Терригенные осадочные породы", 0xe0c097},
                      {kLithCarbonate, "Карбонатные породы", 0xf2e8cf},
                      {kLithVolcanic, "Вулканические породы", 0x6d597a},
                      {kLithAlluvium, "Аллювий и молодые отложения", 0xa7c957},
                      {kLithMetamorphic, "Метаморфические породы складчатых поясов", 0x9a8c98},
                      {kLithMarineSediment, "Морские осадки", 0x4f7cac}};

const V kLandform = {{kLfMountains, "Горы", 0x8b5a3c},
                     {kLfHighlands, "Нагорья", 0xb07d56},
                     {kLfPlateau, "Плато", 0xd2a679},
                     {kLfTableland, "Плоскогорья", 0xc9b28a},
                     {kLfHills, "Холмы", 0xc5c98f},
                     {kLfFoothillPlain, "Предгорные равнины", 0xd9dfa7},
                     {kLfLowland, "Низменности", 0xa8d5a2},
                     {kLfAlluvialLowland, "Аллювиальные низменности", 0x7fc8a9},
                     {kLfDepression, "Депрессии ниже уровня моря", 0x5e9c8f},
                     {kLfPlatformPlain, "Равнины древних платформ", 0xbcd4a0},
                     {kLfCoastalPlain, "Прибрежные равнины", 0xe9f5c9},
                     {kLfRiftValley, "Рифтовые долины", 0xe76f51},
                     {kLfVolcanic, "Вулканические массивы", 0x9d4edd},
                     {kLfGlacial, "Ледниковые формы", 0xdbe9f6},
                     {kLfShelf, "Шельф", 0x9fd3f0},
                     {kLfSlope, "Континентальный склон", 0x5aa5d6},
                     {kLfRise, "Континентальное подножие", 0x3e7fb8},
                     {kLfAbyssalPlain, "Абиссальная равнина", 0x24548f},
                     {kLfRidge, "Срединно-океанический хребет", 0xd66a4f},
                     {kLfTrench, "Глубоководный желоб", 0x0c1f44},
                     {kLfSeamount, "Подводная гора", 0xb58ad6},
                     {kLfGuyot, "Гайот", 0x8a6db8},
                     {kLfArcRidge, "Хребет островной дуги", 0xe0a458},
                     {kLfCanyon, "Подводный каньон", 0x2b2d6e},
                     {kLfBackArcBasin, "Задуговый бассейн", 0x32508f},
                     {kLfFractureZone, "Зона трансформного разлома", 0x4a6fa5}};

const V kIce = {{kIceNone, "Нет", 0xffffff},
                {kIceGlacier, "Горный ледник", 0xa9d6e5},
                {kIceSheet, "Ледяной щит", 0xe9f5fb},
                {kIceShelf, "Шельфовый ледник", 0x89c2d9}};

const V kRiverClass = {{kRiverNone, "Нет водотока", 0xffffff},
                       {kRiverPerennial, "Постоянная река", 0x1565c0},
                       {kRiverSeasonal, "Сезонная река", 0x4fa3e0},
                       {kRiverIntermittent, "Пересыхающее русло", 0xc2a46b},
                       {kRiverDistributary, "Дельтовый рукав", 0x26a69a}};

const V kWetland = {{kWetNone, "Нет", 0xffffff},
                    {kWetMarsh, "Пресноводное болото", 0x5b8e7d},
                    {kWetPeat, "Торфяник", 0x8d6e63},
                    {kWetFloodplain, "Сезонно затапливаемая пойма", 0x80cbc4},
                    {kWetSaltFlat, "Солончак / сухое озеро", 0xf3e5ab}};

const V kBiome = {{kBiomeNone, "Не рассчитано", 0xffffff},
                  {kBiomeTropicalRainforest, "Влажный тропический лес", 0x0b6e2e},
                  {kBiomeMonsoonForest, "Сезонный (муссонный) тропический лес", 0x3f9b3a},
                  {kBiomeTropicalDryForest, "Сухой тропический лес", 0x8fb339},
                  {kBiomeSavanna, "Саванна", 0xc8c447},
                  {kBiomeSteppe, "Степь / прерия", 0xd9cf7a},
                  {kBiomeHotDesert, "Жаркая пустыня", 0xf2d39a},
                  {kBiomeColdDesert, "Холодная пустыня", 0xd8cbb3},
                  {kBiomeSemiDesert, "Полупустыня", 0xe3c178},
                  {kBiomeMediterranean, "Средиземноморская жестколистная растительность", 0xb39b3e},
                  {kBiomeTemperateForest, "Умеренный широколиственный / смешанный лес", 0x4f9a4c},
                  {kBiomeTemperateRainforest, "Умеренный дождевой лес", 0x1f7a5c},
                  {kBiomeTaiga, "Хвойная тайга", 0x2f6b4f},
                  {kBiomeForestTundra, "Лесотундра", 0x7a9b7e},
                  {kBiomeTundra, "Тундра", 0xa8b5a2},
                  {kBiomeAlpine, "Альпийские луга / горная тундра", 0xa98bb5},
                  {kBiomeNival, "Нивальный пояс", 0xeeeef6},
                  {kBiomeIceSheet, "Ледяной щит / ледник", 0xffffff},
                  {kBiomeFreshwaterMarsh, "Пресноводные болота", 0x4d8f88},
                  {kBiomePeatland, "Торфяники", 0x7d6b57},
                  {kBiomeFloodplain, "Сезонно затапливаемые поймы", 0x7cc6b8},
                  {kBiomeSaltFlat, "Солончаки", 0xf5ecc6},
                  {kBiomeMangrove, "Мангровые заросли", 0x2c7a6b},
                  {kBiomeCoastalEcotone, "Прибрежный экотон", 0xc7e3a9},
                  {kBiomeLake, "Озеро", 0x6fa8dc},
                  {kBiomeOcean, "Океан", 0x2b5d8c},
                  {kBiomePolarDesert, "Полярная пустыня", 0xcfd6dc}};

const V kAltBelt = {{kBeltNone, "Нет (вода/лёд)", 0xffffff},
                    {kBeltLowland, "Равнинный (вне высотной поясности)", 0xd8e8c8},
                    {kBeltFoothill, "Предгорный", 0xc5d99b},
                    {kBeltMontane, "Горно-лесной", 0x5a9a5a},
                    {kBeltSubalpine, "Субальпийский (у верхней границы леса)", 0x9cb36b},
                    {kBeltAlpine, "Альпийский", 0xb59cc8},
                    {kBeltNival, "Нивальный", 0xf2f2fa}};

const V kCoast = {{kCoastNone, "Не берег", 0xffffff},
                  {kCoastCliff, "Скальный обрывистый", 0x6d4c41},
                  {kCoastAbrasion, "Абразионный", 0xa1887f},
                  {kCoastAccumulative, "Аккумулятивный", 0xd7ccc8},
                  {kCoastSandy, "Песчаный", 0xf6d186},
                  {kCoastPebble, "Галечный", 0x9e9e9e},
                  {kCoastBarrier, "Барьерные острова", 0xffb74d},
                  {kCoastLagoon, "Лагунный", 0x4dd0e1},
                  {kCoastEstuary, "Эстуарный", 0x5c6bc0},
                  {kCoastDelta, "Дельтовый", 0x66bb6a},
                  {kCoastTidalFlat, "Приливные ватты", 0x8d9db6},
                  {kCoastFjord, "Фьордовый", 0x283593},
                  {kCoastRia, "Риасовый", 0x7e57c2},
                  {kCoastSkerry, "Шхерный", 0x546e7a},
                  {kCoastCoral, "Коралловый", 0xff8a80},
                  {kCoastMangrove, "Мангровый", 0x2e7d32},
                  {kCoastIce, "Ледяной (ледниковый)", 0xe1f5fe}};

const V kKarst = {{kKarstNone, "Нет", 0xffffff}, {kKarstLow, "Возможен", 0xe6d5a7}, {kKarstHigh, "Развит", 0xc29b40}};

const V kKoppen = {
    {kKgNone, "Нет (вода)", 0xffffff},
    {kKgAf, "Af — тропический влажный", 0x0000ff}, {kKgAm, "Am — тропический муссонный", 0x0078ff},
    {kKgAw, "Aw/As — тропический саванный", 0x46aafa}, {kKgBWh, "BWh — жаркий пустынный", 0xff0000},
    {kKgBWk, "BWk — холодный пустынный", 0xff9696}, {kKgBSh, "BSh — жаркий степной", 0xf5a500},
    {kKgBSk, "BSk — холодный степной", 0xffdc64}, {kKgCsa, "Csa — средиземноморский, жаркое лето", 0xffff00},
    {kKgCsb, "Csb — средиземноморский, тёплое лето", 0xc8c800}, {kKgCsc, "Csc — средиземноморский, прохладное лето", 0x969600},
    {kKgCwa, "Cwa — умеренный, сухая зима, жаркое лето", 0x96ff96}, {kKgCwb, "Cwb — умеренный, сухая зима, тёплое лето", 0x64c864},
    {kKgCwc, "Cwc — умеренный, сухая зима, прохладное лето", 0x329632}, {kKgCfa, "Cfa — влажный субтропический", 0xc8ff50},
    {kKgCfb, "Cfb — морской умеренный", 0x64ff50}, {kKgCfc, "Cfc — субполярный морской", 0x32c800},
    {kKgDsa, "Dsa — континентальный, сухое жаркое лето", 0xff00ff}, {kKgDsb, "Dsb — континентальный, сухое тёплое лето", 0xc800c8},
    {kKgDsc, "Dsc — континентальный, сухое прохладное лето", 0x963296}, {kKgDsd, "Dsd — континентальный, сухое лето, очень холодная зима", 0x966496},
    {kKgDwa, "Dwa — континентальный муссонный, жаркое лето", 0xaaafff}, {kKgDwb, "Dwb — континентальный муссонный, тёплое лето", 0x5a78dc},
    {kKgDwc, "Dwc — субарктический муссонный", 0x4b50b4}, {kKgDwd, "Dwd — субарктический муссонный, очень холодная зима", 0x320087},
    {kKgDfa, "Dfa — влажный континентальный, жаркое лето", 0x00ffff}, {kKgDfb, "Dfb — влажный континентальный, тёплое лето", 0x37c8ff},
    {kKgDfc, "Dfc — субарктический", 0x007d7d}, {kKgDfd, "Dfd — субарктический, очень холодная зима", 0x00465f},
    {kKgET, "ET — тундровый", 0xb2b2b2}, {kKgEF, "EF — вечной мерзлоты / ледяной", 0x666666}};

const V kOcean = {{0, "Суша", 0xd9c9a3}, {1, "Море / океан", 0x2f6fa8}};
const V kFlowDir = {{0, "С", 0}, {1, "СВ", 0}, {2, "В", 0}, {3, "ЮВ", 0}, {4, "Ю", 0}, {5, "ЮЗ", 0}, {6, "З", 0}, {7, "СЗ", 0},
                    {kFlowOcean, "В океан", 0}, {kFlowSink, "Бессточный терминал", 0}};
const V kEmpty = {};

}  // namespace

const std::vector<CategoryEntry>& categoriesOf(LayerId id) {
    switch (id) {
        case LayerId::CrustType: return kCrust;
        case LayerId::BoundaryType: return kBoundaryType;
        case LayerId::BoundaryClass: return kBoundaryClass;
        case LayerId::TectonicSide: return kSide;
        case LayerId::Genesis: return kGenesis;
        case LayerId::Lithology: return kLithology;
        case LayerId::Landform: return kLandform;
        case LayerId::IceType: return kIce;
        case LayerId::LgmIce: return kIce;
        case LayerId::RiverClass: return kRiverClass;
        case LayerId::Wetland: return kWetland;
        case LayerId::Biome:
        case LayerId::Biome2: return kBiome;
        case LayerId::AltBelt: return kAltBelt;
        case LayerId::CoastType: return kCoast;
        case LayerId::Karst: return kKarst;
        case LayerId::Koppen: return kKoppen;
        case LayerId::OceanMask: return kOcean;
        case LayerId::FlowDir: return kFlowDir;
        default: return kEmpty;
    }
}

std::string categoryName(LayerId id, int code) {
    for (auto& e : categoriesOf(id))
        if (e.code == code) return e.nameRu;
    return "код " + std::to_string(code);
}

const char* koppenCode(int code) {
    static const char* names[] = {"—", "Af", "Am", "Aw", "BWh", "BWk", "BSh", "BSk", "Csa", "Csb", "Csc",
                                  "Cwa", "Cwb", "Cwc", "Cfa", "Cfb", "Cfc", "Dsa", "Dsb", "Dsc", "Dsd", "Dwa",
                                  "Dwb", "Dwc", "Dwd", "Dfa", "Dfb", "Dfc", "Dfd", "ET", "EF"};
    if (code < 0 || code >= kKgCount) return "?";
    return names[code];
}

}  // namespace mapgen
