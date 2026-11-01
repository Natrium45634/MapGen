#include "ui/MapView.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QPainter>
#include <QWheelEvent>
#include <cmath>

namespace mapgen {

// ---------------------------------------------------------------- interaction

MapInteraction::MapInteraction(QWidget* target) : QObject(target), target_(target) {
    anim_.setDuration(140);
    anim_.setStartValue(0.0);
    anim_.setEndValue(1.0);
    anim_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        double t = v.toDouble();
        double z = animFrom_ * std::pow(animTo_ / animFrom_, t);
        view_.zoomAt(z / view_.zoom, animAt_.x(), animAt_.y());
        emit viewChanged();
        target_->update();
        updateHover(lastMouse_);
    });
}

void MapInteraction::setWorldSize(int w, int h) {
    bool first = gridW_ == 0;
    bool changed = gridW_ != w || gridH_ != h;
    if (changed && !first) {
        // Keep the same geographic view when the resolution changes.
        double sx = static_cast<double>(w) / gridW_;
        view_.centerX *= sx;
        view_.centerY *= sx;
        view_.zoom /= sx;
    }
    gridW_ = w;
    gridH_ = h;
    view_.setWorld(w, h);
    view_.maxZoom = std::max(8.0, 64.0 * 2048.0 / std::max(1, w));
    if (first) view_.fit();
    view_.clamp();
}

void MapInteraction::resize(int w, int h) {
    bool wasFit = std::fabs(view_.zoom - view_.fitZoom()) < 1e-9;
    view_.setViewport(w, h);
    if (wasFit || view_.zoom < view_.minZoom()) view_.zoom = view_.fitZoom();
    view_.clamp();
}

void MapInteraction::fitWorld() {
    anim_.stop();
    view_.fit();
    emit viewChanged();
    target_->update();
}

void MapInteraction::zoomBy(double factor, QPointF at) {
    double cur = view_.zoom;
    double base = anim_.state() == QAbstractAnimation::Running ? animTo_ : cur;
    double target = std::clamp(base * factor, view_.minZoom(), view_.maxZoom);
    anim_.stop();
    animFrom_ = cur;
    animTo_ = target;
    animAt_ = at;
    if (std::fabs(animTo_ - animFrom_) < 1e-12) return;
    anim_.start();
}

void MapInteraction::setPinned(bool p, int64_t cell) {
    pinned_ = p;
    if (p && cell >= 0) hovered_ = cell;
    emit pinChanged(pinned_, hovered_);
}

void MapInteraction::updateHover(QPointF pos) {
    if (pinned_ || gridW_ == 0) return;
    int64_t c = view_.cellAt(pos.x(), pos.y(), gridW_, gridH_);
    if (c == hovered_) return;
    hovered_ = c;
    double lat = 0, lon = 0;
    if (c >= 0) {
        int j = static_cast<int>(c / gridW_), i = static_cast<int>(c % gridW_);
        lat = 90.0 - (j + 0.5) * 180.0 / gridH_;
        lon = -180.0 + (i + 0.5) * 360.0 / gridW_;
    }
    emit hoverCell(c, lat, lon);
}

