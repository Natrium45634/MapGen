#include "core/world/Params.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mapgen {

namespace {

ParamDef realParam(std::string id, double lo, double hi, double def, std::string units, std::string name,
                   std::string desc, std::string group, int stage, std::string realism = {}, double step = 0) {
    ParamDef d;
    d.id = std::move(id);
    d.type = ParamType::Real;
    d.minValue = lo;
    d.maxValue = hi;
    d.defValue = def;
    d.units = std::move(units);
    d.nameRu = std::move(name);
    d.descriptionRu = std::move(desc);
    d.group = std::move(group);
    d.stageInvalidated = stage;
    d.realismNotes = std::move(realism);
    d.step = step;
    return d;
}

ParamDef intParam(std::string id, int lo, int hi, int def, std::string units, std::string name, std::string desc,
                  std::string group, int stage, std::string realism = {}) {
    ParamDef d = realParam(std::move(id), lo, hi, def, std::move(units), std::move(name), std::move(desc),
                           std::move(group), stage, std::move(realism), 1);
    d.type = ParamType::Int;
    return d;
}

ParamDef enumParam(std::string id, std::vector<EnumOption> opts, std::string def, std::string name,
                   std::string desc, std::string group, int stage, std::string realism = {}) {
    ParamDef d;
    d.id = std::move(id);
    d.type = ParamType::Enum;
    d.options = std::move(opts);
    d.defString = std::move(def);
    d.nameRu = std::move(name);
    d.descriptionRu = std::move(desc);
    d.group = std::move(group);
    d.stageInvalidated = stage;
    d.realismNotes = std::move(realism);
    return d;
}

ParamDef boolParam(std::string id, bool def, std::string name, std::string desc, std::string group, int stage,
                   std::string realism = {}) {
    ParamDef d = realParam(std::move(id), 0, 1, def ? 1 : 0, "", std::move(name), std::move(desc), std::move(group),
                           stage, std::move(realism), 1);
    d.type = ParamType::Bool;
    return d;
}

std::vector<ParamDef> buildSchema() {
    const std::string gWorld = "Мир", gTect = "Тектоника", gRelief = "Рельеф", gOcean = "Океан и уровень моря",
                      gClimate = "Климат", gHydro = "Гидрология", gBiome = "Биомы", gQa = "Контроль";
    std::vector<ParamDef> s;
    s.push_back(enumParam("world.resolution",
                          {{"preview", "Preview 1024×512"},
                           {"standard", "Standard 2048×1024"},
                           {"high", "High 4096×2048"},
                           {"ultra", "Ultra 8192×4096"},
                           {"custom", "Своё (для тестов)"}},
                          "standard", "Разрешение карты",
                          "Размер глобальной сетки. Крупные структуры (плиты, континенты, климат) не зависят от "
                          "разрешения; выше разрешение — больше деталей рельефа, рек и берегов.",
                          gWorld, 1));
    {
        auto d = intParam("world.customWidth", 64, 8192, 512, "ячеек", "Ширина своей сетки",
                          "Используется только при разрешении «Своё»; высота = ширина / 2.", gWorld, 1);
        d.dependencies = {"world.resolution"};
        s.push_back(d);
    }
    s.push_back(realParam("world.radiusKm", 3000, 10000, 6371, "км", "Радиус планеты",
                          "Масштаб расстояний и площадей. Физические параметризации калиброваны для "
                          "землеподобного радиуса.",
                          gWorld, 1, "Вдали от 6371 км климатические и эрозионные калибровки становятся условными.", 1));

    s.push_back(intParam("tectonics.plateCountMin", 3, 40, 10, "", "Число плит, минимум",
                         "Нижняя граница случайного числа литосферных плит.", gTect, 1));
    s.push_back(intParam("tectonics.plateCountMax", 3, 48, 16, "", "Число плит, максимум",
                         "Верхняя граница случайного числа литосферных плит.", gTect, 1));
    s.push_back(realParam("tectonics.continentalCrust", 0.08, 0.85, 0.40, "доля площади",
                          "Доля континентальной коры",
                          "Площадь планеты с континентальной корой (включая шельфы). На Земле ≈ 0,4.", gTect, 1, {},
                          0.01));
    s.push_back(intParam("tectonics.landmassCount", 1, 12, 5, "", "Желаемое число крупных массивов",
                         "Мягкая цель: число групп континентальной коры. Точное число материков не гарантируется.",
                         gTect, 1, "Слияние и дробление массивов при генерации допустимы."));
    s.push_back(realParam("tectonics.fragmentation", 0.0, 1.0, 0.35, "0–1", "Раздробленность суши",
                          "0 — компактные материки, 1 — множество островов и архипелагов.", gTect, 1, {}, 0.05));
    s.push_back(realParam("tectonics.activity", 0.25, 2.0, 1.0, "×", "Тектоническая активность",
                          "Множитель скоростей плит: высота орогенов, глубина желобов, вулканизм.", gTect, 1, {}, 0.05));
    s.push_back(enumParam("tectonics.polarMode",
                          {{"free", "Свободно"},
                           {"north", "Материк на северном полюсе"},
                           {"south", "Материк на южном полюсе"},
                           {"both", "Суша на обоих полюсах"},
                           {"none", "Океан на обоих полюсах"}},
                          "free", "Полярный сценарий", "Пожелание к размещению континентальной коры у полюсов.", gTect,
                          1));

    s.push_back(realParam("relief.landFraction", 0.05, 0.95, 0.29, "доля площади", "Доля суши при формировании рельефа",
                          "Нулевая высота проекта выбирается так, чтобы выше неё оказалась эта доля площади. "
                          "Эрозия работает к этому базису.",
                          gRelief, 2, {}, 0.01));
    s.push_back(realParam("relief.age", 0.0, 1.0, 0.5, "0–1", "Относительный возраст рельефа",
                          "0 — молодой расчленённый рельеф, 1 — старый сглаженный (больше эрозии и аккумуляции).",
                          gRelief, 2, {}, 0.05));
    s.push_back(realParam("relief.mountainScale", 0.5, 1.6, 1.0, "×", "Высота горных поясов",
                          "Множитель амплитуды орогенных поднятий.", gRelief, 2, {}, 0.05));
    s.push_back(realParam("relief.hotspotIntensity", 0.0, 2.0, 1.0, "×", "Интенсивность горячих точек",
                          "Число и мощность мантийных горячих точек и их вулканических цепей.", gRelief, 2, {}, 0.05));

    s.push_back(enumParam("ocean.seaLevelMode",
                          {{"relief", "Как при формировании рельефа (0 м)"},
                           {"fraction", "Целевая доля суши"},
                           {"fixed", "Заданный уровень, м"}},
                          "relief", "Режим уровня моря",
                          "Приоритет у выбора пользователя. Изменение уровня перестраивает берега, климат и реки, "
                          "но не плиты и не рельеф.",
                          gOcean, 3));
    {
        auto d = realParam("ocean.landFraction", 0.05, 0.95, 0.29, "доля площади", "Целевая доля суши",
                           "Порог ищется по распределению высот с весами площадей ячеек.", gOcean, 3, {}, 0.01);
        d.dependencies = {"ocean.seaLevelMode"};
        s.push_back(d);
    }
    {
        auto d = realParam("ocean.seaLevel", -2000, 2000, 0, "м", "Уровень моря",
                           "Относительно нулевой высоты проекта.", gOcean, 3, {}, 10);
        d.dependencies = {"ocean.seaLevelMode"};
        s.push_back(d);
    }
    s.push_back(realParam("ocean.tidalScale", 0.0, 3.0, 1.0, "×", "Приливы (параметр)",
                          "Множитель условной приливной амплитуды: ватты, эстуарии, мангры.", gOcean, 3,
                          "Приливы не рассчитываются динамически — это параметризация по ширине шельфа.", 0.1));

    s.push_back(realParam("climate.axialTilt", 0.0, 45.0, 23.44, "°", "Наклон оси", "Определяет сезонность инсоляции.",
                          gClimate, 4, "Выше 35° — за пределами земного опыта; параметризации условны.", 0.1));
    s.push_back(realParam("climate.temperatureOffset", -15.0, 15.0, 0.0, "°C", "Парниковое смещение",
                          "Упрощённый сдвиг уходящей радиации, эквивалентный изменению средней температуры.", gClimate,
                          4, {}, 0.5));
    s.push_back(realParam("climate.seasonality", 0.5, 1.5, 1.0, "×", "Сезонность",
                          "Множитель сезонного отклика температуры (тепловая инерция суши).", gClimate, 4, {}, 0.05));
    s.push_back(realParam("climate.orographic", 0.0, 2.0, 1.0, "×", "Сила орографических осадков",
                          "Подъём воздуха на наветренных склонах и дождевые тени.", gClimate, 4, {}, 0.05));
    s.push_back(realParam("climate.humidity", 0.5, 1.5, 1.0, "×", "Общая влажность",
                          "Множитель испарения с поверхности океана.", gClimate, 4, {}, 0.05));
    s.push_back(realParam("climate.lapseRate", 4.5, 9.0, 6.5, "°C/км", "Средний вертикальный градиент",
                          "Базовый градиент; в модели он зависит от влажности.", gClimate, 4, {}, 0.1));

    s.push_back(realParam("hydro.riverThreshold", 2.0, 500.0, 30.0, "м³/с", "Порог реки",
                          "Минимальный среднегодовой расход для выделения реки.", gHydro, 5, {}, 1));

    s.push_back(boolParam("biome.extendRare", false, "Расширить условия редких биомов",
                          "Смягчает пороги мангров, муссонных и дождевых лесов. Отклонение от реализма "
                          "помечается в отчёте.",
                          gBiome, 6, "Включение — осознанное отклонение от реализма."));

    s.push_back(boolParam("realism.enabled", true, "Режим реализма",
                          "Валидатор показывает недостижимые и конфликтующие пожелания.", gQa, 0));
    return s;
}

}  // namespace

