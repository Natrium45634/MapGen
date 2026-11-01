#include "ui/Panels.h"

#include <QApplication>
#include <QClipboard>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPainter>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

#include "core/stages/Pipeline.h"
#include "io/Export.h"

namespace mapgen {

// ---------------------------------------------------------------- InfoCard

InfoCard::InfoCard(QWidget* parent) : QFrame(parent) {
    setObjectName("InfoCard");
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    setStyleSheet("#InfoCard { background: rgba(255,255,255,238); border: 1px solid #7a8794; border-radius: 6px; }");
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 6, 10, 8);
    lay->setSpacing(4);
    auto* top = new QHBoxLayout;
    title_ = new QLabel("Ячейка");
    QFont f = title_->font();
    f.setBold(true);
    title_->setFont(f);
    pinBtn_ = new QPushButton("Закрепить");
    pinBtn_->setToolTip("Зафиксировать карточку на ячейке (клик по карте или клавиша F)");
    pinBtn_->setCheckable(true);
    copyBtn_ = new QPushButton("Копировать");
    copyBtn_->setToolTip("Скопировать координаты и атрибуты (Ctrl+Shift+C)");
    top->addWidget(title_, 1);
    top->addWidget(pinBtn_);
    top->addWidget(copyBtn_);
    lay->addLayout(top);
    body_ = new QLabel;
    body_->setTextFormat(Qt::RichText);
    body_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body_->setWordWrap(true);
    lay->addWidget(body_);
    connect(pinBtn_, &QPushButton::toggled, this, [this](bool on) {
        if (on != pinned_) emit pinToggled(on);
    });
    connect(copyBtn_, &QPushButton::clicked, this, &InfoCard::copyToClipboard);
    refresh();
}

void InfoCard::setPinned(bool p) {
    pinned_ = p;
    pinBtn_->blockSignals(true);
    pinBtn_->setChecked(p);
    pinBtn_->setText(p ? "Открепить" : "Закрепить");
    pinBtn_->blockSignals(false);
    refresh();
}

QString InfoCard::text() const {
    if (!world_ || cell_ < 0) return {};
    return QString::fromStdString(describeCellText(*world_, cell_, month_));
}

void InfoCard::copyToClipboard() {
    QString t = text();
    if (!t.isEmpty()) QApplication::clipboard()->setText(t);
}

void InfoCard::refresh() {
    if (!world_ || !world_->grid.valid()) {
        title_->setText("Ячейка");
        body_->setText("<i>Мир ещё не создан. Введите сид и нажмите «Сгенерировать».</i>");
        return;
    }
    if (cell_ < 0) {
        title_->setText("Ячейка");
        body_->setText("<i>Наведите курсор на карту. Клик или F — закрепить карточку.</i>");
        return;
    }
    title_->setText(pinned_ ? "Ячейка (закреплена)" : "Ячейка под курсором");
    QString html = "<table cellspacing='0' cellpadding='1'>";
    for (auto& f : describeCell(*world_, cell_, month_)) {
        bool active = f.layer == active_ ||
                      (active_ == LayerId::TempMean && f.key == "temp") || (active_ == LayerId::PrecAnnual && f.key == "prec") ||
                      (active_ == LayerId::WindU && f.key == "wind") || (active_ == LayerId::Aridity && f.key == "humidity");
        QString style = active ? "background:#fff2b3; font-weight:bold;" : "";
        QString val = f.available ? QString::fromStdString(f.valueRu).toHtmlEscaped()
                                  : QString("<span style='color:#888'>%1</span>").arg(kNotComputedRu);
        html += QString("<tr style='%1'><td style='color:#445; padding-right:8px; %1'>%2</td><td style='%1'>%3</td></tr>")
                    .arg(style, QString::fromStdString(f.labelRu).toHtmlEscaped(), val);
    }
    html += "</table>";
    body_->setText(html);
}

// ---------------------------------------------------------------- Legend

LegendWidget::LegendWidget(QWidget* parent) : QWidget(parent) { setMinimumWidth(220); }

void LegendWidget::setLegend(const Legend& l) {
    legend_ = l;
    updateGeometry();
    update();
}

QSize LegendWidget::sizeHint() const {
    int rows = static_cast<int>(legend_.items.size()) + static_cast<int>(legend_.notes.size()) * 2 + (legend_.continuous ? 4 : 0) + 4;
    return QSize(static_cast<int>(280 * scale_), static_cast<int>((40 + rows * 18) * scale_));
}

void LegendWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), palette().window());
    Legend l = legend_;
    if (highContrast)
        for (auto& it : l.items) it.color = shade(it.color, 0.9);
    paintLegend(p, l, QRectF(rect()).adjusted(2, 2, -2, -2), scale_);
}

// ---------------------------------------------------------------- Settings

SettingsPanel::SettingsPanel(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(4, 4, 4, 4);
    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel("Профиль:"));
    preset_ = new QComboBox;
    for (auto& p : presets()) {
        preset_->addItem(QString::fromStdString(p.nameRu), QString::fromStdString(p.id));
        preset_->setItemData(preset_->count() - 1, QString::fromStdString(p.descriptionRu), Qt::ToolTipRole);
    }
    top->addWidget(preset_, 1);
    outer->addLayout(top);
    connect(preset_, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (syncing_ || i < 0) return;
        const Preset* pr = findPreset(preset_->itemData(i).toString().toStdString());
        if (!pr) return;
        applyPreset(params_, *pr);
        syncWidgets();
        rebuildIssues();
        emit paramsChanged(params_);
    });

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* inner = new QWidget;
    auto* lay = new QVBoxLayout(inner);
    std::map<std::string, QFormLayout*> groups;
    std::vector<std::string> order;
    for (auto& d : paramSchema()) {
        if (!groups.count(d.group)) {
            auto* box = new QGroupBox(QString::fromStdString(d.group));
            auto* form = new QFormLayout(box);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            groups[d.group] = form;
            lay->addWidget(box);
        }
        QFormLayout* form = groups[d.group];
        QString tip = QString::fromStdString(d.descriptionRu);
        if (!d.units.empty()) tip += "\nЕдиницы: " + QString::fromStdString(d.units);
        if (d.stageInvalidated > 0) tip += QString("\nИзменение перезапускает этапы %1–8.").arg(d.stageInvalidated);
        else tip += "\nВлияет только на интерфейс/проверку.";
        if (!d.realismNotes.empty()) tip += "\nРеализм: " + QString::fromStdString(d.realismNotes);
        QWidget* ed = nullptr;
        std::string id = d.id;
        if (d.type == ParamType::Enum) {
            auto* cb = new QComboBox;
            for (auto& o : d.options) cb->addItem(QString::fromStdString(o.nameRu), QString::fromStdString(o.id));
            connect(cb, &QComboBox::currentIndexChanged, this, [this, cb, id](int) {
                onEditedStr(id, cb->currentData().toString().toStdString());
            });
            ed = cb;
        } else if (d.type == ParamType::Bool) {
            auto* ch = new QCheckBox;
            connect(ch, &QCheckBox::toggled, this, [this, id](bool on) { onEdited(id, on ? 1 : 0); });
            ed = ch;
        } else if (d.type == ParamType::Int) {
            auto* sb = new QSpinBox;
            sb->setRange(static_cast<int>(d.minValue), static_cast<int>(d.maxValue));
            if (!d.units.empty()) sb->setSuffix(" " + QString::fromStdString(d.units));
            connect(sb, &QSpinBox::valueChanged, this, [this, id](int v) { onEdited(id, v); });
            ed = sb;
        } else {
            auto* sb = new QDoubleSpinBox;
            sb->setRange(d.minValue, d.maxValue);
            double step = d.step > 0 ? d.step : (d.maxValue - d.minValue) / 100.0;
            sb->setSingleStep(step);
            sb->setDecimals(step >= 1 ? 0 : (step >= 0.1 ? 1 : 2));
            if (!d.units.empty() && d.units.size() < 14) sb->setSuffix(" " + QString::fromStdString(d.units));
            connect(sb, &QDoubleSpinBox::valueChanged, this, [this, id](double v) { onEdited(id, v); });
            ed = sb;
        }
        ed->setToolTip(tip);
        auto* label = new QLabel(QString::fromStdString(d.nameRu));
        label->setToolTip(tip);
        label->setWordWrap(true);
        form->addRow(label, ed);
        editors_[d.id] = ed;
    }
    lay->addStretch(1);
    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);
    rerun_ = new QLabel;
    rerun_->setWordWrap(true);
    outer->addWidget(rerun_);
    auto* il = new QLabel("Проверка реализма и конфликтов:");
    outer->addWidget(il);
    issues_ = new QListWidget;
    issues_->setWordWrap(true);
    issues_->setMaximumHeight(150);
    outer->addWidget(issues_);
    applyPreset(params_, *findPreset("earthlike"));
    syncWidgets();
    rebuildIssues();
}

