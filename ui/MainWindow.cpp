#include "ui/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressDialog>
#include <QRadioButton>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QStatusBar>
#include <QThreadPool>
#include <QToolBar>
#include <QVBoxLayout>
#include <chrono>
#include <random>
#include <thread>

#include "core/stages/Pipeline.h"
#include "io/Export.h"
#include "io/GeoWorldFile.h"
#include "mapgen/Version.h"

namespace mapgen {

namespace {
ThreadPool& renderPool() {
    static ThreadPool p(std::max(1u, std::thread::hardware_concurrency() / 2));
    return p;
}

// Runs fn on a background thread while a modal progress dialog keeps the UI responsive.
bool runWithProgress(QWidget* parent, const QString& title, const std::function<bool(FileProgress&)>& fn) {
    QProgressDialog dlg(title, "Отмена", 0, 1000, parent);
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(200);
    std::atomic<double> frac{0};
    std::atomic<bool> cancel{false}, done{false};
    bool ok = false;
    FileProgress fp;
    fp.progress = [&](double f) { frac = f; };
    fp.cancelled = [&]() { return cancel.load(); };
    std::thread t([&] {
        ok = fn(fp);
        done = true;
    });
    while (!done) {
        dlg.setValue(static_cast<int>(frac * 1000));
        if (dlg.wasCanceled()) cancel = true;
        QApplication::processEvents(QEventLoop::AllEvents, 30);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    t.join();
    dlg.setValue(1000);
    return ok;
}
}  // namespace

void GenerationWorker::run() {
    try {
        Pipeline::run(*world_, from_, *pool_, *cancel_, [this](int s, double f, const std::string& m) {
            emit progress(s, f, QString::fromStdString(m));
        });
        emit finished(world_);
    } catch (const CancelledError&) {
        emit cancelled();
    } catch (const std::bad_alloc&) {
        emit failed("Недостаточно памяти для генерации при выбранном разрешении. Уменьшите разрешение и повторите.");
    } catch (const std::exception& ex) {
        emit failed(QString::fromStdString(ex.what()));
    }
}

MainWindow::MainWindow(bool forceSoftware, QWidget* parent) : QMainWindow(parent), pool_(0) {
    qRegisterMetaType<std::shared_ptr<WorldData>>("std::shared_ptr<WorldData>");
    baseFontPt_ = QApplication::font().pointSizeF() > 0 ? QApplication::font().pointSizeF() : 9.0;
    buildUi(forceSoftware);
    QSettings s;
    lastDir_ = s.value("lastDir").toString();
    applyUiScale(s.value("uiScale", 1.0).toDouble());
    view_.colorblind = s.value("colorblind", false).toBool();
    legend_->highContrast = s.value("highContrast", false).toBool();
    setAcceptDrops(true);
    refreshLayer();
}

MainWindow::~MainWindow() {
    cancel_.cancel();
    if (thread_) {
        thread_->quit();
        thread_->wait();
    }
}

void MainWindow::buildUi(bool forceSoftware) {
    setWindowTitle(QString("MapGen %1 — генератор физико-географических карт").arg(kGeneratorVersion));
    resize(1500, 900);

    // ---- top panel
    auto* tb = addToolBar("Генерация");
    tb->setObjectName("mainToolbar");
    tb->setMovable(false);
    tb->addWidget(new QLabel(" Сид: "));
    seedEdit_ = new QLineEdit("A7Test42");
    seedEdit_->setMaxLength(128);
    seedEdit_->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z0-9]{0,128}"), seedEdit_));
    seedEdit_->setToolTip("1–128 символов: латинские буквы A–Z, a–z и цифры 0–9; регистр учитывается.");
    seedEdit_->setMinimumWidth(160);
    tb->addWidget(seedEdit_);
    auto* rnd = new QPushButton("Случайный");
    rnd->setToolTip("Подставить случайный сид");
    tb->addWidget(rnd);
    tb->addSeparator();
    tb->addWidget(new QLabel(" Профиль: "));
    presetCombo_ = new QComboBox;
    for (auto& p : presets()) presetCombo_->addItem(QString::fromStdString(p.nameRu), QString::fromStdString(p.id));
    tb->addWidget(presetCombo_);
    tb->addWidget(new QLabel(" Разрешение: "));
    resCombo_ = new QComboBox;
    for (auto& r : resolutions()) resCombo_->addItem(QString::fromStdString(r.nameRu), QString::fromStdString(r.id));
    resCombo_->setCurrentIndex(1);
    resCombo_->setItemData(2, "Требует больше памяти; генерация асинхронная.", Qt::ToolTipRole);
    resCombo_->setItemData(3, "Дополнительный режим: см. бенчмарк памяти в документации.", Qt::ToolTipRole);
    tb->addWidget(resCombo_);
    auto* paramsBtn = new QPushButton("Параметры…");
    tb->addWidget(paramsBtn);
    tb->addSeparator();
    genBtn_ = new QPushButton("Сгенерировать");
    genBtn_->setDefault(true);
    genBtn_->setStyleSheet("QPushButton { font-weight: bold; padding: 3px 14px; }");
    genBtn_->setToolTip("F5. Если мир уже есть, перезапускаются только зависимые этапы.");
    tb->addWidget(genBtn_);
    cancelBtn_ = new QPushButton("Отмена");
    cancelBtn_->setEnabled(false);
    cancelBtn_->setToolTip("Esc. Отменить генерацию; текущий мир не изменится.");
    tb->addWidget(cancelBtn_);
    tb->addSeparator();
    auto* openBtn = new QPushButton("Открыть…");
    auto* saveBtn = new QPushButton("Сохранить…");
    auto* exportBtn = new QPushButton("Экспорт…");
    tb->addWidget(openBtn);
    tb->addWidget(saveBtn);
    tb->addWidget(exportBtn);

