#pragma once

// Seeded 3D gradient noise evaluated on the unit sphere. Sampling in 3D avoids
// any seam at ±180° and any stretching near the poles. Only + - * and floor
// are used, so values are bit-reproducible.

#include <cstdint>

#include "core/util/Math.h"

namespace mapgen {

class Noise3 {
public:
    Noise3() = default;
    explicit Noise3(uint64_t key) : key_(key) {}

    // Gradient noise in roughly [-1, 1].
    double gradient(double x, double y, double z) const;
    double gradient(const Vec3& p) const { return gradient(p.x, p.y, p.z); }

    // Fractal sum: frequency in cycles per unit radius.
    double fbm(const Vec3& p, double frequency, int octaves, double lacunarity = 2.0, double gain = 0.5) const;
    // Ridged multifractal in [0, 1]: sharp crests (mountain ridges, fracture lines).
    double ridged(const Vec3& p, double frequency, int octaves, double lacunarity = 2.0, double gain = 0.5) const;
    // Domain-warp displacement vector (three decorrelated fbm channels).
    Vec3 warp(const Vec3& p, double frequency, int octaves) const;

    uint64_t key() const { return key_; }

private:
    uint64_t key_ = 0;
};

}  // namespace mapgen
