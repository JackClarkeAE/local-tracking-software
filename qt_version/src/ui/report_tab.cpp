#include "report_tab.h"
#include "../app_controller.h"
#include "../recording/playback.h"
#include "../biofeedback/biofeedback_engine.h"
#include "../data/session_scanner.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QCheckBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QDoubleSpinBox>
#include <QStackedWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QToolButton>
#include <QLabel>
#include <QFrame>
#include <QScrollArea>
#include <QFileDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QPixmap>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QPdfView>
#include <QPdfDocument>
#include <QPdfWriter>
#include <QPainter>
#include <QPainterPath>
#include <QPageSize>
#include <QPageLayout>
#include <QMargins>
#include <QMarginsF>
#include <QFont>
#include <algorithm>
#include <cmath>
#include <functional>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {
constexpr float kConfThreshold = 0.3f;

// Angles offered in the report — ankle dorsiflexion is excluded (markerless
// depth cameras cannot measure it reliably; see the Experimental tab note).
std::vector<int> reportAngles() {
    std::vector<int> v;
    for (int a = 0; a < biomechAngleCount(); ++a)
        if (!QString(getAngleDefinition((BiomechAngle)a).displayName).contains("Ankle", Qt::CaseInsensitive))
            v.push_back(a);
    return v;
}

// Deterministic colour per distinct flag label.
QColor flagColor(const QString& label) {
    static const QColor palette[] = {
        QColor(0,119,190), QColor(214,96,77), QColor(90,160,90), QColor(170,110,200),
        QColor(220,160,40), QColor(60,170,170), QColor(200,90,150), QColor(120,120,120)};
    uint h = 0; for (QChar c : label) h = h * 131 + c.unicode();
    return palette[h % (sizeof(palette)/sizeof(palette[0]))];
}

// Healthy-adult level-walking norms (sagittal plane), degrees. Peak-flexion
// norms are consistent with our per-gait-cycle peaks (~marker-based literature:
// knee ~60-66°, hip ~40°); ROM norms from marker-based older-adult controls
// (Frontiers Aging Neurosci 2021). Ankle excluded (not reliably measurable).
double angleNorm(int angle, int radarStat) {
    const QString name = getAngleDefinition((BiomechAngle)angle).displayName;
    const bool knee = name.contains("Knee", Qt::CaseInsensitive);
    if (radarStat == 0) return knee ? 59.0 : 48.0;  // ROM
    return knee ? 60.0 : 40.0;                       // peak flexion
}

// Local-maxima gait-cycle detection on a flexion signal: each swing-phase peak
// marks one cycle. Returns index ranges [start,end) per cycle. Heuristic.
std::vector<std::pair<int,int>> detectCycles(const std::vector<std::pair<double,double>>& s) {
    std::vector<std::pair<int,int>> cycles;
    const int n = (int)s.size();
    if (n < 6) return cycles;
    double mn = s[0].second, mx = s[0].second;
    for (auto& p : s) { mn = std::min(mn, p.second); mx = std::max(mx, p.second); }
    const double prom = std::max(5.0, (mx - mn) * 0.25);     // min peak prominence
    const double minGap = 0.4;                                // s between peaks
    std::vector<int> peaks;
    for (int i = 1; i < n - 1; ++i) {
        if (s[i].second >= s[i-1].second && s[i].second > s[i+1].second &&
            s[i].second - mn > prom) {
            if (!peaks.empty() && s[i].first - s[peaks.back()].first < minGap) {
                if (s[i].second > s[peaks.back()].second) peaks.back() = i; // keep taller
            } else peaks.push_back(i);
        }
    }
    for (size_t k = 0; k + 1 < peaks.size(); ++k)
        cycles.emplace_back(peaks[k], peaks[k+1]);
    return cycles;
}
} // namespace