    connect(rnd, &QPushButton::clicked, this, &MainWindow::onRandomSeed);
    connect(genBtn_, &QPushButton::clicked, this, &MainWindow::onGenerate);
    connect(cancelBtn_, &QPushButton::clicked, this, &MainWindow::onCancel);
    connect(openBtn, &QPushButton::clicked, this, &MainWindow::onOpen);
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSave);
    connect(exportBtn, &QPushButton::clicked, this, &MainWindow::onExport);

    // ---- central: tab ribbon + map
    auto* central = new QWidget;
    auto* cl = new QVBoxLayout(central);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    auto* tabRow = new QHBoxLayout;
    tabRow->setContentsMargins(4, 2, 4, 2);
    tabBar_ = new QTabBar;
    tabBar_->setExpanding(false);
    tabBar_->setUsesScrollButtons(true);
    for (auto& t : tabs()) tabBar_->addTab(QString::fromUtf8(t.nameRu));
    tabRow->addWidget(tabBar_, 1);
    subCombo_ = new QComboBox;
    tabRow->addWidget(subCombo_);
    monthCombo_ = new QComboBox;
    monthCombo_->addItem("Год");
    for (const char* m : {"Январь", "Февраль", "Март", "Апрель", "Май", "Июнь", "Июль", "Август", "Сентябрь", "Октябрь", "Ноябрь", "Декабрь"})
        monthCombo_->addItem(m);
    monthCombo_->setToolTip("Месяц для сезонных слоёв");
    tabRow->addWidget(monthCombo_);
    contourCombo_ = new QComboBox;
    contourCombo_->addItem("Без изолиний", 0);
    for (int v : {100, 200, 500, 1000, 2000}) contourCombo_->addItem(QString("Изолинии %1 м").arg(v), v);
    tabRow->addWidget(contourCombo_);
    tabRow->addWidget(new QLabel(" Вспом. слои:"));
    opacity_ = new QSlider(Qt::Horizontal);
    opacity_->setRange(0, 100);
    opacity_->setValue(100);
    opacity_->setMaximumWidth(110);
    opacity_->setToolTip("Прозрачность векторных слоёв (реки, берега, стрелки, вулканы)");
    tabRow->addWidget(opacity_);
    cl->addLayout(tabRow);
    mapHost_ = new QWidget;
    mapHost_->setMinimumSize(400, 300);
    auto* hostLay = new QVBoxLayout(mapHost_);
    hostLay->setContentsMargins(0, 0, 0, 0);
    cl->addWidget(mapHost_, 1);
    setCentralWidget(central);
    mapHost_->installEventFilter(this);

    QString reason;
    if (forceSoftware || !openGl33Available(&reason)) {
        installView(new SoftMapView);
        backendLabel_ = new QLabel;
        if (!forceSoftware)
            QMetaObject::invokeMethod(this, [this, reason] {
                statusBar()->showMessage("OpenGL 3.3 недоступен (" + reason + "): включён программный режим отрисовки.", 15000);
            }, Qt::QueuedConnection);
    } else {
        auto* gl = new GlMapView;
        connect(gl, &GlMapView::glFailed, this, [this](QString r) { switchToSoftware(r); });
        installView(gl);
    }
    card_ = new InfoCard(mapHost_);
    card_->raise();
    connect(card_, &InfoCard::pinToggled, this, [this](bool on) {
        auto& in = mapView_->interaction();
        in.setPinned(on, in.hoveredCell());
    });