const std::vector<ParamDef>& paramSchema() {
    static const std::vector<ParamDef> s = buildSchema();
    return s;
}

const ParamDef* findParam(const std::string& id) {
    for (auto& d : paramSchema())
        if (d.id == id) return &d;
    return nullptr;
}

const std::vector<Resolution>& resolutions() {
    static const std::vector<Resolution> r = {{"preview", 1024, 512, "Preview 1024×512"},
                                              {"standard", 2048, 1024, "Standard 2048×1024"},
                                              {"high", 4096, 2048, "High 4096×2048"},
                                              {"ultra", 8192, 4096, "Ultra 8192×4096"}};
    return r;
}

Params::Params() {
    for (auto& d : paramSchema()) {
        if (d.type == ParamType::Enum || d.type == ParamType::String) strs_[d.id] = d.defString;
        else nums_[d.id] = d.defValue;
    }
}

double Params::num(const std::string& id) const {
    auto it = nums_.find(id);
    if (it == nums_.end()) throw std::out_of_range("unknown numeric parameter: " + id);
    return it->second;
}

const std::string& Params::str(const std::string& id) const {
    auto it = strs_.find(id);
    if (it == strs_.end()) throw std::out_of_range("unknown string parameter: " + id);
    return it->second;
}

bool Params::has(const std::string& id) const { return nums_.count(id) || strs_.count(id); }

