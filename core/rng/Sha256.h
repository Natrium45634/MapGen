#pragma once

// FIPS 180-4 SHA-256. Used to turn seed strings into generator state and to
// derive keyed substreams; never std::hash (implementation-defined).

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace mapgen {

class Sha256 {
public:
    using Digest = std::array<uint8_t, 32>;

    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    void update(std::string_view s) { update(s.data(), s.size()); }
    void updateU64(uint64_t v);  // little-endian
    Digest finish();

    static Digest of(std::string_view s) {
        Sha256 h;
        h.update(s);
        return h.finish();
    }
    static std::string hex(const Digest& d);

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    uint64_t len_ = 0;
    size_t used_ = 0;
};

// Fast 64-bit non-cryptographic hash of a byte buffer (layer content hashes).
// Fixed algorithm (MurmurHash64A-style mixing over 8-byte words), endianness-defined.
uint64_t hashBytes64(const void* data, size_t len, uint64_t seed = 0x9E3779B97F4A7C15ull);

inline uint64_t mix64(uint64_t z) {  // SplitMix64 finalizer
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

std::string hex64(uint64_t v);

}  // namespace mapgen
