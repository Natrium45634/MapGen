#include "core/rng/Random.h"

namespace mapgen {

bool isValidSeed(std::string_view seed, std::string* errorRu) {
    if (seed.empty()) {
        if (errorRu) *errorRu = "Сид не может быть пустым.";
        return false;
    }
    if (seed.size() > 128) {
        if (errorRu) *errorRu = "Сид длиннее 128 символов.";
        return false;
    }
    for (size_t i = 0; i < seed.size(); ++i) {
        char c = seed[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!ok) {
            if (errorRu)
                *errorRu = "Недопустимый символ в позиции " + std::to_string(i + 1) +
                           ": разрешены только латинские буквы A–Z, a–z и цифры 0–9.";
            return false;
        }
    }
    return true;
}

SeedKey::SeedKey(std::string seed, int algorithmVersion) : seed_(std::move(seed)), algoVersion_(algorithmVersion) {
    Sha256 h;
    h.update("mapgen-seed\0", 12);
    h.update(seed_);
    root_ = h.finish();
}

Sha256::Digest SeedKey::derive(std::string_view stageId, uint64_t objectId) const {
    Sha256 h;
    h.update(root_.data(), root_.size());
    h.updateU64(static_cast<uint64_t>(algoVersion_));
    h.updateU64(stageId.size());
    h.update(stageId);
    h.updateU64(objectId);
    return h.finish();
}

Pcg64 SeedKey::stream(std::string_view stageId, uint64_t objectId) const {
    auto d = derive(stageId, objectId);
    auto word = [&](int off) {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(d[static_cast<size_t>(off + i)]) << (8 * i);
        return v;
    };
    return Pcg64(U128{word(0), word(8)}, U128{word(16), word(24)});
}

uint64_t SeedKey::key64(std::string_view stageId, uint64_t objectId) const {
    auto d = derive(stageId, objectId);
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(d[static_cast<size_t>(i)]) << (8 * i);
    return v;
}

std::string randomSeedString(uint64_t entropy, int length) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    Pcg64 rng(U128{mix64(entropy), entropy}, U128{0x5851F42D4C957F2Dull, mix64(entropy ^ 0xABCDEFull)});
    std::string s;
    for (int i = 0; i < length; ++i) s.push_back(alphabet[rng.below(62)]);
    return s;
}

}  // namespace mapgen