void Params::set(const std::string& id, double v) {
    const ParamDef* d = findParam(id);
    if (!d || d->type == ParamType::Enum || d->type == ParamType::String)
        throw std::out_of_range("not a numeric parameter: " + id);
    if (d->type == ParamType::Int || d->type == ParamType::Bool) v = std::round(v);
    nums_[id] = v;
}

void Params::set(const std::string& id, const std::string& v) {
    const ParamDef* d = findParam(id);
    if (!d || (d->type != ParamType::Enum && d->type != ParamType::String))
        throw std::out_of_range("not a string parameter: " + id);
    strs_[id] = v;
}

int Params::gridWidth() const {
    const std::string& r = str("world.resolution");
    for (auto& res : resolutions())
        if (res.id == r) return res.width;
    int w = integer("world.customWidth");
    w = std::clamp(w, 64, 8192);
    return w - (w % 2);
}

Json Params::toJson() const {
    Json j = Json::object();
    for (auto& d : paramSchema()) {
        if (d.type == ParamType::Enum || d.type == ParamType::String) j[d.id] = str(d.id);
        else if (d.type == ParamType::Bool) j[d.id] = flag(d.id);
        else j[d.id] = num(d.id);
    }
    for (auto& kv : unknown.members()) j[kv.first] = kv.second;
    return j;
}