// ===================================================================
ReportTab::ReportTab(AppController* ctrl, QWidget* parent)
    : QWidget(parent), ctrl_(ctrl), playback_(std::make_unique<PlaybackManager>()) {

    tempPdfPath_ = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                       .filePath("lts_report_preview.pdf").toStdString();

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    // Left: PDF preview
    auto* leftPane = new QWidget;
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(new QLabel("<b>Report Preview</b>"));
    pdfDoc_ = new QPdfDocument(this);
    pdfView_ = new QPdfView(this);
    pdfView_->setDocument(pdfDoc_);
    pdfView_->setPageMode(QPdfView::PageMode::MultiPage);
    pdfView_->setZoomMode(QPdfView::ZoomMode::FitToWidth);
    pdfView_->setDocumentMargins(QMargins(28, 24, 28, 24));
    pdfView_->setPageSpacing(16);
    leftLayout->addWidget(pdfView_, 1);
    root->addWidget(leftPane, 3);

    // Right: controls
    auto* rightScroll = new QScrollArea;
    rightScroll->setWidgetResizable(true);
    rightScroll->setFrameShape(QFrame::NoFrame);
    auto* rightPane = new QWidget;
    auto* rl = new QVBoxLayout(rightPane);
    rl->setContentsMargins(4, 0, 4, 4);
    rl->setSpacing(6);

    rl->addWidget(new QLabel("<h3>REPORT (experimental)</h3>"));

    auto* sessGroup = new QGroupBox("Session");
    auto* sessLayout = new QVBoxLayout(sessGroup);
    sessionCombo_ = new QComboBox;
    connect(sessionCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ReportTab::onSessionChanged);
    sessLayout->addWidget(sessionCombo_);
    statusLabel_ = new QLabel;
    statusLabel_->setWordWrap(true);
    statusLabel_->setProperty("muted", true);
    sessLayout->addWidget(statusLabel_);
    rl->addWidget(sessGroup);

    // The wide "+" buttons + the dynamic list of metric cards
    addBtn_ = new QPushButton("+  Add Metric");
    addBtn_->setMinimumHeight(34);
    addBtn_->setStyleSheet("font-weight:bold;");
    connect(addBtn_, &QPushButton::clicked, this, &ReportTab::onAddMetric);
    rl->addWidget(addBtn_);

    addRadarBtn_ = new QPushButton("+  Add Radar Chart");
    addRadarBtn_->setMinimumHeight(30);
    connect(addRadarBtn_, &QPushButton::clicked, this, &ReportTab::onAddRadar);
    rl->addWidget(addRadarBtn_);

    auto* cardsHost = new QWidget;
    cardsLayout_ = new QVBoxLayout(cardsHost);
    cardsLayout_->setContentsMargins(0, 0, 0, 0);
    cardsLayout_->setSpacing(5);
    rl->addWidget(cardsHost);

    rl->addStretch(1);

    // Save / load a reusable "report standard" (set of metric blocks)
    auto* stdRow = new QHBoxLayout;
    auto* saveStdBtn = new QPushButton("Save Standard…");
    auto* loadStdBtn = new QPushButton("Load Standard…");
    connect(saveStdBtn, &QPushButton::clicked, this, &ReportTab::onSaveStandard);
    connect(loadStdBtn, &QPushButton::clicked, this, &ReportTab::onLoadStandard);
    stdRow->addWidget(saveStdBtn); stdRow->addWidget(loadStdBtn);
    rl->addLayout(stdRow);

    exportBtn_ = new QPushButton("Export PDF…");
    connect(exportBtn_, &QPushButton::clicked, this, &ReportTab::onExport);
    rl->addWidget(exportBtn_);

    rightScroll->setWidget(rightPane);
    rightScroll->setMinimumWidth(320);
    root->addWidget(rightScroll, 1);

    refreshSessions();
}

ReportTab::~ReportTab() = default;

void ReportTab::refreshSessions() {
    auto& scanner = ctrl_->sessionScanner();
    scanner.scan(ctrl_->config().recordingsDir);
    sessionCombo_->blockSignals(true);
    sessionCombo_->clear();
    for (auto& s : scanner.sessions()) {
        if (s.csvPaths.empty()) continue;
        const QString label = QString::fromStdString(s.displayName.empty() ? s.prefix : s.displayName) +
            (s.date.empty() ? "" : QString("  (%1)").arg(QString::fromStdString(s.date)));
        sessionCombo_->addItem(label, QString::fromStdString(s.csvPaths.front()));
    }
    sessionCombo_->blockSignals(false);
    if (sessionCombo_->count() > 0) onSessionChanged();
    else statusLabel_->setText("No recorded sessions found.");
}

void ReportTab::loadSession(const std::string& csvPath) {
    if (csvPath == loadedCsv_) return;
    playback_->clear();
    if (playback_->loadCSV(csvPath)) {
        loadedCsv_ = csvPath;
        statusLabel_->setText(QString("Loaded %1 frames over %2 s,  %3 flag(s).")
            .arg(playback_->frameCount()).arg(playback_->duration(), 0, 'f', 1)
            .arg((int)playback_->flags().size()));
    } else {
        loadedCsv_.clear();
        statusLabel_->setText("<span style='color:#e05050;'>Failed to load session CSV.</span>");
    }
}

void ReportTab::onSessionChanged() {
    const QString path = sessionCombo_->currentData().toString();
    if (path.isEmpty()) return;
    loadSession(path.toStdString());
    regenerate();
}

// (time, flexion-degrees) over the whole session for one angle.
std::vector<std::pair<double,double>> ReportTab::fullSeries(int angle) const {
    std::vector<std::pair<double,double>> out;
    if (!playback_->isLoaded()) return out;
    const AngleDefinition& def = getAngleDefinition((BiomechAngle)angle);
    out.reserve(playback_->frameCount());
    for (int i = 0; i < playback_->frameCount(); ++i) {
        const PlaybackFrame* f = playback_->getFrame(i);
        if (!f || f->bodies.empty()) continue;
        const TrackedBody& b = f->bodies.front();
        const Joint& p = b.joints[def.proximalJoint];
        const Joint& v = b.joints[def.vertexJoint];
        const Joint& d = b.joints[def.distalJoint];
        if (p.confidence < kConfThreshold || v.confidence < kConfThreshold ||
            d.confidence < kConfThreshold) continue;
        const double flex = 180.0 - BiofeedbackEngine::computeAngle(p, v, d) * 180.0 / M_PI;
        if (std::isfinite(flex)) out.emplace_back(f->timeSeconds, flex);
    }
    return out;
}

void ReportTab::windowBounds(const Metric& m, double& t0, double& t1) const {
    t0 = 0; t1 = playback_->duration();
    if (m.timeMode == TIME_ABSOLUTE) { t0 = m.tStart; t1 = m.tEnd; }
    else if (m.timeMode == TIME_FLAGS) { t0 = m.flagStart; t1 = m.flagEnd; }
    if (t1 < t0) std::swap(t0, t1);
}

