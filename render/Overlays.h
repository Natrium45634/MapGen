#pragma once

// Vector overlays (rivers, coastlines, plate motion, winds, volcanoes, springs)
// painted with QPainter in map-cell coordinates. Used by the interactive views
// and by PNG export, so the exported picture matches the screen.

#include <QPainter>
#include <QRectF>

#include "core/world/WorldData.h"
#include "render/Colorizer.h"

namespace mapgen {

// Screen position of map-cell coordinates: screen = (cell - origin) * scale.
struct MapProjection {
    double scale = 1.0;     // screen pixels per cell
    double originX = 0.0;   // cell coordinate at screen x = 0 (may be negative / beyond W: wraps)
    double originY = 0.0;
    int W = 1, H = 1;
    QPointF toScreen(double cx, double cy) const { return {(cx - originX) * scale, (cy - originY) * scale}; }
    double cellX(double lonDeg) const { return (lonDeg + 180.0) / 360.0 * W; }
    double cellY(double latDeg) const { return (90.0 - latDeg) / 180.0 * H; }
};

struct OverlayOptions {
    bool rivers = true;
    bool coastline = true;
    bool plateArrows = true;
    bool windArrows = true;
    bool volcanoes = true;
    bool springs = true;
    bool waterfalls = true;
    double opacity = 1.0;
};

void paintOverlays(QPainter& p, const WorldData& w, MapTab tab, const ViewOptions& view, const OverlayOptions& ov,
                   const MapProjection& proj, const QRectF& viewport);

}  // namespace mapgen
