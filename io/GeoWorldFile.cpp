#include "io/GeoWorldFile.h"

#include <QByteArray>
#include <QFile>
#include <QSaveFile>
#include <cstring>

#include "core/stages/Pipeline.h"
#include "mapgen/Version.h"

namespace mapgen {

namespace {

constexpr char kMagic[8] = {'G', 'E', 'O', 'W', 'O', 'R', 'L', 'D'};
constexpr char kEndMagic[8] = {'G', 'W', 'E', 'N', 'D', 0, 0, 0};
constexpr uint32_t kFormatVersion = 1;

struct Entry {
    std::string name;
    uint64_t offset = 0, stored = 0, raw = 0;
    uint32_t crc = 0;
    uint8_t compression = 0;
};

template <class T>
void put(QByteArray& b, T v) {
    char tmp[sizeof(T)];
    std::memcpy(tmp, &v, sizeof(T));
    b.append(tmp, sizeof(T));
}

template <class T>
bool get(const char*& p, const char* end, T& v) {
    if (end - p < static_cast<ptrdiff_t>(sizeof(T))) return false;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return true;
}

Json lineToJson(const Polyline& l) {
    Json a = Json::array();
    for (auto& g : l.pts) {
        Json pt = Json::array();
        pt.push(g.lon);
        pt.push(g.lat);
        a.push(pt);
    }
    return a;
}

Polyline lineFromJson(const Json& j) {
    Polyline l;
    for (auto& pt : j.items()) l.pts.push_back({pt[1].asNumber(), pt[0].asNumber()});
    return l;
}

}  // namespace

uint32_t crc32(const void* data, size_t len, uint32_t crc) {
    static uint32_t table[256];
    static bool init = [] {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        return true;
    }();
    (void)init;
    crc = ~crc;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

Json featuresToJson(const WorldData& w) {
    Json j = Json::object();
    if (w.tectonics) {
        Json t = Json::object();
        Json plates = Json::array();
        for (auto& p : w.tectonics->plates) {
            Json o = Json::object();
            o["id"] = p.id; o["seedLat"] = p.seedLat; o["seedLon"] = p.seedLon; o["poleLat"] = p.poleLat;
            o["poleLon"] = p.poleLon; o["omega"] = p.omegaDegPerMyr; o["weight"] = p.weight; o["area"] = p.areaKm2;
            o["continental"] = p.continentalFraction; o["rigidity"] = p.rigidity; o["cLat"] = p.centroidLat;
            o["cLon"] = p.centroidLon; o["speed"] = p.meanSpeedCmYr;
            plates.push(o);
        }
        t["plates"] = plates;
        Json bs = Json::array();
        for (auto& b : w.tectonics->boundaries) {
            Json o = Json::object();
            o["a"] = b.plateA; o["b"] = b.plateB; o["type"] = b.type; o["cls"] = b.cls; o["conv"] = b.convergenceCmYr;
            o["shear"] = b.shearCmYr; o["sub"] = b.subductingPlate; o["line"] = lineToJson(b.line);
            bs.push(o);
        }
        t["boundaries"] = bs;
        Json su = Json::array();
        for (auto& s : w.tectonics->oldSutures) su.push(lineToJson(s));
        t["sutures"] = su;
        t["crustThreshold"] = w.tectonics->crustThreshold;
        j["tectonics"] = t;
    }
    if (w.geomorph) {
        Json g = Json::object();
        Json hs = Json::array();
        for (auto& h : w.geomorph->hotspots) {
            Json o = Json::object();
            o["id"] = h.id; o["lat"] = h.lat; o["lon"] = h.lon; o["strength"] = h.strength; o["plate"] = h.plateId;
            o["oceanic"] = h.oceanic; o["onRidge"] = h.onRidge;
            hs.push(o);
        }
        g["hotspots"] = hs;
        Json vs = Json::array();
        for (auto& v : w.geomorph->volcanoes) {
            Json o = Json::object();
            o["id"] = v.id; o["lat"] = v.lat; o["lon"] = v.lon; o["cell"] = static_cast<double>(v.cell);
            o["genesis"] = v.genesis; o["morph"] = v.morphology; o["height"] = v.edificeHeightM; o["age"] = v.ageMyr;
            o["plate"] = v.plateId; o["hotspot"] = v.hotspotId;
            vs.push(o);
        }
        g["volcanoes"] = vs;
        g["shift"] = w.geomorph->calibrationShiftM;
        j["geomorph"] = g;
    }
    if (w.ocean) {
        Json o = Json::object();
        o["seaLevel"] = w.ocean->seaLevelM;
        o["landFraction"] = w.ocean->landFraction;
        o["components"] = w.ocean->oceanComponents;
        Json cs = Json::array();
        for (auto& c : w.ocean->coastlines) cs.push(lineToJson(c));
        o["coastlines"] = cs;
        Json ar = Json::array();
        for (double a : w.ocean->oceanAreasKm2) ar.push(a);
        o["oceanAreas"] = ar;
        j["ocean"] = o;
    }
    if (w.hydro) {
        Json h = Json::object();
        Json rs = Json::array();
        for (auto& r : w.hydro->rivers) {
            Json o = Json::object();
            o["id"] = r.id; o["order"] = r.order; o["q0"] = r.dischargeStart; o["q1"] = r.dischargeEnd;
            o["len"] = r.lengthKm; o["slope"] = r.slope; o["basin"] = r.basinId; o["down"] = r.downstream;
            o["class"] = r.riverClass; o["meander"] = r.meandering; o["line"] = lineToJson(r.line);
            Json cells = Json::array();
            for (auto c : r.cells) cells.push(static_cast<double>(c));
            o["cells"] = cells;
            rs.push(o);
        }
        h["rivers"] = rs;
        Json ls = Json::array();
        for (auto& l : w.hydro->lakes) {
            Json o = Json::object();
            o["id"] = l.id; o["lat"] = l.lat; o["lon"] = l.lon; o["area"] = l.areaKm2; o["level"] = l.levelM;
            o["depth"] = l.maxDepthM; o["volume"] = l.volumeKm3; o["inflow"] = l.inflowM3s; o["evap"] = l.evaporationM3s;
            o["outflow"] = l.outflowM3s; o["genesis"] = l.genesis; o["regime"] = l.regime; o["saline"] = l.saline;
            o["basin"] = l.basinId; o["outlet"] = static_cast<double>(l.outletCell); o["cells"] = l.cells;
            ls.push(o);
        }
        h["lakes"] = ls;
        Json bs = Json::array();
        for (auto& b : w.hydro->basins) {
            Json o = Json::object();
            o["id"] = b.id; o["outlet"] = static_cast<double>(b.outletCell); o["type"] = b.outlet; o["area"] = b.areaKm2;
            o["q"] = b.dischargeM3s; o["main"] = b.mainRiverKm; o["cells"] = b.cells;
            bs.push(o);
        }
        h["basins"] = bs;
        Json fs = Json::array();
        for (auto& f : w.hydro->waterfalls) {
            Json o = Json::object();
            o["lat"] = f.lat; o["lon"] = f.lon; o["drop"] = f.dropM; o["river"] = f.riverId; o["rapids"] = f.rapids;
            fs.push(o);
        }
        h["waterfalls"] = fs;
        Json ds = Json::array();
        for (auto& d : w.hydro->deltas) {
            Json o = Json::object();
            o["mouth"] = static_cast<double>(d.mouthCell); o["lat"] = d.lat; o["lon"] = d.lon; o["branches"] = d.branches;
            o["q"] = d.dischargeM3s; o["river"] = d.riverId;
            ds.push(o);
        }
        h["deltas"] = ds;
        h["lakeIterations"] = w.hydro->climateLakeIterations;
        j["hydro"] = h;
    }
    if (w.geoActivity) {
        Json g = Json::object();
        Json vs = Json::array();
        for (auto& v : w.geoActivity->volcanoStates) {
            Json o = Json::object();
            o["id"] = v.volcanoId; o["activity"] = v.activity; o["submarine"] = v.submarine; o["summit"] = v.summitM;
            o["hazard"] = v.hazardClass; o["zone"] = v.zone;
            vs.push(o);
        }
        g["volcanoStates"] = vs;
        Json ss = Json::array();
        for (auto& s : w.geoActivity->springs) {
            Json o = Json::object();
            o["lat"] = s.lat; o["lon"] = s.lon; o["geyser"] = s.geyser; o["p"] = s.potential; o["cell"] = static_cast<double>(s.cell);
            ss.push(o);
        }
        g["springs"] = ss;
        j["geoActivity"] = g;
    }
    return j;
}

void featuresFromJson(const Json& j, WorldData& w) {
    if (j.has("tectonics")) {
        auto t = std::make_shared<TectonicsFeatures>();
        const Json& tj = j["tectonics"];
        for (auto& o : tj["plates"].items()) {
            Plate p;
            p.id = o["id"].asInt(); p.seedLat = o["seedLat"].asNumber(); p.seedLon = o["seedLon"].asNumber();
            p.poleLat = o["poleLat"].asNumber(); p.poleLon = o["poleLon"].asNumber(); p.omegaDegPerMyr = o["omega"].asNumber();
            p.weight = o["weight"].asNumber(1); p.areaKm2 = o["area"].asNumber(); p.continentalFraction = o["continental"].asNumber();
            p.rigidity = o["rigidity"].asNumber(1); p.centroidLat = o["cLat"].asNumber(); p.centroidLon = o["cLon"].asNumber();
            p.meanSpeedCmYr = o["speed"].asNumber();
            t->plates.push_back(p);
        }
        for (auto& o : tj["boundaries"].items()) {
            BoundarySegment b;
            b.plateA = o["a"].asInt(); b.plateB = o["b"].asInt(); b.type = static_cast<uint8_t>(o["type"].asInt());
            b.cls = static_cast<uint8_t>(o["cls"].asInt()); b.convergenceCmYr = static_cast<float>(o["conv"].asNumber());
            b.shearCmYr = static_cast<float>(o["shear"].asNumber()); b.subductingPlate = o["sub"].asInt(-1);
            b.line = lineFromJson(o["line"]);
            t->boundaries.push_back(b);
        }
        for (auto& s : tj["sutures"].items()) t->oldSutures.push_back(lineFromJson(s));
        t->crustThreshold = tj["crustThreshold"].asNumber();
        w.tectonics = t;
    }
    if (j.has("geomorph")) {
        auto g = std::make_shared<GeomorphFeatures>();
        const Json& gj = j["geomorph"];
        for (auto& o : gj["hotspots"].items()) {
            Hotspot h;
            h.id = o["id"].asInt(); h.lat = o["lat"].asNumber(); h.lon = o["lon"].asNumber(); h.strength = o["strength"].asNumber(1);
            h.plateId = o["plate"].asInt(); h.oceanic = o["oceanic"].asBool(true); h.onRidge = o["onRidge"].asBool();
            g->hotspots.push_back(h);
        }
        for (auto& o : gj["volcanoes"].items()) {
            Volcano v;
            v.id = o["id"].asInt(); v.lat = o["lat"].asNumber(); v.lon = o["lon"].asNumber();
            v.cell = static_cast<int64_t>(o["cell"].asNumber()); v.genesis = static_cast<uint8_t>(o["genesis"].asInt());
            v.morphology = static_cast<uint8_t>(o["morph"].asInt()); v.edificeHeightM = static_cast<float>(o["height"].asNumber());
            v.ageMyr = static_cast<float>(o["age"].asNumber()); v.plateId = o["plate"].asInt(); v.hotspotId = o["hotspot"].asInt(-1);
            g->volcanoes.push_back(v);
        }
        g->calibrationShiftM = gj["shift"].asNumber();
        w.geomorph = g;
    }
    if (j.has("ocean")) {
        auto o = std::make_shared<OceanFeatures>();
        const Json& oj = j["ocean"];
        o->seaLevelM = oj["seaLevel"].asNumber();
        o->landFraction = oj["landFraction"].asNumber();
        o->oceanComponents = oj["components"].asInt();
        for (auto& c : oj["coastlines"].items()) o->coastlines.push_back(lineFromJson(c));
        for (auto& a : oj["oceanAreas"].items()) o->oceanAreasKm2.push_back(a.asNumber());
        w.ocean = o;
    }
    if (j.has("hydro")) {
        auto h = std::make_shared<HydroFeatures>();
        const Json& hj = j["hydro"];
        for (auto& o : hj["rivers"].items()) {
            RiverSegment r;
            r.id = o["id"].asInt(); r.order = o["order"].asInt(1); r.dischargeStart = static_cast<float>(o["q0"].asNumber());
            r.dischargeEnd = static_cast<float>(o["q1"].asNumber()); r.lengthKm = static_cast<float>(o["len"].asNumber());
            r.slope = static_cast<float>(o["slope"].asNumber()); r.basinId = o["basin"].asInt(-1); r.downstream = o["down"].asInt(-1);
            r.riverClass = static_cast<uint8_t>(o["class"].asInt(1)); r.meandering = o["meander"].asBool();
            r.line = lineFromJson(o["line"]);
            for (auto& c : o["cells"].items()) r.cells.push_back(static_cast<int64_t>(c.asNumber()));
            h->rivers.push_back(std::move(r));
        }
        for (auto& o : hj["lakes"].items()) {
            Lake l;
            l.id = o["id"].asInt(); l.lat = o["lat"].asNumber(); l.lon = o["lon"].asNumber();
            l.areaKm2 = static_cast<float>(o["area"].asNumber()); l.levelM = static_cast<float>(o["level"].asNumber());
            l.maxDepthM = static_cast<float>(o["depth"].asNumber()); l.volumeKm3 = static_cast<float>(o["volume"].asNumber());
            l.inflowM3s = static_cast<float>(o["inflow"].asNumber()); l.evaporationM3s = static_cast<float>(o["evap"].asNumber());
            l.outflowM3s = static_cast<float>(o["outflow"].asNumber()); l.genesis = static_cast<uint8_t>(o["genesis"].asInt());
            l.regime = static_cast<uint8_t>(o["regime"].asInt()); l.saline = o["saline"].asBool(); l.basinId = o["basin"].asInt(-1);
            l.outletCell = static_cast<int64_t>(o["outlet"].asNumber(-1)); l.cells = o["cells"].asInt();
            h->lakes.push_back(l);
        }
        for (auto& o : hj["basins"].items()) {
            Basin b;
            b.id = o["id"].asInt(); b.outletCell = static_cast<int64_t>(o["outlet"].asNumber(-1));
            b.outlet = static_cast<uint8_t>(o["type"].asInt()); b.areaKm2 = static_cast<float>(o["area"].asNumber());
            b.dischargeM3s = static_cast<float>(o["q"].asNumber()); b.mainRiverKm = static_cast<float>(o["main"].asNumber());
            b.cells = o["cells"].asInt();
            h->basins.push_back(b);
        }
        for (auto& o : hj["waterfalls"].items()) {
            Waterfall f;
            f.lat = o["lat"].asNumber(); f.lon = o["lon"].asNumber(); f.dropM = static_cast<float>(o["drop"].asNumber());
            f.riverId = o["river"].asInt(-1); f.rapids = o["rapids"].asBool();
            h->waterfalls.push_back(f);
        }
        for (auto& o : hj["deltas"].items()) {
            Delta d;
            d.mouthCell = static_cast<int64_t>(o["mouth"].asNumber(-1)); d.lat = o["lat"].asNumber(); d.lon = o["lon"].asNumber();
            d.branches = o["branches"].asInt(); d.dischargeM3s = static_cast<float>(o["q"].asNumber()); d.riverId = o["river"].asInt(-1);
            h->deltas.push_back(d);
        }
        h->climateLakeIterations = hj["lakeIterations"].asInt();
        w.hydro = h;
    }
    if (j.has("geoActivity")) {
        auto g = std::make_shared<GeoActivityFeatures>();
        const Json& gj = j["geoActivity"];
        for (auto& o : gj["volcanoStates"].items()) {
            VolcanoState v;
            v.volcanoId = o["id"].asInt(); v.activity = static_cast<uint8_t>(o["activity"].asInt());
            v.submarine = o["submarine"].asBool(); v.summitM = static_cast<float>(o["summit"].asNumber());
            v.hazardClass = static_cast<uint8_t>(o["hazard"].asInt()); v.zone = static_cast<uint8_t>(o["zone"].asInt());
            g->volcanoStates.push_back(v);
        }
        for (auto& o : gj["springs"].items()) {
            ThermalSpring s;
            s.lat = o["lat"].asNumber(); s.lon = o["lon"].asNumber(); s.geyser = o["geyser"].asBool();
            s.potential = static_cast<float>(o["p"].asNumber()); s.cell = static_cast<int64_t>(o["cell"].asNumber());
            g->springs.push_back(s);
        }
        w.geoActivity = g;
    }
}

bool saveGeoWorld(const WorldData& w, const QString& path, std::string* errorRu, const FileProgress& fp) {
    auto fail = [&](const std::string& m) {
        if (errorRu) *errorRu = m;
        return false;
    };
    // Manifest.
    Json man = Json::object();
    man["format"] = "geoworld";
    man["formatVersion"] = static_cast<int>(kFormatVersion);
    man["seed"] = w.seed;
    man["generatorVersion"] = w.generatorVersion.empty() ? std::string(kGeneratorVersion) : w.generatorVersion;
    man["algorithmVersion"] = w.algorithmVersion;
    man["schemaVersion"] = w.schemaVersion;
    man["savedBy"] = std::string("MapGen ") + kGeneratorVersion;
    man["params"] = w.params.toJson();
    Json grid = Json::object();
    grid["width"] = w.grid.width();
    grid["height"] = w.grid.height();
    grid["radiusKm"] = w.grid.radiusKm();
    grid["climateWidth"] = w.climateGrid.width();
    grid["climateHeight"] = w.climateGrid.height();
    grid["projection"] = "equirectangular, cell-centred, longitude -180..180 east, latitude +90..-90";
    man["grid"] = grid;
    man["completedStage"] = w.completedStage;
    Json st = Json::array();
    for (auto& h : w.stageHash) st.push(h);
    man["stageHashes"] = st;
    Json reps = Json::array();
    for (auto& r : w.reports) reps.push(r.toJson());
    man["quality"] = reps;
    man["statistics"] = w.statistics;
    Json li = Json::array();

    std::vector<std::pair<std::string, QByteArray>> blobs;  // name, raw
    int total = 0;
    for (auto& l : w.layers) if (l) ++total;
    int done = 0;
    for (auto& info : allLayers()) {
        auto& l = w.layers[static_cast<size_t>(info.id)];
        if (!l) continue;
        if (fp.cancelled && fp.cancelled()) return fail("Сохранение отменено.");
        Json e = Json::object();
        e["key"] = info.key;
        e["type"] = static_cast<int>(l->type);
        e["grid"] = l->grid == GridKind::Climate ? "climate" : "full";
        e["width"] = l->width;
        e["height"] = l->height;
        e["bands"] = l->bands;
        e["scale"] = info.scale;
        e["units"] = info.units;
        std::string h = w.layerHash[static_cast<size_t>(info.id)];
        if (h.empty()) h = Pipeline::hashLayer(*l);
        e["hash"] = h;
        li.push(e);
        blobs.emplace_back(std::string("layers/") + info.key + ".bin",
                           QByteArray::fromRawData(reinterpret_cast<const char*>(l->bytes.data()), static_cast<qsizetype>(l->bytes.size())));
        if (fp.progress) fp.progress(0.1 * (++done) / std::max(1, total));
    }
    man["layers"] = li;
    std::string manText = man.dump(1);
    std::string featText = featuresToJson(w).dump(0);
    blobs.insert(blobs.begin(), {"manifest.json", QByteArray(manText.data(), static_cast<qsizetype>(manText.size()))});
    blobs.insert(blobs.begin() + 1, {"features/all.json", QByteArray(featText.data(), static_cast<qsizetype>(featText.size()))});

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return fail("Не удалось открыть файл для записи: " + f.errorString().toStdString());
    QByteArray header;
    header.append(kMagic, 8);
    put<uint32_t>(header, kFormatVersion);
    put<uint32_t>(header, static_cast<uint32_t>(blobs.size()));
    put<uint64_t>(header, 0);  // patched below
    if (f.write(header) != header.size()) return fail("Ошибка записи: " + f.errorString().toStdString());
    uint64_t offset = static_cast<uint64_t>(header.size());
    std::vector<Entry> toc;
    for (size_t i = 0; i < blobs.size(); ++i) {
        if (fp.cancelled && fp.cancelled()) { f.cancelWriting(); return fail("Сохранение отменено."); }
        const QByteArray& raw = blobs[i].second;
        Entry e;
        e.name = blobs[i].first;
        e.raw = static_cast<uint64_t>(raw.size());
        e.crc = crc32(raw.constData(), static_cast<size_t>(raw.size()));
        QByteArray stored = qCompress(raw, 6);
        e.compression = 1;
        if (stored.size() >= raw.size()) { stored = raw; e.compression = 0; }
        e.offset = offset;
        e.stored = static_cast<uint64_t>(stored.size());
        if (f.write(stored) != stored.size()) { f.cancelWriting(); return fail("Ошибка записи (возможно, нет места на диске): " + f.errorString().toStdString()); }
        offset += e.stored;
        toc.push_back(e);
        if (fp.progress) fp.progress(0.1 + 0.85 * (i + 1) / blobs.size());
    }
    QByteArray tb;
    for (auto& e : toc) {
        put<uint16_t>(tb, static_cast<uint16_t>(e.name.size()));
        tb.append(e.name.data(), static_cast<qsizetype>(e.name.size()));
        put<uint64_t>(tb, e.offset);
        put<uint64_t>(tb, e.stored);
        put<uint64_t>(tb, e.raw);
        put<uint32_t>(tb, e.crc);
        put<uint8_t>(tb, e.compression);
    }
    put<uint32_t>(tb, crc32(tb.constData(), static_cast<size_t>(tb.size())));
    tb.append(kEndMagic, 8);
    if (f.write(tb) != tb.size()) { f.cancelWriting(); return fail("Ошибка записи: " + f.errorString().toStdString()); }
    // Patch TOC offset in the header.
    if (!f.seek(16)) { f.cancelWriting(); return fail("Ошибка позиционирования в файле."); }
    QByteArray off;
    put<uint64_t>(off, offset);
    f.write(off);
    if (!f.commit()) return fail("Не удалось завершить атомарную запись: " + f.errorString().toStdString());
    if (fp.progress) fp.progress(1.0);
    return true;
}

LoadResult loadGeoWorld(const QString& path, WorldData& out, const FileProgress& fp) {
    LoadResult r;
    auto fail = [&](const std::string& m) {
        r.ok = false;
        r.errorRu = m;
        return r;
    };
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail("Не удалось открыть файл: " + f.errorString().toStdString());
    const qint64 fileSize = f.size();
    if (fileSize < 32) return fail("Файл повреждён или не является проектом .geoworld (слишком короткий).");
    // Memory-mapped read: large worlds are not copied into RAM twice.
    QByteArray fallback;
    const char* base = reinterpret_cast<const char*>(f.map(0, fileSize));
    if (!base) {
        fallback = f.readAll();
        base = fallback.constData();
    }
    const char* p = base;
    const char* end = p + fileSize;
    if (std::memcmp(p, kMagic, 8) != 0) return fail("Это не файл .geoworld (нет сигнатуры).");
    p += 8;
    uint32_t fmt = 0, count = 0;
    uint64_t tocOff = 0;
    get(p, end, fmt);
    get(p, end, count);
    get(p, end, tocOff);
    if (fmt > kFormatVersion) return fail("Файл создан более новой версией формата (" + std::to_string(fmt) + ").");
    if (std::memcmp(end - 8, kEndMagic, 8) != 0) return fail("Файл повреждён: нет завершающей сигнатуры (неполная запись?).");
    if (tocOff >= static_cast<uint64_t>(fileSize)) return fail("Файл повреждён: неверное смещение оглавления.");
    const char* tp = base + tocOff;
    const char* tend = end - 8 - 4;
    uint32_t tocCrc = 0;
    std::memcpy(&tocCrc, tend, 4);
    if (crc32(tp, static_cast<size_t>(tend - tp)) != tocCrc) return fail("Файл повреждён: контрольная сумма оглавления не совпадает.");
    std::vector<Entry> toc;
    for (uint32_t i = 0; i < count; ++i) {
        Entry e;
        uint16_t nl = 0;
        if (!get(tp, tend, nl) || tend - tp < nl) return fail("Файл повреждён: оглавление.");
        e.name.assign(tp, nl);
        tp += nl;
        if (!get(tp, tend, e.offset) || !get(tp, tend, e.stored) || !get(tp, tend, e.raw) || !get(tp, tend, e.crc) ||
            !get(tp, tend, e.compression))
            return fail("Файл повреждён: оглавление.");
        if (e.offset + e.stored > tocOff) return fail("Файл повреждён: запись «" + e.name + "» выходит за пределы данных.");
        toc.push_back(e);
    }
    auto readEntry = [&](const Entry& e, QByteArray& raw) -> bool {
        QByteArray stored = QByteArray::fromRawData(base + e.offset, static_cast<qsizetype>(e.stored));
        raw = e.compression ? qUncompress(stored) : QByteArray(stored.constData(), stored.size());
        if (static_cast<uint64_t>(raw.size()) != e.raw) return false;
        return crc32(raw.constData(), static_cast<size_t>(raw.size())) == e.crc;
    };
    auto find = [&](const std::string& name) -> const Entry* {
        for (auto& e : toc) if (e.name == name) return &e;
        return nullptr;
    };
    const Entry* me = find("manifest.json");
    if (!me) return fail("В файле нет манифеста.");
    QByteArray manRaw;
    if (!readEntry(*me, manRaw)) return fail("Манифест повреждён (CRC).");
    Json man;
    try {
        man = Json::parse(manRaw.toStdString());
    } catch (const std::exception& ex) {
        return fail(std::string("Манифест не читается: ") + ex.what());
    }
    WorldData w;
    w.seed = man["seed"].asString(std::string());
    w.generatorVersion = man["generatorVersion"].asString(std::string());
    w.algorithmVersion = man["algorithmVersion"].asInt();
    w.schemaVersion = man["schemaVersion"].asInt();
    w.params = Params::fromJson(man["params"], &r.warningsRu);
    const Json& grid = man["grid"];
    try {
        w.grid = SphereGrid(grid["width"].asInt(), grid["height"].asInt(), grid["radiusKm"].asNumber(6371));
        w.climateGrid = SphereGrid(grid["climateWidth"].asInt(), grid["climateHeight"].asInt(), grid["radiusKm"].asNumber(6371));
    } catch (const std::exception&) {
        return fail("Неверные размеры сетки в манифесте.");
    }
    w.completedStage = man["completedStage"].asInt(-1);
    for (size_t s = 0; s < w.stageHash.size() && s < man["stageHashes"].size(); ++s) w.stageHash[s] = man["stageHashes"][s].asString(std::string());
    for (size_t s = 0; s < w.reports.size() && s < man["quality"].size(); ++s) w.reports[s] = QualityReport::fromJson(man["quality"][s]);
    w.statistics = man["statistics"];
    bool allHashes = true;
    size_t n = man["layers"].size(), i = 0;
    for (auto& e : man["layers"].items()) {
        if (fp.cancelled && fp.cancelled()) return fail("Открытие отменено.");
        LayerId id;
        std::string key = e["key"].asString(std::string());
        if (!layerFromKey(key, id)) {
            r.warningsRu.push_back("Неизвестный слой «" + key + "» пропущен.");
            continue;
        }
        const Entry* le = find("layers/" + key + ".bin");
        if (!le) return fail("В файле нет данных слоя «" + key + "».");
        QByteArray raw;
        if (!readEntry(*le, raw)) return fail("Слой «" + key + "» повреждён (CRC).");
        auto l = w.makeLayer(id);
        if (static_cast<size_t>(raw.size()) != l->bytes.size()) return fail("Размер слоя «" + key + "» не соответствует сетке.");
        std::memcpy(l->bytes.data(), raw.constData(), l->bytes.size());
        std::string expected = e["hash"].asString(std::string());
        std::string actual = Pipeline::hashLayer(*l);
        if (expected != actual) allHashes = false;
        w.setLayer(id, l);
        w.layerHash[static_cast<size_t>(id)] = actual;
        if (fp.progress) fp.progress(0.9 * (++i) / std::max<size_t>(1, n));
    }
    if (!allHashes) return fail("Контрольные хеши слоёв не совпадают с манифестом: данные повреждены.");
    r.hashesVerified = true;
    if (const Entry* fe = find("features/all.json")) {
        QByteArray raw;
        if (!readEntry(*fe, raw)) return fail("Векторные объекты повреждены (CRC).");
        try {
            featuresFromJson(Json::parse(raw.toStdString()), w);
        } catch (const std::exception& ex) {
            return fail(std::string("Векторные объекты не читаются: ") + ex.what());
        }
    }
    if (w.algorithmVersion != kAlgorithmVersion) {
        r.versionMismatch = true;
        w.storedResultOnly = true;
        w.noticeRu = "Мир сохранён версией алгоритма v" + std::to_string(w.algorithmVersion) + " (текущая v" +
                     std::to_string(kAlgorithmVersion) +
                     "). Открыт как сохранённый результат; повторная генерация даст другой мир.";
        r.warningsRu.push_back(w.noticeRu);
    }
    out = std::move(w);
    r.ok = true;
    if (fp.progress) fp.progress(1.0);
    return r;
}

}  // namespace mapgen