bool MapInteraction::handle(QEvent* e) {
    switch (e->type()) {
        case QEvent::Wheel: {
            auto* we = static_cast<QWheelEvent*>(e);
            double steps = we->angleDelta().y() / 120.0;
            if (steps == 0) steps = we->pixelDelta().y() / 60.0;
            if (steps != 0) zoomBy(std::pow(1.25, steps), we->position());
            return true;
        }
        case QEvent::MouseButtonPress: {
            auto* me = static_cast<QMouseEvent*>(e);
            target_->setFocus();
            pressPos_ = lastPos_ = me->position();
            moved_ = false;
            if (me->button() == Qt::MiddleButton || me->button() == Qt::RightButton ||
                (me->button() == Qt::LeftButton && spaceDown_)) {
                dragging_ = true;
                target_->setCursor(Qt::ClosedHandCursor);
            }
            return true;
        }
        case QEvent::MouseMove: {
            auto* me = static_cast<QMouseEvent*>(e);
            lastMouse_ = me->position();
            if (dragging_) {
                QPointF d = me->position() - lastPos_;
                lastPos_ = me->position();
                if (std::abs(d.x()) + std::abs(d.y()) > 0) moved_ = true;
                view_.panBy(d.x(), d.y());
                emit viewChanged();
                target_->update();
            }
            updateHover(me->position());
            return true;
        }
        case QEvent::MouseButtonRelease: {
            auto* me = static_cast<QMouseEvent*>(e);
            if (dragging_) {
                dragging_ = false;
                target_->setCursor(spaceDown_ ? Qt::OpenHandCursor : Qt::CrossCursor);
            } else if (me->button() == Qt::LeftButton && (me->position() - pressPos_).manhattanLength() < 4) {
                int64_t c = view_.cellAt(me->position().x(), me->position().y(), gridW_, gridH_);
                if (pinned_ && c == hovered_) setPinned(false);
                else {
                    pinned_ = false;
                    hovered_ = -2;
                    updateHover(me->position());
                    setPinned(true, c);
                }
            }
            return true;
        }
        case QEvent::MouseButtonDblClick: {
            auto* me = static_cast<QMouseEvent*>(e);
            if (me->button() == Qt::LeftButton) zoomBy(2.0, me->position());
            return true;
        }
        case QEvent::KeyPress: {
            auto* ke = static_cast<QKeyEvent*>(e);
            QPointF centre(view_.viewW / 2, view_.viewH / 2);
            switch (ke->key()) {
                case Qt::Key_Home: fitWorld(); return true;
                case Qt::Key_Plus: case Qt::Key_Equal: zoomBy(1.5, centre); return true;
                case Qt::Key_Minus: zoomBy(1 / 1.5, centre); return true;
                case Qt::Key_Left: view_.panBy(80, 0); emit viewChanged(); target_->update(); return true;
                case Qt::Key_Right: view_.panBy(-80, 0); emit viewChanged(); target_->update(); return true;
                case Qt::Key_Up: view_.panBy(0, 80); emit viewChanged(); target_->update(); return true;
                case Qt::Key_Down: view_.panBy(0, -80); emit viewChanged(); target_->update(); return true;
                case Qt::Key_F: setPinned(!pinned_, hovered_); return true;
                case Qt::Key_Space:
                    if (!ke->isAutoRepeat()) { spaceDown_ = true; target_->setCursor(Qt::OpenHandCursor); }
                    return true;
                default: return false;
            }
        }
        case QEvent::KeyRelease: {
            auto* ke = static_cast<QKeyEvent*>(e);
            if (ke->key() == Qt::Key_Space && !ke->isAutoRepeat()) {
                spaceDown_ = false;
                if (!dragging_) target_->setCursor(Qt::CrossCursor);
                return true;
            }
            return false;
        }
        case QEvent::Leave:
            if (!pinned_ && hovered_ != -1) { hovered_ = -1; emit hoverCell(-1, 0, 0); }
            return false;
        default: return false;
    }
}

// ---------------------------------------------------------------- OpenGL view

namespace {
const char* kVert = R"(#version 330 core
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;
uniform vec2 viewport;
out vec2 vuv;
void main() {
    vuv = uv;
    gl_Position = vec4(pos.x / viewport.x * 2.0 - 1.0, 1.0 - pos.y / viewport.y * 2.0, 0.0, 1.0);
})";
const char* kFrag = R"(#version 330 core
in vec2 vuv;
uniform sampler2D colorTex;
uniform sampler2D heightTex;
uniform int contours;
uniform float interval;
out vec4 frag;
void main() {
    vec4 c = texture(colorTex, vuv);
    if (contours == 1) {
        float h = texture(heightTex, vuv).r;
        float f = h / interval;
        float w = max(fwidth(f), 1e-5);
        float d = abs(fract(f - 0.5) - 0.5) / w;
        float line = 1.0 - clamp(d - 0.25, 0.0, 1.0);
        float idx = floor(f + 0.5);
        float major = (mod(abs(idx), 5.0) < 0.5) ? 0.8 : 0.45;
        if (w < 0.6) c.rgb = mix(c.rgb, vec3(0.12), line * major);
    }
    frag = vec4(c.rgb, 1.0);
})";
}  // namespace

bool openGl33Available(QString* reason) {
    QOpenGLContext ctx;
    QSurfaceFormat f;
    f.setVersion(3, 3);
    f.setProfile(QSurfaceFormat::CoreProfile);
    ctx.setFormat(f);
    if (!ctx.create()) {
        if (reason) *reason = "не удалось создать контекст OpenGL";
        return false;
    }
    QSurfaceFormat got = ctx.format();
    if (got.majorVersion() < 3 || (got.majorVersion() == 3 && got.minorVersion() < 3)) {
        if (reason) *reason = QString("доступен только OpenGL %1.%2").arg(got.majorVersion()).arg(got.minorVersion());
        return false;
    }
    QOffscreenSurface surf;
    surf.setFormat(got);
    surf.create();
    if (!ctx.makeCurrent(&surf)) {
        if (reason) *reason = "контекст OpenGL не активируется";
        return false;
    }
    ctx.doneCurrent();
    return true;
}

