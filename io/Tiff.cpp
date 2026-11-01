#include "io/Tiff.h"

#include <QFile>
#include <QSaveFile>
#include <cstring>
#include <map>

namespace mapgen {

namespace {

enum TiffType : uint16_t { kAscii = 2, kShort = 3, kLong = 4, kDouble = 12 };

struct Tag {
    uint16_t id;
    uint16_t type;
    uint32_t count;
    QByteArray data;  // little-endian payload
};

template <class T>
void app(QByteArray& b, T v) {
    char tmp[sizeof(T)];
    std::memcpy(tmp, &v, sizeof(T));
    b.append(tmp, sizeof(T));
}

Tag shortTag(uint16_t id, std::vector<uint16_t> v) {
    Tag t{id, kShort, static_cast<uint32_t>(v.size()), {}};
    for (auto x : v) app(t.data, x);
    return t;
}
Tag longTag(uint16_t id, std::vector<uint32_t> v) {
    Tag t{id, kLong, static_cast<uint32_t>(v.size()), {}};
    for (auto x : v) app(t.data, x);
    return t;
}
Tag doubleTag(uint16_t id, std::vector<double> v) {
    Tag t{id, kDouble, static_cast<uint32_t>(v.size()), {}};
    for (auto x : v) app(t.data, x);
    return t;
}
Tag asciiTag(uint16_t id, const std::string& s) {
    Tag t{id, kAscii, static_cast<uint32_t>(s.size() + 1), {}};
    t.data.append(s.data(), static_cast<qsizetype>(s.size()));
    t.data.append('\0');
    return t;
}

size_t typeSize(uint16_t type) {
    switch (type) {
        case kAscii: return 1;
        case kShort: return 2;
        case kLong: return 4;
        case kDouble: return 8;
        default: return 1;
    }
}

}  // namespace

bool writeGeoTiffFloat(const QString& path, const TiffRaster& r, std::string* errorRu) {
    auto fail = [&](const std::string& m) {
        if (errorRu) *errorRu = m;
        return false;
    };
    if (r.width <= 0 || r.height <= 0 || r.data.size() != static_cast<size_t>(r.width) * r.height)
        return fail("Пустой растр для GeoTIFF.");
    const uint32_t rowsPerStrip = 16;
    const uint32_t strips = (static_cast<uint32_t>(r.height) + rowsPerStrip - 1) / rowsPerStrip;
    QByteArray file;
    file.append("II", 2);
    app<uint16_t>(file, 42);
    app<uint32_t>(file, 0);  // IFD offset, patched
    std::vector<uint32_t> offsets, counts;
    for (uint32_t s = 0; s < strips; ++s) {
        uint32_t y0 = s * rowsPerStrip;
        uint32_t rows = std::min(rowsPerStrip, static_cast<uint32_t>(r.height) - y0);
        offsets.push_back(static_cast<uint32_t>(file.size()));
        uint32_t bytes = rows * static_cast<uint32_t>(r.width) * 4;
        counts.push_back(bytes);
        file.append(reinterpret_cast<const char*>(r.data.data() + static_cast<size_t>(y0) * r.width), static_cast<qsizetype>(bytes));
    }
    if (file.size() % 2) file.append('\0');

    const std::string geoAscii = "MapGen planet sphere (not WGS84)|Sphere R=" + std::to_string(static_cast<long long>(r.sphereRadiusM)) + " m|";
    std::vector<uint16_t> keys = {1, 1, 0, 0};
    auto key = [&](uint16_t id, uint16_t loc, uint16_t count, uint16_t value) {
        keys.push_back(id); keys.push_back(loc); keys.push_back(count); keys.push_back(value);
    };
    size_t citation1 = std::string("MapGen planet sphere (not WGS84)|").size();
    key(1024, 0, 1, 2);        // GTModelTypeGeoKey = Geographic
    key(1025, 0, 1, 1);        // GTRasterTypeGeoKey = PixelIsArea
    key(1026, 34737, static_cast<uint16_t>(citation1), 0);
    key(2048, 0, 1, 32767);    // GeographicTypeGeoKey = user-defined
    key(2049, 34737, static_cast<uint16_t>(geoAscii.size() - citation1), static_cast<uint16_t>(citation1));
    key(2050, 0, 1, 32767);    // datum user-defined
    key(2051, 0, 1, 8901);     // Greenwich
    key(2054, 0, 1, 9102);     // degrees
    key(2056, 0, 1, 32767);    // ellipsoid user-defined
    key(2057, 34736, 1, 0);    // semi-major axis
    key(2058, 34736, 1, 1);    // semi-minor axis (sphere)
    keys[3] = static_cast<uint16_t>((keys.size() - 4) / 4);

    std::vector<Tag> tags = {
        longTag(256, {static_cast<uint32_t>(r.width)}),
        longTag(257, {static_cast<uint32_t>(r.height)}),
        shortTag(258, {32}),
        shortTag(259, {1}),
        shortTag(262, {1}),
        asciiTag(270, r.description.empty() ? std::string("MapGen layer") : r.description),
        longTag(273, offsets),
        shortTag(277, {1}),
        longTag(278, {rowsPerStrip}),
        longTag(279, counts),
        shortTag(284, {1}),
        shortTag(339, {3}),
        doubleTag(33550, {r.pixelScaleX, r.pixelScaleY, 0.0}),
        doubleTag(33922, {0, 0, 0, r.originLon, r.originLat, 0}),
        shortTag(34735, keys),
        doubleTag(34736, {r.sphereRadiusM, r.sphereRadiusM}),
        asciiTag(34737, geoAscii),
        asciiTag(42113, std::to_string(static_cast<int>(r.noData))),
    };
    // Out-of-line tag data first, then the IFD.
    std::map<uint16_t, uint32_t> dataOffset;
    for (auto& t : tags) {
        if (static_cast<size_t>(t.data.size()) > 4) {
            dataOffset[t.id] = static_cast<uint32_t>(file.size());
            file.append(t.data);
            if (file.size() % 2) file.append('\0');
        }
    }
    uint32_t ifd = static_cast<uint32_t>(file.size());
    std::memcpy(file.data() + 4, &ifd, 4);
    app<uint16_t>(file, static_cast<uint16_t>(tags.size()));
    for (auto& t : tags) {
        app<uint16_t>(file, t.id);
        app<uint16_t>(file, t.type);
        app<uint32_t>(file, t.count);
        if (static_cast<size_t>(t.data.size()) > 4) {
            app<uint32_t>(file, dataOffset[t.id]);
        } else {
            QByteArray v = t.data;
            while (v.size() < 4) v.append('\0');
            file.append(v);
        }
    }
    app<uint32_t>(file, 0);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return fail("Не удалось открыть файл GeoTIFF: " + f.errorString().toStdString());
    if (f.write(file) != file.size()) return fail("Ошибка записи GeoTIFF: " + f.errorString().toStdString());
    if (!f.commit()) return fail("Не удалось завершить запись GeoTIFF: " + f.errorString().toStdString());
    return true;
}

bool readGeoTiffFloat(const QString& path, TiffRaster& r, std::string* errorRu) {
    auto fail = [&](const std::string& m) {
        if (errorRu) *errorRu = m;
        return false;
    };
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail("Не удалось открыть GeoTIFF.");
    QByteArray b = f.readAll();
    if (b.size() < 8 || b[0] != 'I' || b[1] != 'I') return fail("Поддерживается только little-endian TIFF.");
    auto u16 = [&](size_t off) { uint16_t v; std::memcpy(&v, b.constData() + off, 2); return v; };
    auto u32 = [&](size_t off) { uint32_t v; std::memcpy(&v, b.constData() + off, 4); return v; };
    if (u16(2) != 42) return fail("Неверная сигнатура TIFF.");
    size_t ifd = u32(4);
    uint16_t n = u16(ifd);
    std::map<uint16_t, std::vector<double>> num;
    std::map<uint16_t, std::string> asc;
    for (uint16_t i = 0; i < n; ++i) {
        size_t e = ifd + 2 + 12 * i;
        uint16_t id = u16(e), type = u16(e + 2);
        uint32_t count = u32(e + 4);
        size_t sz = typeSize(type) * count;
        size_t off = sz > 4 ? u32(e + 8) : e + 8;
        if (off + sz > static_cast<size_t>(b.size())) return fail("Повреждённый тег TIFF.");
        if (type == kAscii) asc[id] = std::string(b.constData() + off, count ? count - 1 : 0);
        for (uint32_t k = 0; k < count && type != kAscii; ++k) {
            double v = 0;
            if (type == kShort) v = u16(off + 2 * k);
            else if (type == kLong) v = u32(off + 4 * k);
            else if (type == kDouble) std::memcpy(&v, b.constData() + off + 8 * k, 8);
            num[id].push_back(v);
        }
    }
    if (!num.count(256) || !num.count(257) || !num.count(273)) return fail("В TIFF нет обязательных тегов.");
    if (num[258][0] != 32 || (num.count(339) && num[339][0] != 3)) return fail("Ожидался растр float32.");
    if (num.count(259) && num[259][0] != 1) return fail("Сжатый TIFF не поддерживается.");
    r.width = static_cast<int>(num[256][0]);
    r.height = static_cast<int>(num[257][0]);
    r.data.assign(static_cast<size_t>(r.width) * r.height, 0.0f);
    size_t pos = 0;
    for (size_t s = 0; s < num[273].size(); ++s) {
        size_t off = static_cast<size_t>(num[273][s]), cnt = static_cast<size_t>(num[279][s]);
        if (off + cnt > static_cast<size_t>(b.size()) || pos + cnt / 4 > r.data.size()) return fail("Повреждённые данные TIFF.");
        std::memcpy(r.data.data() + pos, b.constData() + off, cnt);
        pos += cnt / 4;
    }
    if (num.count(33550)) { r.pixelScaleX = num[33550][0]; r.pixelScaleY = num[33550][1]; }
    if (num.count(33922)) { r.originLon = num[33922][3]; r.originLat = num[33922][4]; }
    if (num.count(34736)) r.sphereRadiusM = num[34736][0];
    if (asc.count(270)) r.description = asc[270];
    return true;
}

}  // namespace mapgen
