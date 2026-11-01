#include "io/Export.h"

#include <QDir>
#include <QFile>
#include <QPainter>
#include <QSaveFile>
#include <QTextStream>
#include <cmath>

#include "core/stages/Pipeline.h"
#include "core/world/Categories.h"
#include "io/GeoWorldFile.h"
#include "io/Tiff.h"
#include "mapgen/Version.h"

namespace mapgen {

namespace {

bool writeAll(const QString& path, const QByteArray& data, std::string* errorRu) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (errorRu) *errorRu = "Не удалось открыть файл: " + f.errorString().toStdString();
        return false;
    }
    if (f.write(data) != data.size()) {
        if (errorRu) *errorRu = "Ошибка записи (возможно, нет места на диске): " + f.errorString().toStdString();
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {
        if (errorRu) *errorRu = "Не удалось завершить запись: " + f.errorString().toStdString();
        return false;
    }
    return true;
}

QString sidecarPath(const QString& path) {
    int dot = path.lastIndexOf('.');
    return (dot > 0 ? path.left(dot) : path) + ".json";
}

std::string csvEscape(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += "\"\"";
        else o.push_back(c);
    }
    return o + "\"";
}

std::string fmt(double v, int prec = 4) {
    if (!std::isfinite(v)) return "";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", prec, v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

}  // namespace

Json sidecarJson(const WorldData& w, const std::string& encoding) {
    Json j = Json::object();
    j["generator"] = std::string("MapGen ") + kGeneratorVersion;
    j["seed"] = w.seed;
    j["algorithmVersion"] = w.algorithmVersion;
    j["worldHash"] = Pipeline::worldHash(w);
    j["projection"] = "Equirectangular (plate carrée), cell-centred; x: longitude −180…180° eastwards, y: latitude +90…−90°";
    Json d = Json::object();
    d["type"] = "sphere";
    d["radiusM"] = w.grid.radiusKm() * 1000.0;
    d["note"] = "Planet of the generated world, NOT the Earth WGS84 ellipsoid.";
    j["datum"] = d;
    j["width"] = w.grid.width();
    j["height"] = w.grid.height();
    j["degreesPerPixel"] = 360.0 / w.grid.width();
    j["seaLevelM"] = w.seaLevel();
    j["elevationReference"] = "metres relative to the project zero; sea level given above";
    j["encoding"] = encoding;
    return j;
}

