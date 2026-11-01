#pragma once

// .geoworld project container.
//
// Layout (little-endian):
//   "GEOWORLD" | u32 formatVersion | u32 entryCount | u64 tocOffset
//   entry blobs (zlib-compressed or stored)
//   TOC: per entry  u16 nameLen | name (UTF-8) | u64 offset | u64 storedSize | u64 rawSize | u32 crc32(raw) | u8 compression
//   u32 crc32(TOC) | "GWEND\0\0\0"
// Entries: manifest.json (seed, versions, parameters, layer index with hashes,
// stage hashes, quality reports), layers/<key>.bin, features/<name>.json.
// Writes go through QSaveFile (temporary file + atomic rename).

#include <QString>
#include <functional>
#include <string>

#include "core/world/WorldData.h"

namespace mapgen {

struct FileProgress {
    std::function<void(double)> progress;
    std::function<bool()> cancelled;
};

struct LoadResult {
    bool ok = false;
    std::string errorRu;
    std::vector<std::string> warningsRu;
    bool versionMismatch = false;  // saved by another algorithm version: stored result only
    bool hashesVerified = false;
};

bool saveGeoWorld(const WorldData& w, const QString& path, std::string* errorRu = nullptr, const FileProgress& fp = {});
LoadResult loadGeoWorld(const QString& path, WorldData& out, const FileProgress& fp = {});

uint32_t crc32(const void* data, size_t len, uint32_t crc = 0);

// Feature (de)serialisation used by the container and by tests.
Json featuresToJson(const WorldData& w);
void featuresFromJson(const Json& j, WorldData& w);

}  // namespace mapgen
