#pragma once

// Map and data exports. Every raster export writes a sidecar JSON with the
// projection, sphere radius, sea level and value encoding. 8-bit images are
// never presented as a DEM: elevation goes to 16-bit PNG (with the exact
// linear mapping in the sidecar) or to a 32-bit float GeoTIFF.

#include <QImage>
#include <QString>
#include <string>

#include "core/world/WorldData.h"
#include "render/Colorizer.h"
#include "render/Overlays.h"

namespace mapgen {

struct ExportProgress {
    std::function<void(double)> progress;
    std::function<bool()> cancelled;
};

// Rendered layer image (full grid resolution × scale) with optional legend panel.
QImage renderLayerImage(const WorldData& w, MapTab tab, const ViewOptions& view, bool withLegend, bool withOverlays,
                        int scale = 1);
void paintLegend(QPainter& p, const Legend& legend, const QRectF& area, double uiScale = 1.0);

bool exportLayerPng(const WorldData& w, MapTab tab, const ViewOptions& view, bool withLegend, const QString& path,
                    std::string* errorRu = nullptr);
// 16-bit grey PNG, value = round((h - minM) / (maxM - minM) * 65535) of surface elevation relative to sea level.
bool exportHeightmap16(const WorldData& w, const QString& path, std::string* errorRu = nullptr);
// 32-bit float GeoTIFF of a numeric layer (band) on the planet's sphere, own minimal writer.
bool exportGeoTiff(const WorldData& w, LayerId layer, int band, const QString& path, std::string* errorRu = nullptr);
// GeoJSON: coastlines, rivers, plate boundaries, volcanoes (+ springs), with explicit sphere datum.
bool exportGeoJson(const WorldData& w, const QString& path, const std::string& what, std::string* errorRu = nullptr);
// CSV tables: statistics, volcanoes, lakes, basins, rivers.
bool exportCsv(const WorldData& w, const QString& path, const std::string& what, std::string* errorRu = nullptr);
// JSON metadata: seed, versions, parameters, hashes, quality reports, statistics.
bool exportMetadataJson(const WorldData& w, const QString& path, std::string* errorRu = nullptr);
// Whole export bundle into a folder (used by the export dialog and the CLI).
bool exportAll(const WorldData& w, const QString& dir, const ViewOptions& view, std::string* errorRu = nullptr,
               const ExportProgress& ep = {});

// Rough size estimate (bytes) of an export, for the size warning.
qint64 estimateExportBytes(const WorldData& w, const std::string& what);

Json sidecarJson(const WorldData& w, const std::string& encoding);

}  // namespace mapgen
