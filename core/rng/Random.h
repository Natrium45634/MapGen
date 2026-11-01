#pragma once

// PCG64 (PCG-XSL-RR 128/64, O'Neill 2014) with a portable 128-bit LCG, and
// keyed substreams derived with SHA-256 from
// (seed, algorithmVersion, stageId, objectId).
//
// Every stage, plate, hotspot or cell family draws from its own stream, so the
// order in which stages or parallel tasks run never changes another stream.

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/rng/Sha256.h"

namespace mapgen {

struct U128 {
    uint64_t hi = 0, lo = 0;
};

inline U128 mul128(U128 a, U128 b) {
    // Low 128 bits of a*b, portable (no compiler 128-bit type needed).
    auto mul64 = [](uint64_t x, uint64_t y, uint64_t& hi) -> uint64_t {
        uint64_t x0 = x & 0xffffffffull, x1 = x >> 32, y0 = y & 0xffffffffull, y1 = y >> 32;
        uint64_t p00 = x0 * y0, p01 = x0 * y1, p10 = x1 * y0, p11 = x1 * y1;
        uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffull) + (p10 & 0xffffffffull);
        hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
        return (mid << 32) | (p00 & 0xffffffffull);
    };
    uint64_t hi;
    uint64_t lo = mul64(a.lo, b.lo, hi);
    hi += a.lo * b.hi + a.hi * b.lo;
    return {hi, lo};
}

inline U128 add128(U128 a, U128 b) {
    U128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo ? 1 : 0);
    return r;
}

class Pcg64 {
public:
    Pcg64() = default;
    Pcg64(U128 state, U128 stream) { seed(state, stream); }

    void seed(U128 initState, U128 stream) {
        inc_ = {(stream.hi << 1) | (stream.lo >> 63), (stream.lo << 1) | 1u};
        state_ = {0, 0};
        step();
        state_ = add128(state_, initState);
        step();
    }

    uint64_t next() {
        step();
        uint64_t x = state_.hi ^ state_.lo;
        unsigned rot = static_cast<unsigned>(state_.hi >> 58);
        return (x >> rot) | (x << ((64u - rot) & 63u));
    }

    // Uniform double in [0, 1) with 53 random bits.
    double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    double uniform(double a, double b) { return a + (b - a) * uniform(); }

    // Unbiased integer in [0, n).
    uint64_t below(uint64_t n) {
        if (n <= 1) return 0;
        uint64_t threshold = (0 - n) % n;
        while (true) {
            uint64_t r = next();
            if (r >= threshold) return r % n;
        }
    }
    int rangeInt(int lo, int hi) {  // inclusive
        if (hi <= lo) return lo;
        return lo + static_cast<int>(below(static_cast<uint64_t>(hi - lo) + 1));
    }
    bool chance(double p) { return uniform() < p; }

    // Standard normal (Box-Muller, one value per call; deterministic).
    double normal() {
        double u1 = uniform();
        double u2 = uniform();
        if (u1 < 1e-300) u1 = 1e-300;
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }
    double normal(double mean, double sd) { return mean + sd * normal(); }

private:
    void step() {
        static const U128 kMul{0x2360ED051FC65DA4ull, 0x4385DF649FCCF645ull};
        state_ = add128(mul128(state_, kMul), inc_);
    }
    U128 state_{};
    U128 inc_{};
};

// Seed strings: 1..128 characters from [A-Za-z0-9], case-sensitive.
bool isValidSeed(std::string_view seed, std::string* errorRu = nullptr);

// Root of all randomness of one world: SHA-256 of the seed string.
class SeedKey {
public:
    SeedKey() = default;
    explicit SeedKey(std::string seed, int algorithmVersion);

    const std::string& seed() const { return seed_; }
    int algorithmVersion() const { return algoVersion_; }

    // Independent stream for (stage, object). Same inputs always give the same stream.
    Pcg64 stream(std::string_view stageId, uint64_t objectId = 0) const;
    // 64-bit key for hash-based noise / per-cell randomness.
    uint64_t key64(std::string_view stageId, uint64_t objectId = 0) const;

private:
    Sha256::Digest derive(std::string_view stageId, uint64_t objectId) const;
    std::string seed_;
    int algoVersion_ = 1;
    Sha256::Digest root_{};
};

// Stateless per-cell randomness: uniform [0,1) from (key, a, b).
inline double hashUniform(uint64_t key, uint64_t a, uint64_t b = 0) {
    uint64_t h = mix64(key ^ mix64(a * 0x9E3779B97F4A7C15ull + mix64(b + 0x632BE59BD9B4E019ull)));
    return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0);
}

std::string randomSeedString(uint64_t entropy, int length = 10);

}  // namespace mapgen
