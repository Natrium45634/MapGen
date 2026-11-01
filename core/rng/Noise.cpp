#include "core/rng/Noise.h"

#include <cmath>

#include "core/rng/Sha256.h"

namespace mapgen {

namespace {

constexpr double G[12][3] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
                             {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};

inline double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

inline uint64_t latticeHash(uint64_t key, int64_t x, int64_t y, int64_t z) {
    uint64_t h = key;
    h ^= static_cast<uint64_t>(x) * 0x9E3779B185EBCA87ull;
    h ^= static_cast<uint64_t>(y) * 0xC2B2AE3D27D4EB4Full;
    h ^= static_cast<uint64_t>(z) * 0x165667B19E3779F9ull;
    return mix64(h);
}

inline double grad(uint64_t h, double x, double y, double z) {
    const double* g = G[h % 12];
    return g[0] * x + g[1] * y + g[2] * z;
}

}  // namespace

double Noise3::gradient(double x, double y, double z) const {
    double fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    int64_t ix = static_cast<int64_t>(fx), iy = static_cast<int64_t>(fy), iz = static_cast<int64_t>(fz);
    double dx = x - fx, dy = y - fy, dz = z - fz;
    double u = fade(dx), v = fade(dy), w = fade(dz);

    double n000 = grad(latticeHash(key_, ix, iy, iz), dx, dy, dz);
    double n100 = grad(latticeHash(key_, ix + 1, iy, iz), dx - 1, dy, dz);
    double n010 = grad(latticeHash(key_, ix, iy + 1, iz), dx, dy - 1, dz);
    double n110 = grad(latticeHash(key_, ix + 1, iy + 1, iz), dx - 1, dy - 1, dz);
    double n001 = grad(latticeHash(key_, ix, iy, iz + 1), dx, dy, dz - 1);
    double n101 = grad(latticeHash(key_, ix + 1, iy, iz + 1), dx - 1, dy, dz - 1);
    double n011 = grad(latticeHash(key_, ix, iy + 1, iz + 1), dx, dy - 1, dz - 1);
    double n111 = grad(latticeHash(key_, ix + 1, iy + 1, iz + 1), dx - 1, dy - 1, dz - 1);

    double x00 = lerp(n000, n100, u), x10 = lerp(n010, n110, u);
    double x01 = lerp(n001, n101, u), x11 = lerp(n011, n111, u);
    double y0 = lerp(x00, x10, v), y1 = lerp(x01, x11, v);
    return lerp(y0, y1, w);
}

double Noise3::fbm(const Vec3& p, double frequency, int octaves, double lacunarity, double gain) const {
    double sum = 0, amp = 1, norm = 0, f = frequency;
    for (int o = 0; o < octaves; ++o) {
        // Per-octave offsets decorrelate octaves sharing lattice points.
        double ox = 17.13 * o, oy = -31.71 * o, oz = 47.37 * o;
        sum += amp * gradient(p.x * f + ox, p.y * f + oy, p.z * f + oz);
        norm += amp;
        amp *= gain;
        f *= lacunarity;
    }
    return norm > 0 ? sum / norm : 0.0;
}

double Noise3::ridged(const Vec3& p, double frequency, int octaves, double lacunarity, double gain) const {
    double sum = 0, amp = 1, norm = 0, f = frequency, weight = 1.0;
    for (int o = 0; o < octaves; ++o) {
        double ox = -11.3 * o, oy = 23.9 * o, oz = 5.71 * o;
        double n = 1.0 - std::fabs(gradient(p.x * f + ox, p.y * f + oy, p.z * f + oz) * 1.4);
        n = clampv(n, 0.0, 1.0);
        n *= n;
        n *= weight;
        weight = clampv(n * 1.5, 0.0, 1.0);
        sum += amp * n;
        norm += amp;
        amp *= gain;
        f *= lacunarity;
    }
    return norm > 0 ? sum / norm : 0.0;
}

Vec3 Noise3::warp(const Vec3& p, double frequency, int octaves) const {
    Vec3 a = p + Vec3{5.2, 1.3, -7.7};
    Vec3 b = p + Vec3{-3.1, 9.4, 2.8};
    Vec3 c = p + Vec3{8.6, -6.2, 4.1};
    return {fbm(a, frequency, octaves), fbm(b, frequency, octaves), fbm(c, frequency, octaves)};
}

}  // namespace mapgen