// Re-resolve a flag-range metric's times from its saved labels against the
// currently loaded session, so a saved standard works across recordings.
void ReportTab::resolveFlagTimes(Metric& m) const {
    if (m.timeMode != TIME_FLAGS) return;
    auto resolve = [&](const QString& label, double fallback) -> double {
        if (label.startsWith("Session Start")) return 0.0;
        if (label.startsWith("Session End")) return playback_->duration();
        for (const auto& fl : playback_->flags())
            if (QString::fromStdString(fl.label) == label) return fl.timeSeconds;
        return fallback; // label not present in this session
    };
    m.flagStart = resolve(m.flagStartLabel, 0.0);
    m.flagEnd = resolve(m.flagEndLabel, playback_->duration());
}

ReportTab::Stats ReportTab::computeStats(const Metric& m) const {
    Stats out;
    const auto full = fullSeries(m.angle);
    if (full.empty()) return out;
    double t0, t1; windowBounds(m, t0, t1);

    for (auto& pt : full)
        if (pt.first >= t0 && pt.first <= t1) out.windowSeries.push_back(pt);
    if (out.windowSeries.empty()) return out;

    if (m.agg == AGG_PER_CYCLE) {
        const auto cycles = detectCycles(out.windowSeries);
        if (!cycles.empty()) {
            double sMin = 0, sMax = 0, sMean = 0, sRom = 0;
            for (auto& c : cycles) {
                double mn = out.windowSeries[c.first].second, mx = mn, sum = 0; int n = 0;
                for (int i = c.first; i <= c.second; ++i) {
                    double v = out.windowSeries[i].second;
                    mn = std::min(mn, v); mx = std::max(mx, v); sum += v; ++n;
                }
                sMin += mn; sMax += mx; sMean += (n ? sum/n : 0); sRom += (mx - mn);
            }
            const double nc = cycles.size();
            out.valid = true; out.cycles = (int)nc; out.samples = (int)out.windowSeries.size();
            out.minV = sMin/nc; out.maxV = sMax/nc; out.mean = sMean/nc; out.rom = sRom/nc;
            return out;
        }
        // fall through to whole-window if no cycles detected
    }

    double mn = out.windowSeries[0].second, mx = mn, sum = 0;
    for (auto& pt : out.windowSeries) { mn = std::min(mn, pt.second); mx = std::max(mx, pt.second); sum += pt.second; }
    out.valid = true;
    out.samples = (int)out.windowSeries.size();
    out.minV = mn; out.maxV = mx; out.mean = sum / out.samples; out.rom = mx - mn;
    return out;
}

// ---- metric cards (right panel) ----
static QString timeframeText(const ReportTab::Metric& m) {
    if (m.timeMode == ReportTab::TIME_ABSOLUTE)
        return QString("%1–%2 s").arg(m.tStart, 0, 'f', 1).arg(m.tEnd, 0, 'f', 1);
    if (m.timeMode == ReportTab::TIME_FLAGS)
        return QString("%1 → %2").arg(m.flagStartLabel, m.flagEndLabel);
    return "Full session";
}

void ReportTab::rebuildCards() {
    QLayoutItem* it;
    while ((it = cardsLayout_->takeAt(0))) { if (it->widget()) it->widget()->deleteLater(); delete it; }

    for (size_t i = 0; i < metrics_.size(); ++i) {
        const Metric& m = metrics_[i];
        auto* card = new QFrame;
        card->setStyleSheet("QFrame { background:#1b2027; border:1px solid #2c333d; border-radius:4px; }");
        auto* cl = new QHBoxLayout(card);
        cl->setContentsMargins(8, 5, 6, 5);
        QString title, sub;
        if (m.kind == KIND_RADAR) {
            title = QString("Radar — Knee &amp; Hip (%1)").arg(m.radarStat == 0 ? "ROM" : "peak flexion");
            sub = QString("%1 · %2").arg(timeframeText(m))
                      .arg(m.agg == AGG_PER_CYCLE ? "per gait cycle" : "whole window");
        } else {
            QStringList stats;
            if (m.rom) stats << "ROM"; if (m.max) stats << "Max"; if (m.mean) stats << "Mean";
            if (m.min) stats << "Min"; if (m.graph) stats << "Graph";
            title = getAngleDefinition((BiomechAngle)m.angle).displayName;
            sub = QString("%1 · %2 · %3").arg(stats.join(", ")).arg(timeframeText(m))
                      .arg(m.agg == AGG_PER_CYCLE ? "per gait cycle" : "whole window");
        }
        auto* lbl = new QLabel(QString("<b>%1</b><br><span style='color:#9aa3ad'>%2</span>")
            .arg(title).arg(sub));
        lbl->setTextFormat(Qt::RichText);
        cl->addWidget(lbl, 1);
        auto* rm = new QToolButton; rm->setText("✕");
        rm->setStyleSheet("QToolButton{border:none;color:#c08080;font-weight:bold;}");
        connect(rm, &QToolButton::clicked, this, [this, i]() {
            if (i < metrics_.size()) { metrics_.erase(metrics_.begin() + i); rebuildCards(); regenerate(); }
        });
        cl->addWidget(rm);
        cardsLayout_->addWidget(card);
    }
}

