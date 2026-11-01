#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mapgen {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;
inline constexpr double kDegToRad = kPi / 180.0;
inline constexpr double kRadToDeg = 180.0 / kPi;

template <class T>
constexpr T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline double lerp(double a, double b, double t) { return a + (b - a) * t; }

inline double smoothstep(double e0, double e1, double x) {
    if (e1 == e0) return x < e0 ? 0.0 : 1.0;
    double t = clampv((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

inline double sqr(double x) { return x * x; }

inline double gauss(double x, double width) { return std::exp(-(x * x) / (width * width)); }

struct Vec3 {
    double x = 0, y = 0, z = 0;
    constexpr Vec3() = default;
    constexpr Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3 operator-() const { return {-x, -y, -z}; }
};

inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(const Vec3& a) {
    double l = length(a);
    return l > 0 ? a / l : Vec3{0, 0, 1};
}

// Unit vector from geographic coordinates (radians). x towards (0,0), z north.
inline Vec3 fromLatLon(double lat, double lon) {
    double cl = std::cos(lat);
    return {cl * std::cos(lon), cl * std::sin(lon), std::sin(lat)};
}
inline double latOf(const Vec3& p) { return std::asin(clampv(p.z, -1.0, 1.0)); }
inline double lonOf(const Vec3& p) { return std::atan2(p.y, p.x); }

// Angle between unit vectors (robust for small and large angles).
inline double angleBetween(const Vec3& a, const Vec3& b) {
    return std::atan2(length(cross(a, b)), dot(a, b));
}

// Local tangent basis at p: east and north unit vectors.
inline Vec3 eastAt(const Vec3& p) {
    Vec3 e{-p.y, p.x, 0.0};
    double l = length(e);
    return l > 1e-12 ? e / l : Vec3{0, 1, 0};
}
inline Vec3 northAt(const Vec3& p) { return cross(p, eastAt(p)); }

// Rotate v about unit axis k by angle a (Rodrigues).
inline Vec3 rotateAbout(const Vec3& v, const Vec3& k, double a) {
    double c = std::cos(a), s = std::sin(a);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1.0 - c));
}

// Wrap longitude in degrees to [-180, 180).
inline double wrapLonDeg(double lon) {
    double r = std::fmod(lon + 180.0, 360.0);
    if (r < 0) r += 360.0;
    return r - 180.0;
}

}  // namespace mapgen