    connect(tabBar_, &QTabBar::currentChanged, this, [this](int i) {
        tab_ = static_cast<MapTab>(i);
        refreshSubViews();
        refreshLayer();
    });
    connect(monthCombo_, &QComboBox::currentIndexChanged, this, [this](int i) {
        view_.month = i - 1;
        card_->setMonth(view_.month);
        refreshLayer();
    });
    connect(subCombo_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (i < 0) return;
        switch (tab_) {
            case MapTab::Plates: view_.platesView = i; break;
            case MapTab::Landforms: view_.landformView = i; break;
            case MapTab::Humidity: view_.humidity = static_cast<HumidityMetric>(i); break;
            case MapTab::Rivers: view_.riverView = i; break;
            case MapTab::Biomes: view_.biomeView = i; break;
            case MapTab::Volcanism: view_.volcanoView = i; break;
            case MapTab::Elevation: view_.rawElevation = i == 1; break;
            default: break;
        }
        refreshLayer();
    });
    connect(contourCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        view_.contourInterval = contourCombo_->currentData().toDouble();
        refreshLayer();
    });
    connect(opacity_, &QSlider::valueChanged, this, [this](int v) {
        overlay_.opacity = v / 100.0;
        mapView_->setOverlayOptions(overlay_);
    });

    // ---- docks
    auto* legendDock = new QDockWidget("Легенда", this);
    legendDock->setObjectName("legendDock");
    legend_ = new LegendWidget;
    legendDock->setWidget(legend_);
    addDockWidget(Qt::LeftDockWidgetArea, legendDock);
    auto* setDock = new QDockWidget("Параметры генерации", this);
    setDock->setObjectName("settingsDock");
    settings_ = new SettingsPanel;
    setDock->setWidget(settings_);
    addDockWidget(Qt::RightDockWidgetArea, setDock);
    auto* procDock = new QDockWidget("Процесс и контроль качества", this);
    procDock->setObjectName("processDock");
    process_ = new ProcessPanel;
    procDock->setWidget(process_);
    addDockWidget(Qt::RightDockWidgetArea, procDock);
    tabifyDockWidget(setDock, procDock);
    setDock->raise();
    connect(paramsBtn, &QPushButton::clicked, this, [setDock] { setDock->show(); setDock->raise(); });
    connect(process_, &ProcessPanel::showStage, this, &MainWindow::onShowStage);
    connect(presetCombo_, &QComboBox::currentIndexChanged, this, [this](int i) {
        const Preset* p = findPreset(presetCombo_->itemData(i).toString().toStdString());
        if (!p) return;
        Params np = settings_->params();
        applyPreset(np, *p);
        settings_->setParams(np);
    });
    connect(resCombo_, &QComboBox::currentIndexChanged, this, [this](int) {
        Params np = settings_->params();
        np.set("world.resolution", resCombo_->currentData().toString().toStdString());
        settings_->setParams(np);
    });
    connect(settings_, &SettingsPanel::paramsChanged, this, [this](const Params& p) {
        int idx = resCombo_->findData(QString::fromStdString(p.str("world.resolution")));
        if (idx >= 0 && idx != resCombo_->currentIndex()) {
            resCombo_->blockSignals(true);
            resCombo_->setCurrentIndex(idx);
            resCombo_->blockSignals(false);
        }
    });

    // ---- status bar
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setMaximumWidth(220);
    progress_->setTextVisible(false);
    stageLabel_ = new QLabel("Готово");
    coordLabel_ = new QLabel;
    coordLabel_->setMinimumWidth(220);
    zoomLabel_ = new QLabel;
    if (!backendLabel_) backendLabel_ = new QLabel;
    statusBar()->addWidget(progress_);
    statusBar()->addWidget(stageLabel_, 1);
    statusBar()->addPermanentWidget(coordLabel_);
    statusBar()->addPermanentWidget(zoomLabel_);
    statusBar()->addPermanentWidget(backendLabel_);

    // ---- menus
    auto* file = menuBar()->addMenu("Файл");
    file->addAction("Открыть…", QKeySequence::Open, this, &MainWindow::onOpen);
    file->addAction("Сохранить…", QKeySequence::Save, this, &MainWindow::onSave);
    file->addAction("Экспорт…", QKeySequence("Ctrl+E"), this, &MainWindow::onExport);
    file->addSeparator();
    file->addAction("Выход", QKeySequence::Quit, this, &QWidget::close);
    auto* gen = menuBar()->addMenu("Генерация");
    gen->addAction("Сгенерировать", QKeySequence("F5"), this, &MainWindow::onGenerate);
    gen->addAction("Полная регенерация", QKeySequence("Shift+F5"), this, [this] {
        world_.reset();
        onGenerate();
    });
    gen->addAction("Отмена", QKeySequence("Esc"), this, &MainWindow::onCancel);
    gen->addAction("Случайный сид", QKeySequence("Ctrl+R"), this, &MainWindow::onRandomSeed);
    auto* viewMenu = menuBar()->addMenu("Вид");
    viewMenu->addAction("Весь мир", QKeySequence("Home"), this, [this] { mapView_->interaction().fitWorld(); });
    viewMenu->addAction("Масштаб интерфейса…", this, &MainWindow::onUiScale);
    auto* cbAct = viewMenu->addAction("Режим цветовой доступности");
    cbAct->setCheckable(true);
    cbAct->setChecked(QSettings().value("colorblind", false).toBool());
    connect(cbAct, &QAction::toggled, this, [this](bool on) {
        view_.colorblind = on;
        QSettings().setValue("colorblind", on);
        refreshLayer();
    });
    auto* hcAct = viewMenu->addAction("Контрастные легенды");
    hcAct->setCheckable(true);
    hcAct->setChecked(QSettings().value("highContrast", false).toBool());
    connect(hcAct, &QAction::toggled, this, [this](bool on) {
        legend_->highContrast = on;
        QSettings().setValue("highContrast", on);
        legend_->update();
    });
    auto* shadeAct = viewMenu->addAction("Теневой рельеф");
    shadeAct->setCheckable(true);
    shadeAct->setChecked(true);
    connect(shadeAct, &QAction::toggled, this, [this](bool on) { view_.hillshade = on; refreshLayer(); });
    auto* ecoAct = viewMenu->addAction("Показывать экотоны");
    ecoAct->setCheckable(true);
    ecoAct->setChecked(true);
    connect(ecoAct, &QAction::toggled, this, [this](bool on) { view_.ecotones = on; refreshLayer(); });
    viewMenu->addSeparator();
    viewMenu->addAction(legendDock->toggleViewAction());
    viewMenu->addAction(setDock->toggleViewAction());
    viewMenu->addAction(procDock->toggleViewAction());
    auto* help = menuBar()->addMenu("Справка");
    help->addAction("Управление картой", this, [this] {
        QMessageBox::information(this, "Управление картой",
                                 "Колесо — масштаб относительно курсора; двойной клик — приблизить.\n"
                                 "Средняя/правая кнопка или Пробел+левая — перетаскивание (через шов ±180° без разрывов).\n"
                                 "Home — весь мир; +/− — масштаб; стрелки — сдвиг.\n"
                                 "Клик или F — закрепить карточку ячейки; Ctrl+Shift+C — скопировать её.");
    });
    help->addAction("О программе", this, [this] {
        QMessageBox::about(this, "О программе",
                           QString("MapGen %1 (алгоритм v%2, схема v%3)\n\nВоспроизводимые физико-географические карты "
                                   "землеподобной планеты по сиду.\nРаботает офлайн. Лицензии сторонних компонентов — "
                                   "THIRD_PARTY_LICENSES.md.\nЧисленный движок — правдоподобные параметризации, а не "
                                   "исследовательская симуляция.")
                               .arg(kGeneratorVersion).arg(kAlgorithmVersion).arg(kSchemaVersion));
    });
    auto* copySc = new QShortcut(QKeySequence("Ctrl+Shift+C"), this);
    connect(copySc, &QShortcut::activated, card_, &InfoCard::copyToClipboard);
    refreshSubViews();
}