// ---- add-metric dialog ----
void ReportTab::onAddMetric() {
    if (!playback_->isLoaded()) { QMessageBox::information(this, "Add Metric", "Load a session first."); return; }

    QDialog dlg(this);
    dlg.setWindowTitle("Add Metric");
    auto* form = new QVBoxLayout(&dlg);

    // angle + stats
    auto* angleCombo = new QComboBox;
    for (int a : reportAngles())
        angleCombo->addItem(getAngleDefinition((BiomechAngle)a).displayName, a);
    auto* angRow = new QFormLayout; angRow->addRow("Angle:", angleCombo);
    form->addLayout(angRow);

    auto* statBox = new QGroupBox("Statistics");
    auto* sg = new QHBoxLayout(statBox);
    auto* cbRom = new QCheckBox("ROM"); cbRom->setChecked(true);
    auto* cbMax = new QCheckBox("Max"); auto* cbMean = new QCheckBox("Mean");
    auto* cbMin = new QCheckBox("Min"); auto* cbGraph = new QCheckBox("Graph");
    for (auto* c : {cbRom, cbMax, cbMean, cbMin, cbGraph}) sg->addWidget(c);
    form->addWidget(statBox);

    // timeframe
    auto* tfBox = new QGroupBox("Timeframe");
    auto* tfl = new QVBoxLayout(tfBox);
    auto* rFull = new QRadioButton("Full session"); rFull->setChecked(true);
    auto* rAbs  = new QRadioButton("Absolute times");
    auto* rFlag = new QRadioButton("Between flags");
    tfl->addWidget(rFull); tfl->addWidget(rAbs); tfl->addWidget(rFlag);

    auto* stack = new QStackedWidget;
    auto* blank = new QWidget;
    auto* absW = new QWidget; auto* absForm = new QFormLayout(absW);
    auto* spStart = new QDoubleSpinBox; auto* spEnd = new QDoubleSpinBox;
    for (auto* sp : {spStart, spEnd}) { sp->setRange(0, playback_->duration()); sp->setSuffix(" s"); sp->setDecimals(1); }
    spEnd->setValue(playback_->duration());
    absForm->addRow("Start:", spStart); absForm->addRow("End:", spEnd);
    auto* flagW = new QWidget; auto* flagForm = new QFormLayout(flagW);
    auto* flagStart = new QComboBox; auto* flagEnd = new QComboBox;
    // Each picker offers Session Start, the recorded flags, then Session End.
    for (auto* fc : {flagStart, flagEnd}) {
        QPixmap pm0(12, 12); pm0.fill(QColor(120, 120, 120));
        fc->addItem(QIcon(pm0), "Session Start  (0.0 s)", 0.0);
        for (const auto& fl : playback_->flags()) {
            QPixmap pm(12, 12); pm.fill(flagColor(QString::fromStdString(fl.label)));
            fc->addItem(QIcon(pm),
                QString("%1  (%2 s)").arg(QString::fromStdString(fl.label)).arg(fl.timeSeconds, 0, 'f', 1),
                fl.timeSeconds);
        }
        QPixmap pm1(12, 12); pm1.fill(QColor(120, 120, 120));
        fc->addItem(QIcon(pm1), QString("Session End  (%1 s)").arg(playback_->duration(), 0, 'f', 1),
                    playback_->duration());
    }
    flagStart->setCurrentIndex(0);                       // Session Start
    flagEnd->setCurrentIndex(flagEnd->count() - 1);      // Session End
    flagForm->addRow("Start:", flagStart); flagForm->addRow("End:", flagEnd);
    stack->addWidget(blank); stack->addWidget(absW); stack->addWidget(flagW);
    tfl->addWidget(stack);
    connect(rFull, &QRadioButton::toggled, this, [stack](bool on){ if(on) stack->setCurrentIndex(0); });
    connect(rAbs,  &QRadioButton::toggled, this, [stack](bool on){ if(on) stack->setCurrentIndex(1); });
    connect(rFlag, &QRadioButton::toggled, this, [stack](bool on){ if(on) stack->setCurrentIndex(2); });
    form->addWidget(tfBox);

    // aggregation
    auto* aggBox = new QGroupBox("Aggregation");
    auto* al = new QVBoxLayout(aggBox);
    auto* aWhole = new QRadioButton("Average across window"); aWhole->setChecked(true);
    auto* aCycle = new QRadioButton("Per gait cycle, then averaged");
    al->addWidget(aWhole); al->addWidget(aCycle);
    form->addWidget(aggBox);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addWidget(bb);

    if (dlg.exec() != QDialog::Accepted) return;

    Metric m;
    m.angle = angleCombo->currentData().toInt();
    m.rom = cbRom->isChecked(); m.max = cbMax->isChecked(); m.mean = cbMean->isChecked();
    m.min = cbMin->isChecked(); m.graph = cbGraph->isChecked();
    if (!(m.rom || m.max || m.mean || m.min || m.graph)) m.rom = true;
    m.timeMode = rAbs->isChecked() ? TIME_ABSOLUTE : rFlag->isChecked() ? TIME_FLAGS : TIME_FULL;
    m.tStart = spStart->value(); m.tEnd = spEnd->value();
    if (m.timeMode == TIME_FLAGS) {
        m.flagStart = flagStart->currentData().toDouble();
        m.flagEnd = flagEnd->currentData().toDouble();
        m.flagStartLabel = flagStart->currentText().section("  (", 0, 0);
        m.flagEndLabel = flagEnd->currentText().section("  (", 0, 0);
    }
    m.agg = aCycle->isChecked() ? AGG_PER_CYCLE : AGG_WHOLE;
    metrics_.push_back(m);
    rebuildCards();
    regenerate();
}