Params Params::fromJson(const Json& j, std::vector<std::string>* warnings) {
    Params p;
    for (auto& kv : j.members()) {
        const ParamDef* d = findParam(kv.first);
        if (!d) {
            p.unknown[kv.first] = kv.second;
            if (warnings) warnings->push_back("Неизвестный параметр «" + kv.first + "» сохранён без изменений и не используется.");
            continue;
        }
        if (d->type == ParamType::Enum) {
            std::string v = kv.second.asString(d->defString);
            bool ok = std::any_of(d->options.begin(), d->options.end(), [&](const EnumOption& o) { return o.id == v; });
            if (!ok) {
                if (warnings) warnings->push_back("Недопустимое значение «" + v + "» параметра " + d->id + " заменено значением по умолчанию.");
                v = d->defString;
            }
            p.strs_[d->id] = v;
        } else if (d->type == ParamType::String) {
            p.strs_[d->id] = kv.second.asString(d->defString);
        } else if (d->type == ParamType::Bool) {
            p.nums_[d->id] = kv.second.isBool() ? (kv.second.asBool() ? 1.0 : 0.0) : (kv.second.asNumber(d->defValue) != 0 ? 1.0 : 0.0);
        } else {
            double v = kv.second.asNumber(d->defValue);
            if (d->type == ParamType::Int) v = std::round(v);
            p.nums_[d->id] = v;
        }
    }
    return p;
}

int firstInvalidatedStage(const Params& a, const Params& b) {
    int first = 9;
    for (auto& d : paramSchema()) {
        bool changed;
        if (d.type == ParamType::Enum || d.type == ParamType::String) changed = a.str(d.id) != b.str(d.id);
        else changed = a.num(d.id) != b.num(d.id);
        if (changed) first = std::min(first, d.stageInvalidated);
    }
    return first;
}