QString SettingsPanel::presetId() const { return preset_->currentData().toString(); }

void SettingsPanel::setParams(const Params& p) {
    params_ = p;
    syncWidgets();
    rebuildIssues();
}

void SettingsPanel::setReferenceParams(const Params* ref) {
    ref_ = ref ? std::make_unique<Params>(*ref) : nullptr;
    rebuildIssues();
}

void SettingsPanel::syncWidgets() {
    syncing_ = true;
    for (auto& d : paramSchema()) {
        QWidget* ed = editors_[d.id];
        if (auto* cb = qobject_cast<QComboBox*>(ed)) cb->setCurrentIndex(cb->findData(QString::fromStdString(params_.str(d.id))));
        else if (auto* ch = qobject_cast<QCheckBox*>(ed)) ch->setChecked(params_.flag(d.id));
        else if (auto* sb = qobject_cast<QSpinBox*>(ed)) sb->setValue(params_.integer(d.id));
        else if (auto* ds = qobject_cast<QDoubleSpinBox*>(ed)) ds->setValue(params_.num(d.id));
    }
    // Dependent fields.
    const std::string& mode = params_.str("ocean.seaLevelMode");
    editors_["ocean.landFraction"]->setEnabled(mode == "fraction");
    editors_["ocean.seaLevel"]->setEnabled(mode == "fixed");
    editors_["world.customWidth"]->setEnabled(params_.str("world.resolution") == "custom");
    syncing_ = false;
}

void SettingsPanel::onEdited(const std::string& id, double v) {
    if (syncing_) return;
    params_.set(id, v);
    syncWidgets();
    rebuildIssues();
    emit paramsChanged(params_);
}

void SettingsPanel::onEditedStr(const std::string& id, const std::string& v) {
    if (syncing_) return;
    params_.set(id, v);
    syncWidgets();
    rebuildIssues();
    emit paramsChanged(params_);
}

void SettingsPanel::rebuildIssues() {
    issues_->clear();
    for (auto& i : validateParams(params_)) {
        auto* it = new QListWidgetItem(QString::fromStdString(i.messageRu));
        static const char* prefix[] = {"ℹ ", "⚠ ", "⛔ ", "✖ "};
        it->setText(prefix[i.level] + it->text());
        QColor c = i.level == ValidationIssue::Error ? QColor(200, 0, 0)
                   : i.level == ValidationIssue::Conflict ? QColor(190, 90, 0)
                   : i.level == ValidationIssue::Warning ? QColor(140, 110, 0) : QColor(60, 80, 120);
        it->setForeground(c);
        issues_->addItem(it);
    }
    if (issues_->count() == 0) issues_->addItem("Конфликтов не найдено.");
    if (ref_) {
        int s = firstInvalidatedStage(*ref_, params_);
        if (s >= 9) rerun_->setText("Параметры совпадают с текущим миром.");
        else if (s == 0) rerun_->setText("Изменения влияют только на отображение.");
        else rerun_->setText(QString("«Сгенерировать» перезапустит этапы %1–8; более ранние этапы будут взяты из текущего мира.").arg(s));
    } else {
        rerun_->setText("Мир ещё не создан: будет выполнен полный расчёт.");
    }
}

// ---------------------------------------------------------------- Process

ProcessPanel::ProcessPanel(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(4, 4, 4, 4);
    summary_ = new QLabel("Мир не создан.");
    summary_->setWordWrap(true);
    lay->addWidget(summary_);
    stages_ = new QTreeWidget;
    stages_->setHeaderLabels({"Этап", "Статус", "Время", "Хеш"});
    stages_->setRootIsDecorated(false);
    stages_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (auto& st : Pipeline::stages()) {
        auto* it = new QTreeWidgetItem({QString("%1. %2").arg(st.index).arg(st.nameRu), "—", "", ""});
        stages_->addTopLevelItem(it);
    }
    stages_->setMaximumHeight(230);
    lay->addWidget(stages_);
    auto* btn = new QPushButton("Показать снимок стадии на карте");
    lay->addWidget(btn);
    text_ = new QPlainTextEdit;
    text_->setReadOnly(true);
    text_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    lay->addWidget(text_, 1);
    connect(stages_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it, QTreeWidgetItem*) {
        if (it) showReport(stages_->indexOfTopLevelItem(it));
    });
    connect(btn, &QPushButton::clicked, this, [this] {
        int i = stages_->currentItem() ? stages_->indexOfTopLevelItem(stages_->currentItem()) : 0;
        emit showStage(i);
    });
}