// Builds the shared timeframe + aggregation controls used by both dialogs.
// Returns a lambda that writes the chosen timeframe/aggregation into a Metric.
static std::function<void(ReportTab::Metric&)> buildTimeframe(
        QVBoxLayout* form, PlaybackManager* pb) {
    auto* tfBox = new QGroupBox("Timeframe");
    auto* tfl = new QVBoxLayout(tfBox);
    auto* rFull = new QRadioButton("Full session"); rFull->setChecked(true);
    auto* rAbs  = new QRadioButton("Absolute times");
    auto* rFlag = new QRadioButton("Between flags");
    tfl->addWidget(rFull); tfl->addWidget(rAbs); tfl->addWidget(rFlag);
    auto* stack = new QStackedWidget;
    auto* blank = new QWidget;
    auto* absW = new QWidget; auto* absForm = new QFormLayout(absW);
    auto* spStart = new QDoubleSpinBox; auto* spEnd = new QDoubleSpinBox;
    for (auto* sp : {spStart, spEnd}) { sp->setRange(0, pb->duration()); sp->setSuffix(" s"); sp->setDecimals(1); }
    spEnd->setValue(pb->duration());
    absForm->addRow("Start:", spStart); absForm->addRow("End:", spEnd);
    auto* flagW = new QWidget; auto* flagForm = new QFormLayout(flagW);
    auto* flagStart = new QComboBox; auto* flagEnd = new QComboBox;
    for (auto* fc : {flagStart, flagEnd}) {
        QPixmap pm0(12,12); pm0.fill(QColor(120,120,120));
        fc->addItem(QIcon(pm0), "Session Start  (0.0 s)", 0.0);
        for (const auto& fl : pb->flags()) {
            QPixmap pm(12,12); pm.fill(flagColor(QString::fromStdString(fl.label)));
            fc->addItem(QIcon(pm), QString("%1  (%2 s)").arg(QString::fromStdString(fl.label)).arg(fl.timeSeconds,0,'f',1), fl.timeSeconds);
        }
        QPixmap pm1(12,12); pm1.fill(QColor(120,120,120));
        fc->addItem(QIcon(pm1), QString("Session End  (%1 s)").arg(pb->duration(),0,'f',1), pb->duration());
    }
    flagStart->setCurrentIndex(0); flagEnd->setCurrentIndex(flagEnd->count()-1);
    flagForm->addRow("Start:", flagStart); flagForm->addRow("End:", flagEnd);
    stack->addWidget(blank); stack->addWidget(absW); stack->addWidget(flagW);
    tfl->addWidget(stack);
    QObject::connect(rFull, &QRadioButton::toggled, stack, [stack](bool on){ if(on) stack->setCurrentIndex(0); });
    QObject::connect(rAbs,  &QRadioButton::toggled, stack, [stack](bool on){ if(on) stack->setCurrentIndex(1); });
    QObject::connect(rFlag, &QRadioButton::toggled, stack, [stack](bool on){ if(on) stack->setCurrentIndex(2); });
    form->addWidget(tfBox);

    auto* aggBox = new QGroupBox("Aggregation");
    auto* al = new QVBoxLayout(aggBox);
    auto* aWhole = new QRadioButton("Average across window"); aWhole->setChecked(true);
    auto* aCycle = new QRadioButton("Per gait cycle, then averaged");
    al->addWidget(aWhole); al->addWidget(aCycle);
    form->addWidget(aggBox);

    return [=](ReportTab::Metric& m){
        m.timeMode = rAbs->isChecked() ? ReportTab::TIME_ABSOLUTE
                   : rFlag->isChecked() ? ReportTab::TIME_FLAGS : ReportTab::TIME_FULL;
        m.tStart = spStart->value(); m.tEnd = spEnd->value();
        if (m.timeMode == ReportTab::TIME_FLAGS) {
            m.flagStart = flagStart->currentData().toDouble();
            m.flagEnd = flagEnd->currentData().toDouble();
            m.flagStartLabel = flagStart->currentText().section("  (", 0, 0);
            m.flagEndLabel = flagEnd->currentText().section("  (", 0, 0);
        }
        m.agg = aCycle->isChecked() ? ReportTab::AGG_PER_CYCLE : ReportTab::AGG_WHOLE;
    };
}

void ReportTab::onAddRadar() {
    if (!playback_->isLoaded()) { QMessageBox::information(this, "Add Radar", "Load a session first."); return; }
    QDialog dlg(this); dlg.setWindowTitle("Add Radar Chart");
    auto* form = new QVBoxLayout(&dlg);
    form->addWidget(new QLabel("Compares knee & hip (left/right) against healthy norms."));
    auto* statCombo = new QComboBox; statCombo->addItem("Peak flexion", 1); statCombo->addItem("Range of motion (ROM)", 0);
    auto* sf = new QFormLayout; sf->addRow("Measure:", statCombo); form->addLayout(sf);
    auto apply = buildTimeframe(form, playback_.get());
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addWidget(bb);
    if (dlg.exec() != QDialog::Accepted) return;
    Metric m; m.kind = KIND_RADAR; m.radarStat = statCombo->currentData().toInt();
    apply(m);
    metrics_.push_back(m); rebuildCards(); regenerate();
}