GlMapView::GlMapView(QWidget* parent) : QOpenGLWidget(parent), inter_(new MapInteraction(this)) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    connect(inter_, &MapInteraction::viewChanged, this, [this] { update(); });
}

GlMapView::~GlMapView() {
    makeCurrent();
    cache_.clear();
    vbo_.destroy();
    vao_.destroy();
    doneCurrent();
}

void GlMapView::initializeGL() {
    if (!initializeOpenGLFunctions()) {
        failed_ = true;
        emit glFailed("функции OpenGL 3.3 Core недоступны");
        return;
    }
    if (!prog_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVert) ||
        !prog_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFrag) || !prog_.link()) {
        failed_ = true;
        emit glFailed("шейдеры не компилируются: " + prog_.log());
        return;
    }
    vao_.create();
    vbo_.create();
    GLint mt = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &mt);
    maxTex_ = std::clamp(static_cast<int>(mt), 1024, 8192);
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    backend_ = QString("OpenGL %1.%2 — %3").arg(format().majorVersion()).arg(format().minorVersion()).arg(renderer ? renderer : "?");
    glReady_ = true;
}

void GlMapView::resizeGL(int w, int h) { inter_->resize(w, h); }

bool GlMapView::event(QEvent* e) {
    if (inter_->handle(e)) return true;
    return QOpenGLWidget::event(e);
}

void GlMapView::setWorld(std::shared_ptr<const WorldData> w, MapTab tab, const ViewOptions& view) {
    world_ = std::move(w);
    tab_ = tab;
    view_ = view;
    if (world_ && world_->grid.valid()) inter_->setWorldSize(world_->grid.width(), world_->grid.height());
    update();
}

void GlMapView::setLayerImage(const LayerImage& img) {
    pending_ = std::make_unique<LayerImage>(img);
    current_ = img.key;
    update();
}

bool GlMapView::hasCached(const QString& key) const {
    if (pending_ && pending_->key == key) return true;
    for (auto& c : cache_) if (c.key == key) return true;
    return false;
}

void GlMapView::showCached(const QString& key) {
    current_ = key;
    for (auto it = cache_.begin(); it != cache_.end(); ++it)
        if (it->key == key) { cache_.splice(cache_.begin(), cache_, it); break; }
    update();
}

void GlMapView::clearCache() {
    makeCurrent();
    cache_.clear();
    doneCurrent();
    pending_.reset();
    current_.clear();
    update();
}

void GlMapView::evict() {
    size_t total = 0;
    for (auto& c : cache_) total += c.bytes;
    while (total > budgetBytes_ && cache_.size() > 1) {
        total -= cache_.back().bytes;
        cache_.pop_back();
    }
}

