#pragma once

// Minimal baseline TIFF writer/reader for single-band 32-bit float rasters with
// GeoTIFF tags (ModelPixelScale, ModelTiepoint, GeoKeyDirectory). The geographic
// CRS is user-defined: a sphere of the planet's radius — not WGS84.

#include <QString>
#include <string>
#include <vector>

namespace mapgen {

struct TiffRaster {
    int width = 0, height = 0;
    std::vector<float> data;
    double pixelScaleX = 1, pixelScaleY = 1;  // degrees per pixel
    double originLon = -180, originLat = 90;   // upper-left corner
    double sphereRadiusM = 6371000;
    std::string description;
    float noData = -32768.0f;
};

bool writeGeoTiffFloat(const QString& path, const TiffRaster& r, std::string* errorRu = nullptr);
bool readGeoTiffFloat(const QString& path, TiffRaster& r, std::string* errorRu = nullptr);

}  // namespace mapgen