void paintLegend(QPainter& p, const Legend& L, const QRectF& area, double s) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(area, QColor(255, 255, 255, 235));
    p.setPen(QColor(60, 60, 60));
    p.drawRect(area.adjusted(0.5, 0.5, -0.5, -0.5));
    QFont f = p.font();
    f.setPointSizeF(10 * s);
    f.setBold(true);
    p.setFont(f);
    double x = area.left() + 12 * s, y = area.top() + 10 * s, wdt = area.width() - 24 * s;
    QRectF tr(x, y, wdt, 60 * s);
    QRectF br;
    p.drawText(tr, Qt::TextWordWrap, QString::fromStdString(L.title + (L.units.empty() ? "" : ", " + L.units)), &br);
    y = br.bottom() + 8 * s;
    f.setBold(false);
    f.setPointSizeF(8.5 * s);
    p.setFont(f);
    if (L.continuous) {
        double bh = 16 * s;
        int steps = 200;
        for (int i = 0; i < steps; ++i) {
            double v = L.lo + (L.hi - L.lo) * i / (steps - 1.0);
            Rgb c = L.ramp.at(v);
            p.fillRect(QRectF(x + wdt * i / steps, y, wdt / steps + 1, bh), QColor(c.r, c.g, c.b));
        }
        p.setPen(QColor(40, 40, 40));
        p.drawRect(QRectF(x, y, wdt, bh));
        for (auto& [v, label] : L.ticks) {
            double tx = x + wdt * (v - L.lo) / (L.hi - L.lo);
            if (tx < x - 1 || tx > x + wdt + 1) continue;
            p.drawLine(QPointF(tx, y + bh), QPointF(tx, y + bh + 4 * s));
            QRectF lr(tx - 40 * s, y + bh + 4 * s, 80 * s, 14 * s);
            p.drawText(lr, Qt::AlignHCenter | Qt::AlignTop, QString::fromStdString(label));
        }
        y += bh + 24 * s;
    }
    for (auto& it : L.items) {
        if (y > area.bottom() - 18 * s) break;
        p.fillRect(QRectF(x, y + 1 * s, 18 * s, 12 * s), QColor(it.color.r, it.color.g, it.color.b));
        p.setPen(QColor(90, 90, 90));
        p.drawRect(QRectF(x, y + 1 * s, 18 * s, 12 * s));
        p.setPen(QColor(30, 30, 30));
        QRectF lr(x + 24 * s, y, wdt - 24 * s, 40 * s);
        QRectF used;
        p.drawText(lr, Qt::TextWordWrap, QString::fromStdString(it.label), &used);
        y = std::max(y + 16 * s, used.bottom() + 3 * s);
    }
    y += 6 * s;
    f.setPointSizeF(7.5 * s);
    f.setItalic(true);
    p.setFont(f);
    p.setPen(QColor(70, 70, 70));
    std::vector<std::string> notes = L.notes;
    if (!L.overlayNote.empty()) notes.push_back(L.overlayNote);
    for (auto& n : notes) {
        if (y > area.bottom() - 14 * s) break;
        QRectF used;
        p.drawText(QRectF(x, y, wdt, area.bottom() - y), Qt::TextWordWrap, QString::fromStdString(n), &used);
        y = used.bottom() + 4 * s;
    }
    p.restore();
}