void GlMapView::uploadPending() {
    if (!pending_) return;
    std::unique_ptr<LayerImage> img = std::move(pending_);
    for (auto it = cache_.begin(); it != cache_.end(); ++it)
        if (it->key == img->key) { cache_.erase(it); break; }
    CachedLayer cl;
    cl.key = img->key;
    const QImage& src = img->image;
    const int W = src.width(), H = src.height();
    const int ts = maxTex_;
    bool fullWidth = W <= ts;
    for (int y0 = 0; y0 < H; y0 += ts)
        for (int x0 = 0; x0 < W; x0 += ts) {
            Tile t;
            t.x0 = x0;
            t.y0 = y0;
            t.w = std::min(ts, W - x0);
            t.h = std::min(ts, H - y0);
            QImage part = (t.w == W && t.h == H) ? src : src.copy(x0, y0, t.w, t.h);
            part = part.convertToFormat(QImage::Format_ARGB32);
            t.color = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
            t.color->setSize(t.w, t.h);
            t.color->setFormat(QOpenGLTexture::RGBA8_UNorm);
            t.color->setMipLevels(t.color->maximumMipLevels());
            t.color->allocateStorage(QOpenGLTexture::BGRA, QOpenGLTexture::UInt8);
            QOpenGLPixelTransferOptions opt;
            opt.setAlignment(4);
            opt.setRowLength(part.bytesPerLine() / 4);
            t.color->setData(0, QOpenGLTexture::BGRA, QOpenGLTexture::UInt8, part.constBits(), &opt);
            t.color->generateMipMaps();
            t.color->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
            t.color->setMagnificationFilter(img->categorical ? QOpenGLTexture::Nearest : QOpenGLTexture::Linear);
            t.color->setWrapMode(QOpenGLTexture::DirectionS, fullWidth ? QOpenGLTexture::Repeat : QOpenGLTexture::ClampToEdge);
            t.color->setWrapMode(QOpenGLTexture::DirectionT, QOpenGLTexture::ClampToEdge);
            cl.bytes += static_cast<size_t>(t.w) * t.h * 4 * 4 / 3;
            if (img->heights && img->heights->size() == static_cast<size_t>(W) * H) {
                std::vector<float> hp(static_cast<size_t>(t.w) * t.h);
                for (int y = 0; y < t.h; ++y)
                    std::copy_n(img->heights->data() + static_cast<size_t>(y0 + y) * W + x0, t.w, hp.data() + static_cast<size_t>(y) * t.w);
                t.height = std::make_unique<QOpenGLTexture>(QOpenGLTexture::Target2D);
                t.height->setSize(t.w, t.h);
                t.height->setFormat(QOpenGLTexture::R32F);
                t.height->setMipLevels(1);
                t.height->allocateStorage(QOpenGLTexture::Red, QOpenGLTexture::Float32);
                QOpenGLPixelTransferOptions ho;
                ho.setAlignment(4);
                t.height->setData(0, QOpenGLTexture::Red, QOpenGLTexture::Float32, hp.data(), &ho);
                t.height->setMinificationFilter(QOpenGLTexture::Linear);
                t.height->setMagnificationFilter(QOpenGLTexture::Linear);
                t.height->setWrapMode(QOpenGLTexture::ClampToEdge);
                cl.hasHeights = true;
                cl.bytes += hp.size() * 4;
            }
            cl.tiles.push_back(std::move(t));
        }
    cache_.push_front(std::move(cl));
    evict();
}

void GlMapView::paintGL() {
    glClearColor(0.86f, 0.88f, 0.90f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!glReady_) return;
    uploadPending();
    const CachedLayer* layer = nullptr;
    for (auto& c : cache_) if (c.key == current_) { layer = &c; break; }
    const ViewTransform& v = inter_->view();
    const double dpr = devicePixelRatioF();
    if (layer && !layer->tiles.empty()) {
        prog_.bind();
        prog_.setUniformValue("viewport", QVector2D(static_cast<float>(width()), static_cast<float>(height())));
        prog_.setUniformValue("colorTex", 0);
        prog_.setUniformValue("heightTex", 1);
        bool contours = contour_ > 0 && layer->hasHeights;
        prog_.setUniformValue("contours", contours ? 1 : 0);
        prog_.setUniformValue("interval", static_cast<float>(contour_ > 0 ? contour_ : 1.0));
        vao_.bind();
        vbo_.bind();
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));
        double left = v.originX(), right = v.originX() + v.viewW / v.zoom;
        int k0 = static_cast<int>(std::floor(left / v.W)) - 1, k1 = static_cast<int>(std::floor(right / v.W)) + 1;
        for (int k = k0; k <= k1; ++k) {
            double off = k * v.W;
            for (auto& t : layer->tiles) {
                double sx0, sy0, sx1, sy1;
                v.worldToScreen(t.x0 + off, t.y0, sx0, sy0);
                v.worldToScreen(t.x0 + t.w + off, t.y0 + t.h, sx1, sy1);
                if (sx1 < 0 || sx0 > width() || sy1 < 0 || sy0 > height()) continue;
                float q[24] = {float(sx0), float(sy0), 0, 0, float(sx1), float(sy0), 1, 0, float(sx1), float(sy1), 1, 1,
                               float(sx0), float(sy0), 0, 0, float(sx1), float(sy1), 1, 1, float(sx0), float(sy1), 0, 1};
                vbo_.allocate(q, sizeof q);
                glActiveTexture(GL_TEXTURE0);
                t.color->bind();
                if (contours && t.height) {
                    glActiveTexture(GL_TEXTURE1);
                    t.height->bind();
                }
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
        }
        vbo_.release();
        vao_.release();
        prog_.release();
        glActiveTexture(GL_TEXTURE0);
    }
    (void)dpr;
    if (world_) {
        QPainter p(this);
        MapProjection pr;
        pr.scale = v.zoom;
        pr.originX = v.originX();
        pr.originY = v.originY();
        pr.W = static_cast<int>(v.W);
        pr.H = static_cast<int>(v.H);
        paintOverlays(p, *world_, tab_, view_, overlay_, pr, QRectF(0, 0, width(), height()));
        // Outside the latitude range (zoomed out vertically): neutral background, no wrap of poles.
        double sy0, sy1, sx;
        v.worldToScreen(0, 0, sx, sy0);
        v.worldToScreen(0, v.H, sx, sy1);
        if (sy0 > 0) p.fillRect(QRectF(0, 0, width(), sy0), QColor(220, 224, 230));
        if (sy1 < height()) p.fillRect(QRectF(0, sy1, width(), height() - sy1), QColor(220, 224, 230));
    }
}