void ReportTab::onSaveStandard() {
    if (metrics_.empty()) { QMessageBox::information(this, "Save Standard", "Add metrics first."); return; }
    QJsonArray arr;
    for (const Metric& m : metrics_) {
        QJsonObject o;
        o["kind"]=m.kind; o["angle"]=m.angle;
        o["rom"]=m.rom; o["max"]=m.max; o["mean"]=m.mean; o["min"]=m.min; o["graph"]=m.graph;
        o["radarStat"]=m.radarStat; o["timeMode"]=m.timeMode;
        o["tStart"]=m.tStart; o["tEnd"]=m.tEnd;
        o["flagStartLabel"]=m.flagStartLabel; o["flagEndLabel"]=m.flagEndLabel;
        o["agg"]=m.agg;
        arr.append(o);
    }
    QString dir = QDir(QString::fromStdString(ctrl_->config().protocolsDir)).filePath("../report_standards");
    QDir().mkpath(dir);
    QString path = QFileDialog::getSaveFileName(this, "Save Report Standard", dir, "Report Standard (*.json)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".json")) path += ".json";
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(arr).toJson()); statusLabel_->setText("Saved standard: " + path); }
    else QMessageBox::warning(this, "Save Standard", "Could not write the file.");
}

void ReportTab::onLoadStandard() {
    QString dir = QDir(QString::fromStdString(ctrl_->config().protocolsDir)).filePath("../report_standards");
    QString path = QFileDialog::getOpenFileName(this, "Load Report Standard", dir, "Report Standard (*.json)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { QMessageBox::warning(this, "Load Standard", "Could not read the file."); return; }
    const auto doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) { QMessageBox::warning(this, "Load Standard", "Invalid standard file."); return; }
    metrics_.clear();
    for (const auto& v : doc.array()) {
        const QJsonObject o = v.toObject();
        Metric m;
        m.kind=o["kind"].toInt(); m.angle=o["angle"].toInt();
        m.rom=o["rom"].toBool(); m.max=o["max"].toBool(); m.mean=o["mean"].toBool();
        m.min=o["min"].toBool(); m.graph=o["graph"].toBool();
        m.radarStat=o["radarStat"].toInt(1); m.timeMode=o["timeMode"].toInt();
        m.tStart=o["tStart"].toDouble(); m.tEnd=o["tEnd"].toDouble();
        m.flagStartLabel=o["flagStartLabel"].toString(); m.flagEndLabel=o["flagEndLabel"].toString();
        m.agg=o["agg"].toInt();
        resolveFlagTimes(m);   // map saved flag labels onto this session
        metrics_.push_back(m);
    }
    statusLabel_->setText(QString("Loaded standard (%1 blocks).").arg(metrics_.size()));
    rebuildCards(); regenerate();
}

