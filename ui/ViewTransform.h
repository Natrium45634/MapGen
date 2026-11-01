#pragma once

// Pure view math shared by the OpenGL and software map views (unit-tested).
// World coordinates are fractional cell coordinates: x in [0, W) wraps around
// the globe, y in [0, H) from the north pole down. Zoom = screen px per cell.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mapgen {

struct ViewTransform {
    double W = 1, H = 1;           // world size, cells
    double viewW = 1, viewH = 1;   // viewport, px
    double centerX = 0.5, centerY = 0.5;
    double zoom = 1;
    double maxZoom = 64;

    double fitZoom() const { return std::min(viewW / W, viewH / H); }
    double minZoom() const { return fitZoom() * 0.999; }

    void setWorld(double w, double h) { W = w; H = h; }
    void setViewport(double w, double h) {
        viewW = std::max(1.0, w);
        viewH = std::max(1.0, h);
        clamp();
    }
    void fit() {
        zoom = fitZoom();
        centerX = W / 2;
        centerY = H / 2;
    }

    double originX() const { return centerX - viewW / (2 * zoom); }
    double originY() const { return centerY - viewH / (2 * zoom); }

    void worldToScreen(double x, double y, double& sx, double& sy) const {
        sx = (x - originX()) * zoom;
        sy = (y - originY()) * zoom;
    }
    void screenToWorld(double sx, double sy, double& x, double& y) const {
        x = originX() + sx / zoom;
        y = originY() + sy / zoom;
    }

    // Cell under a screen point; -1 when outside the latitude range.
    int64_t cellAt(double sx, double sy, int iw, int ih) const {
        double x, y;
        screenToWorld(sx, sy, x, y);
        if (y < 0 || y >= H) return -1;
        int i = static_cast<int>(std::floor(x));
        i %= iw;
        if (i < 0) i += iw;
        int j = std::clamp(static_cast<int>(std::floor(y)), 0, ih - 1);
        return static_cast<int64_t>(j) * iw + i;
    }

    // Zoom by factor keeping the world point under (sx, sy) fixed on screen.
    void zoomAt(double factor, double sx, double sy) {
        double wx, wy;
        screenToWorld(sx, sy, wx, wy);
        zoom = std::clamp(zoom * factor, minZoom(), maxZoom);
        centerX = wx - (sx - viewW / 2) / zoom;
        centerY = wy - (sy - viewH / 2) / zoom;
        clamp();
    }

    void panBy(double dxPx, double dyPx) {
        centerX -= dxPx / zoom;
        centerY -= dyPx / zoom;
        clamp();
    }

    // Longitude wraps freely; latitude is clamped so poles are never confused.
    void clamp() {
        zoom = std::clamp(zoom, minZoom(), maxZoom);
        centerX = std::fmod(centerX, W);
        if (centerX < 0) centerX += W;
        double halfH = viewH / (2 * zoom);
        if (2 * halfH >= H) centerY = H / 2;
        else centerY = std::clamp(centerY, halfH, H - halfH);
    }
};

}  // namespace mapgen