void MainWindow::installView(MapViewApi* v) {
    if (mapView_) {
        mapHost_->layout()->removeWidget(mapView_->widget());
        mapView_->widget()->deleteLater();
    }
    mapView_ = v;
    mapHost_->layout()->addWidget(v->widget());
    connect(&v->interaction(), &MapInteraction::hoverCell, this, &MainWindow::onHover);
    connect(&v->interaction(), &MapInteraction::pinChanged, this, [this](bool p, qint64 cell) {
        card_->setPinned(p);
        if (p) card_->setCell(cell);
    });
    connect(&v->interaction(), &MapInteraction::viewChanged, this, [this] {
        zoomLabel_->setText(QString("масштаб ×%1").arg(mapView_->interaction().view().zoom / std::max(1e-9, mapView_->interaction().view().fitZoom()), 0, 'f', 1));
    });
    v->setOverlayOptions(overlay_);
    if (card_) card_->raise();
    if (world_) {
        v->setWorld(world_, tab_, view_);
        refreshLayer();
    }
}

void MainWindow::switchToSoftware(const QString& reason) {
    statusBar()->showMessage("OpenGL недоступен (" + reason + "): включён программный режим отрисовки.", 15000);
    QMetaObject::invokeMethod(this, [this] { installView(new SoftMapView); }, Qt::QueuedConnection);
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    placeCard();
}

bool MainWindow::eventFilter(QObject* o, QEvent* e) {
    if (o == mapHost_ && e->type() == QEvent::Resize) placeCard();
    return QMainWindow::eventFilter(o, e);
}

void MainWindow::placeCard() {
    if (!card_ || !mapHost_) return;
    int w = std::min(static_cast<int>(430 * uiScale_), mapHost_->width() - 24);
    card_->setFixedWidth(std::max(200, w));
    card_->adjustSize();
    int h = std::min(card_->sizeHint().height(), mapHost_->height() - 24);
    card_->setFixedHeight(std::max(80, h));
    card_->move(mapHost_->width() - card_->width() - 12, mapHost_->height() - card_->height() - 12);
    card_->raise();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (generating_) {
        cancel_.cancel();
        if (thread_) thread_->wait(5000);
    }
    QSettings().setValue("lastDir", lastDir_);
    e->accept();
}

bool MainWindow::validateSeed(QString* err) const {
    std::string e;
    if (!isValidSeed(seedEdit_->text().toStdString(), &e)) {
        if (err) *err = QString::fromStdString(e);
        return false;
    }
    return true;
}

Params MainWindow::currentParams() const {
    Params p = settings_->params();
    p.set("world.resolution", resCombo_->currentData().toString().toStdString());
    return p;
}