// ---- PDF generation ----
namespace {
void drawChart(QPainter& p, const QRectF& r, const std::vector<std::pair<double,double>>& series,
               const QString& title, double t0, double t1) {
    p.save();
    p.setPen(QPen(QColor(40,40,40), 1));
    p.setFont(QFont("Helvetica", 9, QFont::Bold));
    p.drawText(QRectF(r.x(), r.y() - 18, r.width(), 16), Qt::AlignLeft, title);
    QRectF plot = r.adjusted(40, 4, -8, -18);
    p.setPen(QPen(QColor(120,120,120), 1)); p.drawRect(plot);
    if (series.size() < 2) { p.restore(); return; }
    double yMin = series[0].second, yMax = series[0].second;
    for (auto& s : series) { yMin = std::min(yMin, s.second); yMax = std::max(yMax, s.second); }
    if (yMax - yMin < 1e-6) yMax = yMin + 1;
    const double span = (t1 - t0) > 1e-6 ? (t1 - t0) : 1;
    auto X = [&](double t){ return plot.x() + (t - t0) / span * plot.width(); };
    auto Y = [&](double v){ return plot.bottom() - (v - yMin) / (yMax - yMin) * plot.height(); };
    p.setFont(QFont("Helvetica", 7)); p.setPen(QColor(90,90,90));
    p.drawText(QRectF(r.x(), plot.y()-6, 36, 12), Qt::AlignRight, QString::number(yMax,'f',0));
    p.drawText(QRectF(r.x(), plot.bottom()-6, 36, 12), Qt::AlignRight, QString::number(yMin,'f',0));
    p.setPen(QPen(QColor(0,119,190), 1.4));
    QPainterPath path; path.moveTo(X(series[0].first), Y(series[0].second));
    for (size_t i = 1; i < series.size(); ++i) path.lineTo(X(series[i].first), Y(series[i].second));
    p.drawPath(path);
    p.restore();
}

struct RadarAxis { QString label; double value; double norm; bool valid; };

// Radar where the healthy norm sits on the 3/4 ring: a value equal to the norm
// plots at 0.75 R, over-range pushes toward the rim, under-range scales in.
void drawRadar(QPainter& p, QPointF c, double R, const std::vector<RadarAxis>& ax) {
    const int N = (int)ax.size();
    if (N < 3) return;
    p.save();
    auto pt = [&](int i, double rad){ double a = -M_PI/2 + 2*M_PI*i/N;
        return QPointF(c.x() + std::cos(a)*rad, c.y() + std::sin(a)*rad); };
    auto poly = [&](double f){ QPainterPath path; for(int i=0;i<N;++i){ auto q=pt(i,f*R);
        if(i==0) path.moveTo(q); else path.lineTo(q); } path.closeSubpath(); return path; };

    p.setPen(QPen(QColor(228,228,228),1));
    p.drawPath(poly(0.25)); p.drawPath(poly(0.5)); p.drawPath(poly(1.0));
    p.setPen(QPen(QColor(215,215,215),1));
    for (int i=0;i<N;++i) p.drawLine(c, pt(i,R));

    // healthy reference at 3/4
    QPen gp(QColor(70,160,70),1.6); gp.setStyle(Qt::DashLine);
    p.setPen(gp); p.setBrush(Qt::NoBrush); p.drawPath(poly(0.75));

    // this session
    p.setPen(QPen(QColor(0,119,190),2)); p.setBrush(QColor(0,119,190,45));
    QPainterPath patient;
    for (int i=0;i<N;++i){ double frac = (ax[i].valid && ax[i].norm>0)
            ? std::min(1.33, ax[i].value/ax[i].norm)*0.75 : 0.0;
        auto q=pt(i,frac*R); if(i==0) patient.moveTo(q); else patient.lineTo(q); }
    patient.closeSubpath(); p.drawPath(patient); p.setBrush(Qt::NoBrush);

    // axis labels + values
    p.setFont(QFont("Helvetica",8)); p.setPen(QColor(40,40,40));
    for (int i=0;i<N;++i){ auto q=pt(i,R+6);
        QString t = ax[i].valid
            ? QString("%1\n%2°  (norm %3°)").arg(ax[i].label).arg(ax[i].value,0,'f',0).arg(ax[i].norm,0,'f',0)
            : ax[i].label + "\n—";
        p.drawText(QRectF(q.x()-72, q.y()-16, 144, 32), Qt::AlignHCenter|Qt::AlignVCenter, t); }
    p.restore();
}
}

