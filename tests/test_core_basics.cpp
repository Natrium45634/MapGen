// Phase A/B: RNG, hashing, JSON, parameters, grid topology, deterministic parallelism.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <set>

#include "core/grids/SphereGrid.h"
#include "core/rng/Noise.h"
#include "core/rng/Random.h"
#include "core/util/Json.h"
#include "core/util/Parallel.h"
#include "core/world/Params.h"

using namespace mapgen;
using Catch::Approx;

TEST_CASE("SHA-256 known answers", "[rng]") {
    REQUIRE(Sha256::hex(Sha256::of("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(Sha256::hex(Sha256::of("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string m(1000, 'a');
    Sha256 h;
    for (int i = 0; i < 1000; ++i) h.update(m);  // one million 'a'
    REQUIRE(Sha256::hex(h.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("PCG64 reference stream (state 42, sequence 54)", "[rng]") {
    Pcg64 r(U128{0, 42}, U128{0, 54});
    const uint64_t expect[] = {0x86b1da1d72062b68ull, 0x1304aa46c9853d39ull, 0xa3670e9e0dd50358ull,
                               0xf9090e529a7dae00ull, 0xc85b9fd837996f2cull, 0x606121f8e3919196ull};
    for (uint64_t e : expect) REQUIRE(r.next() == e);
}

TEST_CASE("Seed validation", "[rng]") {
    std::string err;
    REQUIRE(isValidSeed("A7Test42"));
    REQUIRE(isValidSeed(std::string(128, 'z')));
    REQUIRE_FALSE(isValidSeed(""));
    REQUIRE_FALSE(isValidSeed(std::string(129, 'z')));
    REQUIRE_FALSE(isValidSeed("A7 Test", &err));
    REQUIRE(err.find("позиции 3") != std::string::npos);
    REQUIRE_FALSE(isValidSeed("Сид"));
    REQUIRE_FALSE(isValidSeed("a-b"));
    for (int i = 0; i < 50; ++i) REQUIRE(isValidSeed(randomSeedString(static_cast<uint64_t>(i) * 7919u)));
}

TEST_CASE("Keyed substreams are independent and reproducible", "[rng]") {
    SeedKey a("A7Test42", 1), b("A7Test42", 1), c("a7Test42", 1), d("A7Test42", 2);
    REQUIRE(a.stream("tectonics", 3).next() == b.stream("tectonics", 3).next());
    REQUIRE(a.stream("tectonics", 3).next() != a.stream("tectonics", 4).next());
    REQUIRE(a.stream("tectonics", 3).next() != a.stream("climate", 3).next());
    REQUIRE(a.stream("tectonics").next() != c.stream("tectonics").next());   // case-sensitive
    REQUIRE(a.stream("tectonics").next() != d.stream("tectonics").next());   // algorithm version is part of the key
    // Drawing from one stream never changes another.
    Pcg64 s1 = a.stream("biomes");
    for (int i = 0; i < 1000; ++i) s1.next();
    REQUIRE(a.stream("plates").next() == b.stream("plates").next());
}

TEST_CASE("Uniform and integer draws are in range", "[rng]") {
    Pcg64 r = SeedKey("X", 1).stream("t");
    for (int i = 0; i < 10000; ++i) {
        double u = r.uniform();
        REQUIRE(u >= 0.0);
        REQUIRE(u < 1.0);
        int k = r.rangeInt(3, 9);
        REQUIRE(k >= 3);
        REQUIRE(k <= 9);
    }
}

TEST_CASE("Noise is continuous across the antimeridian and deterministic", "[rng]") {
    Noise3 n(12345);
    Vec3 e = fromLatLon(10 * kDegToRad, 179.9999 * kDegToRad);
    Vec3 w = fromLatLon(10 * kDegToRad, -179.9999 * kDegToRad);
    REQUIRE(std::fabs(n.fbm(e, 4, 5) - n.fbm(w, 4, 5)) < 1e-3);
    REQUIRE(n.fbm(e, 4, 5) == Noise3(12345).fbm(e, 4, 5));
}

TEST_CASE("JSON round trip keeps order and values", "[json]") {
    Json j = Json::object();
    j["b"] = 1.5;
    j["a"] = "текст \"кавычки\"\n";
    j["arr"] = Json::array();
    j["arr"].push(true);
    j["arr"].push(Json());
    j["n"] = 0.1;
    std::string s = j.dump(2);
    Json k = Json::parse(s);
    REQUIRE(k.dump(2) == s);
    REQUIRE(k["a"].asString() == "текст \"кавычки\"\n");
    REQUIRE(k["n"].asNumber() == 0.1);
    REQUIRE(k.members()[0].first == "b");
    REQUIRE_THROWS(Json::parse("{\"a\":}"));
    REQUIRE_THROWS(Json::parse("[1,2"));
}

TEST_CASE("Parameter schema: defaults, JSON, unknown fields, invalidation", "[params]") {
    Params p;
    for (auto& d : paramSchema()) {
        REQUIRE(!d.nameRu.empty());
        REQUIRE(d.stageInvalidated >= 0);
        REQUIRE(d.stageInvalidated <= 8);
        if (d.type != ParamType::Enum) {
            REQUIRE(d.defValue >= d.minValue);
            REQUIRE(d.defValue <= d.maxValue);
        }
    }
    Json j = p.toJson();
    j["future.option"] = 42;
    std::vector<std::string> warn;
    Params q = Params::fromJson(j, &warn);
    REQUIRE(q == p);
    REQUIRE(warn.size() == 1);
    REQUIRE(q.toJson()["future.option"].asNumber() == 42);  // preserved for round trip

    Params r = p;
    r.set("ocean.seaLevel", 120.0);
    REQUIRE(firstInvalidatedStage(p, r) == 3);
    r = p;
    r.set("climate.axialTilt", 30.0);
    REQUIRE(firstInvalidatedStage(p, r) == 4);
    r = p;
    r.set("tectonics.plateCountMax", 20);
    REQUIRE(firstInvalidatedStage(p, r) == 1);
    r = p;
    r.set("realism.enabled", 0.0);
    REQUIRE(firstInvalidatedStage(p, r) == 0);
    REQUIRE(firstInvalidatedStage(p, p) == 9);
}

TEST_CASE("Presets are explicit ranges and validate", "[params]") {
    REQUIRE(presets().size() == 8);
    for (auto& pr : presets()) {
        Params p;
        applyPreset(p, pr);
        for (auto& [id, range] : pr.ranges) {
            REQUIRE(p.num(id) >= range.lo - 1e-9);
            REQUIRE(p.num(id) <= range.hi + 1e-9);
        }
        for (auto& i : validateParams(p)) REQUIRE(i.level != ValidationIssue::Error);
    }
}

TEST_CASE("Realism validator reports conflicting wishes", "[params]") {
    Params p;
    p.set("relief.landFraction", 0.9);
    p.set("tectonics.continentalCrust", 0.3);
    p.set("tectonics.landmassCount", 2);
    auto issues = validateParams(p);
    int conflicts = 0;
    for (auto& i : issues) if (i.level == ValidationIssue::Conflict) ++conflicts;
    REQUIRE(conflicts >= 2);
    p.set("realism.enabled", 0.0);
    for (auto& i : validateParams(p)) REQUIRE(i.level != ValidationIssue::Conflict);
    Params e;
    e.set("tectonics.plateCountMin", 30);
    e.set("tectonics.plateCountMax", 10);
    bool err = false;
    for (auto& i : validateParams(e)) err |= i.level == ValidationIssue::Error;
    REQUIRE(err);
}

TEST_CASE("Topology: no seam at ±180°, correct neighbours across the poles", "[topology]") {
    SphereGrid g(64, 32);
    // Total area equals the sphere.
    double area = 0;
    for (int j = 0; j < g.height(); ++j) area += g.cellAreaKm2(j) * g.width();
    REQUIRE(area == Approx(g.totalAreaKm2()).epsilon(1e-9));
    // Every neighbour relation is symmetric and every cell has 8 distinct neighbours.
    for (int64_t k = 0; k < g.size(); ++k) {
        int64_t nb[8];
        g.neighbors8(k, nb);
        std::set<int64_t> s(nb, nb + 8);
        REQUIRE(s.size() == 8);
        REQUIRE(!s.count(k));
        for (int d = 0; d < 8; ++d) {
            int64_t back[8];
            g.neighbors8(nb[d], back);
            REQUIRE(std::count(back, back + 8, k) == 1);
            // Neighbours are physically close (no tearing at the seam or the poles).
            REQUIRE(g.greatCircleKm(k, nb[d]) < 3.0 * g.dyKm());
        }
    }
    // East neighbour of the last column is the first column.
    REQUIRE(g.neighbor(63, 10, 2) == g.index(0, 10));
    // North neighbour of a polar-row cell is across the pole.
    REQUIRE(g.neighbor(5, 0, 0) == g.index(5 + 32, 0));
    REQUIRE(g.cellAt(89.99, 179.99) == g.index(63, 0));
    REQUIRE(g.cellAt(-89.99, -180.0) == g.index(0, 31));
}

TEST_CASE("Distance transform matches great-circle distances", "[topology]") {
    SphereGrid g(128, 64);
    int64_t src = g.cellAt(0.0, 0.0);
    std::vector<float> d;
    distanceTransform(g, {src}, d, nullptr, 30000);
    int64_t far = g.cellAt(0.0, 90.0);
    double exact = g.greatCircleKm(src, far);
    REQUIRE(d[static_cast<size_t>(far)] == Approx(exact).epsilon(0.09));  // 8-neighbour metric error
    int64_t polar = g.cellAt(85.0, 120.0);
    REQUIRE(d[static_cast<size_t>(polar)] == Approx(g.greatCircleKm(src, polar)).epsilon(0.09));
}

TEST_CASE("Parallel reductions are identical for any thread count", "[parallel]") {
    std::vector<double> v(100000);
    for (size_t i = 0; i < v.size(); ++i) v[i] = std::sin(i * 0.37) * 1e-3 + 1.0 / (1 + i);
    auto sumWith = [&](int threads) {
        ThreadPool p(threads);
        return p.reduce<double>(0, static_cast<int64_t>(v.size()), 1024, 0.0,
                                [&](int64_t a, int64_t b) { double s = 0; for (int64_t i = a; i < b; ++i) s += v[static_cast<size_t>(i)]; return s; },
                                [](double x, double y) { return x + y; });
    };
    double s1 = sumWith(1), s4 = sumWith(4), s7 = sumWith(7);
    REQUIRE(s1 == s4);
    REQUIRE(s1 == s7);
    CancelToken c;
    c.cancel();
    ThreadPool p(4);
    REQUIRE_THROWS_AS(p.forChunks(0, 100, 1, [](int64_t, int64_t) {}, &c), CancelledError);
}