const std::vector<Preset>& presets() {
    static const std::vector<Preset> list = [] {
        std::vector<Preset> v;
        auto add = [&](std::string id, std::string name, std::string desc, std::map<std::string, ParamRange> ranges,
                       std::map<std::string, double> values = {}, std::map<std::string, std::string> strings = {}) {
            v.push_back({std::move(id), std::move(name), std::move(desc), std::move(ranges), std::move(values), std::move(strings)});
        };
        add("earthlike", "Землеподобный",
            "Около 29% суши, несколько материков разного размера, 10–16 плит, земной наклон оси.",
            {{"tectonics.plateCountMin", {8, 12}}, {"tectonics.plateCountMax", {14, 18}},
             {"tectonics.continentalCrust", {0.36, 0.44}}, {"tectonics.landmassCount", {5, 7}},
             {"tectonics.fragmentation", {0.25, 0.45}}, {"tectonics.activity", {0.8, 1.2}},
             {"relief.landFraction", {0.27, 0.31}}, {"relief.hotspotIntensity", {0.8, 1.2}}},
            {{"tectonics.plateCountMin", 10}, {"tectonics.plateCountMax", 16}, {"tectonics.continentalCrust", 0.40},
             {"tectonics.landmassCount", 6}, {"tectonics.fragmentation", 0.35}, {"tectonics.activity", 1.0},
             {"relief.landFraction", 0.29}, {"relief.hotspotIntensity", 1.0}},
            {{"tectonics.polarMode", "free"}});
        add("archipelago", "Архипелаги", "Много мелких массивов суши, островные дуги и вулканические цепи; суши 15–25%.",
            {{"tectonics.plateCountMin", {12, 16}}, {"tectonics.plateCountMax", {20, 26}},
             {"tectonics.continentalCrust", {0.25, 0.35}}, {"tectonics.landmassCount", {8, 12}},
             {"tectonics.fragmentation", {0.75, 0.95}}, {"tectonics.activity", {1.0, 1.5}},
             {"relief.landFraction", {0.15, 0.25}}, {"relief.hotspotIntensity", {1.2, 1.8}}},
            {{"tectonics.plateCountMin", 14}, {"tectonics.plateCountMax", 24}, {"tectonics.continentalCrust", 0.30},
             {"tectonics.landmassCount", 10}, {"tectonics.fragmentation", 0.85}, {"tectonics.activity", 1.2},
             {"relief.landFraction", 0.20}, {"relief.hotspotIntensity", 1.5}},
            {{"tectonics.polarMode", "free"}});
        add("supercontinent", "Один крупный материк", "Компактный суперконтинент и мировой океан.",
            {{"tectonics.plateCountMin", {7, 10}}, {"tectonics.plateCountMax", {12, 16}},
             {"tectonics.continentalCrust", {0.32, 0.42}}, {"tectonics.landmassCount", {1, 1}},
             {"tectonics.fragmentation", {0.1, 0.25}}, {"relief.landFraction", {0.26, 0.34}}},
            {{"tectonics.plateCountMin", 8}, {"tectonics.plateCountMax", 14}, {"tectonics.continentalCrust", 0.38},
             {"tectonics.landmassCount", 1}, {"tectonics.fragmentation", 0.15}, {"relief.landFraction", 0.30}},
            {{"tectonics.polarMode", "free"}});
        add("continents", "Несколько материков", "3–6 крупных континентальных массивов.",
            {{"tectonics.plateCountMin", {9, 12}}, {"tectonics.plateCountMax", {15, 20}},
             {"tectonics.continentalCrust", {0.38, 0.46}}, {"tectonics.landmassCount", {3, 6}},
             {"tectonics.fragmentation", {0.2, 0.4}}, {"relief.landFraction", {0.28, 0.36}}},
            {{"tectonics.plateCountMin", 10}, {"tectonics.plateCountMax", 18}, {"tectonics.continentalCrust", 0.42},
             {"tectonics.landmassCount", 4}, {"tectonics.fragmentation", 0.28}, {"relief.landFraction", 0.32}},
            {{"tectonics.polarMode", "free"}});
        add("landDominant", "Преобладание суши", "Суша 55–70%, внутренние моря и обширные засушливые внутренние области.",
            {{"tectonics.plateCountMin", {7, 10}}, {"tectonics.plateCountMax", {12, 16}},
             {"tectonics.continentalCrust", {0.65, 0.80}}, {"tectonics.landmassCount", {1, 3}},
             {"tectonics.fragmentation", {0.2, 0.4}}, {"relief.landFraction", {0.55, 0.70}}},
            {{"tectonics.plateCountMin", 8}, {"tectonics.plateCountMax", 14}, {"tectonics.continentalCrust", 0.72},
             {"tectonics.landmassCount", 2}, {"tectonics.fragmentation", 0.3}, {"relief.landFraction", 0.62}},
            {{"tectonics.polarMode", "free"}});
        add("oceanPlanet", "Океаническая планета", "Суша 5–9%: острова, вулканические архипелаги, малые материки.",
            {{"tectonics.plateCountMin", {9, 12}}, {"tectonics.plateCountMax", {15, 20}},
             {"tectonics.continentalCrust", {0.08, 0.15}}, {"tectonics.landmassCount", {4, 10}},
             {"tectonics.fragmentation", {0.6, 0.9}}, {"relief.landFraction", {0.05, 0.09}},
             {"relief.hotspotIntensity", {1.2, 2.0}}},
            {{"tectonics.plateCountMin", 10}, {"tectonics.plateCountMax", 18}, {"tectonics.continentalCrust", 0.12},
             {"tectonics.landmassCount", 6}, {"tectonics.fragmentation", 0.75}, {"relief.landFraction", 0.06},
             {"relief.hotspotIntensity", 1.6}},
            {{"tectonics.polarMode", "free"}});
        add("polarContinent", "Полярный материк", "Материк на южном полюсе (ледяной щит), остальные массивы свободно.",
            {{"tectonics.continentalCrust", {0.38, 0.44}}, {"tectonics.landmassCount", {4, 6}},
             {"tectonics.fragmentation", {0.25, 0.4}}, {"relief.landFraction", {0.28, 0.32}}},
            {{"tectonics.continentalCrust", 0.41}, {"tectonics.landmassCount", 5}, {"tectonics.fragmentation", 0.3},
             {"relief.landFraction", 0.30}},
            {{"tectonics.polarMode", "south"}});
        add("noPolar", "Континенты без полюса", "Оба полюса в океане: морской лёд вместо ледяных щитов.",
            {{"tectonics.continentalCrust", {0.36, 0.44}}, {"tectonics.landmassCount", {4, 7}},
             {"tectonics.fragmentation", {0.25, 0.45}}, {"relief.landFraction", {0.27, 0.31}}},
            {{"tectonics.continentalCrust", 0.40}, {"tectonics.landmassCount", 5}, {"tectonics.fragmentation", 0.35},
             {"relief.landFraction", 0.29}},
            {{"tectonics.polarMode", "none"}});
        return v;
    }();
    return list;
}

