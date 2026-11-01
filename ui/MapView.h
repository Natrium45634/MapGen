#pragma once

// Interactive map views. GlMapView renders cached tile textures (with mipmaps)
// through OpenGL 3.3 core and draws contours on the GPU; SoftMapView is the
// software fallback (QPainter + mip pyramid) used when GPU acceleration is
// missing. Both share MapInteraction for zoom/pan/hover/pin behaviour. Images
// are produced on worker threads; only the GUI thread touches textures.

#include <QElapsedTimer>
#include <QImage>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLPixelTransferOptions>
#include <QOpenGLTexture>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QVariantAnimation>
#include <QWidget>
#include <list>
#include <map>
#include <memory>

#include "core/world/WorldData.h"
#include "render/Overlays.h"
#include "ui/ViewTransform.h"

namespace mapgen {

struct LayerImage {
    QString key;                       // cache key (world version + tab + options)
    QImage image;                      // ARGB32, full grid size
    std::shared_ptr<std::vector<float>> heights;  // optional, for GPU contours
    bool categorical = false;
};

class MapInteraction;

// Common interface of both views (used by MainWindow).
class MapViewApi {
public:
    virtual ~MapViewApi() = default;
    virtual QWidget* widget() = 0;
    virtual void setLayerImage(const LayerImage& img) = 0;
    virtual bool hasCached(const QString& key) const = 0;
    virtual void showCached(const QString& key) = 0;
    virtual void clearCache() = 0;
    virtual void setWorld(std::shared_ptr<const WorldData> w, MapTab tab, const ViewOptions& view) = 0;
    virtual void setContours(double intervalM) = 0;
    virtual void setOverlayOptions(const OverlayOptions& o) = 0;
    virtual MapInteraction& interaction() = 0;
    virtual QString backendName() const = 0;
};

// Shared input handling: wheel zoom at cursor with smooth animation, pan with
// middle/right button or Space+left drag, double-click zoom, Home fit, click pin.
class MapInteraction : public QObject {
    Q_OBJECT
public:
    explicit MapInteraction(QWidget* target);
    ViewTransform& view() { return view_; }
    const ViewTransform& view() const { return view_; }
    void setWorldSize(int w, int h);
    void resize(int w, int h);
    bool handle(QEvent* e);   // returns true if consumed
    void fitWorld();
    void zoomBy(double factor, QPointF at);
    int64_t hoveredCell() const { return hovered_; }
    bool pinned() const { return pinned_; }
    void setPinned(bool p, int64_t cell = -1);

signals:
    void viewChanged();
    void hoverCell(qint64 cell, double lat, double lon);
    void pinChanged(bool pinned, qint64 cell);

private:
    void updateHover(QPointF pos);
    QWidget* target_;
    ViewTransform view_;
    int gridW_ = 0, gridH_ = 0;
    bool dragging_ = false, spaceDown_ = false, moved_ = false;
    QPointF lastPos_, pressPos_, lastMouse_;
    int64_t hovered_ = -1;
    bool pinned_ = false;
    QVariantAnimation anim_;
    double animFrom_ = 1, animTo_ = 1;
    QPointF animAt_;
};

class GlMapView : public QOpenGLWidget, public MapViewApi, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    explicit GlMapView(QWidget* parent = nullptr);
    ~GlMapView() override;

    QWidget* widget() override { return this; }
    void setLayerImage(const LayerImage& img) override;
    bool hasCached(const QString& key) const override;
    void showCached(const QString& key) override;
    void clearCache() override;
    void setWorld(std::shared_ptr<const WorldData> w, MapTab tab, const ViewOptions& view) override;
    void setContours(double intervalM) override { contour_ = intervalM; update(); }
    void setOverlayOptions(const OverlayOptions& o) override { overlay_ = o; update(); }
    MapInteraction& interaction() override { return *inter_; }
    QString backendName() const override { return backend_; }
    bool failed() const { return failed_; }

signals:
    void glFailed(QString reason);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    bool event(QEvent* e) override;

private:
    struct Tile {
        int x0, y0, w, h;
        std::unique_ptr<QOpenGLTexture> color;
        std::unique_ptr<QOpenGLTexture> height;
    };
    struct CachedLayer {
        QString key;
        std::vector<Tile> tiles;
        size_t bytes = 0;
        bool hasHeights = false;
    };
    void uploadPending();
    void evict();

    MapInteraction* inter_;
    QOpenGLShaderProgram prog_;
    QOpenGLVertexArrayObject vao_;
    QOpenGLBuffer vbo_{QOpenGLBuffer::VertexBuffer};
    bool glReady_ = false, failed_ = false;
    QString backend_ = "OpenGL";
    std::list<CachedLayer> cache_;  // front = most recent
    std::unique_ptr<LayerImage> pending_;
    QString current_;
    size_t budgetBytes_ = 768u << 20;
    int maxTex_ = 4096;
    double contour_ = 0;
    std::shared_ptr<const WorldData> world_;
    MapTab tab_ = MapTab::Physical;
    ViewOptions view_;
    OverlayOptions overlay_;
};

class SoftMapView : public QWidget, public MapViewApi {
    Q_OBJECT
public:
    explicit SoftMapView(QWidget* parent = nullptr);
    QWidget* widget() override { return this; }
    void setLayerImage(const LayerImage& img) override;
    bool hasCached(const QString& key) const override;
    void showCached(const QString& key) override;
    void clearCache() override { cache_.clear(); current_.clear(); update(); }
    void setWorld(std::shared_ptr<const WorldData> w, MapTab tab, const ViewOptions& view) override;
    void setContours(double) override {}
    void setOverlayOptions(const OverlayOptions& o) override { overlay_ = o; update(); }
    MapInteraction& interaction() override { return *inter_; }
    QString backendName() const override { return "программный"; }

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool event(QEvent* e) override;

private:
    struct Pyramid {
        QString key;
        std::vector<QImage> levels;
    };
    MapInteraction* inter_;
    std::list<Pyramid> cache_;
    QString current_;
    std::shared_ptr<const WorldData> world_;
    MapTab tab_ = MapTab::Physical;
    ViewOptions view_;
    OverlayOptions overlay_;
};

// Probes OpenGL 3.3 core availability without creating a window.
bool openGl33Available(QString* reason);

}  // namespace mapgen