void MainWindow::onRandomSeed() {
    std::random_device rd;
    uint64_t e = (static_cast<uint64_t>(rd()) << 32) ^ rd() ^ static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    seedEdit_->setText(QString::fromStdString(randomSeedString(e, 10)));
}

void MainWindow::generateNow(const QString& seed, const Params& p) {
    seedEdit_->setText(seed);
    settings_->setParams(p);
    int idx = resCombo_->findData(QString::fromStdString(p.str("world.resolution")));
    if (idx >= 0) resCombo_->setCurrentIndex(idx);
    settings_->setParams(p);
    onGenerate();
}

void MainWindow::onGenerate() {
    if (generating_) return;
    QString err;
    if (!validateSeed(&err)) {
        QMessageBox::warning(this, "Неверный сид", err);
        return;
    }
    Params p = currentParams();
    for (auto& i : validateParams(p))
        if (i.level == ValidationIssue::Error) {
            QMessageBox::warning(this, "Ошибка параметров", QString::fromStdString(i.messageRu));
            return;
        }
    std::string seed = seedEdit_->text().toStdString();
    int from = 1;
    std::shared_ptr<WorldData> w;
    if (world_ && world_->seed == seed && !world_->storedResultOnly && world_->algorithmVersion == kAlgorithmVersion &&
        world_->completedStage >= 8) {
        from = firstInvalidatedStage(world_->params, p);
        if (from == 0 || from >= 9) {
            statusBar()->showMessage(from == 0 ? "Изменения влияют только на отображение — карта перерисована." : "Параметры не изменились.", 6000);
            refreshLayer();
            return;
        }
        w = std::make_shared<WorldData>(*world_);
    } else {
        w = std::make_shared<WorldData>();
        if (world_ && world_->storedResultOnly) statusBar()->showMessage("Мир другой версии алгоритма: выполняется полная генерация.", 8000);
    }
    w->seed = seed;
    w->params = p;
    startRun(w, from);
}

void MainWindow::startRun(std::shared_ptr<WorldData> w, int from) {
    generating_ = true;
    cancel_.reset();
    genBtn_->setEnabled(false);
    cancelBtn_->setEnabled(true);
    progress_->setValue(0);
    stageLabel_->setText(from > 1 ? QString("Частичная регенерация: этапы %1–8…").arg(from) : "Генерация…");
    thread_ = new QThread;
    auto* worker = new GenerationWorker(std::move(w), from, &pool_, &cancel_);
    worker->moveToThread(thread_);
    connect(thread_, &QThread::started, worker, &GenerationWorker::run);
    connect(worker, &GenerationWorker::progress, this, &MainWindow::onProgress, Qt::QueuedConnection);
    connect(worker, &GenerationWorker::finished, this, &MainWindow::onFinished, Qt::QueuedConnection);
    connect(worker, &GenerationWorker::failed, this, &MainWindow::onFailed, Qt::QueuedConnection);
    connect(worker, &GenerationWorker::cancelled, this, &MainWindow::onCancelled, Qt::QueuedConnection);
    auto finish = [this] { if (thread_) thread_->quit(); };
    connect(worker, &GenerationWorker::finished, this, finish);
    connect(worker, &GenerationWorker::failed, this, finish);
    connect(worker, &GenerationWorker::cancelled, this, finish);
    connect(thread_, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread_, &QThread::finished, thread_, &QObject::deleteLater);
    connect(thread_, &QThread::finished, this, [this] { thread_ = nullptr; });
    thread_->start();
}

void MainWindow::onCancel() {
    if (!generating_) return;
    cancel_.cancel();
    stageLabel_->setText("Отмена…");
}

void MainWindow::onProgress(int stage, double fraction, QString message) {
    progress_->setValue(static_cast<int>((stage + fraction) / kStageCount * 1000));
    stageLabel_->setText(QString("Этап %1/8: %2 — %3%").arg(stage).arg(message).arg(static_cast<int>(fraction * 100)));
    process_->setRunning(stage, fraction, message);
}

void MainWindow::onFinished(std::shared_ptr<WorldData> w) {
    generating_ = false;
    genBtn_->setEnabled(true);
    cancelBtn_->setEnabled(false);
    world_ = std::move(w);
    ++worldVersion_;
    mapView_->clearCache();
    mapView_->setWorld(world_, tab_, view_);
    card_->setWorld(world_);
    process_->setWorld(world_);
    settings_->setReferenceParams(&world_->params);
    progress_->setValue(1000);
    double total = 0;
    for (auto& r : world_->reports) total += r.seconds;
    int warn = 0;
    bool p0 = true;
    for (auto& r : world_->reports) {
        for (auto& c : r.checks) if (!c.passed) ++warn;
        p0 = p0 && r.passedP0();
    }
    stageLabel_->setText(QString("Готово за %1 с · проверки: %2").arg(total, 0, 'f', 1).arg(p0 ? (warn ? QString("%1 предупрежд.").arg(warn) : "все пройдены") : "НЕ ПРОЙДЕН P0"));
    setWindowTitle(QString("MapGen %1 — сид %2").arg(kGeneratorVersion, QString::fromStdString(world_->seed)));
    backendLabel_->setText(mapView_->backendName());
    refreshLayer();
}