QImage renderLayerImage(const WorldData& w, MapTab tab, const ViewOptions& view, bool withLegend, bool withOverlays, int scale) {
    QImage base = colorizeTab(w, tab, view);
    if (base.isNull()) return base;
    scale = std::clamp(scale, 1, 8);
    QImage map = scale == 1 ? base : base.scaled(base.width() * scale, base.height() * scale, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    int legendW = withLegend ? std::max(300, map.height() / 3) : 0;
    QImage out(map.width() + legendW, map.height(), QImage::Format_ARGB32);
    out.fill(QColor(255, 255, 255));
    QPainter p(&out);
    p.drawImage(0, 0, map);
    if (withOverlays) {
        MapProjection pr;
        pr.scale = scale;
        pr.W = w.grid.width();
        pr.H = w.grid.height();
        p.setClipRect(QRect(0, 0, map.width(), map.height()));
        paintOverlays(p, w, tab, view, OverlayOptions{}, pr, QRectF(0, 0, map.width(), map.height()));
        p.setClipping(false);
    }
    if (withLegend) {
        double s = std::clamp(map.height() / 1024.0, 0.8, 3.0);
        paintLegend(p, legendFor(w, tab, view), QRectF(map.width(), 0, legendW, map.height()), s);
    }
    p.end();
    return out;
}

bool exportLayerPng(const WorldData& w, MapTab tab, const ViewOptions& view, bool withLegend, const QString& path, std::string* errorRu) {
    QImage img = renderLayerImage(w, tab, view, withLegend, true);
    if (img.isNull()) {
        if (errorRu) *errorRu = "Нет данных для экспорта.";
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || !img.save(&f, "PNG") || !f.commit()) {
        if (errorRu) *errorRu = "Не удалось записать PNG: " + f.errorString().toStdString();
        return false;
    }
    Json side = sidecarJson(w, "8-bit RGB rendering of a map tab — NOT elevation data");
    side["tab"] = tabInfo(tab).id;
    side["month"] = view.month;
    side["withLegend"] = withLegend;
    return writeAll(sidecarPath(path), QByteArray::fromStdString(side.dump(2)), errorRu);
}

bool exportHeightmap16(const WorldData& w, const QString& path, std::string* errorRu) {
    if (!w.has(LayerId::Elevation)) {
        if (errorRu) *errorRu = "Рельеф ещё не рассчитан.";
        return false;
    }
    std::vector<float> h = elevationAboveSea(w);
    double mn = 1e18, mx = -1e18;
    for (float v : h) { mn = std::min(mn, (double)v); mx = std::max(mx, (double)v); }
    if (mx <= mn) mx = mn + 1;
    QImage img(w.grid.width(), w.grid.height(), QImage::Format_Grayscale16);
    for (int j = 0; j < w.grid.height(); ++j) {
        auto* line = reinterpret_cast<uint16_t*>(img.scanLine(j));
        for (int i = 0; i < w.grid.width(); ++i) {
            double v = (h[static_cast<size_t>(w.grid.index(i, j))] - mn) / (mx - mn);
            line[i] = static_cast<uint16_t>(std::lround(std::clamp(v, 0.0, 1.0) * 65535.0));
        }
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || !img.save(&f, "PNG") || !f.commit()) {
        if (errorRu) *errorRu = "Не удалось записать 16-битную карту высот: " + f.errorString().toStdString();
        return false;
    }
    Json side = sidecarJson(w, "16-bit unsigned grey: elevation_m = minM + value / 65535 * (maxM - minM); surface (bedrock + land ice) relative to sea level");
    side["minM"] = mn;
    side["maxM"] = mx;
    side["quantizationM"] = (mx - mn) / 65535.0;
    return writeAll(sidecarPath(path), QByteArray::fromStdString(side.dump(2)), errorRu);
}

bool exportGeoTiff(const WorldData& w, LayerId id, int band, const QString& path, std::string* errorRu) {
    const LayerData* l = w.layer(id);
    if (!l) {
        if (errorRu) *errorRu = "Слой ещё не рассчитан.";
        return false;
    }
    TiffRaster r;
    r.width = w.grid.width();
    r.height = w.grid.height();
    r.data.resize(static_cast<size_t>(w.grid.size()));
    for (int64_t k = 0; k < w.grid.size(); ++k) {
        double v = (id == LayerId::Elevation) ? w.surfaceElevation(k) - w.seaLevel() : w.sample(id, k, band);
        r.data[static_cast<size_t>(k)] = std::isfinite(v) ? static_cast<float>(v) : r.noData;
    }
    r.pixelScaleX = 360.0 / r.width;
    r.pixelScaleY = 180.0 / r.height;
    r.sphereRadiusM = w.grid.radiusKm() * 1000.0;
    const LayerInfo& info = layerInfo(id);
    r.description = std::string("MapGen ") + info.key + (info.bands > 1 ? " band " + std::to_string(band + 1) : "") +
                    " [" + info.units + "] seed " + w.seed;
    if (!writeGeoTiffFloat(path, r, errorRu)) return false;
    Json side = sidecarJson(w, std::string("32-bit float, units: ") + info.units + (id == LayerId::Elevation ? ", relative to sea level" : ""));
    side["layer"] = info.key;
    side["band"] = band;
    side["noData"] = r.noData;
    return writeAll(sidecarPath(path), QByteArray::fromStdString(side.dump(2)), errorRu);
}

bool exportGeoJson(const WorldData& w, const QString& path, const std::string& what, std::string* errorRu) {
    std::string out;
    out.reserve(1 << 20);
    out += "{\"type\":\"FeatureCollection\",\"mapgen\":";
    Json meta = sidecarJson(w, "GeoJSON coordinates are [longitude, latitude] in degrees on the planet sphere given in datum");
    out += meta.dump(0);
    out += ",\"features\":[";
    bool first = true;
    auto lineCoords = [&](const Polyline& l) {
        std::string s = "[";
        for (size_t i = 0; i < l.pts.size(); ++i) {
            if (i) s += ",";
            s += "[" + fmt(l.pts[i].lon, 5) + "," + fmt(l.pts[i].lat, 5) + "]";
        }
        return s + "]";
    };
    // Split polylines that cross the antimeridian so that every coordinate is within ±180°.
    auto splitLine = [&](const Polyline& l) {
        std::vector<Polyline> parts;
        Polyline cur;
        for (size_t i = 0; i < l.pts.size(); ++i) {
            GeoPoint g = l.pts[i];
            double lon = wrapLonDeg(g.lon);
            if (!cur.pts.empty()) {
                double prev = cur.pts.back().lon;
                if (std::fabs(lon - prev) > 180.0) {
                    parts.push_back(cur);
                    cur.pts.clear();
                }
            }
            cur.pts.push_back({g.lat, lon});
        }
        if (cur.pts.size() >= 2) parts.push_back(cur);
        return parts;
    };
    auto feature = [&](const std::string& geom, const Json& props) {
        if (!first) out += ",";
        first = false;
        out += "{\"type\":\"Feature\",\"geometry\":" + geom + ",\"properties\":" + props.dump(0) + "}";
    };
    if ((what == "coastlines" || what == "all") && w.ocean) {
        for (auto& c : w.ocean->coastlines)
            for (auto& part : splitLine(c)) {
                Json pr = Json::object();
                pr["kind"] = "coastline";
                feature("{\"type\":\"LineString\",\"coordinates\":" + lineCoords(part) + "}", pr);
            }
    }
    if ((what == "rivers" || what == "all") && w.hydro) {
        for (auto& r : w.hydro->rivers)
            for (auto& part : splitLine(r.line)) {
                Json pr = Json::object();
                pr["kind"] = "river";
                pr["id"] = r.id;
                pr["strahler"] = r.order;
                pr["dischargeM3s"] = r.dischargeEnd;
                pr["lengthKm"] = r.lengthKm;
                pr["basin"] = r.basinId;
                pr["regime"] = categoryName(LayerId::RiverClass, r.riverClass);
                feature("{\"type\":\"LineString\",\"coordinates\":" + lineCoords(part) + "}", pr);
            }
    }
    if ((what == "plates" || what == "all") && w.tectonics) {
        for (auto& b : w.tectonics->boundaries)
            for (auto& part : splitLine(b.line)) {
                Json pr = Json::object();
                pr["kind"] = "plate_boundary";
                pr["plateA"] = b.plateA;
                pr["plateB"] = b.plateB;
                pr["type"] = categoryName(LayerId::BoundaryType, b.type);
                pr["class"] = categoryName(LayerId::BoundaryClass, b.cls);
                pr["convergenceCmYr"] = b.convergenceCmYr;
                pr["note"] = "conventional static reconstruction";
                feature("{\"type\":\"LineString\",\"coordinates\":" + lineCoords(part) + "}", pr);
            }
    }
    if ((what == "volcanoes" || what == "all") && w.geomorph) {
        const auto* ga = w.geoActivity.get();
        static const char* gen[] = {"arc_continental", "arc_island", "continental_rift", "mid_ocean_ridge", "hotspot_oceanic", "hotspot_continental", "back_arc"};
        static const char* morph[] = {"stratovolcano", "shield", "caldera", "cinder_field", "seamount", "guyot", "fissure"};
        static const char* act[] = {"extinct", "dormant", "active"};
        for (size_t i = 0; i < w.geomorph->volcanoes.size(); ++i) {
            auto& v = w.geomorph->volcanoes[i];
            Json pr = Json::object();
            pr["kind"] = "volcano";
            pr["id"] = v.id;
            pr["genesis"] = gen[std::min<int>(v.genesis, 6)];
            pr["morphology"] = morph[std::min<int>(v.morphology, 6)];
            pr["edificeHeightM"] = v.edificeHeightM;
            pr["ageMyr"] = v.ageMyr;
            if (ga && i < ga->volcanoStates.size()) {
                pr["activity"] = act[std::min<int>(ga->volcanoStates[i].activity, 2)];
                pr["submarine"] = ga->volcanoStates[i].submarine;
                pr["zone0to5"] = ga->volcanoStates[i].zone;
            }
            feature("{\"type\":\"Point\",\"coordinates\":[" + fmt(v.lon, 5) + "," + fmt(v.lat, 5) + "]}", pr);
        }
        if (ga)
            for (auto& s : ga->springs) {
                Json pr = Json::object();
                pr["kind"] = s.geyser ? "geyser" : "hot_spring";
                pr["potential"] = s.potential;
                feature("{\"type\":\"Point\",\"coordinates\":[" + fmt(s.lon, 5) + "," + fmt(s.lat, 5) + "]}", pr);
            }
    }
    out += "]}";
    return writeAll(path, QByteArray::fromStdString(out), errorRu);
}

bool exportCsv(const WorldData& w, const QString& path, const std::string& what, std::string* errorRu) {
    std::string s = "\xEF\xBB\xBF";  // UTF-8 BOM for spreadsheet programs on Windows
    if (what == "statistics") {
        s += "stage,metric,value\r\n";
        for (auto& r : w.reports)
            for (auto& m : r.metrics) s += std::to_string(r.stage) + "," + csvEscape(m.first) + "," + fmt(m.second, 6) + "\r\n";
    } else if (what == "volcanoes" && w.geomorph) {
        s += "id,lat,lon,genesis,morphology,edifice_height_m,age_myr,activity,submarine,zone\r\n";
        const auto* ga = w.geoActivity.get();
        for (size_t i = 0; i < w.geomorph->volcanoes.size(); ++i) {
            auto& v = w.geomorph->volcanoes[i];
            s += std::to_string(v.id) + "," + fmt(v.lat, 5) + "," + fmt(v.lon, 5) + "," + std::to_string(v.genesis) + "," +
                 std::to_string(v.morphology) + "," + fmt(v.edificeHeightM, 1) + "," + fmt(v.ageMyr, 2);
            if (ga && i < ga->volcanoStates.size())
                s += "," + std::to_string(ga->volcanoStates[i].activity) + "," + (ga->volcanoStates[i].submarine ? "1" : "0") + "," +
                     std::to_string(ga->volcanoStates[i].zone);
            else s += ",,,";
            s += "\r\n";
        }
    } else if (what == "lakes" && w.hydro) {
        s += "id,lat,lon,area_km2,level_m,max_depth_m,volume_km3,inflow_m3s,evaporation_m3s,outflow_m3s,genesis,regime,saline,basin\r\n";
        for (auto& l : w.hydro->lakes)
            s += std::to_string(l.id) + "," + fmt(l.lat, 4) + "," + fmt(l.lon, 4) + "," + fmt(l.areaKm2, 1) + "," + fmt(l.levelM, 1) + "," +
                 fmt(l.maxDepthM, 1) + "," + fmt(l.volumeKm3, 3) + "," + fmt(l.inflowM3s, 2) + "," + fmt(l.evaporationM3s, 2) + "," +
                 fmt(l.outflowM3s, 2) + "," + std::to_string(l.genesis) + "," + std::to_string(l.regime) + "," + (l.saline ? "1" : "0") +
                 "," + std::to_string(l.basinId) + "\r\n";
    } else if (what == "basins" && w.hydro) {
        s += "id,outlet_type,area_km2,discharge_m3s,main_river_km,cells\r\n";
        for (auto& b : w.hydro->basins)
            s += std::to_string(b.id) + "," + std::to_string(b.outlet) + "," + fmt(b.areaKm2, 1) + "," + fmt(b.dischargeM3s, 2) + "," +
                 fmt(b.mainRiverKm, 1) + "," + std::to_string(b.cells) + "\r\n";
    } else if (what == "rivers" && w.hydro) {
        s += "id,strahler,discharge_start_m3s,discharge_end_m3s,length_km,slope_m_per_km,basin,downstream,regime\r\n";
        for (auto& r : w.hydro->rivers)
            s += std::to_string(r.id) + "," + std::to_string(r.order) + "," + fmt(r.dischargeStart, 2) + "," + fmt(r.dischargeEnd, 2) + "," +
                 fmt(r.lengthKm, 1) + "," + fmt(r.slope, 3) + "," + std::to_string(r.basinId) + "," + std::to_string(r.downstream) + "," +
                 std::to_string(r.riverClass) + "\r\n";
    } else {
        if (errorRu) *errorRu = "Нет данных для таблицы «" + what + "».";
        return false;
    }
    return writeAll(path, QByteArray::fromStdString(s), errorRu);
}

bool exportMetadataJson(const WorldData& w, const QString& path, std::string* errorRu) {
    Json j = sidecarJson(w, "metadata");
    j["generatorVersion"] = w.generatorVersion;
    j["schemaVersion"] = w.schemaVersion;
    j["params"] = w.params.toJson();
    Json sh = Json::array();
    for (auto& h : w.stageHash) sh.push(h);
    j["stageHashes"] = sh;
    Json lh = Json::object();
    for (auto& info : allLayers())
        if (w.has(info.id)) {
            std::string h = w.layerHash[static_cast<size_t>(info.id)];
            if (h.empty()) h = Pipeline::hashLayer(*w.layer(info.id));
            lh[info.key] = h;
        }
    j["layerHashes"] = lh;
    Json q = Json::array();
    for (auto& r : w.reports) q.push(r.toJson());
    j["quality"] = q;
    j["statistics"] = w.statistics;
    return writeAll(path, QByteArray::fromStdString(j.dump(2)), errorRu);
}

qint64 estimateExportBytes(const WorldData& w, const std::string& what) {
    qint64 cells = w.grid.size();
    if (what == "png") return cells * 2;
    if (what == "heightmap") return cells * 2;
    if (what == "geotiff") return cells * 4;
    if (what == "all") return cells * 4 * 6 + cells * 2 * kTabCount;
    return cells;
}

bool exportAll(const WorldData& w, const QString& dir, const ViewOptions& view, std::string* errorRu, const ExportProgress& ep) {
    QDir d(dir);
    if (!d.exists() && !d.mkpath(".")) {
        if (errorRu) *errorRu = "Не удалось создать папку экспорта.";
        return false;
    }
    int total = kTabCount + 12, done = 0;
    auto step = [&]() {
        if (ep.progress) ep.progress(static_cast<double>(++done) / total);
        if (ep.cancelled && ep.cancelled()) {
            if (errorRu) *errorRu = "Экспорт отменён.";
            return false;
        }
        return true;
    };
    for (auto& t : tabs()) {
        if (!exportLayerPng(w, t.tab, view, true, d.filePath(QString("map_%1.png").arg(t.id)), errorRu)) return false;
        if (!step()) return false;
    }
    if (!exportHeightmap16(w, d.filePath("heightmap16.png"), errorRu) || !step()) return false;
    if (!exportGeoTiff(w, LayerId::Elevation, 0, d.filePath("elevation.tif"), errorRu) || !step()) return false;
    if (w.has(LayerId::TempMean) && (!exportGeoTiff(w, LayerId::TempMean, 0, d.filePath("temperature_mean.tif"), errorRu) || !step())) return false;
    if (w.has(LayerId::PrecAnnual) && (!exportGeoTiff(w, LayerId::PrecAnnual, 0, d.filePath("precipitation_annual.tif"), errorRu) || !step())) return false;
    if (!exportGeoJson(w, d.filePath("features.geojson"), "all", errorRu) || !step()) return false;
    for (const char* t : {"statistics", "volcanoes", "lakes", "basins", "rivers"}) {
        std::string e;
        exportCsv(w, d.filePath(QString("%1.csv").arg(t)), t, &e);  // tables without data are skipped
        if (!step()) return false;
    }
    if (!exportMetadataJson(w, d.filePath("metadata.json"), errorRu) || !step()) return false;
    if (ep.progress) ep.progress(1.0);
    return true;
}

}  // namespace mapgen
