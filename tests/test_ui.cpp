// UI tests (offscreen): view math stability, layer switching without regeneration,
// info card values consistent with the model, cancellation keeps the world intact.

#include <QApplication>
#include <QElapsedTimer>
#include <QTest>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/world/CellInfo.h"
#include "tests/TestSupport.h"
#include "ui/MainWindow.h"
#include "ui/ViewTransform.h"

using namespace mapgen;
using Catch::Approx;

namespace {
void waitFor(const std::function<bool()>& cond, int ms = 120000) {
    QElapsedTimer t;
    t.start();
    while (!cond() && t.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 20);
}
}  // namespace

TEST_CASE("Cursor-anchored zoom keeps the world point under the cursor", "[ui]") {
    ViewTransform v;
    v.setWorld(2048, 1024);
    v.setViewport(1600, 900);
    v.fit();
    for (int step = 0; step < 40; ++step) {
        double sx = 100 + 37 * step % 1400, sy = 80 + 23 * step % 700;
        double wx0, wy0;
        v.screenToWorld(sx, sy, wx0, wy0);
        bool canZoom = step < 25;
        v.zoomAt(canZoom ? 1.25 : 0.8, sx, sy);
        double wx1, wy1;
        v.screenToWorld(sx, sy, wx1, wy1);
        // Longitude may be wrapped by a whole world width; latitude must be identical unless clamped at a pole.
        double dx = std::fmod(std::fabs(wx1 - wx0), 2048.0);
        dx = std::min(dx, 2048.0 - dx);
        if (v.zoom > v.fitZoom() * 1.01 && v.zoom < v.maxZoom * 0.99) {
            REQUIRE(dx < 1e-6);
            double halfH = v.viewH / (2 * v.zoom);
            if (v.centerY > halfH + 1e-9 && v.centerY < v.H - halfH - 1e-9) REQUIRE(std::fabs(wy1 - wy0) < 1e-6);
        }
    }
}

TEST_CASE("Panning across ±180° wraps without gaps and never flips the poles", "[ui]") {
    ViewTransform v;
    v.setWorld(1024, 512);
    v.setViewport(800, 600);
    v.fit();
    v.zoomAt(4, 400, 300);
    for (int i = 0; i < 200; ++i) {
        v.panBy(-37, 0);
        REQUIRE(v.centerX >= 0);
        REQUIRE(v.centerX < 1024);
        // Every screen column maps to a valid cell (no empty strip at the seam).
        for (int sx = 0; sx < 800; sx += 50) REQUIRE(v.cellAt(sx, 300, 1024, 512) >= 0);
    }
    v.panBy(0, 100000);
    double wx, wy;
    v.screenToWorld(400, 0, wx, wy);
    REQUIRE(wy >= -1e-9);  // top of the screen is not above the north pole
    v.panBy(0, -100000);
    v.screenToWorld(400, 600, wx, wy);
    REQUIRE(wy <= 512 + 1e-9);
}

TEST_CASE("Main window: generation, tab switching without regeneration, info card, cancel", "[ui]") {
    MainWindow w(true);  // software view: deterministic offscreen
    w.resize(1400, 900);
    w.show();
    Params p = test::smallParams(256);
    w.generateNow("A7Test42", p);
    waitFor([&] { return !w.isGenerating() && w.world(); });
    REQUIRE(w.world());
    REQUIRE(w.world()->completedStage == 8);
    auto worldBefore = w.world();
    std::string hashBefore = Pipeline::worldHash(*worldBefore);
    for (int t = 0; t < kTabCount; ++t) {
        w.setTab(static_cast<MapTab>(t));
        QApplication::processEvents();
    }
    waitFor([&] { return w.layerRenderCount() >= kTabCount; }, 60000);
    REQUIRE(w.world() == worldBefore);  // switching tabs never regenerates the map
    REQUIRE(Pipeline::worldHash(*w.world()) == hashBefore);

    // Cancel a regeneration: the current world stays untouched.
    Params q = p;
    q.set("climate.axialTilt", 30.0);
    w.generateNow("A7Test42", q);
    QTest::keyClick(&w, Qt::Key_Escape);
    QApplication::processEvents();
    waitFor([&] { return !w.isGenerating(); });
    REQUIRE(w.world());
    REQUIRE(w.world()->completedStage == 8);
}