// ---------------------------------------------------------------- software view

SoftMapView::SoftMapView(QWidget* parent) : QWidget(parent), inter_(new MapInteraction(this)) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setAttribute(Qt::WA_OpaquePaintEvent);
    connect(inter_, &MapInteraction::viewChanged, this, [this] { update(); });
}

void SoftMapView::resizeEvent(QResizeEvent*) { inter_->resize(width(), height()); }

bool SoftMapView::event(QEvent* e) {
    if (inter_->handle(e)) return true;
    return QWidget::event(e);
}

void SoftMapView::setWorld(std::shared_ptr<const WorldData> w, MapTab tab, const ViewOptions& view) {
    world_ = std::move(w);
    tab_ = tab;
    view_ = view;
    if (world_ && world_->grid.valid()) inter_->setWorldSize(world_->grid.width(), world_->grid.height());
    update();
}

void SoftMapView::setLayerImage(const LayerImage& img) {
    for (auto it = cache_.begin(); it != cache_.end(); ++it)
        if (it->key == img.key) { cache_.erase(it); break; }
    Pyramid p;
    p.key = img.key;
    QImage level = img.image.convertToFormat(QImage::Format_RGB32);
    p.levels.push_back(level);
    while (level.width() > 256) {
        level = level.scaled(level.width() / 2, level.height() / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        p.levels.push_back(level);
    }
    cache_.push_front(std::move(p));
    while (cache_.size() > 6) cache_.pop_back();
    current_ = img.key;
    update();
}

bool SoftMapView::hasCached(const QString& key) const {
    for (auto& c : cache_) if (c.key == key) return true;
    return false;
}

void SoftMapView::showCached(const QString& key) {
    current_ = key;
    update();
}

void SoftMapView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(220, 224, 230));
    const Pyramid* pyr = nullptr;
    for (auto& c : cache_) if (c.key == current_) { pyr = &c; break; }
    const ViewTransform& v = inter_->view();
    if (pyr && !pyr->levels.empty()) {
        int level = std::clamp(static_cast<int>(std::floor(std::log2(1.0 / std::max(1e-6, v.zoom)))), 0,
                               static_cast<int>(pyr->levels.size()) - 1);
        const QImage& img = pyr->levels[static_cast<size_t>(level)];
        double cellsPerPx = v.W / img.width();
        p.setRenderHint(QPainter::SmoothPixmapTransform, v.zoom * cellsPerPx < 1.0);
        double left = v.originX(), right = v.originX() + v.viewW / v.zoom;
        int k0 = static_cast<int>(std::floor(left / v.W)) - 1, k1 = static_cast<int>(std::floor(right / v.W)) + 1;
        for (int k = k0; k <= k1; ++k) {
            double sx0, sy0, sx1, sy1;
            v.worldToScreen(k * v.W, 0, sx0, sy0);
            v.worldToScreen((k + 1) * v.W, v.H, sx1, sy1);
            QRectF target(sx0, sy0, sx1 - sx0, sy1 - sy0);
            QRectF vis = target.intersected(QRectF(rect()));
            if (vis.isEmpty()) continue;
            double fx = img.width() / target.width(), fy = img.height() / target.height();
            QRectF src((vis.left() - target.left()) * fx, (vis.top() - target.top()) * fy, vis.width() * fx, vis.height() * fy);
            p.drawImage(vis, img, src);
        }
    }
    if (world_) {
        MapProjection pr;
        pr.scale = v.zoom;
        pr.originX = v.originX();
        pr.originY = v.originY();
        pr.W = static_cast<int>(v.W);
        pr.H = static_cast<int>(v.H);
        paintOverlays(p, *world_, tab_, view_, overlay_, pr, QRectF(rect()));
    }
}

}  // namespace mapgen
