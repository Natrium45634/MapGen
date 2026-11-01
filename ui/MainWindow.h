#pragma once

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QTabBar>
#include <QThread>
#include <atomic>
#include <memory>

#include "core/util/Parallel.h"
#include "core/world/WorldData.h"
#include "render/Colorizer.h"
#include "render/Overlays.h"
#include "ui/MapView.h"
#include "ui/Panels.h"

namespace mapgen {

// Runs the pipeline on a worker thread. The world is copied (layers are shared)
// and only handed back when the run completed, so cancellation, errors and
// crashes in the middle of a run never leave a half-updated world.
class GenerationWorker : public QObject {
    Q_OBJECT
public:
    GenerationWorker(std::shared_ptr<WorldData> world, int fromStage, ThreadPool* pool, CancelToken* cancel)
        : world_(std::move(world)), from_(fromStage), pool_(pool), cancel_(cancel) {}
public slots:
    void run();
signals:
    void progress(int stage, double fraction, QString message);
    void finished(std::shared_ptr<WorldData> world);
    void failed(QString message);
    void cancelled();

private:
    std::shared_ptr<WorldData> world_;
    int from_;
    ThreadPool* pool_;
    CancelToken* cancel_;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(bool forceSoftware = false, QWidget* parent = nullptr);
    ~MainWindow() override;

    // Used by UI tests.
    MapViewApi* mapView() { return mapView_; }
    std::shared_ptr<const WorldData> world() const { return world_; }
    void setTab(MapTab t);
    MapTab tab() const { return tab_; }
    bool isGenerating() const { return generating_; }
    void generateNow(const QString& seed, const Params& p);
    bool openFile(const QString& path);
    int layerRenderCount() const { return renderCount_; }

protected:
    void resizeEvent(QResizeEvent* e) override;
    void closeEvent(QCloseEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private slots:
    void onGenerate();
    void onCancel();
    void onSave();
    void onOpen();
    void onExport();
    void onRandomSeed();
    void onProgress(int stage, double fraction, QString message);
    void onFinished(std::shared_ptr<WorldData> world);
    void onFailed(QString message);
    void onCancelled();
    void onHover(qint64 cell, double lat, double lon);
    void onShowStage(int stage);
    void onUiScale();

private:
    void buildUi(bool forceSoftware);
    void installView(MapViewApi* v);
    void switchToSoftware(const QString& reason);
    void startRun(std::shared_ptr<WorldData> w, int from);
    void refreshLayer();
    void refreshSubViews();
    void placeCard();
    void applyUiScale(double s);
    QString layerKey() const;
    ViewOptions viewOptions() const;
    bool validateSeed(QString* err) const;
    Params currentParams() const;

    std::shared_ptr<const WorldData> world_;
    uint64_t worldVersion_ = 0;
    MapTab tab_ = MapTab::Physical;
    ViewOptions view_;
    OverlayOptions overlay_;
    double uiScale_ = 1.0;
    double baseFontPt_ = 9.0;

    ThreadPool pool_;
    CancelToken cancel_;
    QThread* thread_ = nullptr;
    bool generating_ = false;
    std::atomic<uint64_t> renderSerial_{0};
    int renderCount_ = 0;

    MapViewApi* mapView_ = nullptr;
    QWidget* mapHost_ = nullptr;
    InfoCard* card_ = nullptr;
    LegendWidget* legend_ = nullptr;
    SettingsPanel* settings_ = nullptr;
    ProcessPanel* process_ = nullptr;
    QLineEdit* seedEdit_ = nullptr;
    QComboBox* presetCombo_ = nullptr;
    QComboBox* resCombo_ = nullptr;
    QPushButton* genBtn_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;
    QTabBar* tabBar_ = nullptr;
    QComboBox* monthCombo_ = nullptr;
    QComboBox* subCombo_ = nullptr;
    QComboBox* contourCombo_ = nullptr;
    QSlider* opacity_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* stageLabel_ = nullptr;
    QLabel* coordLabel_ = nullptr;
    QLabel* zoomLabel_ = nullptr;
    QLabel* backendLabel_ = nullptr;
    QString lastDir_;
};

}  // namespace mapgen