void ProcessPanel::setRunning(int stage, double fraction, const QString& msg) {
    for (int i = 0; i < stages_->topLevelItemCount(); ++i) {
        auto* it = stages_->topLevelItem(i);
        if (i == stage) it->setText(1, QString("%1%").arg(static_cast<int>(fraction * 100)));
        else if (i > stage && it->text(1).endsWith('%')) it->setText(1, "ожидает");
    }
    (void)msg;
}

void ProcessPanel::setWorld(std::shared_ptr<const WorldData> w) {
    world_ = std::move(w);
    if (!world_) return;
    for (int i = 0; i < stages_->topLevelItemCount(); ++i) {
        auto* it = stages_->topLevelItem(i);
        const QualityReport& r = world_->reports[static_cast<size_t>(i)];
        bool done = i <= world_->completedStage;
        int failed = 0, failedP0 = 0;
        for (auto& c : r.checks) if (!c.passed) { ++failed; if (c.p0) ++failedP0; }
        QString status = !done ? "не выполнен" : failedP0 ? "⛔ P0" : failed ? QString("⚠ %1").arg(failed) : "✔";
        it->setText(1, status);
        it->setText(2, done ? QString::number(r.seconds, 'f', 2) + " с" : "");
        it->setText(3, QString::fromStdString(world_->stageHash[static_cast<size_t>(i)]).left(12));
    }
    const Json& s = world_->statistics;
    QString sum = QString("Сид %1 · %2×%3 · хеш мира %4")
                      .arg(QString::fromStdString(world_->seed))
                      .arg(world_->grid.width()).arg(world_->grid.height())
                      .arg(QString::fromStdString(Pipeline::worldHash(*world_)).left(16));
    if (s.isObject()) {
        sum += QString("\nСуша %1% · материков %2 · пустынь %3% суши · ледников %4% суши · вулканов %5")
                   .arg(s["landPercent"].asNumber(), 0, 'f', 1)
                   .arg(s["continents"].asInt())
                   .arg(s["desertPercentOfLand"].asNumber(), 0, 'f', 1)
                   .arg(s["glacierPercentOfLand"].asNumber(), 0, 'f', 1)
                   .arg(s["volcanoes"].asInt());
    }
    if (!world_->noticeRu.empty()) sum += "\n⚠ " + QString::fromStdString(world_->noticeRu);
    summary_->setText(sum);
    showReport(stages_->currentItem() ? stages_->indexOfTopLevelItem(stages_->currentItem()) : 8);
}

void ProcessPanel::showReport(int stage) {
    if (!world_ || stage < 0 || stage >= kStageCount) return;
    const QualityReport& r = world_->reports[static_cast<size_t>(stage)];
    QString t = QString("Этап %1 — %2\nВремя: %3 с · суррогатная достоверность: %4\n\n")
                    .arg(stage).arg(QString::fromStdString(r.stageNameRu)).arg(r.seconds, 0, 'f', 3).arg(r.confidence, 0, 'f', 2);
    t += "Проверки:\n";
    for (auto& c : r.checks)
        t += QString("  %1 %2%3 — значение %4 (ожидается %5)%6\n")
                 .arg(c.passed ? "✔" : (c.p0 ? "⛔" : "⚠"))
                 .arg(QString::fromStdString(c.nameRu))
                 .arg(c.p0 ? " [P0]" : "")
                 .arg(QString::fromStdString(formatNumber(c.value)))
                 .arg(QString::fromStdString(c.expectedRu))
                 .arg(c.detailRu.empty() ? QString() : " — " + QString::fromStdString(c.detailRu));
    if (!r.warningsRu.empty()) {
        t += "\nПредупреждения:\n";
        for (auto& w : r.warningsRu) t += "  • " + QString::fromStdString(w) + "\n";
    }
    if (!r.metrics.empty()) {
        t += "\nПоказатели:\n";
        for (auto& m : r.metrics) t += QString("  %1 = %2\n").arg(QString::fromStdString(m.first), QString::fromStdString(formatNumber(m.second)));
    }
    if (!r.logRu.empty()) {
        t += "\nЖурнал:\n";
        for (auto& l : r.logRu) t += "  " + QString::fromStdString(l) + "\n";
    }
    text_->setPlainText(t);
}

}  // namespace mapgen
