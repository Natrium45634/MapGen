#pragma once

// Info card, legend, parameter editor and "show process" panel.

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QTreeWidget>
#include <QWidget>
#include <map>
#include <memory>

#include "core/world/CellInfo.h"
#include "core/world/WorldData.h"
#include "render/Colorizer.h"

namespace mapgen {

// Pinned card in the bottom-right corner of the map: all main attributes of the
// cell under the cursor; the active tab's value is highlighted.
class InfoCard : public QFrame {
    Q_OBJECT
public:
    explicit InfoCard(QWidget* parent = nullptr);
    void setWorld(std::shared_ptr<const WorldData> w) { world_ = std::move(w); refresh(); }
    void setCell(qint64 cell) { cell_ = cell; refresh(); }
    void setMonth(int m) { month_ = m; refresh(); }
    void setActiveLayer(LayerId id) { active_ = id; refresh(); }
    void setPinned(bool p);
    QString text() const;
    qint64 cell() const { return cell_; }

signals:
    void pinToggled(bool pinned);

public slots:
    void copyToClipboard();

private:
    void refresh();
    std::shared_ptr<const WorldData> world_;
    qint64 cell_ = -1;
    int month_ = -1;
    LayerId active_ = LayerId::Elevation;
    bool pinned_ = false;
    QLabel* title_;
    QLabel* body_;
    QPushButton* pinBtn_;
    QPushButton* copyBtn_;
};

class LegendWidget : public QWidget {
    Q_OBJECT
public:
    explicit LegendWidget(QWidget* parent = nullptr);
    void setLegend(const Legend& l);
    void setUiScale(double s) { scale_ = s; updateGeometry(); update(); }
    QSize sizeHint() const override;
    bool highContrast = false;

protected:
    void paintEvent(QPaintEvent*) override;

private:
    Legend legend_;
    double scale_ = 1.0;
};

// Parameter editor generated from the declarative schema.
class SettingsPanel : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPanel(QWidget* parent = nullptr);
    Params params() const { return params_; }
    void setParams(const Params& p);
    void setReferenceParams(const Params* ref);  // current world, for "will rerun stages N–8"
    QString presetId() const;

signals:
    void paramsChanged(const Params& p);

private:
    void rebuildIssues();
    void syncWidgets();
    void onEdited(const std::string& id, double v);
    void onEditedStr(const std::string& id, const std::string& v);
    Params params_;
    std::unique_ptr<Params> ref_;
    QComboBox* preset_;
    QLabel* rerun_;
    QListWidget* issues_;
    std::map<std::string, QWidget*> editors_;
    bool syncing_ = false;
};

// Stage list with time, hash, checks, warnings, logs and statistics.
class ProcessPanel : public QWidget {
    Q_OBJECT
public:
    explicit ProcessPanel(QWidget* parent = nullptr);
    void setWorld(std::shared_ptr<const WorldData> w);
    void setRunning(int stage, double fraction, const QString& msg);

signals:
    void showStage(int stage);

private:
    void showReport(int stage);
    std::shared_ptr<const WorldData> world_;
    QTreeWidget* stages_;
    QPlainTextEdit* text_;
    QLabel* summary_;
};

}  // namespace mapgen