void MainWindow::onFailed(QString message) {
    generating_ = false;
    genBtn_->setEnabled(true);
    cancelBtn_->setEnabled(false);
    stageLabel_->setText("Ошибка генерации");
    QMessageBox::critical(this, "Ошибка генерации", message + "\n\nТекущий мир не изменён.");
}

void MainWindow::onCancelled() {
    generating_ = false;
    genBtn_->setEnabled(true);
    cancelBtn_->setEnabled(false);
    progress_->setValue(0);
    stageLabel_->setText("Генерация отменена; текущий мир не изменён.");
}

void MainWindow::onHover(qint64 cell, double lat, double lon) {
    if (cell >= 0)
        coordLabel_->setText(QString("%1° %2, %3° %4").arg(std::fabs(lat), 0, 'f', 2).arg(lat >= 0 ? "с.ш." : "ю.ш.")
                                 .arg(std::fabs(lon), 0, 'f', 2).arg(lon >= 0 ? "в.д." : "з.д."));
    else coordLabel_->clear();
    card_->setCell(cell);
    placeCard();
}

void MainWindow::setTab(MapTab t) { tabBar_->setCurrentIndex(static_cast<int>(t)); }

void MainWindow::onShowStage(int stage) {
    static const MapTab map[] = {MapTab::Physical, MapTab::Plates, MapTab::Elevation, MapTab::Physical, MapTab::Temperature,
                                 MapTab::Rivers, MapTab::Biomes, MapTab::Volcanism, MapTab::Physical};
    setTab(map[std::clamp(stage, 0, 8)]);
    if (stage == 2) subCombo_->setCurrentIndex(1);
}

ViewOptions MainWindow::viewOptions() const { return view_; }

QString MainWindow::layerKey() const {
    const ViewOptions& o = view_;
    return QString("%1|%2|m%3|h%4|l%5|b%6|p%7|v%8|r%9|cb%10|hs%11|e%12|raw%13")
        .arg(worldVersion_).arg(static_cast<int>(tab_)).arg(o.month).arg(static_cast<int>(o.humidity)).arg(o.landformView)
        .arg(o.biomeView).arg(o.platesView).arg(o.volcanoView).arg(o.riverView).arg(o.colorblind).arg(o.hillshade)
        .arg(o.ecotones).arg(o.rawElevation);
}

void MainWindow::refreshSubViews() {
    subCombo_->blockSignals(true);
    subCombo_->clear();
    QStringList items;
    int cur = 0;
    switch (tab_) {
        case MapTab::Plates: items = {"Плиты и типы коры", "Классы границ", "Возраст океанической коры"}; cur = view_.platesView; break;
        case MapTab::Landforms: items = {"Формы рельефа", "Структурный генезис", "Литология"}; cur = view_.landformView; break;
        case MapTab::Humidity: items = {"Индекс аридности P/PET", "Относительная влажность, %", "Запас воды в почве, мм"}; cur = static_cast<int>(view_.humidity); break;
        case MapTab::Rivers: items = {"Бассейны и озёра", "Расход"}; cur = view_.riverView; break;
        case MapTab::Biomes: items = {"Биомы", "Высотные пояса"}; cur = view_.biomeView; break;
        case MapTab::Volcanism: items = {"Потенциал активности 0–5", "Геотермальный потенциал", "Сейсмотектоническая активность"}; cur = view_.volcanoView; break;
        case MapTab::Elevation: items = {"Итоговый рельеф", "Структурный рельеф до эрозии"}; cur = view_.rawElevation ? 1 : 0; break;
        default: break;
    }
    subCombo_->addItems(items);
    subCombo_->setCurrentIndex(cur);
    subCombo_->setVisible(!items.isEmpty());
    subCombo_->blockSignals(false);
    monthCombo_->setEnabled(tabInfo(tab_).monthly);
    contourCombo_->setVisible(tab_ == MapTab::Elevation);
    card_->setActiveLayer(tabInfo(tab_).primaryLayer);
}