void ReportTab::regenerate() {
    if (!pdfDoc_) return;
    pdfDoc_->close();

    QPdfWriter writer(QString::fromStdString(tempPdfPath_));
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageMargins(QMarginsF(15,15,15,15), QPageLayout::Millimeter);
    writer.setResolution(150);
    writer.setTitle("Session Report");
    QPainter p(&writer);
    const double W = writer.width(), H = writer.height();
    double y = 0;

    p.setFont(QFont("Helvetica", 18, QFont::Bold)); p.setPen(QColor(0,119,190));
    p.drawText(QRectF(0,y,W,40), Qt::AlignLeft, "Session Report"); y += 44;
    const auto& meta = playback_->metadata();
    p.setFont(QFont("Helvetica", 10)); p.setPen(QColor(40,40,40));
    auto line = [&](const QString& s){ p.drawText(QRectF(0,y,W,18), Qt::AlignLeft, s); y += 18; };
    line(QString("Session: %1").arg(QString::fromStdString(loadedCsv_).section('/', -1)));
    if (!meta.patientId.empty()) line("Patient: " + QString::fromStdString(meta.patientId));
    if (!meta.date.empty()) line("Date: " + QString::fromStdString(meta.date));
    line(QString("Duration: %1 s   Frames: %2").arg(playback_->duration(),0,'f',1).arg(playback_->frameCount()));
    y += 12;

    if (metrics_.empty()) {
        p.setFont(QFont("Helvetica", 11)); p.setPen(QColor(120,120,120));
        p.drawText(QRectF(0,y,W,24), Qt::AlignLeft, "Add a metric on the right to build the report.");
        p.end(); pdfDoc_->load(QString::fromStdString(tempPdfPath_)); return;
    }

    auto timeframeStr = [&](const Metric& m, double t0, double t1) {
        return (m.timeMode == TIME_FULL) ? QString("Full session")
             : (m.timeMode == TIME_ABSOLUTE) ? QString("Timeframe: %1–%2 s").arg(t0,0,'f',1).arg(t1,0,'f',1)
             : QString("Timeframe: %1 → %2  (%3–%4 s)").arg(m.flagStartLabel, m.flagEndLabel).arg(t0,0,'f',1).arg(t1,0,'f',1);
    };
    auto shortAngle = [](int a){ QString n = getAngleDefinition((BiomechAngle)a).displayName;
        n.replace("Left ","L "); n.replace("Right ","R "); n.replace(" Flexion",""); return n; };

    for (const Metric& m : metrics_) {
        double t0, t1; windowBounds(m, t0, t1);

        // ---- Radar block ----
        if (m.kind == KIND_RADAR) {
            const double radarR = std::min(W * 0.32, 210.0);
            if (y + 2*radarR + 80 > H) { writer.newPage(); y = 0; }
            p.setFont(QFont("Helvetica", 13, QFont::Bold)); p.setPen(QColor(0,0,0));
            p.drawText(QRectF(0,y,W,22), Qt::AlignLeft,
                       QString("Radar — Knee & Hip vs healthy norm (%1)")
                           .arg(m.radarStat == 0 ? "ROM" : "peak flexion"));
            y += 20;
            p.setFont(QFont("Helvetica", 9)); p.setPen(QColor(90,90,90));
            p.drawText(QRectF(0,y,W,16), Qt::AlignLeft, timeframeStr(m, t0, t1) + "   ·   " +
                       (m.agg == AGG_PER_CYCLE ? "per gait cycle, averaged" : "averaged across window"));
            y += 22;
            std::vector<RadarAxis> axes;
            for (int a : reportAngles()) {
                Metric mm = m; mm.kind = KIND_TABLE; mm.angle = a;
                const Stats s = computeStats(mm);
                const double val = (m.radarStat == 0) ? s.rom : s.maxV;
                axes.push_back({ shortAngle(a), val, angleNorm(a, m.radarStat), s.valid });
            }
            const QPointF cen(W * 0.5, y + radarR + 18);
            drawRadar(p, cen, radarR, axes);
            // legend
            p.setFont(QFont("Helvetica", 8));
            QPen gp(QColor(70,160,70)); gp.setStyle(Qt::DashLine); p.setPen(gp);
            p.drawLine(QPointF(0, cen.y()+radarR+22), QPointF(24, cen.y()+radarR+22));
            p.setPen(QColor(60,60,60));
            p.drawText(QRectF(28, cen.y()+radarR+14, W*0.5, 16), Qt::AlignLeft, "Healthy reference (¾ ring)");
            p.setPen(QPen(QColor(0,119,190),2));
            p.drawLine(QPointF(W*0.5, cen.y()+radarR+22), QPointF(W*0.5+24, cen.y()+radarR+22));
            p.setPen(QColor(60,60,60));
            p.drawText(QRectF(W*0.5+28, cen.y()+radarR+14, W*0.4, 16), Qt::AlignLeft, "This session");
            y = cen.y() + radarR + 44;
            continue;
        }

        const Stats st = computeStats(m);
        if (y > H - 90) { writer.newPage(); y = 0; }

        // Section header with the timeframe + aggregation, clearly marked
        p.setFont(QFont("Helvetica", 13, QFont::Bold)); p.setPen(QColor(0,0,0));
        p.drawText(QRectF(0,y,W,22), Qt::AlignLeft, getAngleDefinition((BiomechAngle)m.angle).displayName);
        y += 20;
        p.setFont(QFont("Helvetica", 9)); p.setPen(QColor(90,90,90));
        QString tf = (m.timeMode == TIME_FULL) ? "Full session"
                   : (m.timeMode == TIME_ABSOLUTE) ? QString("Timeframe: %1–%2 s").arg(t0,0,'f',1).arg(t1,0,'f',1)
                   : QString("Timeframe: %1 → %2  (%3–%4 s)").arg(m.flagStartLabel, m.flagEndLabel).arg(t0,0,'f',1).arg(t1,0,'f',1);
        QString aggTxt = (m.agg == AGG_PER_CYCLE)
            ? QString("per gait cycle, averaged (%1 cycles)").arg(st.cycles)
            : "averaged across window";
        p.drawText(QRectF(0,y,W,16), Qt::AlignLeft, tf + "   ·   " + aggTxt + QString("   ·   %1 samples").arg(st.samples));
        y += 18;
        p.setPen(QColor(190,190,190)); p.drawLine(QPointF(0,y), QPointF(W,y)); y += 10;

        // stat rows
        p.setFont(QFont("Helvetica", 10)); p.setPen(QColor(30,30,30));
        auto row = [&](const char* name, double val, bool show){
            if (!show) return;
            if (y > H - 24) { writer.newPage(); y = 0; }
            p.drawText(QRectF(20,y,W*0.4,18), Qt::AlignLeft, name);
            p.drawText(QRectF(W*0.4,y,W*0.3,18), Qt::AlignLeft, st.valid ? QString("%1°").arg(val,0,'f',1) : "—");
            y += 19;
        };
        row("Range of motion (ROM)", st.rom, m.rom);
        row("Maximum", st.maxV, m.max);
        row("Mean", st.mean, m.mean);
        row("Minimum", st.minV, m.min);

        if (m.graph && st.windowSeries.size() >= 2) {
            const double chartH = 150;
            if (y + chartH > H) { writer.newPage(); y = 0; }
            y += 20;
            drawChart(p, QRectF(0,y,W,chartH), st.windowSeries,
                      QString("%1 flexion (°)").arg(getAngleDefinition((BiomechAngle)m.angle).displayName), t0, t1);
            y += chartH + 16;
        }
        y += 18; // gap between metric blocks
    }

    p.end();
    pdfDoc_->load(QString::fromStdString(tempPdfPath_));
}

void ReportTab::onExport() {
    if (loadedCsv_.empty()) { QMessageBox::information(this, "Export", "Load a session first."); return; }
    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
        .filePath("session_report.pdf");
    const QString dest = QFileDialog::getSaveFileName(this, "Export Report", suggested, "PDF (*.pdf)");
    if (dest.isEmpty()) return;
    QFile::remove(dest);
    if (QFile::copy(QString::fromStdString(tempPdfPath_), dest))
        statusLabel_->setText("Exported to " + dest);
    else
        QMessageBox::warning(this, "Export", "Failed to write the PDF to that location.");
}
