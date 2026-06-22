#include "report_tab.h"
#include "../app_controller.h"
#include "../recording/playback.h"
#include "../biofeedback/biofeedback_engine.h"
#include "../data/session_scanner.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QFileDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QPdfView>
#include <QPdfDocument>
#include <QPdfWriter>
#include <QPainter>
#include <QPainterPath>
#include <QPageSize>
#include <QPageLayout>
#include <QMarginsF>
#include <QFont>
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {
constexpr float kConfThreshold = 0.3f;
const char* kStatNames[ReportTab::STAT_COUNT] = {"ROM", "Max", "Mean", "Min", "Graph"};
}

ReportTab::ReportTab(AppController* ctrl, QWidget* parent)
    : QWidget(parent), ctrl_(ctrl), playback_(std::make_unique<PlaybackManager>()) {

    tempPdfPath_ = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                       .filePath("lts_report_preview.pdf").toStdString();

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    // -------- Left: PDF preview --------
    auto* leftPane = new QWidget;
    auto* leftLayout = new QVBoxLayout(leftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(new QLabel("<b>Report Preview</b>"));
    pdfDoc_ = new QPdfDocument(this);
    pdfView_ = new QPdfView(this);
    pdfView_->setDocument(pdfDoc_);
    pdfView_->setPageMode(QPdfView::PageMode::MultiPage);
    pdfView_->setZoomMode(QPdfView::ZoomMode::FitToWidth);
    leftLayout->addWidget(pdfView_, 1);
    root->addWidget(leftPane, 3);

    // -------- Right: controls --------
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

    // Per-angle metric checkboxes
    for (int a = 0; a < biomechAngleCount(); ++a) {
        const AngleDefinition& def = getAngleDefinition((BiomechAngle)a);
        auto* g = new QGroupBox(def.displayName);
        auto* grid = new QGridLayout(g);
        grid->setContentsMargins(8, 4, 8, 4);
        grid->setHorizontalSpacing(10);
        for (int s = 0; s < STAT_COUNT; ++s) {
            auto* cb = new QCheckBox(kStatNames[s]);
            connect(cb, &QCheckBox::toggled, this, &ReportTab::onSelectionChanged);
            checks_[a][s] = cb;
            grid->addWidget(cb, s / 3, s % 3);
        }
        rl->addWidget(g);
    }

    exportBtn_ = new QPushButton("Export PDF…");
    connect(exportBtn_, &QPushButton::clicked, this, &ReportTab::onExport);
    rl->addWidget(exportBtn_);
    rl->addStretch(1);

    rightScroll->setWidget(rightPane);
    rightScroll->setMinimumWidth(300);
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
        const QString label = QString::fromStdString(
            s.displayName.empty() ? s.prefix : s.displayName) +
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
        statusLabel_->setText(QString("Loaded %1 frames over %2 s.")
            .arg(playback_->frameCount())
            .arg(playback_->duration(), 0, 'f', 1));
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

void ReportTab::onSelectionChanged() { regenerate(); }

bool ReportTab::anySelection() const {
    for (int a = 0; a < 6; ++a)
        for (int s = 0; s < STAT_COUNT; ++s)
            if (checks_[a][s] && checks_[a][s]->isChecked()) return true;
    return false;
}

ReportTab::AngleStats ReportTab::computeAngle(int angleId) const {
    AngleStats out;
    if (!playback_->isLoaded()) return out;
    const AngleDefinition& def = getAngleDefinition((BiomechAngle)angleId);

    double sum = 0;
    for (int i = 0; i < playback_->frameCount(); ++i) {
        const PlaybackFrame* f = playback_->getFrame(i);
        if (!f || f->bodies.empty()) continue;
        const TrackedBody& b = f->bodies.front();
        const Joint& p = b.joints[def.proximalJoint];
        const Joint& v = b.joints[def.vertexJoint];
        const Joint& d = b.joints[def.distalJoint];
        if (p.confidence < kConfThreshold || v.confidence < kConfThreshold ||
            d.confidence < kConfThreshold) continue;
        // computeAngle returns the included joint angle in radians; convert to
        // degrees and express as flexion from straight (0° = fully extended).
        const double includedDeg = BiofeedbackEngine::computeAngle(p, v, d) * 180.0 / M_PI;
        const double ang = 180.0 - includedDeg;
        if (!std::isfinite(ang)) continue;
        if (!out.valid) { out.minV = out.maxV = ang; out.valid = true; }
        out.minV = std::min(out.minV, ang);
        out.maxV = std::max(out.maxV, ang);
        sum += ang;
        out.series.emplace_back(f->timeSeconds, ang);
        out.samples++;
    }
    if (out.samples > 0) {
        out.mean = sum / out.samples;
        out.rom = out.maxV - out.minV;
    }
    return out;
}

// -------- PDF generation --------
namespace {
void drawChart(QPainter& p, const QRectF& r, const std::vector<std::pair<double,double>>& series,
               const QString& title, double tMax) {
    p.save();
    p.setPen(QPen(QColor(40, 40, 40), 1));
    p.setFont(QFont("Helvetica", 9, QFont::Bold));
    p.drawText(QRectF(r.x(), r.y() - 18, r.width(), 16), Qt::AlignLeft, title);

    // axes box
    QRectF plot = r.adjusted(38, 4, -8, -18);
    p.setPen(QPen(QColor(120, 120, 120), 1));
    p.drawRect(plot);

    if (series.size() < 2) { p.restore(); return; }
    double yMin = series[0].second, yMax = series[0].second;
    for (auto& s : series) { yMin = std::min(yMin, s.second); yMax = std::max(yMax, s.second); }
    if (yMax - yMin < 1e-6) yMax = yMin + 1;

    auto X = [&](double t){ return plot.x() + (tMax > 0 ? t / tMax : 0) * plot.width(); };
    auto Y = [&](double v){ return plot.bottom() - (v - yMin) / (yMax - yMin) * plot.height(); };

    // y labels (min/max)
    p.setFont(QFont("Helvetica", 7));
    p.setPen(QColor(90, 90, 90));
    p.drawText(QRectF(r.x(), plot.y() - 6, 34, 12), Qt::AlignRight, QString::number(yMax, 'f', 0));
    p.drawText(QRectF(r.x(), plot.bottom() - 6, 34, 12), Qt::AlignRight, QString::number(yMin, 'f', 0));

    // line
    p.setPen(QPen(QColor(0, 119, 190), 1.4));
    QPainterPath path;
    path.moveTo(X(series[0].first), Y(series[0].second));
    for (size_t i = 1; i < series.size(); ++i) path.lineTo(X(series[i].first), Y(series[i].second));
    p.drawPath(path);
    p.restore();
}
}

void ReportTab::regenerate() {
    if (!pdfDoc_) return;
    // Detach the preview from the temp file before overwriting it
    pdfDoc_->close();

    QPdfWriter writer(QString::fromStdString(tempPdfPath_));
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setPageMargins(QMarginsF(15, 15, 15, 15), QPageLayout::Millimeter);
    writer.setResolution(150);
    writer.setTitle("Session Report");

    QPainter p(&writer);
    const double W = writer.width(), H = writer.height();
    double y = 0;

    // Header
    p.setFont(QFont("Helvetica", 18, QFont::Bold));
    p.setPen(QColor(0, 119, 190));
    p.drawText(QRectF(0, y, W, 40), Qt::AlignLeft, "Session Report"); y += 44;

    const auto& meta = playback_->metadata();
    p.setFont(QFont("Helvetica", 10));
    p.setPen(QColor(40, 40, 40));
    auto line = [&](const QString& s){ p.drawText(QRectF(0, y, W, 18), Qt::AlignLeft, s); y += 18; };
    line(QString("Session: %1").arg(QString::fromStdString(loadedCsv_).section('/', -1)));
    if (!meta.patientId.empty()) line("Patient: " + QString::fromStdString(meta.patientId));
    if (!meta.date.empty())      line("Date: " + QString::fromStdString(meta.date));
    line(QString("Duration: %1 s   Frames: %2")
         .arg(playback_->duration(), 0, 'f', 1).arg(playback_->frameCount()));
    y += 10;

    if (!anySelection()) {
        p.setFont(QFont("Helvetica", 11));
        p.setPen(QColor(120, 120, 120));
        p.drawText(QRectF(0, y, W, 24), Qt::AlignLeft,
                   "Select metrics on the right to build the report.");
        p.end();
        pdfDoc_->load(QString::fromStdString(tempPdfPath_));
        return;
    }

    const double tMax = playback_->duration();

    // Metrics table
    bool anyStat = false;
    for (int a = 0; a < 6 && !anyStat; ++a)
        for (int s = 0; s < STAT_GRAPH; ++s)
            if (checks_[a][s]->isChecked()) anyStat = true;

    if (anyStat) {
        p.setFont(QFont("Helvetica", 12, QFont::Bold));
        p.setPen(QColor(0, 0, 0));
        p.drawText(QRectF(0, y, W, 22), Qt::AlignLeft, "Metrics"); y += 26;

        p.setFont(QFont("Helvetica", 9, QFont::Bold));
        const double c0 = 0, c1 = W * 0.45, c2 = W * 0.68, c3 = W * 0.84;
        p.drawText(QRectF(c0, y, c1, 16), Qt::AlignLeft, "Joint / Angle");
        p.drawText(QRectF(c1, y, c2 - c1, 16), Qt::AlignLeft, "Statistic");
        p.drawText(QRectF(c2, y, c3 - c2, 16), Qt::AlignLeft, "Value");
        p.drawText(QRectF(c3, y, W - c3, 16), Qt::AlignLeft, "Samples");
        y += 16;
        p.setPen(QColor(180, 180, 180));
        p.drawLine(QPointF(0, y), QPointF(W, y)); y += 4;
        p.setFont(QFont("Helvetica", 9));
        p.setPen(QColor(30, 30, 30));

        for (int a = 0; a < 6; ++a) {
            bool wantStat = false;
            for (int s = 0; s < STAT_GRAPH; ++s) if (checks_[a][s]->isChecked()) wantStat = true;
            if (!wantStat) continue;
            const AngleStats st = computeAngle(a);
            const QString aname = getAngleDefinition((BiomechAngle)a).displayName;
            for (int s = 0; s < STAT_GRAPH; ++s) {
                if (!checks_[a][s]->isChecked()) continue;
                double val = 0;
                switch (s) { case STAT_ROM: val = st.rom; break; case STAT_MAX: val = st.maxV; break;
                             case STAT_MEAN: val = st.mean; break; case STAT_MIN: val = st.minV; break; }
                if (y > H - 30) { writer.newPage(); y = 0; }
                p.drawText(QRectF(c0, y, c1, 16), Qt::AlignLeft, aname);
                p.drawText(QRectF(c1, y, c2 - c1, 16), Qt::AlignLeft, kStatNames[s]);
                p.drawText(QRectF(c2, y, c3 - c2, 16), Qt::AlignLeft,
                           st.valid ? QString("%1°").arg(val, 0, 'f', 1) : "—");
                p.drawText(QRectF(c3, y, W - c3, 16), Qt::AlignLeft, QString::number(st.samples));
                y += 17;
            }
        }
        y += 16;
    }

    // Graphs
    for (int a = 0; a < 6; ++a) {
        if (!checks_[a][STAT_GRAPH]->isChecked()) continue;
        const AngleStats st = computeAngle(a);
        if (st.series.size() < 2) continue;
        const double chartH = 150;
        if (y + chartH > H) { writer.newPage(); y = 0; }
        y += 18; // room for title above the chart
        drawChart(p, QRectF(0, y, W, chartH),
                  st.series, QString("%1 (°)").arg(getAngleDefinition((BiomechAngle)a).displayName), tMax);
        y += chartH + 18;
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