void MainWindow::refreshLayer() {
    if (!mapView_) return;
    ViewOptions opt = view_;
    mapView_->setWorld(world_, tab_, opt);
    legend_->setLegend(world_ ? legendFor(*world_, tab_, opt) : Legend{});
    double contours = tab_ == MapTab::Elevation ? opt.contourInterval : 0;
    bool gl = dynamic_cast<GlMapView*>(mapView_) != nullptr;
    mapView_->setContours(gl ? contours : 0);
    if (!world_) return;
    if (!gl) opt.contourInterval = contours;  // baked into the image in software mode
    else opt.contourInterval = 0;
    QString key = layerKey() + (gl ? "" : QString("|c%1").arg(contours));
    if (mapView_->hasCached(key)) {
        mapView_->showCached(key);
        return;
    }
    uint64_t serial = ++renderSerial_;
    std::shared_ptr<const WorldData> w = world_;
    MapTab tab = tab_;
    bool wantHeights = gl && tab == MapTab::Elevation;
    stageLabel_->setText(stageLabel_->text());
    QThreadPool::globalInstance()->start([this, w, tab, opt, key, serial, wantHeights] {
        LayerImage li;
        li.key = key;
        li.image = colorizeTab(*w, tab, opt, &renderPool());
        static const MapTab categorical[] = {MapTab::Plates, MapTab::Landforms, MapTab::Koppen, MapTab::Biomes, MapTab::Coasts, MapTab::Rivers, MapTab::SnowIce};
        li.categorical = std::find(std::begin(categorical), std::end(categorical), tab) != std::end(categorical);
        if (wantHeights) li.heights = std::make_shared<std::vector<float>>(elevationAboveSea(*w));
        QMetaObject::invokeMethod(this, [this, li, serial] {
            ++renderCount_;
            if (serial != renderSerial_.load() && mapView_->hasCached(li.key)) return;
            mapView_->setLayerImage(li);
            if (serial != renderSerial_.load()) mapView_->showCached(layerKey());
        }, Qt::QueuedConnection);
    });
}