const Preset* findPreset(const std::string& id) {
    for (auto& p : presets())
        if (p.id == id) return &p;
    return nullptr;
}

void applyPreset(Params& p, const Preset& preset) {
    Params defaults;
    // Reset everything a preset may touch to schema defaults first, keeping resolution/realism.
    for (auto& d : paramSchema()) {
        if (d.id == "world.resolution" || d.id == "world.customWidth" || d.id == "realism.enabled") continue;
        if (d.type == ParamType::Enum || d.type == ParamType::String) p.set(d.id, defaults.str(d.id));
        else p.set(d.id, defaults.num(d.id));
    }
    for (auto& [id, r] : preset.ranges) {
        auto it = preset.values.find(id);
        p.set(id, it != preset.values.end() ? it->second : 0.5 * (r.lo + r.hi));
    }
    for (auto& [id, v] : preset.values)
        if (!preset.ranges.count(id)) p.set(id, v);
    for (auto& [id, v] : preset.strings) p.set(id, v);
    // Keep the sea-level target consistent with the relief target.
    p.set("ocean.landFraction", p.num("relief.landFraction"));
}

std::vector<ValidationIssue> validateParams(const Params& p) {
    std::vector<ValidationIssue> out;
    for (auto& d : paramSchema()) {
        if (d.type == ParamType::Enum || d.type == ParamType::String) continue;
        double v = p.num(d.id);
        if (v < d.minValue || v > d.maxValue)
            out.push_back({ValidationIssue::Error,
                           "«" + d.nameRu + "» вне допустимого диапазона [" + formatNumber(d.minValue) + "; " +
                               formatNumber(d.maxValue) + "].",
                           {d.id}});
    }
    if (p.integer("tectonics.plateCountMin") > p.integer("tectonics.plateCountMax"))
        out.push_back({ValidationIssue::Error, "Минимальное число плит больше максимального.",
                       {"tectonics.plateCountMin", "tectonics.plateCountMax"}});
    if (!p.flag("realism.enabled")) return out;

    double crust = p.num("tectonics.continentalCrust");
    double land = p.num("relief.landFraction");
    int masses = p.integer("tectonics.landmassCount");
    double frag = p.num("tectonics.fragmentation");
    if (land > crust + 0.08)
        out.push_back({ValidationIssue::Conflict,
                       "Доля суши заметно больше доли континентальной коры: суше придётся занять поднятое "
                       "океаническое дно. Увеличьте долю континентальной коры или уменьшите долю суши.",
                       {"relief.landFraction", "tectonics.continentalCrust"}});
    if (land < crust - 0.30)
        out.push_back({ValidationIssue::Warning,
                       "Доля суши намного меньше доли континентальной коры: материки будут в основном затоплены "
                       "(обширные шельфовые моря).",
                       {"relief.landFraction", "tectonics.continentalCrust"}});
    if (land > 0.75 && masses > 1)
        out.push_back({ValidationIssue::Conflict,
                       "При доле суши выше 75% отдельные материки сливаются: число крупных массивов будет "
                       "трактоваться как мягкая цель.",
                       {"relief.landFraction", "tectonics.landmassCount"}});
    if (frag > 0.7 && masses <= 2)
        out.push_back({ValidationIssue::Conflict,
                       "Высокая раздробленность противоречит одному-двум крупным массивам.",
                       {"tectonics.fragmentation", "tectonics.landmassCount"}});
    if (p.str("tectonics.polarMode") == "both" && masses == 1 && frag < 0.3)
        out.push_back({ValidationIssue::Conflict,
                       "Один компактный материк не может одновременно занимать оба полюса.",
                       {"tectonics.polarMode", "tectonics.landmassCount"}});
    if (masses > 8 && crust < 0.25 && frag < 0.3)
        out.push_back({ValidationIssue::Warning, "Много массивов при малой доле коры: материки будут мелкими.",
                       {"tectonics.landmassCount", "tectonics.continentalCrust"}});
    double tilt = p.num("climate.axialTilt");
    if (tilt > 35)
        out.push_back({ValidationIssue::Warning,
                       "Наклон оси больше 35°: экстремальные сезоны, параметризации климата за пределами "
                       "земной калибровки.",
                       {"climate.axialTilt"}});
    if (tilt < 5)
        out.push_back({ValidationIssue::Info, "Наклон оси меньше 5°: сезоны почти отсутствуют.", {"climate.axialTilt"}});
    double dT = p.num("climate.temperatureOffset");
    if (std::fabs(dT) > 8)
        out.push_back({ValidationIssue::Warning,
                       dT > 0 ? "Сильное парниковое смещение: вероятна «тепличная» планета без полярных льдов."
                              : "Сильное охлаждение: вероятно обширное оледенение (ледово-альбедная обратная связь).",
                       {"climate.temperatureOffset"}});
    const std::string& mode = p.str("ocean.seaLevelMode");
    if (mode == "fixed" && std::fabs(p.num("ocean.seaLevel")) > 500)
        out.push_back({ValidationIssue::Warning,
                       "Уровень моря далеко от базиса эрозии: ожидаются затопленные долины (риасы) или осушенные "
                       "шельфы.",
                       {"ocean.seaLevel"}});
    if (mode == "fraction" && std::fabs(p.num("ocean.landFraction") - land) > 0.15)
        out.push_back({ValidationIssue::Warning,
                       "Целевая доля суши сильно отличается от доли при формировании рельефа: уровень моря сместится "
                       "далеко от базиса эрозии.",
                       {"ocean.landFraction", "relief.landFraction"}});
    if (p.flag("biome.extendRare"))
        out.push_back({ValidationIssue::Info,
                       "Включено расширение условий редких биомов — это осознанное отклонение от реализма.",
                       {"biome.extendRare"}});
    return out;
}

}  // namespace mapgen
