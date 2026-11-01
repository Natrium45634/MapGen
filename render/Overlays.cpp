#include "render/Overlays.h"

#include <QPainterPath>
#include <cmath>

#include "core/world/Categories.h"

namespace mapgen {

namespace {

// Longitude copies of the world visible in the viewport.
std::vector<double> copiesFor(const MapProjection& pr, const QRectF& vp, double padCells = 2) {
    std::vector<double> offs;
    double left = pr.originX + vp.left() / pr.scale - padCells;
    double right = pr.originX + vp.right() / pr.scale + padCells;
    int k0 = static_cast<int>(std::floor(left / pr.W)) - 1;
    int k1 = static_cast<int>(std::floor(right / pr.W)) + 1;
    for (int k = k0; k <= k1; ++k) offs.push_back(k * static_cast<double>(pr.W));
    return offs;
}

void drawPolyline(QPainter& p, const Polyline& line, const MapProjection& pr, const std::vector<double>& offs,
                  const QRectF& vp) {
    if (line.pts.size() < 2) return;
    double minx = 1e18, maxx = -1e18, miny = 1e18, maxy = -1e18;
    std::vector<QPointF> base;
    base.reserve(line.pts.size());
    for (auto& g : line.pts) {
        double x = pr.cellX(g.lon), y = pr.cellY(g.lat);
        base.emplace_back(x, y);
        minx = std::min(minx, x); maxx = std::max(maxx, x); miny = std::min(miny, y); maxy = std::max(maxy, y);
    }
    for (double off : offs) {
        QPointF a = pr.toScreen(minx + off, miny), b = pr.toScreen(maxx + off, maxy);
        if (b.x() < vp.left() - 4 || a.x() > vp.right() + 4 || b.y() < vp.top() - 4 || a.y() > vp.bottom() + 4) continue;
        QPainterPath path;
        path.moveTo(pr.toScreen(base[0].x() + off, base[0].y()));
        for (size_t i = 1; i < base.size(); ++i) {
            // Break at an unwrapped jump (should not happen inside one segment).
            if (std::fabs(base[i].x() - base[i - 1].x()) > pr.W / 2.0) path.moveTo(pr.toScreen(base[i].x() + off, base[i].y()));
            else path.lineTo(pr.toScreen(base[i].x() + off, base[i].y()));
        }
        p.drawPath(path);
    }
}

void arrow(QPainter& p, QPointF a, QPointF b, double head) {
    p.drawLine(a, b);
    QPointF d = b - a;
    double len = std::hypot(d.x(), d.y());
    if (len < 1e-6) return;
    QPointF u = d / len, n(-u.y(), u.x());
    p.drawLine(b, b - u * head + n * head * 0.5);
    p.drawLine(b, b - u * head - n * head * 0.5);
}

void star(QPainter& p, QPointF c, double r) {
    QPainterPath path;
    for (int i = 0; i < 10; ++i) {
        double a = -kPi / 2 + i * kPi / 5;
        double rr = (i % 2) ? r * 0.45 : r;
        QPointF q(c.x() + rr * std::cos(a), c.y() + rr * std::sin(a));
        if (i == 0) path.moveTo(q); else path.lineTo(q);
    }
    path.closeSubpath();
    p.drawPath(path);
}

}  // namespace

void paintOverlays(QPainter& p, const WorldData& w, MapTab tab, const ViewOptions& view, const OverlayOptions& ov,
                   const MapProjection& pr, const QRectF& vp) {
    if (!w.grid.valid()) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setOpacity(std::clamp(ov.opacity, 0.0, 1.0));
    const auto offs = copiesFor(pr, vp);
    const double zoom = pr.scale;  // px per cell

    // Coastlines.
    bool wantCoast = ov.coastline && w.ocean &&
                     (tab == MapTab::Physical || tab == MapTab::Elevation || tab == MapTab::Temperature ||
                      tab == MapTab::Precipitation || tab == MapTab::Humidity || tab == MapTab::Wind ||
                      tab == MapTab::Plates || tab == MapTab::Volcanism || tab == MapTab::SnowIce);
    if (wantCoast) {
        QPen pen(QColor(30, 40, 50, tab == MapTab::Physical ? 150 : 200));
        pen.setWidthF(std::clamp(zoom * 0.18, 0.6, 1.6));
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        for (auto& c : w.ocean->coastlines) drawPolyline(p, c, pr, offs, vp);
    }

    // Rivers.
    bool wantRivers = ov.rivers && w.hydro &&
                      (tab == MapTab::Physical || tab == MapTab::Rivers || tab == MapTab::Elevation ||
                       tab == MapTab::Biomes || tab == MapTab::Landforms || tab == MapTab::Koppen ||
                       tab == MapTab::Precipitation || tab == MapTab::Coasts);
    if (wantRivers) {
        int minOrder = zoom >= 4 ? 1 : (zoom >= 2 ? 2 : (zoom >= 1 ? 3 : 4));
        if (tab == MapTab::Rivers) minOrder = std::max(1, minOrder - 1);
        for (auto& r : w.hydro->rivers) {
            if (r.order < minOrder && r.riverClass != kRiverDistributary) continue;
            QColor col = tab == MapTab::Physical ? QColor(40, 100, 190) : QColor(20, 80, 180);
            if (r.riverClass == kRiverDistributary) col = QColor(30, 150, 140);
            QPen pen(col);
            double width = (0.35 + 0.28 * r.order) * std::clamp(zoom / 2.0, 0.5, 3.0);
            pen.setWidthF(std::clamp(width, 0.5, 6.0));
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            if (r.riverClass == kRiverSeasonal) pen.setStyle(Qt::DashLine);
            if (r.riverClass == kRiverIntermittent) { pen.setStyle(Qt::DotLine); pen.setColor(QColor(150, 120, 70)); }
            p.setPen(pen);
            drawPolyline(p, r.line, pr, offs, vp);
        }
        if (ov.waterfalls && tab == MapTab::Rivers && zoom >= 2) {
            p.setPen(QPen(QColor(255, 255, 255), 1));
            p.setBrush(QColor(0, 60, 140));
            for (auto& f : w.hydro->waterfalls)
                for (double off : offs) {
                    QPointF s = pr.toScreen(pr.cellX(f.lon) + off, pr.cellY(f.lat));
                    if (!vp.adjusted(-5, -5, 5, 5).contains(s)) continue;
                    p.drawRect(QRectF(s.x() - 2.5, s.y() - 2.5, 5, 5));
                }
        }
    }

    // Plate motion arrows.
    if (ov.plateArrows && tab == MapTab::Plates && w.has(LayerId::PlateVelE)) {
        const float* ve = w.f32(LayerId::PlateVelE);
        const float* vn = w.f32(LayerId::PlateVelN);
        double stepPx = 48.0;
        int step = std::max(1, static_cast<int>(stepPx / zoom));
        p.setPen(QPen(QColor(20, 20, 20, 210), 1.3));
        for (int y = step / 2; y < pr.H; y += step) {
            double lat = 90.0 - (y + 0.5) * 180.0 / pr.H;
            int sx = std::max(1, static_cast<int>(step / std::max(0.15, std::cos(lat * kDegToRad))));
            int x0 = static_cast<int>(std::floor(pr.originX + vp.left() / zoom)) - sx;
            int x1 = static_cast<int>(std::ceil(pr.originX + vp.right() / zoom)) + sx;
            for (int x = (x0 / sx) * sx; x <= x1; x += sx) {
                int i = ((x % pr.W) + pr.W) % pr.W;
                int64_t k = w.grid.index(i, y);
                double e = ve[k], n = vn[k];
                double len = std::hypot(e, n);
                if (len < 0.05) continue;
                QPointF a = pr.toScreen(x + 0.5, y + 0.5);
                double L = std::min(stepPx * 0.8, 6.0 + len * 4.0);
                QPointF b = a + QPointF(e / len * L, -n / len * L);
                arrow(p, a, b, 5);
            }
        }
    }

    // Wind arrows (climate grid).
    if (ov.windArrows && tab == MapTab::Wind && w.has(LayerId::WindU)) {
        const LayerData* U = w.layer(LayerId::WindU);
        const LayerData* V = w.layer(LayerId::WindV);
        const SphereGrid& cg = w.climateGrid;
        double cellPx = zoom * pr.W / cg.width();
        int step = std::max(1, static_cast<int>(36.0 / cellPx));
        p.setPen(QPen(QColor(20, 30, 60, 220), 1.2));
        for (int cj = step / 2; cj < cg.height(); cj += step) {
            int sx = std::max(1, static_cast<int>(step / std::max(0.2, cg.cosLat(cj))));
            double fy = (cj + 0.5) * pr.H / cg.height();
            double x0 = (pr.originX + vp.left() / zoom) * cg.width() / pr.W - sx;
            double x1 = (pr.originX + vp.right() / zoom) * cg.width() / pr.W + sx;
            for (int cx = static_cast<int>(std::floor(x0 / sx)) * sx; cx <= x1; cx += sx) {
                int ci = ((cx % cg.width()) + cg.width()) % cg.width();
                int64_t k = cg.index(ci, cj);
                double u = 0, v = 0;
                if (view.month >= 0) { u = U->band<float>(view.month)[k]; v = V->band<float>(view.month)[k]; }
                else for (int m = 0; m < 12; ++m) { u += U->band<float>(m)[k] / 12.0; v += V->band<float>(m)[k] / 12.0; }
                double s = std::hypot(u, v);
                if (s < 0.3) continue;
                double fx = (cx + 0.5) * pr.W / cg.width();
                QPointF a = pr.toScreen(fx, fy);
                double L = std::min(30.0, 5.0 + s * 2.2);
                QPointF b = a + QPointF(u / s * L, -v / s * L);
                arrow(p, a - (b - a) * 0.5, a + (b - a) * 0.5, 4.5);
            }
        }
    }

    // Volcanoes, hotspots, springs.
    if (ov.volcanoes && tab == MapTab::Volcanism && w.geomorph) {
        const auto& vs = w.geomorph->volcanoes;
        const GeoActivityFeatures* ga = w.geoActivity.get();
        double r = std::clamp(zoom * 1.2, 2.5, 7.0);
        for (size_t i = 0; i < vs.size(); ++i) {
            const Volcano& v = vs[i];
            int act = kVaActive;
            bool sub = false;
            if (ga && i < ga->volcanoStates.size()) { act = ga->volcanoStates[i].activity; sub = ga->volcanoStates[i].submarine; }
            if (act == kVaExtinct && zoom < 1.5) continue;
            for (double off : offs) {
                QPointF s = pr.toScreen(pr.cellX(v.lon) + off, pr.cellY(v.lat));
                if (!vp.adjusted(-8, -8, 8, 8).contains(s)) continue;
                if (act == kVaExtinct) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(QColor(60, 50, 50, 180));
                    p.drawEllipse(s, r * 0.35, r * 0.35);
                    continue;
                }
                QPolygonF tri;
                tri << QPointF(s.x(), s.y() - r) << QPointF(s.x() - r * 0.9, s.y() + r * 0.7) << QPointF(s.x() + r * 0.9, s.y() + r * 0.7);
                p.setPen(QPen(sub ? QColor(20, 90, 200) : QColor(40, 10, 10), sub ? 1.6 : 1.0));
                p.setBrush(act == kVaActive ? QColor(220, 30, 20) : QColor(255, 255, 255, 220));
                p.drawPolygon(tri);
            }
        }
        p.setPen(QPen(QColor(90, 30, 0), 1));
        p.setBrush(QColor(255, 170, 0));
        for (auto& h : w.geomorph->hotspots)
            for (double off : offs) {
                QPointF s = pr.toScreen(pr.cellX(h.lon) + off, pr.cellY(h.lat));
                if (vp.adjusted(-10, -10, 10, 10).contains(s)) star(p, s, std::clamp(zoom * 2.0, 5.0, 10.0));
            }
        if (ov.springs && ga && zoom >= 1.0) {
            for (auto& sp : ga->springs)
                for (double off : offs) {
                    QPointF s = pr.toScreen(pr.cellX(sp.lon) + off, pr.cellY(sp.lat));
                    if (!vp.adjusted(-5, -5, 5, 5).contains(s)) continue;
                    if (sp.geyser) { p.setPen(QPen(QColor(0, 70, 120), 1)); p.setBrush(QColor(0, 170, 220)); p.drawEllipse(s, 3.2, 3.2); }
                    else { p.setPen(QPen(QColor(0, 110, 150), 1.2)); p.setBrush(Qt::NoBrush); p.drawEllipse(s, 2.4, 2.4); }
                }
        }
    }
    p.restore();
}

}  // namespace mapgen