void MainWindow::onSave() {
    if (!world_) {
        QMessageBox::information(this, "Сохранение", "Сначала создайте мир.");
        return;
    }
    QString path = QFileDialog::getSaveFileName(this, "Сохранить проект", QDir(lastDir_).filePath(QString::fromStdString(world_->seed) + ".geoworld"),
                                                "Проект MapGen (*.geoworld)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".geoworld", Qt::CaseInsensitive)) path += ".geoworld";
    lastDir_ = QFileInfo(path).absolutePath();
    std::string err;
    auto w = world_;
    bool ok = runWithProgress(this, "Сохранение проекта…", [&](FileProgress& fp) { return saveGeoWorld(*w, path, &err, fp); });
    if (!ok) QMessageBox::critical(this, "Сохранение", QString::fromStdString(err.empty() ? "Сохранение не выполнено." : err));
    else statusBar()->showMessage("Сохранено: " + path, 8000);
}

bool MainWindow::openFile(const QString& path) {
    auto w = std::make_shared<WorldData>();
    LoadResult r;
    runWithProgress(this, "Открытие проекта…", [&](FileProgress& fp) {
        r = loadGeoWorld(path, *w, fp);
        return r.ok;
    });
    if (!r.ok) {
        QMessageBox::critical(this, "Открытие", QString::fromStdString(r.errorRu));
        return false;
    }
    lastDir_ = QFileInfo(path).absolutePath();
    seedEdit_->setText(QString::fromStdString(w->seed));
    settings_->setParams(w->params);
    int idx = resCombo_->findData(QString::fromStdString(w->params.str("world.resolution")));
    if (idx >= 0) {
        resCombo_->blockSignals(true);
        resCombo_->setCurrentIndex(idx);
        resCombo_->blockSignals(false);
    }
    onFinished(w);
    QString msg = "Открыт проект " + path + " (контрольные суммы и хеши слоёв проверены)";
    if (!r.warningsRu.empty()) {
        QString all;
        for (auto& s : r.warningsRu) all += "• " + QString::fromStdString(s) + "\n";
        QMessageBox::warning(this, "Открытие", all);
    }
    statusBar()->showMessage(msg, 10000);
    return true;
}

void MainWindow::onOpen() {
    QString path = QFileDialog::getOpenFileName(this, "Открыть проект", lastDir_, "Проект MapGen (*.geoworld)");
    if (!path.isEmpty()) openFile(path);
}

void MainWindow::onExport() {
    if (!world_) {
        QMessageBox::information(this, "Экспорт", "Сначала создайте мир.");
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle("Экспорт");
    auto* lay = new QVBoxLayout(&dlg);
    struct Opt { QString id, name; };
    std::vector<Opt> opts = {{"png_legend", "PNG текущей вкладки с легендой"},
                             {"png", "PNG текущей вкладки без легенды"},
                             {"height16", "Карта высот 16 бит (PNG) + описание масштаба"},
                             {"tif_elev", "GeoTIFF float32: высоты/глубины"},
                             {"tif_layer", "GeoTIFF float32: основной слой текущей вкладки"},
                             {"geojson", "GeoJSON: берега, реки, границы плит, вулканы"},
                             {"csv", "CSV: статистика, вулканы, озёра, бассейны, реки"},
                             {"json", "JSON метаданных и параметров"},
                             {"all", "Полный комплект в папку"}};
    std::vector<QRadioButton*> radios;
    for (auto& o : opts) {
        auto* rb = new QRadioButton(o.name);
        radios.push_back(rb);
        lay->addWidget(rb);
    }
    radios[0]->setChecked(true);
    auto* note = new QLabel(QString("Размер сетки %1×%2. 8-битный PNG не является точной моделью высот — для DEM используйте 16 бит или GeoTIFF.")
                                .arg(world_->grid.width()).arg(world_->grid.height()));
    note->setWordWrap(true);
    lay->addWidget(note);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    QString which;
    for (size_t i = 0; i < radios.size(); ++i) if (radios[i]->isChecked()) which = opts[i].id;
    std::string sizeKey = which == "all" ? "all" : (which.startsWith("tif") ? "geotiff" : "png");
    qint64 est = estimateExportBytes(*world_, sizeKey);
    if (est > (300ll << 20) &&
        QMessageBox::question(this, "Экспорт", QString("Ожидаемый размер около %1 МБ. Продолжить?").arg(est >> 20)) != QMessageBox::Yes)
        return;
    QString base = QDir(lastDir_).filePath(QString::fromStdString(world_->seed) + "_" + tabInfo(tab_).id);
    QString path;
    if (which == "all") path = QFileDialog::getExistingDirectory(this, "Папка для экспорта", lastDir_);
    else {
        QString filter = which.startsWith("png") || which == "height16" ? "PNG (*.png)" : which.startsWith("tif") ? "GeoTIFF (*.tif)"
                         : which == "geojson" ? "GeoJSON (*.geojson)" : which == "csv" ? "CSV (*.csv)" : "JSON (*.json)";
        QString ext = filter.mid(filter.indexOf("*.") + 1).chopped(1);
        path = QFileDialog::getSaveFileName(this, "Экспорт", base + ext, filter);
    }
    if (path.isEmpty()) return;
    lastDir_ = QFileInfo(path).absolutePath();
    auto w = world_;
    MapTab tab = tab_;
    ViewOptions vo = view_;
    std::string err;
    bool ok = runWithProgress(this, "Экспорт…", [&](FileProgress& fp) {
        if (which == "png_legend") return exportLayerPng(*w, tab, vo, true, path, &err);
        if (which == "png") return exportLayerPng(*w, tab, vo, false, path, &err);
        if (which == "height16") return exportHeightmap16(*w, path, &err);
        if (which == "tif_elev") return exportGeoTiff(*w, LayerId::Elevation, 0, path, &err);
        if (which == "tif_layer") {
            LayerId id = tabInfo(tab).primaryLayer;
            const LayerInfo& li = layerInfo(id);
            if (li.categorical || li.grid == GridKind::Climate) id = LayerId::Elevation;
            int band = (li.bands > 1 && vo.month >= 0) ? vo.month : 0;
            return exportGeoTiff(*w, id, band, path, &err);
        }
        if (which == "geojson") return exportGeoJson(*w, path, "all", &err);
        if (which == "csv") {
            QString stem = path.endsWith(".csv") ? path.chopped(4) : path;
            bool any = false;
            for (const char* t : {"statistics", "volcanoes", "lakes", "basins", "rivers"}) {
                std::string e;
                any |= exportCsv(*w, stem + "_" + t + ".csv", t, &e);
            }
            if (!any) err = "Нет табличных данных.";
            return any;
        }
        if (which == "json") return exportMetadataJson(*w, path, &err);
        ExportProgress ep;
        ep.progress = fp.progress;
        ep.cancelled = fp.cancelled;
        return exportAll(*w, path, vo, &err, ep);
    });
    if (!ok) QMessageBox::critical(this, "Экспорт", QString::fromStdString(err.empty() ? "Экспорт не выполнен." : err));
    else statusBar()->showMessage("Экспорт завершён: " + path, 8000);
}

void MainWindow::onUiScale() {
    QDialog dlg(this);
    dlg.setWindowTitle("Масштаб интерфейса");
    auto* lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel("Масштаб элементов интерфейса (не влияет на масштаб карты).\nУчитывается вместе с масштабом Windows (DPI)."));
    auto* row = new QHBoxLayout;
    auto* slider = new QSlider(Qt::Horizontal);
    slider->setRange(75, 200);
    slider->setValue(static_cast<int>(std::lround(uiScale_ * 100)));
    auto* spin = new QSpinBox;
    spin->setRange(75, 200);
    spin->setSuffix(" %");
    spin->setValue(slider->value());
    row->addWidget(slider, 1);
    row->addWidget(spin);
    lay->addLayout(row);
    connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
    connect(spin, &QSpinBox::valueChanged, slider, &QSlider::setValue);
    connect(spin, &QSpinBox::valueChanged, this, [this](int v) { applyUiScale(v / 100.0); });
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);
    dlg.exec();
    QSettings().setValue("uiScale", uiScale_);
}

void MainWindow::applyUiScale(double s) {
    uiScale_ = std::clamp(s, 0.75, 2.0);
    QFont f = QApplication::font();
    f.setPointSizeF(baseFontPt_ * uiScale_);
    QApplication::setFont(f);
    for (QWidget* w : findChildren<QWidget*>()) w->setFont(f);
    legend_->setUiScale(uiScale_);
    placeCard();
    QSettings().setValue("uiScale", uiScale_);
}

}  // namespace mapgen
